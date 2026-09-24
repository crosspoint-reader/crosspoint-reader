#pragma once

#include <I18n.h>
#include <LibraryText.h>

#include "CrossPointSettings.h"

// The Library's article lists, from the translation files. A book whose
// language is missing or unknown is sorted with the UI language's articles, or
// with English's when the UI language has none.
inline library::ArticlesByLanguage libraryArticleConfig() {
  constexpr uint8_t english = static_cast<uint8_t>(Language::EN);
  const uint8_t ui = SETTINGS.language < getLanguageCount() ? SETTINGS.language : english;
  const library::Articles fallback = LANGUAGE_ARTICLES[ui].empty() ? LANGUAGE_ARTICLES[english] : LANGUAGE_ARTICLES[ui];
  return {LANGUAGE_BCP47, LANGUAGE_ISO639_2, LANGUAGE_ARTICLES, getLanguageCount(), fallback};
}
