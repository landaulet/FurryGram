// FurryGram: Focus mode — suppress notifications from non-allowed chats while
// "busy", driven by named profiles (see FocusProfile in ayu_settings.h).
#pragma once

class HistoryItem;

namespace Ayu::Focus {

// Starts the per-minute schedule timer that auto-toggles Focus inside the
// active profile's daily window.
void Start();

// Whether Focus is currently on (the master toggle; the schedule drives it).
[[nodiscard]] bool Active();

// Whether the active profile hides the toast entirely (vs a silent toast).
[[nodiscard]] bool HideToast();

// Whether the notification for `item` should be suppressed right now.
[[nodiscard]] bool Suppresses(HistoryItem *item);

} // namespace Ayu::Focus
