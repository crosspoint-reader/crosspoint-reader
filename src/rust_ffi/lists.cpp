// Lists and option popups: the two widgets the theme paints end to end.

#include <functional>
#include <string>
#include <vector>

#include "components/OptionPopup.h"
#include "components/UITheme.h"
#include "internal.h"

using rust_ffi::asText;

namespace {

// Collects the Rust-side option strings once, for the popup calls below.
std::vector<std::string> collectOptions(const char* (*optionText)(void*, int32_t), void* ctx, const int32_t count) {
  std::vector<std::string> options;
  if (!optionText || count <= 0) return options;

  options.reserve(static_cast<size_t>(count));
  for (int32_t i = 0; i < count; i++) {
    const char* text = optionText(ctx, i);
    options.emplace_back(text ? text : "");
  }
  return options;
}

}  // namespace

extern "C" {

void cpp_theme_draw_option_popup(const uint8_t* title, const char* (*optionText)(void*, int32_t), void* ctx,
                                 const int32_t count, const int32_t selected) {
  if (!g_rustRendererPtr || !optionText) return;
  const auto options = collectOptions(optionText, ctx, count);
  if (options.empty()) return;

  GUI.drawOptionPopup(*g_rustRendererPtr, title ? asText(title) : "", options, selected);
}

uint8_t cpp_option_popup_row_rect(const uint8_t* title, const char* (*optionText)(void*, int32_t), void* ctx,
                                  const int32_t count, const int32_t index, int32_t* outXywh) {
  if (!g_rustRendererPtr || !optionText || !outXywh || index < 0) return 0;
  const auto options = collectOptions(optionText, ctx, count);
  if (index >= static_cast<int32_t>(options.size())) return 0;

  // Reuses OptionPopup's own geometry so the dialog math is not duplicated.
  const auto layout = OptionPopup::computeLayout(*g_rustRendererPtr, title ? asText(title) : "", options);
  const auto& rect = layout.options[static_cast<size_t>(index)];

  outXywh[0] = rect.x;
  outXywh[1] = rect.y;
  outXywh[2] = rect.width;
  outXywh[3] = rect.height;
  return 1;
}

void cpp_theme_draw_list(const int32_t x, const int32_t y, const int32_t width, const int32_t height,
                         const int32_t itemCount, const int32_t selectedIndex,
                         const char* (*rowText)(void*, int32_t, int32_t), void* ctx) {
  if (!g_rustRendererPtr || !rowText) return;

  // The theme picks row height and text placement from whether the subtitle
  // accessor is null, not from what it returns, so probe first and pass nullptr
  // when no row has the field.
  const auto anyRowHas = [rowText, ctx, itemCount](const int32_t field) {
    for (int32_t i = 0; i < itemCount; i++) {
      if (rowText(ctx, i, field) != nullptr) return true;
    }
    return false;
  };

  const auto title = [rowText, ctx](const int index) -> std::string {
    const char* text = rowText(ctx, index, 0);
    return text ? std::string(text) : std::string();
  };
  const auto subtitle = [rowText, ctx](const int index) -> std::string {
    const char* text = rowText(ctx, index, 1);
    return text ? std::string(text) : std::string();
  };
  const auto value = [rowText, ctx](const int index) -> std::string {
    const char* text = rowText(ctx, index, 2);
    return text ? std::string(text) : std::string();
  };

  using RowAccessor = std::function<std::string(int)>;
  const RowAccessor subtitleFn = anyRowHas(1) ? RowAccessor(subtitle) : nullptr;
  const RowAccessor valueFn = anyRowHas(2) ? RowAccessor(value) : nullptr;

  // highlightValue stays false: no settings screen features a changeable value,
  // and a Rust screen that did would look foreign beside them.
  GUI.drawList(*g_rustRendererPtr, Rect{x, y, width, height}, itemCount, selectedIndex, title, subtitleFn, nullptr,
               valueFn, false);
}
}
