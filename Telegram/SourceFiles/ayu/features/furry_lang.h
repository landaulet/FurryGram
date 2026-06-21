// FurryGram: tiny built-in localization helper for FurryGram-only strings.
//
// FurryGram's own features (Whisper transcription, OCR) are not part of the
// AyuGram remote language packs, so their UI strings live in code. This helper
// picks a Russian or English variant based on the active client language, so a
// Russian user sees Russian text instead of hardcoded English.
#pragma once

#include <QtCore/QString>

namespace FurryLang {

// True when the active UI language is Russian.
[[nodiscard]] bool IsRussian();

// Returns `ru` when the UI language is Russian, otherwise `en`.
[[nodiscard]] QString Pick(const QString &en, const QString &ru);

} // namespace FurryLang
