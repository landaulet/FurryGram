// FurryGram: local CLIP image/text embeddings for semantic media search.
#include "ayu/features/media_clip.h"

#include "settings.h" // cWorkingDir

#include <clip.h>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QImage>

#include <mutex>
#include <thread>

namespace Ayu::Clip {
namespace {

[[nodiscard]] std::mutex &Mutex() {
	static std::mutex mutex;
	return mutex;
}

// Cached model (guarded by Mutex()).
clip_ctx *GContext = nullptr;
bool GTriedLoad = false;
int GDim = 0;

[[nodiscard]] int Threads() {
	const auto cores = std::thread::hardware_concurrency();
	return std::max(1, int(cores ? (cores / 2) : 4));
}

// Loads (and caches) the model. Must be called under Mutex().
clip_ctx *AcquireLocked() {
	if (GContext || GTriedLoad) {
		return GContext;
	}
	GTriedLoad = true;
	if (!QFile::exists(ModelPath())) {
		return nullptr;
	}
	const auto utf8 = ModelPath().toUtf8();
	GContext = clip_model_load(utf8.constData(), 0);
	if (GContext) {
		GDim = clip_get_vision_hparams(GContext)->projection_dim;
	}
	return GContext;
}

} // namespace

namespace {

// Selected model id, lazily loaded from disk (default "b32").
QString GModelId;
bool GModelIdLoaded = false;

[[nodiscard]] QString ModelFileName(const QString &id) {
	return (id == u"l14"_q)
		? u"clip-vitl14-f16.gguf"_q
		: u"clip-vitb32-f16.gguf"_q;
}

} // namespace

QString ModelsDir() {
	return cWorkingDir() + u"tdata/furry_clip/"_q;
}

QString CurrentModel() {
	if (!GModelIdLoaded) {
		GModelIdLoaded = true;
		auto file = QFile(ModelsDir() + u"model.txt"_q);
		if (file.open(QIODevice::ReadOnly)) {
			GModelId = QString::fromUtf8(file.readAll()).trimmed();
		}
		if (GModelId != u"l14"_q && GModelId != u"b32"_q) {
			GModelId = u"b32"_q;
		}
	}
	return GModelId;
}

void SetModel(const QString &id) {
	const auto value = (id == u"l14"_q) ? u"l14"_q : u"b32"_q;
	if (CurrentModel() == value) {
		return;
	}
	GModelId = value;
	GModelIdLoaded = true;
	QDir().mkpath(ModelsDir());
	auto file = QFile(ModelsDir() + u"model.txt"_q);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(value.toUtf8());
		file.close();
	}
	FreeContext();        // drop the old model from RAM
	RefreshModelStatus(); // settings download button reflects the new model
}

QString ModelPath() {
	return ModelsDir() + ModelFileName(CurrentModel());
}

bool ModelAvailable() {
	return QFile::exists(ModelPath());
}

int Dim() {
	auto lock = std::lock_guard(Mutex());
	AcquireLocked();
	return GDim;
}

void FreeContext() {
	auto lock = std::lock_guard(Mutex());
	if (GContext) {
		clip_free(GContext);
		GContext = nullptr;
	}
	GTriedLoad = false;
	GDim = 0;
}

std::vector<float> EmbedImage(const QImage &image) {
	auto lock = std::lock_guard(Mutex());
	const auto ctx = AcquireLocked();
	if (!ctx) {
		return {};
	}
	auto rgb = image.convertToFormat(QImage::Format_RGB888);
	if (rgb.isNull()) {
		return {};
	}
	const auto nx = rgb.width();
	const auto ny = rgb.height();
	auto u8 = clip_image_u8{};
	u8.nx = nx;
	u8.ny = ny;
	u8.size = size_t(nx) * ny * 3;
	u8.data = new uint8_t[u8.size];
	// QImage rows are padded to a 4-byte boundary, so copy row by row.
	for (auto y = 0; y != ny; ++y) {
		memcpy(
			u8.data + size_t(y) * nx * 3,
			rgb.constScanLine(y),
			size_t(nx) * 3);
	}

	auto f32 = clip_image_f32{};
	auto result = std::vector<float>();
	if (clip_image_preprocess(ctx, &u8, &f32)) {
		result.resize(GDim);
		if (!clip_image_encode(ctx, Threads(), &f32, result.data(), true)) {
			result.clear();
		}
	}
	clip_image_u8_clean(&u8);
	clip_image_f32_clean(&f32);
	return result;
}

std::vector<float> EmbedText(const QString &text) {
	auto lock = std::lock_guard(Mutex());
	const auto ctx = AcquireLocked();
	if (!ctx) {
		return {};
	}
	const auto utf8 = text.toUtf8();
	auto tokens = clip_tokens{};
	auto result = std::vector<float>();
	if (clip_tokenize(ctx, utf8.constData(), &tokens)) {
		// The text tower has a fixed context length; never index past it.
		const auto maxPos = clip_get_text_hparams(ctx)->num_positions;
		if (int(tokens.size) > maxPos) {
			tokens.size = maxPos;
		}
		result.resize(GDim);
		if (!clip_text_encode(ctx, Threads(), &tokens, result.data(), true)) {
			result.clear();
		}
	}
	delete[] tokens.data;
	return result;
}

float Cosine(const std::vector<float> &a, const std::vector<float> &b) {
	if (a.empty() || a.size() != b.size()) {
		return 0.f;
	}
	auto dot = 0.f;
	for (auto i = size_t(); i != a.size(); ++i) {
		dot += a[i] * b[i];
	}
	return dot;
}

} // namespace Ayu::Clip
