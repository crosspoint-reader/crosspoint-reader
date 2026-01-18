#pragma once

class GfxRenderer;

// Reload custom reader font - removes old font and loads new one
// Call this when font settings change to apply immediately without reboot
// Returns true if custom font was loaded successfully
bool reloadCustomReaderFont();

// Get reference to global renderer (for font operations from other modules)
GfxRenderer& getGlobalRenderer();
