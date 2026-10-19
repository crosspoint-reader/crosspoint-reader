#pragma once

#include <I18n.h>

/// The one way to switch the UI language at run time: the language picker and the serial test API
/// (CMD:SET:language=) both come through here, so a language set either way is the same switch.
namespace UiLanguage {

/// Make `language` the UI language and save it. Takes the render lock: a render pass reads the
/// strings this swaps, and building a language pack writes flash. The screen on display keeps its old
/// strings until it next draws.
void apply(Language language);

}  // namespace UiLanguage
