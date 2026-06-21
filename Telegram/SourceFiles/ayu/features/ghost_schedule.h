// FurryGram: ghost-mode schedule. Auto-enables/disables ghost mode within a
// daily time window (e.g. 22:00–08:00), checked once a minute.
#pragma once

namespace FurryGhostSchedule {

// Start the schedule timer. Safe to call once at startup; does nothing until the
// user enables the schedule in settings.
void Start();

} // namespace FurryGhostSchedule
