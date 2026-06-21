// FurryGram: chat picker for Focus "always notify" exceptions.
#pragma once

#include "base/basic_types.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu::Focus {

// Opens a multi-select chat list (checkboxes), pre-checked with the current
// Focus exceptions; Save writes the selection back to AyuSettings.
void ShowExceptionsBox(not_null<Window::SessionController*> controller);

} // namespace Ayu::Focus
