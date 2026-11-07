# Context — рабочие договорённости и тонкости (для продолжения работы)

Этот файл — «память» проекта для любой новой сессии/агента. Читайте его
перед работой вместе с `AGENTS.md` (апстрим-правила) и `skills.md`
(инженерный справочник по кодовой базе).

---

## Проект

- **Репозиторий**: `D:\github\crosspoint` — личный форк
  [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader)
  (origin = `unfilled/crosspoint`, upstream = официальный). Ветка `gigacodetest`.
- **Прошивка — только для личного использования**, не распространяется
  (поэтому можно держать чужие заимствования с атрибуцией и не следовать
  апстрим-скоупу; лицензионная чистота всё равно соблюдена: OFL/free шрифты,
  MIT-код форков с указанием источника).
- **Устройство**: Xteink X4 Pro (env `x4pro`, ESP32-S3 + PSRAM, touch,
  frontlight). Проверка — в эмуляторе x4pro, лог смотрим в консоли эмулятора.

## Договорённости о работе

1. **Сборку запускает человек.** Агент готовит код и выдаёт готовую команду;
   сам `pio run` не запускает (сборка на drvfs занимает часы, нельзя занимать
   впустую и мешать запущенной).
2. **Пока сборка идёт — файлы проекта не менять.** В это время можно:
   читать код, писать файлы ВНЕ репозитория (E:\crosspoint), исследовать
   форки в интернете.
3. Перед каждой сборкой — **«традиционное ревью»** свежим взглядом: агент сам
   находит и чинит проблемы до передачи команды.
4. Пользователь сам делает коммиты (своими смысловыми сообщениями).
5. Отключение функциональности, которое не используется (BLE), и перенос
  «тяжёлых» данных (шрифты) с прошивки на SD — приветствуется: flash-запас
  ценнее.
6. Язык общения — русский. Комментарии в коде — английские (по AGENTS.md).

## Сборка (WSL) — рабочая конфигурация

```bash
cd /mnt/d/github/crosspoint && \
HOME=/home/unfilled \
PLATFORMIO_CORE_DIR=/mnt/e/crosspoint/.platformio \
/mnt/e/crosspoint/venv/bin/pio run -e x4pro \
> /mnt/e/crosspoint/build_fb2.log 2>&1; echo "EXIT=$?"
```

- **PlatformIO 6.1.19** (НЕ 6.2.x!) в `/mnt/e/crosspoint/venv` (Python 3.12 из
  `~/.pyenv`). Версия 6.2 тянет SCons 4.11 → крах линковки
  `No module named 'SCons.Tool.FortranCommon'`. То же требование к
  `penv` внутри `.platformio`.
- Все тяжёлые кэши/логи — на `E:\crosspoint` (диск C беречь):
  `.platformio` (тулчейны), `build*.log`, venv, cpfont-пакеты.
- **`.pio` обязан лежать в проекте** (реальный каталог): патч-скрипты
  прошивки проверяют `PROJECT_DIR/.pio/libdeps` через `Path.resolve()`.
- Скорость: полная пересборка — часы (drvfs); инкремент — десятки минут;
  LDF-скан один ~час. Это нормально.
- Хостовые тесты: `cmake -S test -B build/test && cmake --build build/test
  --target Fb2ConverterTest -j4 && ./build/test/fb2_converter/Fb2ConverterTest`
  (сьюты library_index_file/library_builder в этой WSL не собираются:
  GCC 9.4 vs C++20-concepts в их родных стабах — сломано до нас).

## Критические грабли (однократно наступленные)

1. **CRLF-патчи**: патчи `scripts/*_patches/*.patch` обязаны быть LF в рабочей
   копии; ренормализация blob'ов в истории НЕ сделана — после любого
   Windows-checkout прогнать
   `sed -i 's/\r$//' scripts/jpegdec_patches/*.patch scripts/sdfat_patches/*.patch`.
2. **Libdeps нельзя чистить вслепую**: `rm -rf .pio` затирает libdeps →
   JPEGDEC клонируется заново; если клон оборвался — остаётся изуродованный
   jpeg.inl (симптом: `patch does not apply` в обе стороны). Лечение —
   пересоздать libdeps.
3. **Долгие блокировки loop-таска** (конвертация FB2, лукап словаря) —
   допустимы: activity-таск не подписан на watchdog. Но рисовать попап надо
   ДО блокировки и БЕЗ `renderer.displayBuffer()` из loop-контекста.
4. **Изменение lib_deps → перегенерация библиотек** долгая; после ошибок
   компиляции пересборка падает на «потерянном» хвосте — проверяйте функцию
   целиком после серии Edit'ов.
5. **Font-очистка между запусками**: `~/.platformio` нельзя удалять — там
   тулчейны на много ГБ; чистить только `packages` точечно при порче.

## Инструменты и артефакты вне репозитория (E:\crosspoint)

- `reference/papyrix` — клон papyrix-reader (донор; эталон кодировок).
- `Literata-cpfont/`, `Inglobal-cpfont/` — готовые .cpfont-пакеты для SD.
- `literata-fonts.yaml`, `inglobal-fonts.yaml` — мини-конфиги для
  `build-sd-fonts.py` (генерация пакетов повторно).
- `*.py` — одноразовые хост-тесты/скрипты (genfontids, swap-literata,
  test-note-extract2 и др.) — пригодны как образцы.
- `BUILD_NOTES.md` — история сборки/граблей (старее этого файла).
- Логи: `build.log`, `build_fb2.log`, `build-gigacode-failed.log`,
  `emulator_log.txt`, тестовые книги `book.fb2`, `book_popups.fb2`.

## Ключевые архитектурные точки (куда смотреть)

- FB2-движок: `lib/Fb2/` (парсеры — `Fb2Passes.h`, писатель —
  `StoredZipWriter`, оркестратор — `Fb2::ensureEpub`).
- Попапы: `src/activities/util/TextPopupActivity` (база) +
  `DictionaryPopupActivity` (наследник) + `FootnoteTextExtractor.h`
  (текст сноски из исходного XHTML).
- Вход в FB2: `EpubReaderActivity::loadBook` (ветка конвертации);
  очистка кэша: `BookCacheUtils.cpp`.
- Go to page: `goToPage`/`jumpToSpinePage` в `EpubReaderActivity`.
- Шрифты SD: `lib/EpdFont/scripts/sd-fonts.yaml` + `build-sd-fonts.py`;
  исходники TTF — `lib/EpdFont/builtinFonts/source/{Literata,Inglobal}`.
- Зависимости генераторов в venv: fonttools, freetype-py, zopfli, pyyaml.
