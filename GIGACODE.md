# CrossPoint Reader — Open-Source E-Reader Firmware

## Обзор проекта

**CrossPoint** — это прошивка для электронных книгочитателей на базе чипов ESP32, разрабатываемая сообществом. Полностью открытая, бесплатная и настраиваемая замена заводской прошивке.

- **Версия:** 2.0.0
- **Репозиторий:** https://github.com/crosspoint-reader/crosspoint-reader
- **Лицензия:** Open Source
- **Основной язык:** C++20 (Arduino framework)
- **Платформа:** ESP32 (ESP32-C3 и ESP32-S3)

## Поддерживаемые устройства

### ESP32-C3 (Xteink)
- Xteink X4
- Xteink X3

### ESP32-S3 (Xteink + другие)
- Xteink X4 Pro (16MB flash / 8MB PSRAM, SSD1677 + GT911 touch, frontlight)
- Xteink X4 Classic / X4C (кнопочная версия X4 Pro)
- Seeed reTerminal Sticky (ESP32-S3R8, 3.97 inch 800x480)
- M5Stack Paper Mono (SSD1683, PMIC, M5IOE1)
- Metalio E-Ink 4 (16MB flash / 8MB octal PSRAM)
- EEGO Reader A4 (UC8279C, GSLX680 touch)
- Onyx BOOX Picco / Tiles (SSD1677/SSD2677, 4-bit SDMMC)

## Архитектура проекта

### Директории

| Директория | Описание |
|---|---|
| src/ | Основной код прошивки (~319 файлов) |
| lib/ | Встроенные библиотеки (31 subdir): EPUB, GfxRenderer, I18n, KOReaderSync, SdFat, JPEG/PNG/GIF decoding, StarDict dictionary и др. |
| freeink-sdk/ | Отдельный SDK-субмодуль с UI-фреймворком и hardware abstraction layer (HAL) |
| docs/ | Документация (contributing, webserver, dictionary, troubleshooting и др.) |
| scripts/ | PIO extra scripts: patching, HTML generation, i18n, git branch info |
| bin/ | Бинарные утилиты (clang-format-fix) |
| test/ | Юнит-тесты |
| nix/ | Nix/NixOS build environment |
| platformio.ini | Конфигурация PlatformIO (основной файл сборки) |

### Ключевые компоненты src/

src/
- main.cpp — Точка входа, setup()/loop(), sleep/wake, BLE page turner
- activities/ — UI-активности: Home, Reader, Settings, Browse Files, File Transfer
- components/ — UI компоненты: темы (Classic, Lyra, Lyra Extended, RoundedRaff)
- font/ — Встроенные шрифты (Noto Sans 12/14/16/18pt, Ubuntu 8/10/12)
- images/ — Логотипы, иконки (SVG, PNG)
- network/ — WiFi, WebDAV, HTTP, OPDS, OTA updates, web server
- platform/ — ESP32 platform-specific code (USB Serial JTAG handoff)
- util/ — Утилиты: dictionary, bookmarks, caching, QR codes, strings

### SDK (freeink-sdk/)

Субмодуль с независимой разработкой:
- libs/ — HAL, UI framework (FreeInkUI), networking, hardware drivers
- tools/ — тулинг для разработки
- Библиотеки подключаются через symlink:// в platformio.ini

## Основные возможности

### Ридер
- EPUB 2/3 — рендеринг с embedded-стилями, переносы, кернинг, адаптивные таблицы
- Форматы: .epub, .xtc/.xtch, .txt, .bmp
- Шрифты: SD-card (TTF/OTF/TTC), .cpfont, Noto Sans встроенные
- Навигация: по главам, закладкам, проценту, автоперелистывание
- Темная тема, несколько UI-тем, ориентация экрана
- Focus Reading (Bionic Reading)
- Screenshot (Power+Side Down или через меню)

### Словарь
- StarDict словари с SD-карты
- Поиск по выделенному тексту (тач или кнопки)
- Поддержка CJK, RTL, кириллицы

### Закладки и вырезки
- До 256 вырезок на ESP32-C3, 1024 на ESP32-S3
- Экспорт в My Clippings.txt (Kindle-compatible формат)
- Sync Clippings через KOReader Sync сервер

### Сеть и WiFi
- Web Server UI — передача файлов, настройки, OPDS
- WebSocket быстрые загрузки
- WebDAV — монтирование как сетевой диск
- AP mode (hotspot) + QR-код для подключения
- STA mode с индикатором signal strength
- Calibre plugin для беспроводных передачь
- OPDS браузер (до 8 серверов), поиск, пагинация
- OTA updates из GitHub releases

### Синхронизация (KOReader Sync)
- CrossPoint Sync server (sync.crosspointreader.com)
- KOReader Sync server (sync.koreader.rocks)
- Self-hosted Docker (koreader/kosync)
- Сервер/клиент, smart sync, metadata (ISBN, ASIN, series)

### Bluetooth Page Turner
- NimBLE HID remote (чтение страниц)
- Pairing, button mapping (page, chapter, confirm, back, menu, clipping)
- Поддержка до 4 спаренных устройств

### Плагины
- SD-card плагины через Plugin Hub
- JavaScript runs on phone/computer browser
- Plugin events (sleep.enter, reader.exit, session.start и др.)

### Кэширование
- Кэш на SD-карте (.crosspoint/):
  - epub_<hash>/ — один кэш на книгу (cover, progress, book metadata, CSS, images, sections)
  - settings.json, state.json, recent.json
- Удаление .crosspoint/ сбрасывает все кэши

## Сборка и запуск

### Требования
- pioarduino PlatformIO Core или VS Code + pioarduino IDE
- Python 3.8+
- clang-format 21
- USB-C кабель с поддержкой данных

### Клонирование
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader
git submodule update --init --recursive

### Nix/NixOS
nix develop -f nix
# или
nix-shell nix

### Сборка / прошивка / мониторинг
pio run --target upload    # сборка + прошивка
pio monitor                # серийный монитор

### Pre-PR проверки
./bin/clang-format-fix
pio check -e default
pio run -e default

### Отладка
pip3 install pyserial colorama matplotlib

# Linux
python3 scripts/debugging_monitor.py

# macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101

# Windows
python3 scripts/debugging_monitor.py COM7

### Варианты сборки (platformio.ini)

| Env | Описание |
|---|---|
| default | ESP32-C3, X3+X4, debug log (LOG_LEVEL=2) |
| gh_release | ESP32-C3, release log (LOG_LEVEL=1) |
| gh_release_rc | Release candidate |
| slim | Без serial output для экономии места |
| sticky | Seeed reTerminal Sticky (ESP32-S3) |
| x4pro | Xteink X4 Pro (ESP32-S3, USB-MSC) |
| x4c | Xteink X4 Classic (ESP32-S3, кнопочная) |
| papermono | M5Stack Paper Mono (ESP32-S3) |
| eego_a4 | EEGO Reader A4 (ESP32-S3) |
| metalio_eink4 | Metalio E-Ink 4 (ESP32-S3) |
| picco | BOOX Picco / Onyx Tiles (ESP32-S3, SDMMC 4-bit) |

### Прошивка через esptool

# ESP32-C3 (X3, X4)
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 firmware.bin

# ESP32-S3 (X4 Pro, Sticky, Paper Mono, etc.)
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 firmware.bin

### Web installer
1. Подключить устройство по USB-C
2. Открыть https://crosspointreader.com/#flash-tools
3. Выбрать устройство и версию прошивки

## Локализация

34 языка интерфейса: English, Spanish, French, German, Czech, Portuguese (BR/PT), Russian, Swedish, Romanian, Catalan, Ukrainian, Belarusian, Italian, Polish, Finnish, Danish, Dutch, Turkish, Kazakh, Hungarian, Lithuanian, Slovenian, Valencian, Hebrew, Arabic, Slovak, Bosnian, Vietnamese, Norwegian Bokmål, Indonesian и другие.

- custom_i18n_builtin_langs = all (все встроенные)
- CJK font fallback
- RTL support (Arabic, Hebrew)
- docs/i18n.md, docs/translators.md

## Безопасность контента

- WolfSSL — TLS 1.3, ECC (P-256), RSA, ECDHE
- LCP (Licensed Content Protection) — RSA ключи, PBE-SHA1-RC2
- Browser-based activation, device-side crypto
- TrustedTime — защищённое время

## Структура SD-карты

/
- books/ — EPUB, XTC, TXT, BMP
- dictionaries/ — StarDict словари
- fonts/ — .cpfont или TTF/OTF/TTC
- .fonts/ — Скрытые шрифты
- My Clippings.txt — Экспорт вырезок (Kindle формат)
- screenshots/ — Скриншоты
- .sleep/ — Кастомные sleep-скриншоты
- .sleep-overlay/ — Прозрачные overlay
- .crosspoint/ — Проприетарный кэш
  - epub_<hash>/
    - progress.bin
    - cover.bmp
    - book.bin
    - css_rules.cache
    - img_*
    - sections/
  - settings.json
  - state.json
  - recent.json
  - bookmarks.json
  - clippings/
  - plugins/ — SD-card плагины

## Управление питанием

- Deep Sleep — ESP32 глубокий сон, пробуждение по кнопке Power
- Quick Resume — сохранение кадра на e-ink панели, мгновенное возвращение
- USB-powered sleep hold — удержание сна при подключении USB (BOOX Picco)
- Bluetooth sleep guard — BLE stops before plugin WiFi delivery
- CPU frequency scaling — power saving on idle, full speed on activity

## Contributing

1. Начать с docs/contributing/README.md
2. Проверить ideas discussion board: https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas
3. Коммитить в fork, PR в main repo
4. Все волонтёры — быть вежливым и терпеливым

## Важные файлы

| Файл | Описание |
|---|---|
| platformio.ini | Основная конфигурация сборки (env, libraries, build flags) |
| src/main.cpp | Точка входа, setup(), loop(), sleep/wake logic |
| freeink-sdk/platformio.ini | Конфигурация SDK (sample) |
| AGENTS.md | Инструкция для AI-агентов |
| SCOPE.md | Scope проекта (what's in/out) |
| ROADMAP.md | Путь развития |
| GOVERNANCE.md | Правила сообщества |
| USER_GUIDE.md | Полное руководство пользователя |
| .clang-format | Форматирование кода (clang-format 21) |
| requirements.txt | Python dependencies
