// FurryGram: per-chat semantic image search UI (offline, local CLIP).
#include "ayu/features/media_clip_search.h"

#include "ayu/features/media_clip.h"
#include "ayu/features/media_clip_db.h"
#include "ayu/features/media_clip_index.h"
#include "ayu/features/furry_lang.h"

#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "mainwidget.h"
#include "ui/abstract_button.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"
#include "base/weak_ptr.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QBuffer>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Ayu::ClipSearch {
namespace {

// Cap how many images we embed per search (CPU cost) and how many we show.
constexpr auto kMaxImages = 400;
constexpr auto kTopResults = 30;
constexpr auto kThumbPx = 56;

// One indexable media occurrence. We keep the ENCODED bytes (or a file path)
// and decode off the main thread — grabbing the bytes is a cheap COW copy.
struct Meta {
	MsgId id = 0;
	int kind = 0;       // 0 = photo, 1 = image-document
	int64 mediaId = 0;  // photo/document id (the cache key)
	QByteArray bytes;   // encoded image bytes (preferred)
	QString path;       // fallback: on-disk file path
};

struct Result {
	MsgId id = 0;
	QByteArray thumbJpeg; // kept compact; decoded only for the shown top-K
	QImage thumb;
	float score = 0.f; // raw cosine
	float prob = 0.f;  // softmax confidence across candidates (for display)
};

using Key = std::pair<int, int64>;

// Forward decls (the boxes reference each other).
void ShowQueryBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

// MAIN thread: encoded bytes of the largest resident size of a photo (no decode).
[[nodiscard]] QByteArray CachedPhotoBytes(not_null<PhotoData*> photo) {
	const auto view = photo->createMediaView();
	for (const auto size : {
			Data::PhotoSize::Large,
			Data::PhotoSize::Thumbnail,
			Data::PhotoSize::Small }) {
		auto bytes = view->imageBytes(size);
		if (!bytes.isEmpty()) {
			return bytes;
		}
	}
	return {};
}

// MAIN thread: walk the chat's loaded history and collect every photo /
// image-document whose pixels are available locally (no network). Only cheap
// metadata + encoded bytes are taken here; decoding happens on the worker.
[[nodiscard]] std::vector<Meta> CollectCached(not_null<PeerData*> peer) {
	auto out = std::vector<Meta>();
	const auto history = peer->owner().history(peer);
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			const auto media = item->media();
			if (!media) {
				continue;
			}
			if (const auto photo = media->photo()) {
				auto bytes = CachedPhotoBytes(photo);
				if (!bytes.isEmpty()) {
					out.push_back({
						item->id,
						0,
						static_cast<int64>(photo->id),
						std::move(bytes),
						QString() });
				}
			} else if (const auto document = media->document()) {
				if (document->isImage()
					&& !document->sticker()
					&& !document->isAnimation()) {
					const auto dview = document->createMediaView();
					auto bytes = dview->loaded()
						? dview->bytes()
						: QByteArray();
					auto path = bytes.isEmpty()
						? document->filepath(true)
						: QString();
					if (!bytes.isEmpty() || !path.isEmpty()) {
						out.push_back({
							item->id,
							1,
							static_cast<int64>(document->id),
							std::move(bytes),
							path });
					}
				}
			}
			if (int(out.size()) >= kMaxImages) {
				return out;
			}
		}
	}
	return out;
}

void ShowResults(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const QString &query,
		std::vector<Result> results) {
	if (results.empty()) {
		controller->show(Ui::MakeInformBox(FurryLang::Pick(
			u"No matching cached images. Open and scroll the chat to cache "
			"more photos, then try again."_q,
			QString::fromUtf8("\xd0\x9f\xd0\xbe\xd0\xb4\xd1\x85\xd0\xbe\xd0\xb4\xd1\x8f\xd1\x89\xd0\xb8\xd1\x85\x20\xd0\xba\xd0\xb0\xd1\x80\xd1\x82\xd0\xb8\xd0\xbd\xd0\xbe\xd0\xba\x20\xd0\xbd\xd0\xb5\x20\xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xbe\x2e\x20\xd0\x9e\xd1\x82\xd0\xba\xd1\x80\xd0\xbe\xd0\xb9\xd1\x82\xd0\xb5\x20\xd0\xb8\x20\xd0\xbf\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb8\xd1\x81\xd1\x82\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb5\x20\xd1\x87\xd0\xb0\xd1\x82\x2c\x20\xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b\x20\xd0\xba\xd1\x8d\xd1\x88\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c\x20\xd0\xb1\xd0\xbe\xd0\xbb\xd1\x8c\xd1\x88\xd0\xb5\x20\xd1\x84\xd0\xbe\xd1\x82\xd0\xbe\x2c\x20\xd0\xb7\xd0\xb0\xd1\x82\xd0\xb5\xd0\xbc\x20\xd0\xbf\xd0\xbe\xd0\xb2\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5\x2e"))));
		return;
	}
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(FurryLang::Pick(
			u"Results for \"%1\""_q,
			QString::fromUtf8("\xd0\xa0\xd0\xb5\xd0\xb7\xd1\x83\xd0\xbb\xd1\x8c\xd1\x82\xd0\xb0\xd1\x82\xd1\x8b\x20\xd0\xbf\xd0\xbe\x20\xc2\xab\x25\x31\xc2\xbb")).arg(query)));
		for (const auto &r : results) {
			const auto id = r.id;
			const auto thumb = r.thumb;
			const auto prob = r.prob;
			// Fix the row height before adding it, so the layout reserves the
			// right vertical space (a fresh AbstractButton starts 0-high).
			auto owned = object_ptr<Ui::AbstractButton>(box);
			const auto row = owned.data();
			row->resize(0, kThumbPx + 12);
			box->addRow(std::move(owned));
			row->paintRequest(
			) | rpl::on_next([=](QRect) {
				auto p = QPainter(row);
				const auto h = row->height();
				const auto top = (h - kThumbPx) / 2;
				if (!thumb.isNull()) {
					const auto x = 4 + (kThumbPx - thumb.width()) / 2;
					const auto y = top + (kThumbPx - thumb.height()) / 2;
					p.drawImage(QPoint(x, y), thumb);
				}
				p.setPen(st::windowFg);
				const auto textLeft = 4 + kThumbPx + 12;
				p.drawText(
					QRect(textLeft, 0, row->width() - textLeft - 12, h),
					Qt::AlignVCenter | Qt::AlignLeft,
					FurryLang::Pick(
						u"match %1%"_q,
						QString::fromUtf8("\xd1\x81\xd0\xbe\xd0\xb2\xd0\xbf\xd0\xb0\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5\x20\x25\x31\x25")).arg(qRound(prob * 100)));
			}, row->lifetime());
			row->setClickedCallback([=] {
				box->closeBox();
				controller->showPeerHistory(
					peer,
					Window::SectionShow::Way::Forward,
					id);
			});
		}
		box->addButton(rpl::single(FurryLang::Pick(
			u"Close"_q,
			QString::fromUtf8("\xd0\x97\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd1\x8c"))), [=] {
			box->closeBox();
		});
	}));
}

// Prompt ensemble: CLIP zero-shot is more robust when the query is embedded
// through several caption templates and the (normalized) vectors are averaged.
[[nodiscard]] std::vector<float> EmbedQuery(const QString &query) {
	static const auto kTemplates = {
		u"a photo of %1"_q,
		u"a picture of %1"_q,
		u"an image of %1"_q,
		u"%1"_q,
	};
	auto acc = std::vector<float>();
	for (const auto &t : kTemplates) {
		const auto e = Ayu::Clip::EmbedText(t.arg(query));
		if (e.empty()) {
			continue;
		}
		if (acc.empty()) {
			acc.assign(e.size(), 0.f);
		}
		if (acc.size() != e.size()) {
			continue;
		}
		for (auto i = size_t(); i != e.size(); ++i) {
			acc[i] += e[i];
		}
	}
	auto norm = 0.0;
	for (const auto v : acc) {
		norm += double(v) * v;
	}
	if (norm > 0.0) {
		const auto inv = float(1.0 / std::sqrt(norm));
		for (auto &v : acc) {
			v *= inv;
		}
	}
	return acc;
}

void RunSearch(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		const QString &query) {
	auto metas = CollectCached(peer);
	const auto peerId = static_cast<int64>(peer->id.value);
	Ui::Toast::Show(
		controller->content().get(),
		FurryLang::Pick(
			u"Searching images..."_q,
			QString::fromUtf8("\xd0\x9f\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba\x20\xd0\xbf\xd0\xbe\x20\xd0\xba\xd0\xb0\xd1\x80\xd1\x82\xd0\xb8\xd0\xbd\xd0\xba\xd0\xb0\xd0\xbc\xe2\x80\xa6")));

	const auto weak = base::make_weak(controller);
	crl::async([=, metas = std::move(metas)]() mutable {
		auto results = std::vector<Result>();
		// Ensemble of caption-style prompts (CLIP was trained on captions).
		const auto q = EmbedQuery(query.trimmed());
		if (!q.empty()) {
			// Candidates = EVERY cached/indexed embedding (this is what makes a
			// previously-indexed chat searchable even when scrolled away), plus
			// any resident photo that isn't cached yet (embedded on the fly).
			auto cached = Ayu::ClipDb::LoadPeer(peerId);
			auto cachedByKey = std::map<Key, const Ayu::ClipDb::Entry*>();
			for (const auto &e : cached) {
				if (e.emb.size() == q.size()) {
					cachedByKey[Key(e.kind, e.mediaId)] = &e;
				}
			}

			auto seen = std::set<Key>();
			auto toStore = std::vector<Ayu::ClipDb::Entry>();

			// 1) Resident photos: prefer the cache, else embed + persist.
			for (auto &m : metas) {
				const auto key = Key(m.kind, m.mediaId);
				if (!seen.insert(key).second) {
					continue;
				}
				const auto it = cachedByKey.find(key);
				if (it != cachedByKey.end()) {
					auto r = Result();
					r.id = m.id;
					r.score = Ayu::Clip::Cosine(q, it->second->emb);
					r.thumbJpeg = it->second->thumb;
					results.push_back(std::move(r));
					continue;
				}
				auto image = m.bytes.isEmpty()
					? QImage(m.path)
					: QImage::fromData(m.bytes);
				if (image.isNull()) {
					continue;
				}
				auto emb = Ayu::Clip::EmbedImage(image);
				if (emb.empty()) {
					continue;
				}
				auto thumb = image.scaled(
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
				entry.kind = m.kind;
				entry.mediaId = m.mediaId;
				entry.msgId = static_cast<int64>(m.id.bare);
				entry.emb = emb;
				entry.thumb = thumbBytes;
				toStore.push_back(std::move(entry));
				auto r = Result();
				r.id = m.id;
				r.score = Ayu::Clip::Cosine(q, emb);
				r.thumbJpeg = thumbBytes;
				results.push_back(std::move(r));
			}

			// 2) Everything else in the cache (indexed but not resident now).
			for (const auto &e : cached) {
				if (e.emb.size() != q.size()) {
					continue;
				}
				const auto key = Key(e.kind, e.mediaId);
				if (!seen.insert(key).second) {
					continue;
				}
				auto r = Result();
				r.id = MsgId(e.msgId);
				r.score = Ayu::Clip::Cosine(q, e.emb);
				r.thumbJpeg = e.thumb;
				results.push_back(std::move(r));
			}

			Ayu::ClipDb::StoreMany(peerId, toStore);
		}
		// Zero-shot CLIP readout: logit = cosine * logit_scale (~100), softmax
		// across ALL candidates → a confidence that actually separates matches
		// from non-matches (raw cosine sits in a flat ~0.15-0.35 band, which is
		// why "cat" looked the same on a cat and a shoe).
		if (!results.empty()) {
			constexpr auto kLogitScale = 100.f;
			auto maxLogit = -std::numeric_limits<float>::infinity();
			for (const auto &r : results) {
				maxLogit = std::max(maxLogit, r.score * kLogitScale);
			}
			auto sum = 0.0;
			for (auto &r : results) {
				r.prob = float(std::exp(
					double(r.score * kLogitScale - maxLogit)));
				sum += r.prob;
			}
			if (sum > 0.0) {
				for (auto &r : results) {
					r.prob = float(r.prob / sum);
				}
			}
		}
		std::sort(results.begin(), results.end(), [](
				const Result &a,
				const Result &b) {
			return a.score > b.score;
		});
		if (int(results.size()) > kTopResults) {
			results.resize(kTopResults);
		}
		// Decode thumbnails only for the handful we actually show.
		for (auto &r : results) {
			r.thumb = QImage::fromData(r.thumbJpeg);
		}
		crl::on_main([=, results = std::move(results)]() mutable {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			if (results.empty()) {
				strong->show(Ui::MakeInformBox(FurryLang::Pick(
					u"No images to search yet. Use \"Index chat\" to scan this "
					"chat's photos, or scroll it to load some, then try again."_q,
					QString::fromUtf8("\xd0\x9f\xd0\xbe\xd0\xba\xd0\xb0\x20\xd0\xbd\xd0\xb5\xd1\x87\xd0\xb5\xd0\xb3\xd0\xbe\x20\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0\xd1\x82\xd1\x8c\x2e\x20\xd0\x9d\xd0\xb0\xd0\xb6\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb5\x20\xc2\xab\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb8\xd0\xbd\xd0\xb4\xd0\xb5\xd0\xba\xd1\x81\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c\x20\xd1\x87\xd0\xb0\xd1\x82\xc2\xbb\x2c\x20\xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b\x20\xd0\xbf\xd1\x80\xd0\xbe\xd1\x81\xd0\xba\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c\x20\xd1\x84\xd0\xbe\xd1\x82\xd0\xbe\x20\xd1\x87\xd0\xb0\xd1\x82\xd0\xb0\x2c\x20\xd0\xbb\xd0\xb8\xd0\xb1\xd0\xbe\x20\xd0\xbf\xd1\x80\xd0\xbe\xd0\xbb\xd0\xb8\xd1\x81\xd1\x82\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb5\x20\xd1\x87\xd0\xb0\xd1\x82\x2c\x20\xd0\xb7\xd0\xb0\xd1\x82\xd0\xb5\xd0\xbc\x20\xd0\xbf\xd0\xbe\xd0\xb2\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5\x2e"))));
				return;
			}
			ShowResults(strong, peer, query, std::move(results));
		});
	});
}

void ShowQueryBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(FurryLang::Pick(
			u"Search images by text"_q,
			QString::fromUtf8("\xd0\x9f\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba\x20\xd0\xba\xd0\xb0\xd1\x80\xd1\x82\xd0\xb8\xd0\xbd\xd0\xbe\xd0\xba\x20\xd0\xbf\xd0\xbe\x20\xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82\xd1\x83"))));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(FurryLang::Pick(
				u"Type what to find (in English), fully offline. Searches photos "
				"cached in this chat; use \"Index chat\" to cover the whole "
				"history."_q,
				QString::fromUtf8("\xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5\x2c\x20\xd1\x87\xd1\x82\xd0\xbe\x20\xd0\xbd\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb8\x20\x28\xd0\xbf\xd0\xbe\x2d\xd0\xb0\xd0\xbd\xd0\xb3\xd0\xbb\xd0\xb8\xd0\xb9\xd1\x81\xd0\xba\xd0\xb8\x29\x2c\x20\xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c\xd1\x8e\x20\xd0\xbe\xd1\x84\xd0\xbb\xd0\xb0\xd0\xb9\xd0\xbd\x2e\x20\xd0\x9f\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba\x20\xd0\xbf\xd0\xbe\x20\xd1\x84\xd0\xbe\xd1\x82\xd0\xbe\x2c\x20\xd0\xba\xd1\x8d\xd1\x88\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xbd\xd1\x8b\xd0\xbc\x20\xd0\xb2\x20\xd1\x8d\xd1\x82\xd0\xbe\xd0\xbc\x20\xd1\x87\xd0\xb0\xd1\x82\xd0\xb5\x3b\x20\xc2\xab\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb8\xd0\xbd\xd0\xb4\xd0\xb5\xd0\xba\xd1\x81\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c\x20\xd1\x87\xd0\xb0\xd1\x82\xc2\xbb\x20\xd0\xbe\xd1\x85\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8b\xd0\xb2\xd0\xb0\xd0\xb5\xd1\x82\x20\xd0\xb2\xd1\x81\xd1\x8e\x20\xd0\xb8\xd1\x81\xd1\x82\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8e\x2e"))),
			st::boxLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box->verticalLayout(),
			st::windowFilterNameInput,
			rpl::single(FurryLang::Pick(
				u"e.g. a dog on the beach"_q,
				QString::fromUtf8("\xd0\xbd\xd0\xb0\xd0\xbf\xd1\x80\x2e\x20\xd1\x81\xd0\xbe\xd0\xb1\xd0\xb0\xd0\xba\xd0\xb0\x20\xd0\xbd\xd0\xb0\x20\xd0\xbf\xd0\xbb\xd1\x8f\xd0\xb6\xd0\xb5")))));
		box->setFocusCallback([=] { field->setFocusFast(); });
		const auto submit = [=] {
			const auto text = field->getLastText().trimmed();
			if (text.isEmpty()) {
				field->showError();
				return;
			}
			box->closeBox();
			RunSearch(controller, peer, text);
		};
		box->addButton(rpl::single(FurryLang::Pick(
			u"Search"_q,
			QString::fromUtf8("\xd0\x9d\xd0\xb0\xd0\xb9\xd1\x82\xd0\xb8"))), submit);
		box->addButton(rpl::single(FurryLang::Pick(
			u"Index chat"_q,
			QString::fromUtf8("\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb8\xd0\xbd\xd0\xb4\xd0\xb5\xd0\xba\xd1\x81\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c\x20\xd1\x87\xd0\xb0\xd1\x82"))), [=] {
			box->closeBox();
			Ayu::ClipIndex::Start(controller, peer);
		});
		box->addButton(rpl::single(FurryLang::Pick(
			u"Cancel"_q,
			QString::fromUtf8("\xd0\x9e\xd1\x82\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb0"))), [=] {
			box->closeBox();
		});
	}));
}

void ShowDownloadBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(FurryLang::Pick(
			u"Image search (CLIP)"_q,
			QString::fromUtf8("\xd0\x9f\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba\x20\xd0\xba\xd0\xb0\xd1\x80\xd1\x82\xd0\xb8\xd0\xbd\xd0\xbe\xd0\xba\x20\x28\x43\x4c\x49\x50\x29"))));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(FurryLang::Pick(
				u"Downloads a one-time ~290 MB CLIP model. After that, you can "
				"search this chat's photos by meaning, fully offline."_q,
				QString::fromUtf8("\xd0\x9e\xd0\xb4\xd0\xb8\xd0\xbd\x20\xd1\x80\xd0\xb0\xd0\xb7\x20\xd1\x81\xd0\xba\xd0\xb0\xd1\x87\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f\x20\xd0\xbc\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c\x20\x43\x4c\x49\x50\x20\x28\x7e\x32\x39\x30\x20\xd0\x9c\xd0\x91\x29\x2e\x20\xd0\x9f\xd0\xbe\xd1\x81\xd0\xbb\xd0\xb5\x20\xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe\x20\xd0\xbc\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe\x20\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0\xd1\x82\xd1\x8c\x20\xd1\x84\xd0\xbe\xd1\x82\xd0\xbe\x20\xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe\x20\xd1\x87\xd0\xb0\xd1\x82\xd0\xb0\x20\xd0\xbf\xd0\xbe\x20\xd1\x81\xd0\xbc\xd1\x8b\xd1\x81\xd0\xbb\xd1\x83\x2c\x20\xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c\xd1\x8e\x20\xd0\xbe\xd1\x84\xd0\xbb\xd0\xb0\xd0\xb9\xd0\xbd\x2e"))),
			st::boxLabel));
		Ayu::Clip::RefreshModelStatus();
		box->addButton(Ayu::Clip::ModelStatusValue(
		) | rpl::map([](Ayu::Clip::ModelProgress p) {
			using S = Ayu::Clip::ModelStatus;
			switch (p.status) {
			case S::Ready:
				return FurryLang::Pick(
					u"Start search"_q,
					QString::fromUtf8("\xd0\x9d\xd0\xb0\xd1\x87\xd0\xb0\xd1\x82\xd1\x8c\x20\xd0\xbf\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba"));
			case S::Downloading:
				return FurryLang::Pick(
					u"Downloading... %1%"_q,
					QString::fromUtf8("\xd0\x97\xd0\xb0\xd0\xb3\xd1\x80\xd1\x83\xd0\xb7\xd0\xba\xd0\xb0\x20\xd0\xbc\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb8\xe2\x80\xa6\x20\x25\x31\x25")).arg(p.percent);
			default:
				return FurryLang::Pick(
					u"Download model (290 MB)"_q,
					QString::fromUtf8("\xd0\xa1\xd0\xba\xd0\xb0\xd1\x87\xd0\xb0\xd1\x82\xd1\x8c\x20\xd0\xbc\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c\x20\x28\x32\x39\x30\x20\xd0\x9c\xd0\x91\x29"));
			}
		}), [=] {
			using S = Ayu::Clip::ModelStatus;
			switch (Ayu::Clip::CurrentModelStatus()) {
			case S::Ready:
				box->closeBox();
				ShowQueryBox(controller, peer);
				break;
			case S::Downloading:
				break;
			default:
				Ayu::Clip::StartModelDownload();
				break;
			}
		});
		box->addButton(rpl::single(FurryLang::Pick(
			u"Cancel"_q,
			QString::fromUtf8("\xd0\x9e\xd1\x82\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb0"))), [=] {
			box->closeBox();
		});
	}));
}

} // namespace

void ShowSearchBox(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer) {
	Ayu::Clip::RefreshModelStatus();
	if (Ayu::Clip::ModelAvailable()) {
		ShowQueryBox(controller, peer);
	} else {
		ShowDownloadBox(controller, peer);
	}
}

} // namespace Ayu::ClipSearch
