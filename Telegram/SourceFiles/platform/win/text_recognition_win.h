/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// FurryGram: Windows text recognition is implemented out-of-line in
// text_recognition_win.cpp using the vendored Tesseract engine. The
// IsAvailable()/RecognizeText() declarations live in
// platform/platform_text_recognition.h (included before this header).
