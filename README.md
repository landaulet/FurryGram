# FurryGram

A customized [Telegram Desktop](https://github.com/telegramdesktop/tdesktop) client, focused on
privacy features and offline, on-device tools.

[ English | [Русский](README-RU.md) ]

## Features

On top of the AyuGram feature set (full ghost mode, message history / anti-recall, local
Telegram Premium, streamer mode, font customization, translator and more), FurryGram adds:

- **Offline OCR** — recognize text in photos locally (Tesseract, nothing leaves your device),
  with a viewer "Recognize text" action and per-language data download
- **Offline voice transcription** — voice messages → text on-device (whisper.cpp, CPU)
- **CLIP image search** — search a chat's images by a text description, fully offline
- **Channel discovery** — find channels by what they post, not just by name
- **Focus mode** — named notification profiles (Work / Sleep / Game) with allow-lists & a schedule
- **Compose bar redesign** (toggleable) and restyled notification toasts
- **Small QoL** — live character counter, "Copy as Markdown"

## Downloads

Prebuilt Windows builds are posted to the Telegram channel:
**[@FurryGramReleases](https://t.me/FurryGramReleases)**

(No package-manager distributions yet — grab the build from the channel and run it.)

## Building (Windows)

FurryGram builds on the upstream Telegram Desktop toolchain (CMake + MSVC 2022, Qt 5.15).
Prepare the third-party libraries as for tdesktop, then from the repo root:

```bat
:: configure once (Ninja Multi-Config — fast, bounded parallel builds)
configure-ninja.bat
:: build
build-ninja.bat
```

The Visual Studio generator is also supported via `configure-furry.bat` + `build-furry-step1.bat`.

Make sure VS Build Tools has: C++ MFC (x86 & x64), C++ ATL (x86 & x64), latest Windows 11 SDK.

## Credits

FurryGram is a derivative work and stands on the shoulders of:

- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop) — the upstream client
- [AyuGram](https://github.com/AyuGram/AyuGramDesktop) by [@Radolyn](https://github.com/Radolyn) — the fork FurryGram is based on

### Libraries

- [JSON for Modern C++](https://github.com/nlohmann/json), [SQLite](https://github.com/sqlite/sqlite), [sqlite_orm](https://github.com/fnc12/sqlite_orm)
- [Tesseract](https://github.com/tesseract-ocr/tesseract) + [Leptonica](https://github.com/DanBloomberg/leptonica) — OCR
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) — voice transcription
- [QuickJS](https://github.com/quickjs-ng/quickjs)

## License

Licensed under the GNU General Public License v3, same as Telegram Desktop and AyuGram.
See [LICENSE](LICENSE).
