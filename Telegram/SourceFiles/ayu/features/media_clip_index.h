// FurryGram: full-chat photo indexer for CLIP image search.
//
// Enumerates ALL photos in a chat via messages.search (not just the resident
// ones), then loads each thumbnail (from local cache, or downloading via
// tdesktop's throttled loader) and CLIP-embeds it into the persistent cache so
// search covers the whole history. Runs in the background with a progress box;
// it's resumable (already-cached embeddings are skipped) and cancellable.
#pragma once

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu::ClipIndex {

// Pre-counts the chat's photos, warns if it's a big job, then runs the indexer
// with a progress box.
void Start(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer);

} // namespace Ayu::ClipIndex
