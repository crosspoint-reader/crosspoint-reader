// Icons, resolved by role so Rust never names an asset.

#include "components/icons/customListIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/search32.h"
#include "internal.h"

namespace {

// Mirrors IconRole on the Rust side.
enum class Role : uint8_t {
  Book = 0,
  Bookmark = 1,
  Download = 2,
  File = 3,
  FileText = 4,
  Folder = 5,
  Image = 6,
  Library = 7,
  Moon = 8,
  Hotspot = 9,
  Search = 10,
  Sun = 11,
  Upload = 12,
  Wifi = 13,
};

// `filled` picks the solid variant where one exists; `size` chooses between the
// 24 and 32 px cuts, falling back to whatever the role ships.
const freeink::Icon* resolve(const Role role, const bool filled, const int32_t size) {
  const bool large = size >= 32;
  switch (role) {
    case Role::Book:
      return large ? &icon_book_32 : &icon_book_24;
    case Role::Bookmark:
      return large ? &icon_bookmark_32 : &icon_bookmark_24;
    case Role::Download:
      return large ? &icon_download_32 : &icon_download_24;
    case Role::File:
      return large ? &icon_file_32 : &icon_file_24;
    case Role::FileText:
      return large ? &icon_file_text_32 : &icon_file_text_24;
    case Role::Folder:
      return large ? &icon_folder_32 : &icon_folder_24;
    case Role::Image:
      return large ? &icon_image_32 : &icon_image_24;
    case Role::Library:
      return large ? &icon_library_32 : &icon_library_24;
    case Role::Moon:
      if (filled) return large ? &icon_moon_filled_32 : &icon_moon_filled_24;
      return large ? &icon_moon_32 : &icon_moon_24;
    case Role::Hotspot:
      return large ? &icon_radio_tower_32 : &icon_radio_tower_24;
    case Role::Search:
      return &icon_search_32;  // one cut only
    case Role::Sun:
      if (filled) return large ? &icon_sun_filled_32 : &icon_sun_filled_24;
      return large ? &icon_sun_32 : &icon_sun_24;
    case Role::Upload:
      return large ? &icon_upload_32 : &icon_upload_24;
    case Role::Wifi:
      return large ? &icon_wifi_32 : &icon_wifi_24;
  }
  return nullptr;
}

}  // namespace

extern "C" {

void cpp_draw_icon(const uint8_t role, const uint8_t filled, const int32_t size, const int32_t x, const int32_t y) {
  if (!g_rustRendererPtr) return;

  const freeink::Icon* icon = resolve(static_cast<Role>(role), filled != 0, size);
  if (!icon || !icon->bits) return;

  // These assets are NOT pre-rotated, so they go through drawPixel, which
  // applies the orientation transform. Bit 0 is ink and bit 1 is left alone, so
  // this is a transparent blit.
  const int rowBytes = (icon->w + 7) / 8;
  for (int row = 0; row < icon->h; row++) {
    for (int col = 0; col < icon->w; col++) {
      const uint8_t byte = icon->bits[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) g_rustRendererPtr->drawPixel(x + col, y + row, true);
    }
  }
}

int32_t cpp_icon_size(const uint8_t role, const uint8_t filled, const int32_t size) {
  const freeink::Icon* icon = resolve(static_cast<Role>(role), filled != 0, size);
  return icon ? icon->w : 0;
}
}
