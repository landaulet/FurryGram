// FurryGram: local OCR via Tesseract (CPU, offline).
#pragma once

#include <rpl/producer.h>

#include <memory>

class QImage;

namespace Ui {
class Show;
} // namespace Ui

namespace Ayu::Ocr {

// Recognize text in `image` using a '+'-joined language list (e.g. "eng+rus").
// Heavy — must be called on a worker thread. Returns trimmed UTF-8 text, or an
// empty string on failure. `dataPath` is the directory that contains the
// `tessdata/` folder with the <lang>.traineddata files.
[[nodiscard]] QString Recognize(
	const QImage &image,
	const QString &languages,
	const QString &dataPath);

// Show recognized text in a box with a Copy button (main thread). `show` can be
// a window controller's uiShow() or the media viewer's uiShow().
void ShowResultBox(
	std::shared_ptr<Ui::Show> show,
	const QString &text);

// --- traineddata (per-language model) download management ---

enum class LangStatus {
	Missing,
	Downloading,
	Ready,
};

struct LangProgress {
	LangStatus status = LangStatus::Missing;
	int percent = 0;

	friend bool operator==(const LangProgress &, const LangProgress &) = default;
};

// Directory that holds the <lang>.traineddata files (Tesseract datapath).
[[nodiscard]] QString TessdataDir();

// Reactive status of a single language model (e.g. "eng", "rus").
[[nodiscard]] rpl::producer<LangProgress> LangStatusValue(const QString &lang);
[[nodiscard]] LangStatus CurrentLangStatus(const QString &lang);
void RefreshLangStatus(const QString &lang);

// Download / cancel / delete the tessdata_best model for a language.
void DownloadLang(const QString &lang);
void CancelLangDownload(const QString &lang);
void DeleteLang(const QString &lang);

} // namespace Ayu::Ocr
