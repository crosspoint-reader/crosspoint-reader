# CrossPoint Reader — Skills: инженерный справочник по кодовой базе

Практические навыки работы с проектом: навигация по коду, типовые задачи, инварианты архитектуры,
сборка, тесты, отладка. Дополняет [GIGACODE.md](GIGACODE.md) (обзор проекта и возможностей) и
[AGENTS.md](AGENTS.md) (процесс для AI-агентов: правила, ревью, firmware-handoff).

---

## 1. Навигация по коду

| Что ищем | Где смотреть |
|---|---|
| Точка входа, главный цикл, сон/пробуждение | `src/main.cpp` |
| Менеджер экранов, стек переходов, render-task | `src/activities/ActivityManager.cpp`, `docs/activity-manager.md` |
| Базовые классы экранов (списки/табы) | `src/activities/UiListActivity.h`, `UiTabListActivity.h` |
| Мост к UI-фреймворку SDK (FreeInkUI) | `src/components/UiAppHost.cpp` |
| Ридер EPUB | `src/activities/reader/EpubReaderActivity.cpp` (ядро экрана) + `lib/Epub/` (движок) |
| Разбор глав/CSS | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`, `lib/Epub/Epub/css/CssParser.cpp` |
| 2D-рендер, e-ink обновления, кэш глифов | `lib/GfxRenderer/GfxRenderer.cpp` |
| Шрифты (встроенные + SD TTF/OTF) | `lib/EpdFont/` (`SdCardFont.cpp`, `fontconvert_sdcard.py`) |
| Web-сервер, загрузка файлов, WebSocket | `src/network/CrossPointWebServer.cpp` + сгенерированный HTML в `src/network/html/` |
| OTA, прошивка, WebDAV, OPDS-транспорт | `src/network/` (`OtaUpdater`, `FirmwareFlasher`, `WebDAVHandler`, `OpdsHttpTransport`) |
| Настройки и состояние устройства | `src/CrossPointSettings.cpp`, `src/CrossPointState.cpp`, `src/SettingsList.cpp` |
| Кнопки и ремаппинг | `src/MappedInputManager.cpp`, `src/util/ButtonNavigator` |
| Вырезки/выделения | `src/clippings/`, `src/ClippingStore.cpp` |
| Словарь StarDict | `src/util/Dictionary*`, `src/util/DictZip`, `docs/dictionary.md` |
| BLE-пульт | `lib/BlePageTurner/` (`tick()` вызывается из главного цикла) |
| Синхронизация прогресса (KOReader) | `lib/KOReaderSync/`, `src/activities/reader/KOReaderSyncActivity` |
| Индекс библиотеки на SD (формат CLX1) | `lib/LibraryIndex/` (`LibraryBuilder.cpp`) |
| Локализация (34 языка) | `lib/I18n/` (`I18nKeys.h` — ID строк; `translations/`; генерация `scripts/gen_i18n.py`) |
| HAL-слой над SDK | `lib/hal/` (`HalDisplay`, `HalGPIO`, `HalStorage`, `HalPowerManager`, …) |
| Профили плат, драйверы панелей, тач | `freeink-sdk/libs/hardware/BoardConfig`, `freeink-sdk/libs/display/FreeInkDisplay` |
| UI-фреймворк SDK | `freeink-sdk/libs/ui/FreeInkUI` (списки, клавиатуры, диалоги, cover-grid) |
| Форматы кэш-файлов на SD | `docs/file-formats.md` |

Крупнейшие файлы (точки наибольшей сложности): `EpubReaderActivity.cpp` (~3.4k строк),
`ChapterHtmlSlimParser.cpp` (~3.2k), `CrossPointWebServer.cpp` (~3.1k), `GfxRenderer.cpp` (~2.6k).

## 2. Архитектурные инварианты (не нарушать)

- **Единый render-task.** Вся отрисовка идёт через один общий рендер-таск `ActivityManager`;
  экран получает кадр через RAII-защиту `RenderLock` (`src/activities/RenderLock.h`).
  Не создавайте собственные рендер-таски в активностях.
- **Стек активностей.** Одна текущая Activity + стек; навигация — через маршруты
  `goHome()/goToReader()/…` в `ActivityManager`, результаты — `startActivityForResult()/setResult()`
  (андроидоподобная модель без onResume).
- **HAL-шов.** Приложение не касается железа напрямую — только через `lib/hal/`; всё
  девайс-специфичное живёт в FreeInk SDK (профили плат). Новое устройство = сначала профиль в SDK,
  потом env в `platformio.ini` этого репозитория.
- **Дисциплина памяти.** Базовое устройство (ESP32-C3) имеет ~380 КБ RAM без PSRAM. Любая новая
  куча-аллокация требует обоснования; аллокации — no-throw через `lib/Memory` (`makeUniqueNoThrow`,
  `BuildScratch` — PSRAM-арена для тяжёлых построек на S3). Большие данные — стриминг на/с SD.
- **Кэш на SD (`.crosspoint/`).** Главы, обложки, CSS, секции кэшируются на SD и стримятся.
  Смена формата кэша требует бампа версии + инвалидации (правила — `.agents/rules/generated-files-cache.md`).
- **Сгенерированные файлы не правятся руками**: `I18nStrings.cpp`, HTML `*.generated.h`,
  встроенные шрифты `lib/EpdFont/builtinFonts/` — всё генерируется скриптами (`scripts/gen_i18n.py`,
  `scripts/build_html.py`, `scripts/generate-font-manifest.py`).
- **Скоуп.** Новые фичи сверяются с `SCOPE.md`; сетевые интеграции — через SD-плагины
  (`docs/sd-plugins.md`, `docs/plugin-events.md`), не компиляцией в ядро.

## 3. Типовые задачи

**Добавить экран/активность**
1. Унаследоваться от `UiListActivity` / `UiTabListActivity` (`src/activities/`) — они дают тач-скролл,
   навигацию кнопками, FreeInkUI-хостинг через `UiAppHost`.
2. Переопределить `onEnter()/loop()`, данные и действия строк; отрисовку — только с `RenderLock&&`.
3. Переход — `activityManager.pushActivity()` или маршрут в `ActivityManager`.
4. Паттерны и жизненный цикл (workers, шрифты, ресурсы): `.agents/rules/ui-activities.md`,
   `docs/contributing/touch-and-ui.md`.

**Добавить настройку**
1. Поле в `src/CrossPointSettings.cpp` (persist в `/.crosspoint/settings.json`).
2. Экран в `src/activities/settings/` (обычно на базе `SettingsListActivity`, секции — `SettingsSection.h`).
3. При необходимости — exposed в web-UI настроек (`src/network/CrossPointWebServer.cpp`, список
   эндпоинтов — `docs/webserver-endpoints.md`).

**Добавить/изменить перевод**
1. Ключ в `lib/I18n/I18nKeys.h`, строки — `lib/I18n/translations/`.
2. Регенерация: `scripts/gen_i18n.py`; детали — `docs/i18n.md`, `docs/translators.md`.

**Добавить web-эндпоинт**
`src/network/CrossPointWebServer.cpp`; клиентская часть — исходники HTML/JS, из которых
`scripts/build_html.py` генерирует `*.generated.h`. Доки: `docs/webserver.md`,
`docs/webserver-endpoints.md`.

**Добавить устройство**
1. Профиль платы в `freeink-sdk/libs/hardware/BoardConfig` (+ драйверы панели/тача в SDK).
2. Env в `platformio.ini` (+ `partitions*.csv` при необходимости). Примеры: `sticky`, `x4pro`,
   `picco` с их `gh_release*` вариантами.

**Изменить формат кэша/файла**
Прочитать `.agents/rules/generated-files-cache.md` (versioning + invalidation) и
`docs/file-formats.md`. Не забыть миграцию старых кэшей (пример: `src/util/ParagraphIndentMigration`).

## 4. Сборка

```bash
pio run                          # env default (X3/X4, ESP32-C3)
pio run -e <env> --target upload # собрать+прошить конкретное устройство
pio monitor                      # серийный лог
```

Основные env (`platformio.ini`): `default`/`slim` (C3), `sticky`, `x4pro`, `x4c`, `papermono`,
`metalio_eink4`, `eego_a4`, `picco` (S3); релизные — суффикс `-gh_release`, RC — `-gh_release_rc`.
Локальные переопределения без коммита — `platformio.local.ini` (см. `.agents/rules/environment.md`).

Обязательные проверки перед PR:

```bash
./bin/clang-format-fix   # clang-format 21
pio check -e default     # статический анализ
pio run -e default       # сборка
```

## 5. Тесты

Хостовые unit-тесты (железо не нужно), GoogleTest через FetchContent:

```bash
cmake -S test -B build/test
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j
```

- ~45 gtest-наборов по темам (CSS-парсер, JSON, библиотечный индекс, вырезки, BLE, BiDi, ZIP…)
  в `test/`, каждая тема со своими `stubs/` для подмены прошивочных зависимостей.
- 9 Python-интеграционных тестов (`test/test_*.py`) зарегистрированы в CTest.
- EPUB-фикстуры — `test/epubs/`, часть генерируется `scripts/generate_*_epub.py`.
- `scripts/register_unit_tests_target.py` подключает тесты как таргет PlatformIO.

## 6. Отладка и диагностика

```bash
pip install pyserial colorama matplotlib
python scripts/debugging_monitor.py [PORT]   # Linux/macOS/Windows (например COM7)
python scripts/firmware_size_history.py      # динамика размера прошивки
```

- Логирование — макросы `lib/Logging` (`LOG_ERR/LOG_INF/LOG_DBG`), уровень задаётся env
  (`LOG_LEVEL=2` в debug-сборках, `1` в release).
- Крахи/паники: классификация загрузки и отчёт — `src/activities/home/CrashActivity`,
  правила — `.agents/rules/testing-debugging.md` (раздел Debugging crashes).
- Профилирование памяти — `scripts/script_profile_mem.sh`.

## 7. Git-процесс и ревью

- Интеграционная ветка — `develop`; PR из форка (см. `.agents/rules/git-workflow.md`).
- Форк: `origin` = свой форк, `upstream` = `crosspoint-reader/crosspoint-reader`; синхронизация с
  upstream перед началом работы.
- Любое изменение, влияющее на прошивку (C/C++, сборка, партиции, генерация, переводы,
  релиз-скрипты), проходит **firmware-handoff** (`.agents/skills/firmware-handoff/SKILL.md`):
  сборка + 4 независимых ревью + явное приёмочное тестирование человеком на железе.
- Коммит/пуш — только с явного одобрения человека; описание PR пишет человек.
- Правила по темам — `.agents/rules/` (environment, hardware-resources, coding-standards,
  ui-activities, architecture-hal, generated-files-cache, testing-debugging, git-workflow);
  процедуры — `.agents/skills/` (heap-discipline, scope-discipline, control-flow-clarity,
  refactor-for-review, review-*).

## 8. Карта документов

| Документ | Содержание |
|---|---|
| `README.md` | Обзор, установка, внутренности кэша |
| `USER_GUIDE.md` | Руководство пользователя (все функции) |
| `SCOPE.md` / `ROADMAP.md` | Границы проекта, фазы развития |
| `docs/activity-manager.md` | Архитектура активностей и render-task |
| `docs/file-formats.md` | Форматы кэш-файлов на SD |
| `docs/webserver.md`, `docs/webserver-endpoints.md` | Web-UI и API |
| `docs/sd-plugins.md`, `docs/plugin-events.md` | SD-плагины |
| `docs/sd-card-fonts.md` | Шрифты с SD-карты |
| `docs/dictionary.md` | StarDict-словари |
| `docs/i18n.md`, `docs/translators.md` | Локализация |
| `docs/hyphenation-trie-format.md` | Формат триггеров переносов |
| `docs/troubleshooting.md` | Типовые проблемы |
| `docs/contributing/` | Гайд для контрибьюторов, touch-and-ui |
