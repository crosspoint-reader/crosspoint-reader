#pragma once

class GfxRenderer;
class MappedInputManager;

// An on-screen Exit button for touch boards.
//
// The screens that hold a connection open (File Transfer and its hotspot, Calibre Wireless, USB
// Drive) leave by Back alone. A board without a Back key reaches that only through a small hint
// box at the bottom edge or a held home key, so on a touch board these screens also draw a button
// a finger can find. Nothing is drawn, and nothing is tappable, on a board without touch or with
// touch turned off in the settings.
//
// The button is recorded in TapTargets::screenActions(), which every activity transition clears.
namespace TouchExitButton {

// Draws the button across [x, x + width) with its top at y and records it for tapped(). Returns
// the y just below it, or y unchanged when no button is drawn.
int draw(GfxRenderer& renderer, const MappedInputManager& input, int x, int y, int width);

// Drops the recorded button, for a screen that stops offering it (USB Drive once a host mounts
// the card).
void forget();

// True when this tick's tap landed on the button the current screen drew. Reads the tap without
// consuming it.
bool tapped(const MappedInputManager& input);

}  // namespace TouchExitButton
