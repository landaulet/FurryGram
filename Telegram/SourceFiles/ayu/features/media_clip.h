// FurryGram: local CLIP image/text embeddings for semantic media search.
//
// Wraps the ported clip.cpp (CPU, offline) on the bundled ggml. Encodes images
// and text into the same 512-d CLIP space so a text query can be matched
// against a chat's images by cosine similarity — all on-device.
#pragma once

#include <QtCore/QString>

#include <rpl/producer.h>

#include <vector>

class QImage;

namespace Ayu::Clip {

// Directory and path of the currently selected CLIP gguf model.
[[nodiscard]] QString ModelsDir();
[[nodiscard]] QString ModelPath();
[[nodiscard]] bool ModelAvailable();

// Selectable model quality (persisted): "b32" = ViT-B/32 (fast, ~290 MB),
// "l14" = ViT-L/14 (more accurate, ~860 MB, slower on CPU). Switching frees the
// loaded context; embeddings of a different model are simply ignored at search
// time (dimension mismatch), so re-index after changing this.
[[nodiscard]] QString CurrentModel();
void SetModel(const QString &id);

// Projection dimension of the loaded model (512 for ViT-B/32), or 0 if the
// model could not be loaded.
[[nodiscard]] int Dim();

// Frees the cached model from RAM (e.g. when the feature is disabled).
void FreeContext();

// Heavy — must be called on a worker thread (e.g. crl::async). Returns a
// normalized embedding, or an empty vector on failure.
[[nodiscard]] std::vector<float> EmbedImage(const QImage &image);
[[nodiscard]] std::vector<float> EmbedText(const QString &text);

// Cosine similarity of two (already normalized) embeddings; 0 if sizes differ.
[[nodiscard]] float Cosine(
	const std::vector<float> &a,
	const std::vector<float> &b);

// --- Model download management (media_clip_download.cpp) ---

enum class ModelStatus {
	Missing,
	Downloading,
	Ready,
};

struct ModelProgress {
	ModelStatus status = ModelStatus::Missing;
	int percent = 0;

	friend bool operator==(
		const ModelProgress &,
		const ModelProgress &) = default;
};

// Reactive status of the CLIP model (for the settings UI / search box).
[[nodiscard]] rpl::producer<ModelProgress> ModelStatusValue();
[[nodiscard]] ModelStatus CurrentModelStatus();

// Recompute status from disk (Missing/Ready) when nothing is downloading.
void RefreshModelStatus();

// Start / cancel the model download, or delete the on-disk model.
void StartModelDownload();
void CancelModelDownload();
void DeleteModel();

} // namespace Ayu::Clip
