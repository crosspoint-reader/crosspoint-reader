#pragma once
#include <initializer_list>

#include "UiAppHost.h"

class GfxRenderer;
class MappedInputManager;

// Shared FreeInkUI screen builders for the network catalog activities (the
// OPDS book browser and the SD-plugin catalogs). Both are UiAppHost state
// machines whose states share the same themed chrome: a header band over the
// button hints, centered status/message blocks, a download-progress screen,
// and a ListNav-synced list body.

// Reserve the firmware's button-hint band, align with GUI.drawHeader's top
// offset, and draw a fui header. A trailing icon (e.g. search) becomes a
// header action button optically aligned with the title glyphs.
void catalogScreenHeader(UiAppHost::UiScreen& screen, const GfxRenderer& renderer, const char* title,
                         freeink::ui::BitmapRef trailingIcon = {},
                         freeink::ui::ActionId trailingAction = freeink::ui::NO_ACTION);

// One line of a centered message block; bold marks a heading line.
struct CatalogLine {
  const char* text;
  bool bold = false;
};

// Vertically centered stack of short lines in the remaining content band
// (error screens, completion notices, sign-in hints).
void catalogCenteredBlock(UiAppHost::UiScreen& screen, std::initializer_list<CatalogLine> lines);

// Centered download screen: heading, item title, download progress and, when
// cancelAction is a real action, a Cancel button. A progress bar is shown when
// the total is known (total > 0), otherwise a running byte count.
void catalogDownloadScreen(UiAppHost::UiScreen& screen, const char* status, size_t progress, size_t total,
                           freeink::ui::ActionId cancelAction = freeink::ui::NO_ACTION);

// List body with the shared viewport protocol: on non-touch hardware keep the
// denser per-theme row height (see UiListActivity::syncListViewport), sync
// `nav` to the screen band, and emit the list.
void catalogListBody(UiAppHost::UiScreen& screen, const MappedInputManager& input, freeink::ui::ListNav& nav,
                     freeink::ui::ListProps& props, int count, bool hasSubtitle);
