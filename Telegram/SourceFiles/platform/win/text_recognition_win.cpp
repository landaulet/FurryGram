// FurryGram: Windows text recognition backed by the vendored Tesseract engine.
// Implements Platform::TextRecognition for Windows (the upstream stub returned
// nothing), which lights up the media viewer's built-in Live-Text-style UI.
#include "platform/platform_text_recognition.h"

#include "settings.h" // cWorkingDir

#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>
#include <tesseract/publictypes.h>

#include <QtCore/QFile>
#include <QtGui/QImage>

namespace Platform::TextRecognition {
namespace {

[[nodiscard]] QString TessdataDir() {
	return cWorkingDir() + u"tdata/furry_ocr/tessdata/"_q;
}

[[nodiscard]] bool HasLanguage(const QString &code) {
	return QFile::exists(TessdataDir() + code + u".traineddata"_q);
}

// Use every traineddata that is present (Tesseract joins them with '+').
[[nodiscard]] QByteArray Languages() {
	auto langs = QStringList();
	for (const auto &code : { u"eng"_q, u"rus"_q }) {
		if (HasLanguage(code)) {
			langs.push_back(code);
		}
	}
	return (langs.isEmpty() ? u"eng"_q : langs.join('+')).toUtf8();
}

} // namespace

bool IsAvailable() {
	return HasLanguage(u"eng"_q) || HasLanguage(u"rus"_q);
}

Result RecognizeText(const QImage &image) {
	auto result = Result();
	if (image.isNull() || !IsAvailable()) {
		return result;
	}
	// Tesseract works best on 8-bit grayscale; feed the raw bytes directly.
	// (Upscaling was tried but made sparse-text mode hallucinate words from
	// interpolation artifacts, so we feed the image at its native size.)
	auto gray = image.convertToFormat(QImage::Format_Grayscale8);
	if (gray.isNull()) {
		return result;
	}

	auto api = tesseract::TessBaseAPI();
	const auto dataPath = TessdataDir().toUtf8();
	const auto langs = Languages();
	if (api.Init(dataPath.constData(), langs.constData()) != 0) {
		return result;
	}
	// Photos/memes have text scattered over imagery rather than a document
	// layout, so sparse-text mode finds far more than PSM_AUTO.
	api.SetPageSegMode(tesseract::PSM_SPARSE_TEXT);
	api.SetImage(
		gray.constBits(),
		gray.width(),
		gray.height(),
		1,
		int(gray.bytesPerLine()));
	if (api.Recognize(nullptr) != 0) {
		api.End();
		return result;
	}

	// One selectable item per recognized text line, in image pixel coordinates
	// (top-left origin) — matching the macOS implementation's convention.
	const auto level = tesseract::RIL_TEXTLINE;
	// Drop low-confidence lines — those are usually background/noise picked up
	// by sparse-text mode (the garbage "/ : * %" fragments).
	constexpr auto kMinConfidence = 50.f;
	if (const auto it = api.GetIterator()) {
		do {
			if (it->Empty(level) || it->Confidence(level) < kMinConfidence) {
				continue;
			}
			const auto utf8 = it->GetUTF8Text(level);
			if (!utf8) {
				continue;
			}
			auto text = QString::fromUtf8(utf8).trimmed();
			delete[] utf8;
			auto left = 0, top = 0, right = 0, bottom = 0;
			if (!text.isEmpty()
				&& it->BoundingBox(level, &left, &top, &right, &bottom)) {
				result.items.push_back(RectWithText{
					.text = text,
					.rect = QRect(QPoint(left, top), QPoint(right, bottom)),
				});
			}
		} while (it->Next(level));
		delete it;
	}
	api.End();

	result.success = !result.items.empty();
	return result;
}

} // namespace Platform::TextRecognition
