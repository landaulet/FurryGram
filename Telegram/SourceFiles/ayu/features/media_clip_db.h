// FurryGram: persistent cache of CLIP image embeddings (per-chat search).
//
// Stored in its own SQLite file (tdata/furry_clip.db) so it never contends with
// AyuGram's main database. Embeddings are int8-quantized (unit vectors -> 1 byte
// per dim) plus a tiny JPEG thumbnail, so a 10k-image chat is only a few MB and
// a repeat search needs no CLIP re-encoding. All access is mutex-guarded and is
// meant to be called from a worker thread (crl::async).
#pragma once

#include <QtCore/QByteArray>

#include <vector>

namespace Ayu::ClipDb {

// kind: 0 = photo, 1 = image-document.
struct Entry {
	int kind = 0;
	int64 mediaId = 0;
	int64 msgId = 0;        // a message carrying this photo (to jump on click)
	std::vector<float> emb; // normalized
	QByteArray thumb;       // small JPEG for the results list
};

// Every cached embedding for a peer (empty if none / on error).
[[nodiscard]] std::vector<Entry> LoadPeer(int64 peerId);

// Persist freshly computed entries (quantized to int8 internally), one txn.
void StoreMany(int64 peerId, const std::vector<Entry> &entries);

// Forget all cached embeddings for a peer.
void ClearPeer(int64 peerId);

} // namespace Ayu::ClipDb
