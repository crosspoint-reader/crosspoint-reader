# Font Integration Pipeline (Agent Runbook)

Mục tiêu: cung cấp quy trình chuẩn để agent tích hợp font mới vào firmware CrossPoint một cách nhất quán, an toàn với RAM/cache, và có thể verify rõ ràng.

## 0. Pre-flight (bắt buộc)
1. Phát hiện platform:
```bash
uname -s
```
2. Kiểm tra trạng thái repo:
```bash
git branch --show-current
git remote -v
git status --short
```
3. Xác định phạm vi tích hợp:
- `UI-only`: chỉ cho `SMALL_FONT_ID`, `UI_10_FONT_ID`, `UI_12_FONT_ID`.
- `Reader + UI`: thêm vào font reader trong settings, cache và render path.
4. Kiểm tra Python/tooling cho pipeline font:
```bash
python3 --version
```
Nếu máy chưa có môi trường Python phù hợp hoặc thiếu package, phải dùng virtual environment tại root repo: `/.venv`.
5. Tạo hoặc tái sử dụng virtual environment ở root:
```bash
# từ root repo
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r lib/EpdFont/scripts/requirements.txt
```
6. Trước khi chạy convert script, xác nhận các package đã sẵn sàng:
```bash
source .venv/bin/activate
python -c "import freetype, fontTools; print('font deps ok')"
```

## 1. Thêm file font nguồn
1. Chép `.ttf` vào `lib/EpdFont/builtinFonts/source/<FontName>/`.
2. Không commit file tạm; chỉ commit file cần thiết cho build.

## 2. Cập nhật script convert font
1. Sửa `lib/EpdFont/scripts/convert-builtin-fonts.sh`:
- Khai báo size/style cho font mới.
- Tránh ghi đè cùng một output header nhiều lần với thông số khác nhau.
2. Rule kỹ thuật:
- Font reader nên dùng `--2bit --compress` (đúng pattern ở script hiện tại).
- UI font có thể để 1-bit nếu muốn giữ nhẹ.
3. Reference:
- `lib/EpdFont/scripts/convert-builtin-fonts.sh:7`
- `lib/EpdFont/scripts/convert-builtin-fonts.sh:48`
- `lib/EpdFont/scripts/convert-builtin-fonts.sh:62`

## 3. Generate header font
1. Chạy script convert:
```bash
source .venv/bin/activate
bash lib/EpdFont/scripts/convert-builtin-fonts.sh
```
2. Đảm bảo các header mới xuất hiện trong `lib/EpdFont/builtinFonts/`.
3. Cập nhật `lib/EpdFont/builtinFonts/all.h` để include đủ header mới.

## 4. Đồng bộ Font IDs (bắt buộc)
1. Sửa `lib/EpdFont/scripts/build-font-ids.sh` để hash đúng file header mới:
- Reader IDs
- UI IDs (`UI_10`, `UI_12`, `SMALL`)
2. Regenerate `src/fontIds.h`:
```bash
bash lib/EpdFont/scripts/build-font-ids.sh > src/fontIds.h
```
3. Reference:
- `lib/EpdFont/scripts/build-font-ids.sh:11`
- `lib/EpdFont/scripts/build-font-ids.sh:83`
- `src/fontIds.h:1`

## 5. Wiring runtime trong main
1. Tạo `EpdFont` và `EpdFontFamily` cho font mới ở `src/main.cpp`.
2. Đăng ký map bằng `renderer.insertFont(...)` trong `setupDisplayAndFonts()`.
3. Reference:
- Khai báo family: `src/main.cpp:36`
- Insert map: `src/main.cpp:205`
- UI font binding hiện tại: `src/main.cpp:221`

## 6. Tích hợp vào Reader Settings (nếu là Reader font)
1. Thêm enum family trong `CrossPointSettings`:
- `src/CrossPointSettings.h:101`
2. Map family+size -> font id:
- `src/CrossPointSettings.cpp:286`
3. Thêm option ở settings list:
- `src/SettingsList.h:41`

## 7. I18n cho option mới
1. Thêm key mới (ví dụ `STR_<FONT_NAME>`) vào **tất cả** file trong `lib/I18n/translations/*.yaml` (không chỉ English).
2. Verify không còn file thiếu key:
```bash
rg --files-without-match '^STR_<FONT_NAME>:' lib/I18n/translations/*.yaml
```
Lệnh trên phải không in ra file nào.
3. Regenerate i18n:
```bash
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```
4. Nếu generator báo fallback cho key font mới, xem là chưa đạt checklist.

## 8. Cache compatibility check
1. Reader cache section có check `fontId`; đổi ID đúng sẽ tự invalidate cache.
2. Reference:
- `lib/Epub/Epub/Section.cpp:101`

## 9. Verification checklist
1. Build:
```bash
pio run
```
2. Nếu dùng static analysis:
```bash
pio check
```
3. Verify UI:
- Boot screen, Settings, status bar hiển thị đúng glyph.
4. Verify Reader:
- Đổi qua font mới trong settings.
- Mở EPUB cũ và xác nhận re-layout/cache rebuild.
5. Verify RAM:
- Theo dõi heap: `LOG_DBG("MEM", "Free heap: %d", ESP.getFreeHeap());`
- Mục tiêu không tụt heap bất thường sau nhiều lần vào/ra reader.

## 10. PR checklist
1. Không commit file generated bị ignore (`I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`, `.pio/`, `*.generated.h`).
2. Có commit:
- script convert/build-font-ids
- headers font mới
- `all.h`
- `main.cpp`, `fontIds.h`
- settings/i18n source liên quan
3. Ghi rõ trong PR:
- Font nào là UI vs Reader
- Có/không có italic/bold-italic
- Cơ chế fallback style (nếu thiếu style)
