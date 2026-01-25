#include "BasicElements.h"

#include "Bitmap.h"
#include "ListElement.h"
#include "ThemeManager.h"
#include "ThemeTypes.h"
#include <GfxRenderer.h>

namespace ThemeEngine {

// --- BitmapElement ---
void BitmapElement::draw(const GfxRenderer& renderer, const ThemeContext& context) {
  if (!isVisible(context)) {
    markClean();
    return;
  }

  std::string path = context.evaluatestring(srcExpr);
  if (path.empty()) {
    markClean();
    return;
  }

  // Resolve simplified or relative paths
  if (path.find('/') == std::string::npos || (path.length() > 0 && path[0] != '/')) {
     path = ThemeManager::get().getAssetPath(path);
  }

  // 1. Check if we have a cached 1-bit render
  const ProcessedAsset* processed = ThemeManager::get().getProcessedAsset(path, renderer.getOrientation(), absW, absH);
  if (processed && processed->w == absW && processed->h == absH) {
    const int rowBytes = (absW + 7) / 8;
    for (int y = 0; y < absH; y++) {
      const uint8_t* srcRow = processed->data.data() + y * rowBytes;
      for (int x = 0; x < absW; x++) {
        // Cached 1-bit data: 0=Black, 1=White
        bool isBlack = !(srcRow[x / 8] & (1 << (7 - (x % 8))));
        // Draw opaque (true=black, false=white)
        renderer.drawPixel(absX + x, absY + y, isBlack);
      }
    }
    markClean();
    return;
  }

  bool drawSuccess = false;

  // 2. Try Streaming (Absolute paths, large images)
  if (path.length() > 0 && path[0] == '/') {
      FsFile file;
      if (SdMan.openFileForRead("HOME", path, file)) {
          Bitmap bmp(file, true); // (file, dithering=true)
          if (bmp.parseHeaders() == BmpReaderError::Ok) {
              // Center logic
              int drawX = absX;
              int drawY = absY;
              if (bmp.getWidth() < absW) drawX += (absW - bmp.getWidth()) / 2;
              if (bmp.getHeight() < absH) drawY += (absH - bmp.getHeight()) / 2;
              
              if (borderRadius > 0) {
                  renderer.drawRoundedBitmap(bmp, drawX, drawY, absW, absH, borderRadius);
              } else {
                  renderer.drawBitmap(bmp, drawX, drawY, absW, absH);
              }
              drawSuccess = true;
          }
          file.close();
      }
  }

  // 3. Fallback to RAM Cache (Standard method)
  if (!drawSuccess) {
     const std::vector<uint8_t>* data = ThemeManager::get().getCachedAsset(path);
     if (data && !data->empty()) {
        Bitmap bmp(data->data(), data->size());
        if (bmp.parseHeaders() == BmpReaderError::Ok) {
            int drawX = absX;
            int drawY = absY;
            if (bmp.getWidth() < absW) drawX += (absW - bmp.getWidth()) / 2;
            if (bmp.getHeight() < absH) drawY += (absH - bmp.getHeight()) / 2;
            
            if (borderRadius > 0) {
                renderer.drawRoundedBitmap(bmp, drawX, drawY, absW, absH, borderRadius);
            } else {
                renderer.drawBitmap(bmp, drawX, drawY, absW, absH);
            }
            drawSuccess = true;
        }
     }
  }

  // 4. Cache result if successful
  if (drawSuccess) {
    ProcessedAsset asset;
    asset.w = absW;
    asset.h = absH;
    asset.orientation = renderer.getOrientation();

    const int rowBytes = (absW + 7) / 8;
    asset.data.resize(rowBytes * absH, 0xFF);  // Initialize to 0xFF (White)

    for (int y = 0; y < absH; y++) {
      uint8_t* dstRow = asset.data.data() + y * rowBytes;
      for (int x = 0; x < absW; x++) {
        // Read precise pixel state from framebuffer
        bool isBlack = renderer.readPixel(absX + x, absY + y);
        if (isBlack) {
          // Clear bit for black (0)
          dstRow[x / 8] &= ~(1 << (7 - (x % 8)));
        }
      }
    }
    ThemeManager::get().cacheProcessedAsset(path, asset, absW, absH);
  }

  markClean();
}

// --- List ---
void List::draw(const GfxRenderer& renderer, const ThemeContext& context) {
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
        if (currentX > absX + absW) break;
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
        if (currentY > absY + absH) break;
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

}  // namespace ThemeEngine
