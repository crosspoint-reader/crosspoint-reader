#include "PdfReaderActivity.h"

#include <Epub/converters/JpegToFramebufferConverter.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "PdfReaderChapterSelectionActivity.h"
#include "PdfReaderMenuActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
// Cap stream buffer for embedded heap; corrupt PDFs can advertise huge lengths.
constexpr size_t kMaxPdfImageStreamBytes = 2 * 1024 * 1024;
}

void PdfReaderActivity::jumpToPage(uint32_t page) {
  if (page < totalPages) {
    currentPage = page;
    requestUpdate();
  }
}

PdfReaderActivity::PdfReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Pdf> pdfIn)
    : Activity("PdfReader", renderer, mappedInput), pdf(std::move(pdfIn)) {}

void PdfReaderActivity::ensureLayout() {
  if (layoutReady) {
    return;
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  cachedFontId = SETTINGS.getReaderFontId();
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const uint8_t sm = SETTINGS.screenMargin;
  marginTop += sm;
  marginLeft += sm;
  marginRight += sm;
  marginBottom += std::max(sm, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));
  viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  layoutReady = true;
}

void PdfReaderActivity::onEnter() {
  Activity::onEnter();
  if (!pdf) {
    return;
  }
  layoutReady = false;
  ensureLayout();
  pageBuffer.clear();

  totalPages = pdf->pageCount();
  currentPage = 0;
  if (pdf->loadProgress(currentPage) && currentPage >= totalPages && totalPages > 0) {
    currentPage = totalPages - 1;
  }

  const auto path = std::string(pdf->filePath().c_str());
  const auto slash = path.find_last_of('/');
  const auto fileName = slash == std::string::npos ? path : path.substr(slash + 1);
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(path, fileName, "", "");

  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  lastSavedPage = UINT32_MAX;
  requestUpdate();
}

void PdfReaderActivity::onExit() {
  Activity::onExit();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  saveProgressNow();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  lastSavedPage = UINT32_MAX;
  pdf.reset();
  layoutReady = false;
}

void PdfReaderActivity::saveProgressNow() {
  if (!pdf || currentPage == lastSavedPage) {
    return;
  }
  if (pdf->saveProgress(currentPage)) {
    lastSavedPage = currentPage;
  }
}

void PdfReaderActivity::loop() {
  if (!pdf) {
    finish();
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    saveProgressNow();
    activityManager.goToFileBrowser(std::string(pdf->filePath().c_str()));
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    saveProgressNow();
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool hasOutline = !pdf->outline().empty();
    startActivityForResult(std::make_unique<PdfReaderMenuActivity>(renderer, mappedInput, hasOutline),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               return;
                             }
                             const auto& menu = std::get<MenuResult>(result.data);
                             if (menu.action == PdfReaderMenuActivity::ACTION_HOME) {
                               saveProgressNow();
                               onGoHome();
                             } else if (menu.action == PdfReaderMenuActivity::ACTION_OUTLINE) {
                                startActivityForResult(std::make_unique<PdfReaderChapterSelectionActivity>(
                                                          renderer, mappedInput, pdf->outline(), currentPage),
                                                      [this](const ActivityResult& res) {
                                                        if (!res.isCancelled) {
                                                          const uint32_t p = std::get<PageResult>(res.data).page;
                                                          jumpToPage(p);
                                                        }
                                                      });
                             }
                           });
    return;
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (prevTriggered && currentPage > 0) {
    currentPage--;
    requestUpdate();
  } else if (nextTriggered && totalPages > 0 && currentPage + 1 < totalPages) {
    currentPage++;
    requestUpdate();
  }
}

void PdfReaderActivity::renderStatusBar() const {
  if (!pdf || totalPages == 0) {
    return;
  }
  const float progress = static_cast<float>(currentPage + 1) * 100.0f / static_cast<float>(totalPages);
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    const auto path = std::string(pdf->filePath().c_str());
    const auto slash = path.find_last_of('/');
    title = slash == std::string::npos ? path : path.substr(slash + 1);
  }
  GUI.drawStatusBar(renderer, progress, static_cast<int>(currentPage + 1), static_cast<int>(totalPages), title);
}

void PdfReaderActivity::renderContents(const PdfPage& page) {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  int y = marginTop;
  const int bottomLimit = renderer.getScreenHeight() - marginBottom - lineHeight;

  auto toRendererStyle = [](uint8_t pdfStyle) {
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    if ((pdfStyle & PdfTextStyleBold) != 0) {
      style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
    }
    if ((pdfStyle & PdfTextStyleItalic) != 0) {
      style = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::ITALIC);
    }
    return style;
  };

  auto drawTextBlock = [&](const PdfTextBlock& block) {
    if (block.text.empty()) {
      return;
    }
    const bool isHeader = (block.style & PdfTextStyleHeader) != 0;
    EpdFontFamily::Style textStyle = toRendererStyle(block.style);
    if (isHeader) {
      textStyle = static_cast<EpdFontFamily::Style>(textStyle | EpdFontFamily::BOLD);
    }
    constexpr int kMaxLines = 400;
    const auto lines = renderer.wrappedText(cachedFontId, block.text.c_str(), viewportWidth, kMaxLines, textStyle);
    if (isHeader && y > marginTop) {
      y += std::max(2, lineHeight / 3);
    }
    for (const auto& line : lines) {
      if (y > bottomLimit) {
        return;
      }
      int x = marginLeft;
      if (isHeader) {
        x = marginLeft + (viewportWidth - renderer.getTextWidth(cachedFontId, line.c_str(), textStyle)) / 2;
      } else {
        switch (SETTINGS.paragraphAlignment) {
          case CrossPointSettings::CENTER_ALIGN:
            x = marginLeft + (viewportWidth - renderer.getTextWidth(cachedFontId, line.c_str(), textStyle)) / 2;
            break;
          case CrossPointSettings::RIGHT_ALIGN:
            x = marginLeft + viewportWidth - renderer.getTextWidth(cachedFontId, line.c_str(), textStyle);
            break;
          default:
            break;
        }
      }
      renderer.drawText(cachedFontId, x, y, line.c_str(), true, textStyle);
      y += lineHeight;
    }
    if (isHeader) {
      y += std::max(2, lineHeight / 4);
    }
  };

  auto drawImage = [&](const PdfImageDescriptor& img) {
    if (!pdf) {
      return;
    }
    const auto dir = pdf->cacheDirectory().view();
    if (dir.empty()) {
      renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_PDF_IMAGE_PLACEHOLDER));
      y += lineHeight;
      return;
    }
    const char* name = img.format == 0 ? "_tmpimg.jpg" : "_tmpimg.png";
    const std::string tmpPath = std::string(dir) + "/" + name;
    if (img.pdfStreamLength == 0 || img.pdfStreamLength > kMaxPdfImageStreamBytes) {
      renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_PDF_IMAGE_PLACEHOLDER));
      y += lineHeight;
      return;
    }
    auto* raw = static_cast<uint8_t*>(malloc(img.pdfStreamLength));
    if (!raw) {
      renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_PDF_IMAGE_PLACEHOLDER));
      y += lineHeight;
      return;
    }
    const size_t got = pdf->extractImageStream(img, raw, img.pdfStreamLength);
    if (got < 4) {
      free(raw);
      renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_PDF_IMAGE_PLACEHOLDER));
      y += lineHeight;
      return;
    }
    FsFile wf;
    if (!Storage.openFileForWrite("PDF", tmpPath, wf)) {
      free(raw);
      renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_PDF_IMAGE_PLACEHOLDER));
      y += lineHeight;
      return;
    }
    wf.write(raw, got);
    wf.close();
    free(raw);

    RenderConfig cfg;
    cfg.x = marginLeft;
    cfg.y = y;
    cfg.maxWidth = viewportWidth;
    cfg.maxHeight = bottomLimit - y;
    if (cfg.maxHeight < 16) {
      Storage.remove(tmpPath.c_str());
      return;
    }
    cfg.useGrayscale = true;
    cfg.useDithering = true;

    int advanceY = lineHeight;
    if (img.width > 0 && img.height > 0) {
      advanceY = static_cast<int>(img.height) * viewportWidth / static_cast<int>(img.width);
      if (advanceY > cfg.maxHeight) advanceY = cfg.maxHeight;
      if (advanceY < lineHeight) advanceY = lineHeight;
    }

    bool ok = false;
    if (img.format == 0) {
      JpegToFramebufferConverter jpg;
      ok = jpg.decodeToFramebuffer(tmpPath, renderer, cfg);
    } else {
      PngToFramebufferConverter png;
      ok = png.decodeToFramebuffer(tmpPath, renderer, cfg);
    }
    Storage.remove(tmpPath.c_str());
    if (!ok) {
      renderer.drawText(UI_10_FONT_ID, marginLeft, y, tr(STR_PDF_IMAGE_PLACEHOLDER));
      y += lineHeight;
      return;
    }
    y += advanceY;
  };

  // Single layout pass (no two-pass prewarm scan). PDF pages can hold far more text than reflowed EPUB;
  // the prewarm path concatenates all glyphs into one string and runs layout twice, which exhausted heap
  // (operator new -> terminate) on ESP32 with dense PDFs + anti-aliasing.
  auto drawBody = [&]() {
    y = marginTop;
    if (!page.drawOrder.empty()) {
      for (const auto& step : page.drawOrder) {
        if (step.isImage) {
          if (step.index < page.images.size()) {
            drawImage(page.images[step.index]);
          }
        } else if (step.index < page.textBlocks.size()) {
          drawTextBlock(page.textBlocks[step.index]);
        }
      }
    } else {
      for (const auto& tb : page.textBlocks) {
        drawTextBlock(tb);
      }
      for (const auto& im : page.images) {
        drawImage(im);
      }
    }
  };

  drawBody();

  renderStatusBar();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&, this]() {
      y = marginTop;
      drawBody();
      renderStatusBar();
    });
  }
}

void PdfReaderActivity::render(RenderLock&&) {
  if (!pdf) {
    return;
  }
  ensureLayout();

  renderer.clearScreen();

  if (!pdf->getPage(currentPage, pageBuffer)) {
    renderer.drawCenteredText(UI_12_FONT_ID, 200, tr(STR_PDF_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderContents(pageBuffer);
  saveProgressNow();
}
