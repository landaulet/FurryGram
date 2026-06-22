# FurryGram

Кастомизированный клиент [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
с упором на приватность и офлайн-инструменты, работающие прямо на устройстве.

[ [English](README.md) | Русский ]

## Возможности

Поверх набора функций AyuGram (полный режим призрака, история сообщений / анти-recall,
локальный Telegram Premium, режим стримера, кастомизация шрифта, переводчик и др.)
FurryGram добавляет:

- **Офлайн-OCR** — распознавание текста на фото локально (Tesseract, ничего не уходит наружу),
  с пунктом «Распознать текст» в просмотрщике и загрузкой данных по языкам
- **Офлайн-транскрипция голосовых** — голосовые → текст на устройстве (whisper.cpp, CPU)
- **Поиск картинок по смыслу (CLIP)** — поиск изображений в чате по текстовому описанию, офлайн
- **Поиск каналов** — находит каналы по содержимому постов, а не только по названию
- **Focus mode** — именованные профили уведомлений (Работа / Сон / Игра) со списками
  разрешений и расписанием
- **Переработанная панель ввода** (включается в настройках) и обновлённые тосты уведомлений
- **Мелкие удобства** — счётчик символов, «Скопировать как Markdown»

## Загрузка

Готовые сборки под Windows выкладываются в Telegram-канал:
**[@FurryGramReleases](https://t.me/FurryGramReleases)**

(Пакетных менеджеров пока нет — берите сборку из канала и запускайте.)

## Сборка (Windows)

FurryGram собирается на тулчейне upstream Telegram Desktop (CMake + MSVC 2022, Qt 5.15).
Подготовьте сторонние библиотеки как для tdesktop, затем из корня репозитория:

```bat
:: конфигурация (Ninja Multi-Config — быстрые инкрементальные сборки)
configure-ninja.bat
:: сборка
build-ninja.bat
```

Также поддерживается генератор Visual Studio через `configure-furry.bat` + `build-furry-step1.bat`.

Убедитесь, что в VS Build Tools установлено: C++ MFC (x86 и x64), C++ ATL (x86 и x64),
последний Windows 11 SDK.

## Благодарности

FurryGram — производная работа, стоящая на плечах:

- [Telegram Desktop](https://github.com/telegramdesktop/tdesktop) — upstream-клиент
- [AyuGram](https://github.com/AyuGram/AyuGramDesktop) от [@Radolyn](https://github.com/Radolyn) — форк, на котором основан FurryGram

### Библиотеки

- [JSON for Modern C++](https://github.com/nlohmann/json), [SQLite](https://github.com/sqlite/sqlite), [sqlite_orm](https://github.com/fnc12/sqlite_orm)
- [Tesseract](https://github.com/tesseract-ocr/tesseract) + [Leptonica](https://github.com/DanBloomberg/leptonica) — OCR
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) — транскрипция голоса
- [QuickJS](https://github.com/quickjs-ng/quickjs)

## Лицензия

Лицензируется под GNU General Public License v3, как Telegram Desktop и AyuGram.
См. [LICENSE](LICENSE).
