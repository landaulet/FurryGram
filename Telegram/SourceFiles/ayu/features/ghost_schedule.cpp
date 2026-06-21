// FurryGram: ghost-mode schedule.
#include "ayu/features/ghost_schedule.h"

#include "ayu/ayu_settings.h"
#include "base/timer.h"

#include <QtCore/QTime>

namespace FurryGhostSchedule {
namespace {

base::Timer *GlobalTimer = nullptr;
bool LastInsideWindow = false;
bool HasLastState = false;

// Is `now` inside [from, to)? Handles windows that wrap past midnight
// (e.g. from=22:00 to=08:00).
[[nodiscard]] bool insideWindow(int nowMinutes, int from, int to) {
	if (from == to) {
		return false; // empty window
	} else if (from < to) {
		return (nowMinutes >= from) && (nowMinutes < to);
	}
	// Wraps midnight.
	return (nowMinutes >= from) || (nowMinutes < to);
}

void tick() {
	auto &settings = AyuSettings::getInstance();
	if (!settings.ghostScheduleEnabled()) {
		HasLastState = false; // reset so re-enabling re-applies immediately
		return;
	}
	const auto now = QTime::currentTime();
	const auto nowMinutes = now.hour() * 60 + now.minute();
	const auto inside = insideWindow(
		nowMinutes,
		settings.ghostScheduleFrom(),
		settings.ghostScheduleTo());

	// Only act on a transition (or the first tick after enabling), so we don't
	// fight the user toggling ghost manually within a stable window.
	if (HasLastState && inside == LastInsideWindow) {
		return;
	}
	LastInsideWindow = inside;
	HasLastState = true;
	AyuSettings::ghost().setGhostModeEnabled(inside);
	AyuSettings::save();
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

} // namespace FurryGhostSchedule
