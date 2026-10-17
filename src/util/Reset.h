#pragma once

// Erases CrossPoint's SD data. The caller must restart before any store can save again.
bool resetCrossPointData();

// Removes saved preferences so boot restores defaults. The caller must restart before saving settings.
bool resetCrossPointSettings();
