// FurryGram: Focus mode engine + schedule.
#include "ayu/features/focus_mode.h"

#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "data/data_chat_filters.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"

#include <QtCore/QTime>

namespace Ayu::Focus {
namespace {

base::Timer *GlobalTimer = nullptr;
bool LastInside = false;
bool HasLast = false;

// Is `now` inside [from, to)? Handles windows that wrap past midnight.
[[nodiscard]] bool insideWindow(int now, int from, int to) {
	if (from == to) {
		return false;
	} else if (from < to) {
		return (now >= from) && (now < to);
	}
	return (now >= from) || (now < to);
}

void tick() {
	auto &settings = AyuSettings::getInstance();
	const auto profile = settings.activeFocusProfile();
	if (!profile.scheduleEnabled) {
		HasLast = false; // reset so re-enabling re-applies immediately
		return;
	}
	const auto now = QTime::currentTime();
	const auto nowMinutes = now.hour() * 60 + now.minute();
	const auto inside = insideWindow(
		nowMinutes,
		profile.scheduleFrom,
		profile.scheduleTo);

	// Only act on a transition (or the first tick after enabling), so manual
	// toggling within a stable window is respected.
	if (HasLast && inside == LastInside) {
		return;
	}
	LastInside = inside;
	HasLast = true;
	settings.setFocusEnabled(inside);
}

[[nodiscard]] bool profileAllows(not_null<History*> history) {
	auto &settings = AyuSettings::getInstance();
	const auto profile = settings.activeFocusProfile();
	const auto peer = history->peer;
	if (profile.allowPinned && history->isPinnedDialog(FilterId(0))) {
		return true;
	}
	if (profile.allowPrivateOnly && peer->isUser()) {
		return true;
	}
	if (profile.allowExceptions
		&& settings.hasFocusException(qint64(peer->id.value))) {
		return true;
	}
	if (profile.allowFolderId) {
		const auto &filters = peer->owner().chatsFilters();
		for (const auto &filter : filters.list()) {
			if (filter.id() == FilterId(profile.allowFolderId)) {
				return filter.contains(history);
			}
		}
	}
	return false;
}

} // namespace

void Start() {
	if (GlobalTimer) {
		return;
	}
	GlobalTimer = new base::Timer();
	GlobalTimer->setCallback([] { tick(); });
	GlobalTimer->callEach(60 * crl::time(1000)); // every minute
	tick(); // apply immediately on startup
}

bool Active() {
	return AyuSettings::getInstance().focusEnabled();
}

bool HideToast() {
	return AyuSettings::getInstance().activeFocusProfile().hideToast;
}

bool Suppresses(HistoryItem *item) {
	if (!item || !Active()) {
		return false;
	}
	return !profileAllows(item->history());
}

} // namespace Ayu::Focus
