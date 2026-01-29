#include "BasicElements.h"

#include <GfxRenderer.h>

#include "Bitmap.h"
#include "ListElement.h"
#include "ThemeManager.h"
#include "ThemeTypes.h"

namespace ThemeEngine {

// --- Container ---
void Container::draw(const GfxRenderer& renderer, const ThemeContext& context) {
  if (!isVisible(context)) return;

  if (hasBg) {
    std::string colStr = context.evaluatestring(bgColorExpr);
    uint8_t color = Color::parse(colStr).value;
    // Use dithered fill for grayscale values, solid fill for black/white
    // Use rounded rect if borderRadius > 0
    if (color == 0x00) {
      if (borderRadius > 0) {
        renderer.fillRoundedRect(absX, absY, absW, absH, borderRadius, true);
      } else {
        renderer.fillRect(absX, absY, absW, absH, true);
      }
    } else if (color >= 0xF0) {
      if (borderRadius > 0) {
        renderer.fillRoundedRect(absX, absY, absW, absH, borderRadius, false);
      } else {
        renderer.fillRect(absX, absY, absW, absH, false);
      }
    } else {
      if (borderRadius > 0) {
        renderer.fillRoundedRectDithered(absX, absY, absW, absH, borderRadius, color);
      } else {
        renderer.fillRectDithered(absX, absY, absW, absH, color);
      }
    }
  }

  // Handle dynamic border expression
  bool drawBorder = border;
  if (hasBorderExpr()) {
    drawBorder = context.evaluateBool(borderExpr.rawExpr);
  }

  if (drawBorder) {
    if (borderRadius > 0) {
      renderer.drawRoundedRect(absX, absY, absW, absH, borderRadius, true);
    } else {
      renderer.drawRect(absX, absY, absW, absH, true);
    }
  }

  for (auto child : children) {
    child->draw(renderer, context);
  }

  markClean();
}

// --- Rectangle ---
void Rectangle::draw(const GfxRenderer& renderer, const ThemeContext& context) {
  if (!isVisible(context)) return;

  std::string colStr = context.evaluatestring(colorExpr);
  uint8_t color = Color::parse(colStr).value;

  bool shouldFill = fill;
  if (!fillExpr.empty()) {
    shouldFill = context.evaluateBool(fillExpr.rawExpr);
  }

  if (shouldFill) {
    // Use dithered fill for grayscale values, solid fill for black/white
    // Use rounded rect if borderRadius > 0
    if (color == 0x00) {
      if (borderRadius > 0) {
        renderer.fillRoundedRect(absX, absY, absW, absH, borderRadius, true);
      } else {
        renderer.fillRect(absX, absY, absW, absH, true);
      }
    } else if (color >= 0xF0) {
      if (borderRadius > 0) {
        renderer.fillRoundedRect(absX, absY, absW, absH, borderRadius, false);
      } else {
        renderer.fillRect(absX, absY, absW, absH, false);
      }
    } else {
      if (borderRadius > 0) {
        renderer.fillRoundedRectDithered(absX, absY, absW, absH, borderRadius, color);
      } else {
        renderer.fillRectDithered(absX, absY, absW, absH, color);
      }
    }
  } else {
    // Draw border
    bool black = (color == 0x00);
    if (borderRadius > 0) {
      renderer.drawRoundedRect(absX, absY, absW, absH, borderRadius, black);
    } else {
      renderer.drawRect(absX, absY, absW, absH, black);
    }
  }

  markClean();
}

// --- Label ---
void Label::draw(const GfxRenderer& renderer, const ThemeContext& context) {
  if (!isVisible(context)) return;

  std::string finalStr = context.evaluatestring(textExpr);

  if (finalStr.empty()) {
    markClean();
    return;
  }

  std::string colStr = context.evaluatestring(colorExpr);
  uint8_t color = Color::parse(colStr).value;
  bool black = (color == 0x00);

  int textWidth = renderer.getTextWidth(fontId, finalStr.c_str());
  int lineHeight = renderer.getLineHeight(fontId);

  std::vector<std::string> lines;
  lines.reserve(maxLines);  // Pre-allocate to avoid reallocations
  if (absW > 0 && textWidth > absW && maxLines > 1) {
    // Logic to wrap text
    std::string remaining = finalStr;
    while (!remaining.empty() && (int)lines.size() < maxLines) {
      // If it fits, add entire line
      if (renderer.getTextWidth(fontId, remaining.c_str()) <= absW) {
        lines.push_back(remaining);
        break;
      }

      // Binary search for maximum characters that fit (O(log n) instead of O(n))
      int len = remaining.length();
      int lo = 1, hi = len;
      while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (renderer.getTextWidth(fontId, remaining.substr(0, mid).c_str()) <= absW) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      int cut = lo;

      // Find last space before cut
      if (cut < (int)remaining.length()) {
        int space = -1;
        for (int i = cut; i > 0; i--) {
          if (remaining[i] == ' ') {
            space = i;
            break;
          }
        }
        if (space != -1) cut = space;
      }

      std::string line = remaining.substr(0, cut);

      // If we're at the last allowed line but still have more text
      if ((int)lines.size() == maxLines - 1 && cut < (int)remaining.length()) {
        if (ellipsis) {
          line = renderer.truncatedText(fontId, remaining.c_str(), absW);
        }
        lines.push_back(line);
        break;
      }

      lines.push_back(line);
      // Advance
      if (cut < (int)remaining.length()) {
        // Skip the space if check
        if (remaining[cut] == ' ') cut++;
        remaining = remaining.substr(cut);
      } else {
        remaining = "";
      }
    }
  } else {
    // Single line handling (truncate if needed)
    if (ellipsis && textWidth > absW && absW > 0) {
      finalStr = renderer.truncatedText(fontId, finalStr.c_str(), absW);
    }
    lines.push_back(finalStr);
  }

  // Draw lines
  int totalTextHeight = lines.size() * lineHeight;
  int startY = absY;

  // Vertical centering
  if (absH > 0 && totalTextHeight < absH) {
    startY = absY + (absH - totalTextHeight) / 2;
  }

  for (size_t i = 0; i < lines.size(); i++) {
    int lineWidth = renderer.getTextWidth(fontId, lines[i].c_str());
    int drawX = absX;

    if (alignment == Alignment::Center && absW > 0) {
      drawX = absX + (absW - lineWidth) / 2;
    } else if (alignment == Alignment::Right && absW > 0) {
      drawX = absX + absW - lineWidth;
    }

    renderer.drawText(fontId, drawX, startY + i * lineHeight, lines[i].c_str(), black);
  }

  markClean();
}

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

  if (path.find('/') == std::string::npos || (path.length() > 0 && path[0] != '/')) {
    path = ThemeManager::get().getAssetPath(path);
  }

  // Fast path: use cached 1-bit render
  const ProcessedAsset* processed = ThemeManager::get().getProcessedAsset(path, renderer.getOrientation(), absW, absH);
  if (processed && processed->w == absW && processed->h == absH) {
    renderer.restoreRegion(processed->data.data(), absX, absY, absW, absH);
    markClean();
    return;
  }

  // Helper to draw bitmap with centering and optional rounded corners
  auto drawBmp = [&](Bitmap& bmp) {
    int drawX = absX;
    int drawY = absY;
    if (bmp.getWidth() < absW) drawX += (absW - bmp.getWidth()) / 2;
    if (bmp.getHeight() < absH) drawY += (absH - bmp.getHeight()) / 2;
    if (borderRadius > 0) {
      renderer.drawRoundedBitmap(bmp, drawX, drawY, absW, absH, borderRadius);
    } else {
      renderer.drawBitmap(bmp, drawX, drawY, absW, absH);
    }
  };

  bool drawSuccess = false;

  // Try RAM cache first
  const std::vector<uint8_t>* cachedData = ThemeManager::get().getCachedAsset(path);
  if (cachedData && !cachedData->empty()) {
    Bitmap bmp(cachedData->data(), cachedData->size());
    if (bmp.parseHeaders() == BmpReaderError::Ok) {
      drawBmp(bmp);
      drawSuccess = true;
    }
  }

  // Fallback: load from SD card
  if (!drawSuccess && path.length() > 0 && path[0] == '/') {
    FsFile file;
    if (SdMan.openFileForRead("HOME", path, file)) {
      size_t fileSize = file.size();
      if (fileSize > 0 && fileSize < 100000) {
        std::vector<uint8_t> fileData(fileSize);
        if (file.read(fileData.data(), fileSize) == fileSize) {
          ThemeManager::get().cacheAsset(path, std::move(fileData));
          const std::vector<uint8_t>* newCachedData = ThemeManager::get().getCachedAsset(path);
          if (newCachedData && !newCachedData->empty()) {
            Bitmap bmp(newCachedData->data(), newCachedData->size());
            if (bmp.parseHeaders() == BmpReaderError::Ok) {
              drawBmp(bmp);
              drawSuccess = true;
            }
          }
        }
      } else {
        Bitmap bmp(file, true);
        if (bmp.parseHeaders() == BmpReaderError::Ok) {
          drawBmp(bmp);
          drawSuccess = true;
        }
      }
      file.close();
    }
  }

  // Cache rendered result for fast subsequent draws using captureRegion
  if (drawSuccess && absW * absH <= 40000) {
    size_t capturedSize = 0;
    uint8_t* captured = renderer.captureRegion(absX, absY, absW, absH, &capturedSize);
    if (captured && capturedSize > 0) {
      ProcessedAsset asset;
      asset.w = absW;
      asset.h = absH;
      asset.orientation = renderer.getOrientation();
      asset.data.assign(captured, captured + capturedSize);
      free(captured);
      ThemeManager::get().cacheProcessedAsset(path, asset, absW, absH);
    }
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

  // Pre-allocate string buffers to avoid repeated allocations
  std::string prefix;
  prefix.reserve(source.length() + 16);
  std::string key;
  key.reserve(source.length() + 32);
  char numBuf[12];

  // Handle different layout modes
  if (direction == Direction::Horizontal || layoutMode == LayoutMode::Grid) {
    // Horizontal or Grid layout
    int col = 0;
    int row = 0;
    int currentX = absX;
    int currentY = absY;

    // For grid, calculate item width based on columns only if not explicitly set
    if (layoutMode == LayoutMode::Grid && columns > 1 && itemWidth == 0) {
      int totalSpacing = (columns - 1) * spacing;
      itemW = (absW - totalSpacing) / columns;
    }

    for (int i = 0; i < count; ++i) {
      // Build prefix efficiently: "source.i."
      prefix.clear();
      prefix += source;
      prefix += '.';
      snprintf(numBuf, sizeof(numBuf), "%d", i);
      prefix += numBuf;
      prefix += '.';

      // Create item context with scoped variables
      ThemeContext itemContext(&context);

      // Standard list item variables - include all properties for full flexibility
      std::string nameVal = context.getString(prefix + "Name");
      itemContext.setString("Item.Name", nameVal);
      itemContext.setString("Item.Title", context.getString(prefix + "Title"));
      itemContext.setString("Item.Value", context.getAnyAsString(prefix + "Value"));
      itemContext.setString("Item.Type", context.getString(prefix + "Type"));
      itemContext.setString("Item.ValueLabel", context.getString(prefix + "ValueLabel"));
      itemContext.setString("Item.BgColor", context.getString(prefix + "BgColor"));
      itemContext.setBool("Item.Selected", context.getBool(prefix + "Selected"));
      itemContext.setBool("Item.Value", context.getBool(prefix + "Value"));
      itemContext.setString("Item.Icon", context.getString(prefix + "Icon"));
      itemContext.setString("Item.Image", context.getString(prefix + "Image"));
      itemContext.setString("Item.Progress", context.getString(prefix + "Progress"));

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
      itemContext.setInt("Item.Index", i);
      itemContext.setInt("Item.Count", count);
      // ValueIndex may not exist for all item types, so check first
      if (context.hasKey(prefix + "ValueIndex")) {
        itemContext.setInt("Item.ValueIndex", context.getInt(prefix + "ValueIndex"));
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

      // Build prefix efficiently: "source.i."
      prefix.clear();
      prefix += source;
      prefix += '.';
      snprintf(numBuf, sizeof(numBuf), "%d", i);
      prefix += numBuf;
      prefix += '.';

      // Create item context with scoped variables
      ThemeContext itemContext(&context);

      // Standard list item variables - include all properties for full flexibility
      std::string nameVal = context.getString(prefix + "Name");
      itemContext.setString("Item.Name", nameVal);
      itemContext.setString("Item.Title", context.getString(prefix + "Title"));
      itemContext.setString("Item.Value", context.getAnyAsString(prefix + "Value"));
      itemContext.setString("Item.Type", context.getString(prefix + "Type"));
      itemContext.setString("Item.ValueLabel", context.getString(prefix + "ValueLabel"));
      itemContext.setString("Item.BgColor", context.getString(prefix + "BgColor"));
      itemContext.setBool("Item.Selected", context.getBool(prefix + "Selected"));
      itemContext.setBool("Item.Value", context.getBool(prefix + "Value"));
      itemContext.setString("Item.Icon", context.getString(prefix + "Icon"));
      itemContext.setString("Item.Image", context.getString(prefix + "Image"));
      itemContext.setString("Item.Progress", context.getString(prefix + "Progress"));
      itemContext.setInt("Item.Index", i);
      itemContext.setInt("Item.Count", count);
      // ValueIndex may not exist for all item types, so check first
      if (context.hasKey(prefix + "ValueIndex")) {
        itemContext.setInt("Item.ValueIndex", context.getInt(prefix + "ValueIndex"));
      }

      // Layout and draw the template for this item
      itemTemplate->layout(itemContext, absX, currentY, absW, itemH);
      itemTemplate->draw(renderer, itemContext);

      currentY += itemH + spacing;
    }
  }

  markClean();
}

}  // namespace ThemeEngine
