#include "UiLanguage.h"

#include "activities/RenderLock.h"

void UiLanguage::apply(const Language language) {
  RenderLock lock;
  I18N.setLanguage(language);
}
