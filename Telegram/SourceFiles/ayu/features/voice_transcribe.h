// FurryGram: local voice transcription via whisper.cpp (CPU, offline).
#pragma once

#include <rpl/producer.h>

class HistoryItem;

namespace Ayu::Voice {

struct TranscribeResult {
	QString text;
	bool failed = false;
	bool toolong = false;
};

// Whether local whisper transcription is enabled in FurryGram settings.
[[nodiscard]] bool LocalTranscribeEnabled();

// Selected model size id (e.g. "base", "small", "medium").
[[nodiscard]] QString ModelSize();

// Absolute path to the currently selected ggml model file (may not exist yet).
[[nodiscard]] QString ModelPath();

// Absolute path of the directory where models are stored.
[[nodiscard]] QString ModelsDir();

// Whether the selected model file is present on disk.
[[nodiscard]] bool ModelAvailable();

// Decode the voice/video message audio and run whisper on a worker thread.
// `done` is always invoked on the main thread.
void Transcribe(
	not_null<HistoryItem*> item,
	Fn<void(TranscribeResult)> done);

// Free the cached whisper model from RAM (call when disabling the feature or
// switching the model). Safe to call any time; the next run reloads on demand.
void FreeContext();

// --- Model download management (Step 2) ---

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

// Approximate on-disk size in MB for a model size id (for UI labels).
[[nodiscard]] int ModelSizeMb(const QString &size);

// Reactive status of the currently selected model.
[[nodiscard]] rpl::producer<ModelProgress> ModelStatusValue();
[[nodiscard]] ModelStatus CurrentModelStatus();

// Recompute status for the currently selected size (call after a size change).
void RefreshModelStatus();

// Start / cancel download of the currently selected model; or delete it.
void StartModelDownload();
void CancelModelDownload();
void DeleteModel();

} // namespace Ayu::Voice
