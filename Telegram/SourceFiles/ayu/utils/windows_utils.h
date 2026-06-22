// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Modified as part of FurryGram, 2026.
#pragma once

class QWidget;

void reloadAppIconFromTaskBar();

// FurryGram: anti-screenshare for an extra top-level window (e.g. notification
// popups). Sets WDA_EXCLUDEFROMCAPTURE so it renders black in captures.
// Windows-only impl; call sites must guard with #ifdef Q_OS_WIN.
void setWindowExcludeFromCapture(QWidget *widget, bool exclude);
