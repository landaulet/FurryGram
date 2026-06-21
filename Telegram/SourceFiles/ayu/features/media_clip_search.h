// FurryGram: per-chat semantic image search UI (offline, local CLIP).
//
// Opens a box where the user types an English query; the chat's cached photos
// are embedded with CLIP and ranked by cosine similarity, fully on-device.
#pragma once

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu::ClipSearch {

// Entry point from the chat "..." menu. If the model is missing, offers to
// download it; otherwise shows the query box.
void ShowSearchBox(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

} // namespace Ayu::ClipSearch
