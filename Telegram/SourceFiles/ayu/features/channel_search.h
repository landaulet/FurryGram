// FurryGram: extended channel search (separate button).
//
// The stock Telegram channel search (contacts.search) only matches channel
// name/username and returns few results. This finds channels by the *content*
// of their public posts too — natively and for free — by combining
// contacts.search (name matches) with messages.searchGlobal + broadcasts_only
// (post-content matches), then merging and ranking the source channels.
#pragma once

#include "base/object_ptr.h"

namespace Ui {
class BoxContent;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu::ChannelSearch {

// Builds the "Find channels" box.
[[nodiscard]] object_ptr<Ui::BoxContent> Box(
	not_null<Window::SessionController*> window);

} // namespace Ayu::ChannelSearch
