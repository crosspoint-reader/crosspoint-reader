#pragma once

// Forward declaration only: the hooks traffic in pointers, so this header
// stays free of the FreeInkUI include and can be compiled on the host.
namespace freeink {
namespace ui {
struct KeyboardLayout;
}  // namespace ui
}  // namespace freeink

// Optional hook for a downstream integration to contribute ONE extra keyboard
// layer -- an additional script, say -- without CrossPoint knowing which script
// it is, what it is called, or when it should be preferred.
//
// Both hooks are unset by default (nullptr), which reproduces CrossPoint's
// built-in keyboard exactly: the Mode key keeps its two-stop abc/?123 cycle and
// every call site collapses to a single null check. Nothing here is reachable
// unless a downstream build installs a provider at startup.
//
// Plain function pointers rather than a virtual interface, matching
// SettingsExtension.h: the no-provider case stays one null check, with no heap
// allocation and no vtable.

// Returns the layout for the extra layer, or nullptr to decline (so a provider
// may offer an unshifted layer only, or opt out for a given state). Called on
// every layout query, so it must be cheap and must return a layout that
// outlives the keyboard activity -- static tables, exactly like the URL layers
// KeyboardEntryActivity already defines for its own use.
//
// The returned layout owns its whole grid, including its bottom action row, so
// a provider is responsible for including the usual Mode/Delete/OK keys
// (fui::QWERTY_KEY_MODE, QWERTY_KEY_BACKSPACE, QWERTY_KEY_ENTER). Mode from
// inside the extra layer returns to the letter layer.
using KeyboardExtensionLayerProvider = const freeink::ui::KeyboardLayout* (*)(bool shifted);

// Returns true when the keyboard should OPEN on the extra layer instead of the
// letter layer. Optional and independent of the layer provider: a downstream
// that wants the layer reachable but not default simply leaves this unset.
// Consulted once per onEnter(), so a provider may key it off the UI language,
// a stored preference, or anything else it owns.
using KeyboardExtensionDefaultPredicate = bool (*)();

// Short label for the Mode key at the point in the cycle where the NEXT stop is
// the extra layer -- the layer's own name, e.g. a script name. CrossPoint never
// interprets the string, only draws it, so nothing here has to know what the
// layer is. Optional: when unset, the Mode key keeps whatever label its own
// layout table carries, which will read as though the third stop were not there.
using KeyboardExtensionLabelProvider = const char* (*)();

void setKeyboardExtensionLayerProvider(KeyboardExtensionLayerProvider provider);
KeyboardExtensionLayerProvider getKeyboardExtensionLayerProvider();

void setKeyboardExtensionDefaultPredicate(KeyboardExtensionDefaultPredicate predicate);
KeyboardExtensionDefaultPredicate getKeyboardExtensionDefaultPredicate();

void setKeyboardExtensionLabelProvider(KeyboardExtensionLabelProvider provider);
KeyboardExtensionLabelProvider getKeyboardExtensionLabelProvider();
