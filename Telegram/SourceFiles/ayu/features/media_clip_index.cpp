// FurryGram: full-chat photo indexer for CLIP image search.
#include "ayu/features/media_clip_index.h"

#include "ayu/features/media_clip.h"
#include "ayu/features/media_clip_db.h"
#include "ayu/features/furry_lang.h"

#include "apiwrap.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_msg_id.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_search_controller.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/storage_shared_media.h"
#include "ui/image/image.h"
#include "ui/image/image_location.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "base/timer.h"
#include "base/debug_log.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <map>
#include <memory>
#include <set>

namespace Ayu::ClipIndex {
namespace {

constexpr auto kWarnThreshold = 300; // call it a "large" chat above this.
constexpr auto kLoadWindow = 12;     // max thumbnail loads in flight.
constexpr auto kEmbedBatch = 8;      // embeddings per worker task / DB txn.
constexpr auto kThumbPx = 56;
constexpr auto kLoadTimeoutMs = crl::time(60000); // give up on a stuck photo.
constexpr auto kPumpIntervalMs = crl::time(1000);

constexpr auto kPhotoType = Storage::SharedMediaType::Photo;

struct State {
	bool scanning = true;
	bool finished = false;
	int total = 0;     // fullCount from the server (scan phase)
	int scanned = 0;   // photos seen while scanning
	int work = 0;      // photos queued to embed (after dedup vs cache)
	int processed = 0; // embedded + skipped
	int indexed = 0;   // newly embedded
	int cached = 0;    // photos already in the cache (skipped)

	friend bool operator==(const State &, const State &) = default;
};

// A genuinely-loaded, decoded image for the photo (Large or the 320px
// Thumbnail). We deliberately skip Small / the inline strip: the stripped
// preview is a header-less JPEG that decodes as garbage and is too tiny for
// CLIP anyway. Returns a null QImage until a real thumbnail has loaded.
[[nodiscard]] QImage ReadyImage(
		const std::shared_ptr<Data::PhotoMedia> &view) {
	for (const auto size : {
			Data::PhotoSize::Large,
			Data::PhotoSize::Thumbnail }) {
		if (const auto image = view->image(size)) {
			auto result = image->original();
			if (!result.isNull()) {
				return result;
			}
		}
	}
	return {};
}

class Indexer final
	: public std::enable_shared_from_this<Indexer> {
public:
	Indexer(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer);

	void begin();

private:
	struct Work {
		MsgId msgId = 0;
		int64 photoId = 0;
	};
	struct Pending {
		int64 photoId = 0;
		crl::time deadline = 0;
		std::shared_ptr<Data::PhotoMedia> view;
	};
	struct EmbInput {
		int64 photoId = 0;
		int64 msgId = 0;
		QImage image;
	};

	void requestPage(MsgId fromId);
	void scanDone();
	void startProcessing();
	void pump();
	void launchEmbed();
	void finish();
	void cancel();
	void publish();
	void showProgressBox();

	const not_null<Window::SessionController*> _controller;
	const not_null<PeerData*> _peer;
	const not_null<Main::Session*> _session;
	const int64 _peerId = 0;

	std::set<int64> _haveCached;
	std::vector<Work> _work;
	std::size_t _next = 0;
	std::map<MsgId, Pending> _pending;
	std::vector<EmbInput> _toEmbed;
	bool _embedRunning = false;

	MsgId _lastFrom = 0;
	int _total = 0;
	int _scanned = 0;
	int _alreadyCached = 0;
	int _processed = 0;
	int _indexed = 0;
	bool _scanning = true;
	bool _finished = false;
	bool _cancelled = false;

	base::Timer _timer;
	rpl::lifetime _downloader;
	rpl::variable<State> _state;

};

Indexer::Indexer(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer)
	: _controller(controller)
	, _peer(peer)
	, _session(&peer->session())
	, _peerId(static_cast<int64>(peer->id.value))
	, _timer([this] { pump(); }) {
	for (const auto &e : Ayu::ClipDb::LoadPeer(_peerId)) {
		if (e.kind == 0) {
			_haveCached.insert(e.mediaId);
		}
	}
}

void Indexer::begin() {
	// Show the box first: it holds a shared_ptr to us for its whole lifetime,
	// so we stay alive across all the async work.
	showProgressBox();
	// Start from the newest: messages.search returns messages with id <= offset_id,
	// so offset_id=0 yields NOTHING. ServerMaxMsgId (clamped to the API ceiling in
	// PrepareSearchRequest) returns the newest page.
	requestPage(ServerMaxMsgId);
}

void Indexer::cancel() {
	_cancelled = true;
	_timer.cancel();
	_downloader.destroy();
	_pending.clear();
}

void Indexer::requestPage(MsgId fromId) {
	if (_cancelled) {
		return;
	}
	auto request = Api::PrepareSearchRequest(
		_peer,
		MsgId(0),
		PeerId(0),
		kPhotoType,
		QString(),
		fromId,
		Data::LoadDirection::Before);
	if (!request) {
		scanDone();
		return;
	}
	const auto self = shared_from_this();
	_session->api().request(
		std::move(*request)
	).done([this, self, fromId](const Api::SearchRequestResult &result) {
		if (_cancelled) {
			return;
		}
		// ParseSearchResult processes users/chats and adds the messages to the
		// session (the important side effect); but its filtered messageIds came
		// back empty here, so we read the ids straight from the raw response.
		auto parsed = Api::ParseSearchResult(
			_peer,
			kPhotoType,
			fromId,
			Data::LoadDirection::Before,
			result);
		if (!_total) {
			_total = parsed.fullCount;
		}
		auto pageIds = std::vector<MsgId>();
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &d) {
			for (const auto &m : d.vmessages().v) {
				const auto id = m.match([](const MTPDmessageEmpty &e) {
					return e.vid().v;
				}, [](const MTPDmessage &e) {
					return e.vid().v;
				}, [](const MTPDmessageService &e) {
					return e.vid().v;
				});
				if (id) {
					pageIds.push_back(MsgId(id));
				}
			}
		});

		auto minId = MsgId(0);
		auto withPhoto = 0;
		auto added = 0;
		for (const auto &id : pageIds) {
			if (!minId || id < minId) {
				minId = id;
			}
			const auto item = _session->data().message(_peer->id, id);
			const auto media = item ? item->media() : nullptr;
			const auto photo = media ? media->photo() : nullptr;
			if (!photo) {
				continue;
			}
			++withPhoto;
			const auto pid = static_cast<int64>(photo->id);
			if (_haveCached.contains(pid)) {
				++_alreadyCached;
				continue;
			}
			_work.push_back({ id, pid });
			++added;
		}
		_scanned += int(pageIds.size());
		(void)withPhoto;
		(void)added;
		publish();
		// Stop when a page is empty or pagination stops moving to older ids
		// (_lastFrom is 0 only before the first page, so the first proceeds).
		if (pageIds.empty()
			|| !minId
			|| (_lastFrom && minId >= _lastFrom)) {
			scanDone();
		} else {
			_lastFrom = minId;
			requestPage(minId);
		}
	}).fail([this, self](const MTP::Error &) {
		if (!_cancelled) {
			scanDone();
		}
	}).send();
}

void Indexer::scanDone() {
	if (_cancelled || !_scanning) {
		return;
	}
	_scanning = false;
	startProcessing();
}

void Indexer::startProcessing() {
	LOG(("ClipIndex: scan done. total=%1 scanned=%2 work=%3 alreadyCached=%4")
		.arg(_total)
		.arg(_scanned)
		.arg(_work.size())
		.arg(_alreadyCached));
	publish();
	if (_work.empty() && _toEmbed.empty()) {
		finish();
		return;
	}
	const auto self = shared_from_this();
	_session->downloaderTaskFinished(
	) | rpl::on_next([this, self] {
		pump();
	}, _downloader);
	_timer.callEach(kPumpIntervalMs);
	pump();
}

void Indexer::pump() {
	if (_cancelled || _finished) {
		return;
	}
	const auto now = crl::now();

	// Harvest loaded / timed-out pending photos.
	for (auto i = _pending.begin(); i != _pending.end();) {
		auto image = ReadyImage(i->second.view);
		if (!image.isNull()) {
			_toEmbed.push_back({
				i->second.photoId,
				static_cast<int64>(i->first.bare),
				std::move(image) });
			i = _pending.erase(i);
		} else if (now >= i->second.deadline) {
			++_processed; // give up: not available / no longer exists
			i = _pending.erase(i);
		} else {
			++i;
		}
	}

	// Fill the in-flight window. Cap on (downloads in flight + images waiting to
	// be embedded) so a single pump() never drains all of _work onto the main
	// thread — the CLIP worker is the real rate limiter.
	while (int(_pending.size() + _toEmbed.size()) < kLoadWindow
		&& _next < _work.size()) {
		const auto w = _work[_next++];
		const auto item = _session->data().message(_peer->id, w.msgId);
		const auto media = item ? item->media() : nullptr;
		const auto photo = media ? media->photo() : nullptr;
		if (!photo) {
			++_processed;
			continue;
		}
		auto view = photo->createMediaView();
		auto image = ReadyImage(view);
		if (!image.isNull()) {
			_toEmbed.push_back({
				w.photoId,
				static_cast<int64>(w.msgId.bare),
				std::move(image) });
			continue;
		}
		const auto origin = Data::FileOrigin(
			Data::FileOriginMessage(_peer->id, w.msgId));
		photo->load(Data::PhotoSize::Thumbnail, origin);
		_pending.emplace(w.msgId, Pending{
			w.photoId,
			now + kLoadTimeoutMs,
			std::move(view) });
	}

	// Hand a batch off to the embedding worker.
	const auto drained = (_next >= _work.size()) && _pending.empty();
	if (!_embedRunning
		&& !_toEmbed.empty()
		&& (int(_toEmbed.size()) >= kEmbedBatch || drained)) {
		launchEmbed();
	}

	publish();

	if (drained && _toEmbed.empty() && !_embedRunning) {
		finish();
	}
}

void Indexer::launchEmbed() {
	_embedRunning = true;
	auto batch = std::move(_toEmbed);
	_toEmbed.clear();
	const auto self = shared_from_this();
	const auto peerId = _peerId;
	crl::async([this, self, peerId, batch = std::move(batch)]() mutable {
		auto store = std::vector<Ayu::ClipDb::Entry>();
		for (auto &in : batch) {
			if (in.image.isNull()) {
				continue;
			}
			auto emb = Ayu::Clip::EmbedImage(in.image);
			if (emb.empty()) {
				continue;
			}
			auto thumb = in.image.scaled(
				kThumbPx,
				kThumbPx,
				Qt::KeepAspectRatio,
				Qt::SmoothTransformation);
			auto thumbBytes = QByteArray();
			{
				auto buffer = QBuffer(&thumbBytes);
				buffer.open(QIODevice::WriteOnly);
				thumb.save(&buffer, "JPG", 80);
			}
			auto entry = Ayu::ClipDb::Entry();
			entry.kind = 0;
			entry.mediaId = in.photoId;
			entry.msgId = in.msgId;
			entry.emb = std::move(emb);
			entry.thumb = thumbBytes;
			store.push_back(std::move(entry));
		}
		Ayu::ClipDb::StoreMany(peerId, store);
		const auto count = int(batch.size());
		const auto indexed = int(store.size());
		crl::on_main([this, self, count, indexed] {
			if (_cancelled) {
				return;
			}
			_embedRunning = false;
			_processed += count;
			_indexed += indexed;
			pump();
		});
	});
}

void Indexer::finish() {
	if (_finished) {
		return;
	}
	_finished = true;
	_timer.cancel();
	_downloader.destroy();
	LOG(("ClipIndex: finished. processed=%1 indexed=%2 alreadyCached=%3")
		.arg(_processed)
		.arg(_indexed)
		.arg(_alreadyCached));
	publish();
}

void Indexer::publish() {
	_state = State{
		.scanning = _scanning,
		.finished = _finished,
		.total = _total,
		.scanned = _scanned,
		.work = int(_work.size()),
		.processed = _processed,
		.indexed = _indexed,
		.cached = _alreadyCached,
	};
}

void Indexer::showProgressBox() {
	const auto self = shared_from_this();
	_controller->show(Box([this, self](not_null<Ui::GenericBox*> box) {
		// Pin ourselves to the box: as long as it's open, we stay alive.
		box->lifetime().add([self] {});

		box->setTitle(rpl::single(FurryLang::Pick(
			u"Indexing chat photos"_q,
			QString::fromUtf8("\xd0\x98\xd0\xbd\xd0\xb4\xd0\xb5\xd0\xba\xd1\x81\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f\x20\xd1\x84\xd0\xbe\xd1\x82\xd0\xbe\x20\xd1\x87\xd0\xb0\xd1\x82\xd0\xb0"))));

		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(FurryLang::Pick(
				u"Scanning chat..."_q,
				QString::fromUtf8("\xd0\xa1\xd0\xba\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5\x20\xd1\x87\xd0\xb0\xd1\x82\xd0\xb0\xe2\x80\xa6"))),
			st::boxLabel));

		const auto bar = box->addRow(object_ptr<Ui::RpWidget>(box));
		bar->resize(0, 10);
		const auto fraction = bar->lifetime().make_state<float>(0.f);
		bar->paintRequest(
		) | rpl::on_next([=](QRect) {
			auto p = QPainter(bar);
			const auto w = bar->width();
			const auto h = bar->height();
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(0, 0, w, h, h / 2., h / 2.);
			const auto fill = int(w * std::clamp(*fraction, 0.f, 1.f));
			if (fill > 0) {
				p.setBrush(st::windowActiveTextFg);
				p.drawRoundedRect(0, 0, fill, h, h / 2., h / 2.);
			}
		}, bar->lifetime());

		_state.value(
		) | rpl::on_next([=](State s) {
			auto text = QString();
			if (s.finished) {
				text = FurryLang::Pick(
					u"Done. Indexed %1 new photos (%2 already cached)."_q,
					QString::fromUtf8("\xd0\x93\xd0\xbe\xd1\x82\xd0\xbe\xd0\xb2\xd0\xbe\x2e\x20\xd0\x94\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbe\x20\x25\x31\x20\xd0\xbd\xd0\xbe\xd0\xb2\xd1\x8b\xd1\x85\x20\xd1\x84\xd0\xbe\xd1\x82\xd0\xbe\x20\x28\x25\x32\x20\xd1\x83\xd0\xb6\xd0\xb5\x20\xd0\xb2\x20\xd0\xba\xd1\x8d\xd1\x88\xd0\xb5\x29\x2e"))
					.arg(s.indexed).arg(s.cached);
				*fraction = 1.f;
			} else if (s.scanning) {
				text = s.total
					? FurryLang::Pick(
						u"Scanning chat... found %1 / %2"_q,
						QString::fromUtf8("\xd0\xa1\xd0\xba\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5\x20\xd1\x87\xd0\xb0\xd1\x82\xd0\xb0\xe2\x80\xa6\x20\xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xbe\x20\x25\x31\x20\x2f\x20\x25\x32"))
						.arg(s.scanned).arg(s.total)
					: FurryLang::Pick(
						u"Scanning chat... found %1"_q,
						QString::fromUtf8("\xd0\xa1\xd0\xba\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5\x20\xd1\x87\xd0\xb0\xd1\x82\xd0\xb0\xe2\x80\xa6\x20\xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xbe\x20\x25\x31"))
						.arg(s.scanned);
				*fraction = s.total
					? float(s.scanned) / float(s.total)
					: 0.f;
			} else {
				const auto note = (s.work > kWarnThreshold)
					? FurryLang::Pick(
						u" (large chat - this may take a while)"_q,
						QString::fromUtf8("\x20\x28\xd0\xb1\xd0\xbe\xd0\xbb\xd1\x8c\xd1\x88\xd0\xbe\xd0\xb9\x20\xd1\x87\xd0\xb0\xd1\x82\x20\xe2\x80\x94\x20\xd0\xbc\xd0\xbe\xd0\xb6\xd0\xb5\xd1\x82\x20\xd0\xb7\xd0\xb0\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c\x20\xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd1\x8f\x29"))
					: QString();
				text = FurryLang::Pick(
					u"Indexing... %1 / %2 (added %3)%4"_q,
					QString::fromUtf8("\xd0\x98\xd0\xbd\xd0\xb4\xd0\xb5\xd0\xba\xd1\x81\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f\xe2\x80\xa6\x20\x25\x31\x20\x2f\x20\x25\x32\x20\x28\xd0\xb4\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbe\x20\x25\x33\x29\x25\x34"))
					.arg(s.processed).arg(s.work).arg(s.indexed).arg(note);
				*fraction = s.work
					? float(s.processed) / float(s.work)
					: 1.f;
			}
			label->setText(text);
			bar->update();
		}, box->lifetime());

		// "Stop indexing" (not "Cancel"): indexing is resumable — already-embedded
		// photos are saved, so stopping never throws away progress.
		box->addButton(
			_state.value() | rpl::map([](State s) {
				return s.finished
					? FurryLang::Pick(
						u"Close"_q,
						QString::fromUtf8("\xd0\x97\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd1\x8c"))
					: FurryLang::Pick(
						u"Stop indexing"_q,
						QString::fromUtf8("\xd0\x9e\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xb8\xd1\x82\xd1\x8c\x20\xd0\xb8\xd0\xbd\xd0\xb4\xd0\xb5\xd0\xba\xd1\x81\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8e"));
			}),
			[this, box] {
				cancel();
				box->closeBox();
			});

		box->boxClosing(
		) | rpl::on_next([this] {
			cancel();
		}, box->lifetime());
	}));
}

} // namespace

void Start(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	std::make_shared<Indexer>(controller, peer)->begin();
}

} // namespace Ayu::ClipIndex
