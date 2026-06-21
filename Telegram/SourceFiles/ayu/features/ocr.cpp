// FurryGram: local OCR via Tesseract (CPU, offline).
#include "ayu/features/ocr.h"

#include <tesseract/baseapi.h>

#include <QtGui/QImage>

namespace Ayu::Ocr {

QString Recognize(
		const QImage &image,
		const QString &languages,
		const QString &dataPath) {
	if (image.isNull()) {
		return QString();
	}
	// Tesseract works best on 8-bit grayscale; feed the raw bytes directly
	// (no Leptonica PIX needed on our side).
	auto gray = image.convertToFormat(QImage::Format_Grayscale8);
	if (gray.isNull()) {
		return QString();
	}

	auto api = tesseract::TessBaseAPI();
	const auto langs = languages.isEmpty()
		? QByteArray("eng")
		: languages.toUtf8();
	const auto path = dataPath.toUtf8();
	if (api.Init(path.constData(), langs.constData()) != 0) {
		return QString();
	}
	api.SetPageSegMode(tesseract::PSM_SPARSE_TEXT);
	api.SetImage(
		gray.constBits(),
		gray.width(),
		gray.height(),
		1,
		int(gray.bytesPerLine()));
	const auto utf8 = api.GetUTF8Text();
	auto result = utf8 ? QString::fromUtf8(utf8) : QString();
	delete[] utf8;
	api.End();
	return result.trimmed();
}

} // namespace Ayu::Ocr
