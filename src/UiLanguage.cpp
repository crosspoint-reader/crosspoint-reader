#include "UiLanguage.h"

#include "CjkUiFonts.h"
#include "activities/RenderLock.h"

void UiLanguage::apply(const Language language) {
  RenderLock lock;
  I18N.setLanguage(language);
  CjkUiFonts::setLanguage(I18N.getLanguage());
}
