#pragma once

class GfxRenderer;

namespace textsettings {

// Renders the shared live sample-text pane for TextSettingsActivity. Reads the
// live reader settings (font, size, spacing, margin, alignment, focus reading)
// from SETTINGS; the caller supplies only the pane geometry and the label's
// family/size names. Kept in its own translation unit so the activity stays a
// thin tab/nav shell.
void renderPreview(GfxRenderer& renderer, int previewPadding, int labelGap, int top, int height, const char* familyName,
                   const char* sizeName);

}  // namespace textsettings
