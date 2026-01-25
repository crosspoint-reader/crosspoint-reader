#include "BasicElements.h"
#include "Bitmap.h"
#include "ListElement.h"
#include "ThemeManager.h"
#include "ThemeTypes.h"

namespace ThemeEngine {

// --- BitmapElement ---
void BitmapElement::draw(const GfxRenderer &renderer,
                         const ThemeContext &context) {
  if (!isVisible(context)) {
    markClean();
    return;
  }

  std::string path = context.evaluatestring(srcExpr);
  if (path.empty()) {
    markClean();
    return;
  }

  // 1. Try Processed Cache in ThemeManager (keyed by path + target dimensions)
  const ProcessedAsset *processed =
      ThemeManager::get().getProcessedAsset(path, renderer.getOrientation(), absW, absH);
  if (processed && processed->w == absW && processed->h == absH) {
    renderer.draw2BitImage(processed->data.data(), absX, absY, absW, absH);
    markClean();
    return;
  }

  // 2. Try raw asset cache, then process and cache
  const std::vector<uint8_t> *data = ThemeManager::get().getCachedAsset(path);
  if (!data || data->empty()) {
    markClean();
    return;
  }

  Bitmap bmp(data->data(), data->size());
  if (bmp.parseHeaders() != BmpReaderError::Ok) {
    markClean();
    return;
  }

  // Draw the bitmap (handles scaling internally)
  renderer.drawBitmap(bmp, absX, absY, absW, absH);
  
  // After drawing, capture the rendered region and cache it for next time
  ProcessedAsset asset;
  asset.w = absW;
  asset.h = absH;
  asset.orientation = renderer.getOrientation();
  
  // Capture the rendered region from framebuffer
  uint8_t *frameBuffer = renderer.getFrameBuffer();
  if (frameBuffer) {
    const int screenW = renderer.getScreenWidth();
    const int bytesPerRow = (absW + 3) / 4;
    asset.data.resize(bytesPerRow * absH);
    
    for (int y = 0; y < absH; y++) {
      int srcOffset = ((absY + y) * screenW + absX) / 4;
      int dstOffset = y * bytesPerRow;
      // Copy 2-bit packed pixels
      for (int x = 0; x < absW; x++) {
        int sx = absX + x;
        int srcByteIdx = ((absY + y) * screenW + sx) / 4;
        int srcBitIdx = (sx % 4) * 2;
        int dstByteIdx = dstOffset + x / 4;
        int dstBitIdx = (x % 4) * 2;
        
        uint8_t pixel = (frameBuffer[srcByteIdx] >> (6 - srcBitIdx)) & 0x03;
        asset.data[dstByteIdx] &= ~(0x03 << (6 - dstBitIdx));
        asset.data[dstByteIdx] |= (pixel << (6 - dstBitIdx));
      }
    }
    
    ThemeManager::get().cacheProcessedAsset(path, asset, absW, absH);
  }

  markClean();
}

// --- List ---
void List::draw(const GfxRenderer &renderer, const ThemeContext &context) {
  if (!isVisible(context)) {
    markClean();
    return;
  }

  // Draw background
  if (hasBg) {
    std::string colStr = context.evaluatestring(bgColorExpr);
    uint8_t color = Color::parse(colStr).value;
    renderer.fillRect(absX, absY, absW, absH, color == 0x00);
  }
  if (border) {
    renderer.drawRect(absX, absY, absW, absH, true);
  }

  if (!itemTemplate) {
    markClean();
    return;
  }

  int count = context.getInt(source + ".Count");
  if (count <= 0) {
    markClean();
    return;
  }

  // Get item dimensions
  int itemW = getItemWidth();
  int itemH = getItemHeight();

  // Handle different layout modes
  if (direction == Direction::Horizontal || layoutMode == LayoutMode::Grid) {
    // Horizontal or Grid layout
    int col = 0;
    int row = 0;
    int currentX = absX;
    int currentY = absY;

    // For grid, calculate item width based on columns
    if (layoutMode == LayoutMode::Grid && columns > 1) {
      int totalSpacing = (columns - 1) * spacing;
      itemW = (absW - totalSpacing) / columns;
    }

    for (int i = 0; i < count; ++i) {
      // Create item context with scoped variables
      ThemeContext itemContext(&context);
      std::string prefix = source + "." + std::to_string(i) + ".";

      // Standard list item variables
      itemContext.setString("Item.Title", context.getString(prefix + "Title"));
      itemContext.setString("Item.Value", context.getString(prefix + "Value"));
      itemContext.setBool("Item.Selected", context.getBool(prefix + "Selected"));
      itemContext.setString("Item.Icon", context.getString(prefix + "Icon"));
      itemContext.setString("Item.Image", context.getString(prefix + "Image"));
      itemContext.setString("Item.Progress", context.getString(prefix + "Progress"));
      itemContext.setInt("Item.Index", i);
      itemContext.setInt("Item.Count", count);

      // Viewport check
      if (direction == Direction::Horizontal) {
        if (currentX + itemW < absX) {
          currentX += itemW + spacing;
          continue;
        }
        if (currentX > absX + absW)
          break;
      } else {
        // Grid mode
        if (currentY + itemH < absY) {
          col++;
          if (col >= columns) {
            col = 0;
            row++;
            currentY += itemH + spacing;
          }
          currentX = absX + col * (itemW + spacing);
          continue;
        }
        if (currentY > absY + absH)
          break;
      }

      // Layout and draw
      itemTemplate->layout(itemContext, currentX, currentY, itemW, itemH);
      itemTemplate->draw(renderer, itemContext);

      if (layoutMode == LayoutMode::Grid && columns > 1) {
        col++;
        if (col >= columns) {
          col = 0;
          row++;
          currentX = absX;
          currentY += itemH + spacing;
        } else {
          currentX += itemW + spacing;
        }
      } else {
        // Horizontal list
        currentX += itemW + spacing;
      }
    }
  } else {
    // Vertical list (default)
    int currentY = absY;
    int viewportBottom = absY + absH;

    for (int i = 0; i < count; ++i) {
      // Skip items above viewport
      if (currentY + itemH < absY) {
        currentY += itemH + spacing;
        continue;
      }
      // Stop if below viewport
      if (currentY > viewportBottom) {
        break;
      }

      // Create item context with scoped variables
      ThemeContext itemContext(&context);
      std::string prefix = source + "." + std::to_string(i) + ".";

      itemContext.setString("Item.Title", context.getString(prefix + "Title"));
      itemContext.setString("Item.Value", context.getString(prefix + "Value"));
      itemContext.setBool("Item.Selected", context.getBool(prefix + "Selected"));
      itemContext.setString("Item.Icon", context.getString(prefix + "Icon"));
      itemContext.setString("Item.Image", context.getString(prefix + "Image"));
      itemContext.setString("Item.Progress", context.getString(prefix + "Progress"));
      itemContext.setInt("Item.Index", i);
      itemContext.setInt("Item.Count", count);

      // Layout and draw the template for this item
      itemTemplate->layout(itemContext, absX, currentY, absW, itemH);
      itemTemplate->draw(renderer, itemContext);

      currentY += itemH + spacing;
    }
  }

  markClean();
}

} // namespace ThemeEngine
