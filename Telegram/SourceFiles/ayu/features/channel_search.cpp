// FurryGram: extended channel search (separate button).
#include "ayu/features/channel_search.h"

#include "apiwrap.h" // ApiWrap::joinChannel
#include "ayu/features/furry_lang.h"
#include "base/flat_map.h"
#include "base/timer.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h" // PeerListRowWithLink
#include "config.h" // SearchPeopleLimit
#include "data/data_channel.h"
#include "data/data_session.h"
#include "data/data_types.h" // PeerFromMessage, IdFromMessage
#include "lang/lang_keys.h" // tr::lng_close
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "ui/layers/box_content.h"
#include "window/window_session_controller.h"

namespace Ayu::ChannelSearch {
namespace {

// Free, instant name matches fire quickly as the user types. The expensive,
// quota-metered global post search only fires once the query settles, so a
// burst of keystrokes spends at most one of the daily free searches.
constexpr auto kQuickDelay = crl::time(350);
constexpr auto kDeepDelay = crl::time(1100);
constexpr auto kMinDeepLength = 3;
constexpr auto kPerPage = 100;
constexpr auto kMaxPages = 3; // up to ~300 posts scanned per query.

struct Meta {
	int postMatches = 0;
	bool nameMatch = false;
};

// Shared between the search controller (writes) and the list controller
// (reads, when building rows). Avoids a back-pointer between the two.
struct SharedState {
	base::flat_map<PeerId, Meta> meta;
};

[[nodiscard]] QString PostsLabel(int count) {
	if (FurryLang::IsRussian()) {
		const auto n10 = count % 10;
		const auto n100 = count % 100;
		const auto word = (n10 == 1 && n100 != 11)
			// "пост"
			? QString::fromUtf8("\xD0\xBF\xD0\xBE\xD1\x81\xD1\x82")
			: (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14))
			// "поста"
			? QString::fromUtf8("\xD0\xBF\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0")
			// "постов"
			: QString::fromUtf8("\xD0\xBF\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD0\xB2");
		return QString::number(count) + ' ' + word;
	}
	return QString::number(count) + (count == 1 ? " post" : " posts");
}

[[nodiscard]] QString JoinLabel() {
	return FurryLang::Pick(
		u"Join"_q,
		// "Вступить"
		QString::fromUtf8("\xD0\x92\xD1\x81\xD1\x82\xD1\x83\xD0\xBF\xD0\xB8\xD1\x82\xD1\x8C"));
}

class SearchController final : public PeerListSearchController {
public:
	SearchController(
		not_null<Main::Session*> session,
		std::shared_ptr<SharedState> state);

	void searchQuery(const QString &query) override;
	bool isLoading() override;
	bool loadMoreRows() override {
		return false;
	}

private:
	enum class Source {
		Posts, // channels.searchPosts — global discovery (free daily quota).
		Global, // messages.searchGlobal + broadcasts_only — free fallback.
	};

	void requestContacts();
	void startDeep();
	void requestFlood();
	void startContent();
	void requestContentPage();
	void advanceContent(const MTPmessages_Messages &result);
	[[nodiscard]] bool handleContentPage(const MTPmessages_Messages &result);
	void contentFinish();
	[[nodiscard]] bool channelOk(PeerId peerId) const;
	void emitCurrent();

	const not_null<Main::Session*> _session;
	const std::shared_ptr<SharedState> _state;
	MTP::Sender _api;
	base::Timer _quickTimer;
	base::Timer _deepTimer;
	QString _query;

	// Cache the full meta map per query so repeats / refining back to an older
	// query cost nothing (and never spend a daily search).
	base::flat_map<QString, base::flat_map<PeerId, Meta>> _cache;

	mtpRequestId _contactsId = 0;
	mtpRequestId _floodId = 0;
	mtpRequestId _contentId = 0;
	bool _contactsDone = false;
	bool _contentDone = false;

	Source _source = Source::Global;
	bool _triedGlobalFallback = false;
	int _page = 0;
	int _offsetRate = 0;
	PeerData *_offsetPeer = nullptr;
	MsgId _offsetId = 0;

};

SearchController::SearchController(
	not_null<Main::Session*> session,
	std::shared_ptr<SharedState> state)
: _session(session)
, _state(std::move(state))
, _api(&session->mtp())
, _quickTimer([=] { requestContacts(); })
, _deepTimer([=] { startDeep(); }) {
}

void SearchController::searchQuery(const QString &query) {
	if (_query == query) {
		return;
	}
	_query = query;
	_api.request(base::take(_contactsId)).cancel();
	_api.request(base::take(_floodId)).cancel();
	_api.request(base::take(_contentId)).cancel();
	_quickTimer.cancel();
	_deepTimer.cancel();
	_contactsDone = _contentDone = false;
	_triedGlobalFallback = false;
	_state->meta.clear();
	if (_query.isEmpty()) {
		return;
	}
	const auto cached = _cache.find(_query);
	if (cached != end(_cache)) {
		_state->meta = cached->second;
		_contactsDone = _contentDone = true;
		emitCurrent();
		return;
	}
	_quickTimer.callOnce(kQuickDelay);
	if (_query.size() >= kMinDeepLength) {
		_deepTimer.callOnce(kDeepDelay);
	} else {
		_contentDone = true; // No global search for very short queries.
	}
}

bool SearchController::channelOk(PeerId peerId) const {
	const auto peer = _session->data().peerLoaded(peerId);
	const auto channel = peer ? peer->asChannel() : nullptr;
	return channel && channel->isBroadcast();
}

void SearchController::requestContacts() {
	_contactsId = _api.request(MTPcontacts_Search(
		MTP_flags(0),
		MTP_string(_query),
		MTP_int(SearchPeopleLimit)
	)).done([=](const MTPcontacts_Found &result, mtpRequestId requestId) {
		if (_contactsId != requestId) {
			return;
		}
		_contactsId = 0;
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		const auto feed = [&](const MTPVector<MTPPeer> &list) {
			for (const auto &mtpPeer : list.v) {
				const auto peerId = peerFromMTP(mtpPeer);
				if (channelOk(peerId)) {
					_state->meta[peerId].nameMatch = true;
				}
			}
		};
		feed(data.vmy_results());
		feed(data.vresults());
		_contactsDone = true;
		emitCurrent();
	}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
		if (_contactsId != requestId) {
			return;
		}
		_contactsId = 0;
		_contactsDone = true;
		emitCurrent();
	}).send();
}

void SearchController::startDeep() {
	if (_query.isEmpty()) {
		return;
	}
	requestFlood();
}

// Ask Telegram whether a global post search is free right now. We use
// channels.searchPosts whenever the query is free OR we still have free daily
// searches to spend (a fresh query consumes one, no Stars). Only when the daily
// quota is exhausted do we silently fall back to the free messages.searchGlobal
// — we never spend Stars.
void SearchController::requestFlood() {
	using Flag = MTPchannels_CheckSearchPostsFlood::Flag;
	_floodId = _api.request(MTPchannels_CheckSearchPostsFlood(
		MTP_flags(Flag::f_query),
		MTP_string(_query)
	)).done([=](const MTPSearchPostsFlood &result, mtpRequestId requestId) {
		if (_floodId != requestId) {
			return;
		}
		_floodId = 0;
		const auto &data = result.data();
		const auto canUsePosts = data.is_query_is_free()
			|| (data.vremains().v > 0);
		_source = canUsePosts ? Source::Posts : Source::Global;
		startContent();
	}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
		if (_floodId != requestId) {
			return;
		}
		_floodId = 0;
		_source = Source::Global;
		startContent();
	}).send();
}

void SearchController::startContent() {
	_page = 0;
	_offsetRate = 0;
	_offsetPeer = nullptr;
	_offsetId = 0;
	requestContentPage();
}

void SearchController::requestContentPage() {
	const auto offsetPeer = _offsetPeer
		? _offsetPeer->input()
		: MTP_inputPeerEmpty();
	if (_source == Source::Posts) {
		using Flag = MTPchannels_SearchPosts::Flag;
		_contentId = _api.request(MTPchannels_SearchPosts(
			MTP_flags(Flag::f_query),
			MTP_string(), // hashtag
			MTP_string(_query),
			MTP_int(_offsetRate),
			offsetPeer,
			MTP_int(_offsetId),
			MTP_int(kPerPage),
			MTP_long(0) // allow_paid_stars: never pay.
		)).done([=](
				const MTPmessages_Messages &result,
				mtpRequestId requestId) {
			if (_contentId != requestId) {
				return;
			}
			_contentId = 0;
			advanceContent(result);
		}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
			if (_contentId != requestId) {
				return;
			}
			_contentId = 0;
			// Posts unavailable (flood/payment) — fall back to global once.
			if (!_triedGlobalFallback) {
				_triedGlobalFallback = true;
				_source = Source::Global;
				startContent();
			} else {
				contentFinish();
			}
		}).send();
	} else {
		using Flag = MTPmessages_SearchGlobal::Flag;
		_contentId = _api.request(MTPmessages_SearchGlobal(
			MTP_flags(Flag::f_broadcasts_only),
			MTP_int(0), // folder_id
			MTP_string(_query),
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(_offsetRate),
			offsetPeer,
			MTP_int(_offsetId),
			MTP_int(kPerPage)
		)).done([=](
				const MTPmessages_Messages &result,
				mtpRequestId requestId) {
			if (_contentId != requestId) {
				return;
			}
			_contentId = 0;
			advanceContent(result);
		}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
			if (_contentId != requestId) {
				return;
			}
			_contentId = 0;
			contentFinish();
		}).send();
	}
}

void SearchController::advanceContent(const MTPmessages_Messages &result) {
	const auto hasMore = handleContentPage(result);
	++_page;
	if (hasMore && _page < kMaxPages) {
		requestContentPage();
	} else {
		contentFinish();
	}
}

bool SearchController::handleContentPage(const MTPmessages_Messages &result) {
	const auto owner = &_session->data();
	const auto process = [&](
			const MTPVector<MTPUser> &users,
			const MTPVector<MTPChat> &chats,
			const MTPVector<MTPMessage> &messages) {
		owner->processUsers(users);
		owner->processChats(chats);
		for (const auto &message : messages.v) {
			const auto peerId = PeerFromMessage(message);
			if (channelOk(peerId)) {
				++_state->meta[peerId].postMatches;
			}
			if (const auto peer = owner->peerLoaded(peerId)) {
				_offsetPeer = peer;
			}
			_offsetId = IdFromMessage(message);
		}
		return !messages.v.empty();
	};
	return result.match([&](const MTPDmessages_messages &data) {
		process(data.vusers(), data.vchats(), data.vmessages());
		return false; // Full (non-sliced) result — no more pages.
	}, [&](const MTPDmessages_messagesSlice &data) {
		const auto got = process(
			data.vusers(),
			data.vchats(),
			data.vmessages());
		if (const auto nextRate = data.vnext_rate()) {
			_offsetRate = nextRate->v;
		}
		return got;
	}, [&](const MTPDmessages_channelMessages &data) {
		return process(data.vusers(), data.vchats(), data.vmessages());
	}, [](const MTPDmessages_messagesNotModified &) {
		return false;
	});
}

void SearchController::contentFinish() {
	_contentDone = true;
	emitCurrent();
	if (!_query.isEmpty()) {
		_cache[_query] = _state->meta;
	}
}

void SearchController::emitCurrent() {
	// Discovery only: drop channels the user already follows entirely.
	auto channels = std::vector<not_null<ChannelData*>>();
	channels.reserve(_state->meta.size());
	for (const auto &[peerId, meta] : _state->meta) {
		if (const auto peer = _session->data().peerLoaded(peerId)) {
			if (const auto channel = peer->asChannel()) {
				if (!channel->amIn()) {
					channels.push_back(channel);
				}
			}
		}
	}
	// Rank: name matches first, then by the number of matching posts.
	ranges::sort(channels, [&](
			not_null<ChannelData*> a,
			not_null<ChannelData*> b) {
		const auto &ma = _state->meta[a->id];
		const auto &mb = _state->meta[b->id];
		if (ma.nameMatch != mb.nameMatch) {
			return ma.nameMatch;
		} else if (ma.postMatches != mb.postMatches) {
			return ma.postMatches > mb.postMatches;
		}
		return a->name().compare(b->name(), Qt::CaseInsensitive) < 0;
	});
	// Re-adding an already-shown channel is a no-op (PeerListContent dedups by
	// id), so the two phases — quick name matches, then deep post matches —
	// accumulate cleanly without duplicates.
	for (const auto &channel : channels) {
		delegate()->peerListSearchAddRow(channel);
	}
	delegate()->peerListSearchRefreshRows();
}

bool SearchController::isLoading() {
	return _quickTimer.isActive()
		|| _deepTimer.isActive()
		|| _contactsId
		|| _floodId
		|| _contentId;
}

class Controller final : public PeerListController {
public:
	Controller(
		not_null<Window::SessionController*> window,
		std::shared_ptr<SharedState> state,
		std::unique_ptr<PeerListSearchController> search);

	Main::Session &session() const override;
	void prepare() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	void rowRightActionClicked(not_null<PeerListRow*> row) override;
	std::unique_ptr<PeerListRow> createSearchRow(
		not_null<PeerData*> peer) override;

private:
	const not_null<Window::SessionController*> _window;
	const std::shared_ptr<SharedState> _state;

};

Controller::Controller(
	not_null<Window::SessionController*> window,
	std::shared_ptr<SharedState> state,
	std::unique_ptr<PeerListSearchController> search)
: PeerListController(std::move(search))
, _window(window)
, _state(std::move(state)) {
}

Main::Session &Controller::session() const {
	return _window->session();
}

void Controller::prepare() {
	delegate()->peerListSetSearchMode(PeerListSearchMode::Enabled);
	delegate()->peerListSetTitle(rpl::single(FurryLang::Pick(
		u"Find channels"_q,
		// "Найти каналы"
		QString::fromUtf8("\xD0\x9D\xD0\xB0\xD0\xB9\xD1\x82\xD0\xB8\x20\xD0\xBA\xD0\xB0\xD0\xBD\xD0\xB0\xD0\xBB\xD1\x8B"))));
	setDescriptionText(FurryLang::Pick(
		u"Type to search channels by name and post content"_q,
		// "Введите запрос — поиск каналов по названию и содержимому постов"
		QString::fromUtf8("\xD0\x92\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xB8\xD1\x82\xD0\xB5\x20\xD0\xB7\xD0\xB0\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\x20\xE2\x80\x94\x20\xD0\xBF\xD0\xBE\xD0\xB8\xD1\x81\xD0\xBA\x20\xD0\xBA\xD0\xB0\xD0\xBD\xD0\xB0\xD0\xBB\xD0\xBE\xD0\xB2\x20\xD0\xBF\xD0\xBE\x20\xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x8E\x20\xD0\xB8\x20\xD1\x81\xD0\xBE\xD0\xB4\xD0\xB5\xD1\x80\xD0\xB6\xD0\xB8\xD0\xBC\xD0\xBE\xD0\xBC\xD1\x83\x20\xD0\xBF\xD0\xBE\xD1\x81\xD1\x82\xD0\xBE\xD0\xB2")));
	setSearchNoResultsText(FurryLang::Pick(
		u"No channels found"_q,
		// "Каналы не найдены"
		QString::fromUtf8("\xD0\x9A\xD0\xB0\xD0\xBD\xD0\xB0\xD0\xBB\xD1\x8B\x20\xD0\xBD\xD0\xB5\x20\xD0\xBD\xD0\xB0\xD0\xB9\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8B")));
}

std::unique_ptr<PeerListRow> Controller::createSearchRow(
		not_null<PeerData*> peer) {
	const auto channel = peer->asChannel();
	if (!channel) {
		return nullptr;
	}
	auto row = std::make_unique<PeerListRowWithLink>(peer);
	const auto i = _state->meta.find(peer->id);
	if (i != end(_state->meta) && i->second.postMatches > 0) {
		row->setCustomStatus(PostsLabel(i->second.postMatches));
	} else if (!channel->username().isEmpty()) {
		row->setCustomStatus('@' + channel->username());
	}
	if (!channel->amIn()) {
		row->setActionLink(JoinLabel());
	}
	return row;
}

void Controller::rowClicked(not_null<PeerListRow*> row) {
	// Navigate on the main column. showPeerHistory hides the layer itself once
	// this click dispatch unwinds — do NOT hideLayer() here (it would destroy
	// the box, and this controller, synchronously mid-dispatch and crash).
	_window->showPeerHistory(
		row->peer(),
		Window::SectionShow::Way::ClearStack);
}

// The "Join" link next to each result — subscribe without leaving the search.
void Controller::rowRightActionClicked(not_null<PeerListRow*> row) {
	const auto channel = row->peer()->asChannel();
	if (!channel || channel->amIn()) {
		return;
	}
	_window->session().api().joinChannel(channel);
	// Optimistically drop the Join link so it reads as done.
	static_cast<PeerListRowWithLink*>(row.get())->setActionLink(QString());
	delegate()->peerListUpdateRow(row);
}

} // namespace

object_ptr<Ui::BoxContent> Box(not_null<Window::SessionController*> window) {
	auto state = std::make_shared<SharedState>();
	auto search = std::make_unique<SearchController>(&window->session(), state);
	auto controller = std::make_unique<Controller>(
		window,
		state,
		std::move(search));
	return Box<PeerListBox>(std::move(controller), [=](
			not_null<PeerListBox*> box) {
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	});
}

} // namespace Ayu::ChannelSearch
