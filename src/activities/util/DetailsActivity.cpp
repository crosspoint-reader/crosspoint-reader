#include "DetailsActivity.h"

#include <Epub/converters/ImageDecoderFactory.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "util/DictHtmlPages.h"
#include "util/HtmlToPlainText.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_PRIMARY = 1;
constexpr fui::ActionId ACTION_MORE = 2;
// htmlToPlainText marks a heading line with this byte. The plain-text target
// used here has no bold style, so the marker is dropped again after conversion.
constexpr char DETAILS_HEADING_MARKER = '\x01';
}  // namespace

DetailsActivity::DetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, DetailsSpec spec)
    : Activity("Details", renderer, mappedInput), UiAppHost(renderer), spec(std::move(spec)) {}

void DetailsActivity::onEnter() {
  Activity::onEnter();
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  resetUi();
  app.on(ACTION_PRIMARY, &DetailsActivity::onPrimaryEvent, this);
  app.on(ACTION_MORE, &DetailsActivity::onMoreEvent, this);
  app.setScreen(&DetailsActivity::detailsScreen, this);
  requestUpdate();
}

bool DetailsActivity::needsLayout() const {
  return spec.descriptionHtml && descriptionRect.width > 0 && descriptionRect.height > 0 &&
         (pages.empty() || laidOutRect.width != descriptionRect.width || laidOutRect.height != descriptionRect.height);
}

// Runs on the loop task: the EPUB parser is too deep for the render task's stack.
void DetailsActivity::layoutDescription() {
  pages.clear();
  laidOutRect = descriptionRect;
  if (!buildDictionaryHtmlPages(renderer, spec.description, static_cast<uint16_t>(descriptionRect.width),
                                static_cast<uint16_t>(descriptionRect.height), pages) ||
      pages.empty()) {
    LOG_INF("DETAILS", "Styled description unavailable; using plain text");
    pages.clear();
    spec.description = htmlToPlainText(spec.description, DETAILS_HEADING_MARKER);
    // The UI text target wraps only at spaces; a newline would draw as a box.
    std::replace(spec.description.begin(), spec.description.end(), '\n', ' ');
    // The same target cannot style a label line, so the heading marker goes too.
    std::replace(spec.description.begin(), spec.description.end(), DETAILS_HEADING_MARKER, ' ');
    spec.descriptionHtml = false;
    return;
  }
  pages.resize(1);  // the summary shows one page; More lays out the rest
}

void DetailsActivity::loop() {
  if (needsLayout()) {
    {
      RenderLock lock;
      layoutDescription();
    }
    requestUpdate();
    return;
  }

  const auto route = routeTouch(mappedInput);
  if (route.routed && app.invalidated()) requestUpdate();
  if (route) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirm();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
             mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openMore();
  }
}

void DetailsActivity::confirm() {
  setResult(ActivityResult{});
  finish();
}

void DetailsActivity::cancel() { finish(); }

void DetailsActivity::openMore() {
  if (spec.description.empty()) return;
  // The viewer consumes its copy; this page keeps the original for repaints.
  auto viewer = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, spec.title, spec.description,
                                                                spec.descriptionHtml);
  if (!viewer) {
    LOG_ERR("DETAILS", "OOM: description viewer");
    return;
  }
  startActivityForResult(std::move(viewer), nullptr);
}

void DetailsActivity::onPrimaryEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<DetailsActivity*>(user);
  self->app.clearTapFlash();
  self->confirm();
}

void DetailsActivity::onMoreEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<DetailsActivity*>(user);
  self->app.clearTapFlash();
  self->openMore();
}

// Records the cover rect only; render() decodes into it after the app paints,
// keeping the image decoder's stack off the component call chain.
bool DetailsActivity::paintCover(fui::DrawTarget&, const fui::Rect rect, const fui::PublicationHeaderProps&,
                                 void* user) {
  auto* self = static_cast<DetailsActivity*>(user);
  if (self->spec.coverPath.empty()) {
    GUI.drawCoverPlaceholder(self->renderer, Rect{rect.x, rect.y, rect.width, rect.height});
    self->coverRect = fui::Rect{};
  } else {
    self->coverRect = rect;
  }
  return true;
}

// Claims the area for HTML and records it; render() draws the page after the
// app paints, keeping the page renderer off the component call chain.
bool DetailsActivity::paintDescription(fui::DrawTarget&, const fui::Rect rect, const fui::PublicationPageProps&,
                                       void* user) {
  auto* self = static_cast<DetailsActivity*>(user);
  if (!self->spec.descriptionHtml) return false;
  self->descriptionRect = rect;
  return true;
}

void DetailsActivity::detailsScreen(UiScreen& screen, void* user) {
  static_cast<DetailsActivity*>(user)->buildScreen(screen);
}

void DetailsActivity::buildScreen(UiScreen& screen) {
  catalogScreenHeader(screen, renderer, spec.header.c_str());
  const auto& theme = screen.theme();
  const auto asPtr = [](const std::string& s) { return s.empty() ? nullptr : s.c_str(); };
  const int16_t line = uiTarget.lineHeight(theme.bodyText.font);

  fui::PublicationPageProps props;
  props.book.title = asPtr(spec.title);
  props.book.author = asPtr(spec.author);
  props.book.series = asPtr(spec.source);  // the series slot is just a hero line
  props.book.titleText = theme.bodyText;
  props.book.detailText = theme.bodyText;
  props.book.seriesLines = 1;
  if (spec.showCover) {
    // Component default hero height, sized for the cover.
    props.book.coverPainter = &DetailsActivity::paintCover;
    props.book.coverPainterUserData = this;
  } else {
    props.book.coverSize = fui::Size{0, 0};  // text-only hero
    props.book.titleLines = 2;
    props.book.authorLines = 1;
    // Title (2 lines) and author each end with titleGap; the source line with textGap.
    const bool hasSource = !spec.source.empty();
    props.heroHeight = static_cast<int16_t>((hasSource ? 4 : 3) * line + 2 * props.book.titleGap +
                                            (hasSource ? props.book.textGap : 0));
  }
  props.availability.status = asPtr(spec.status);
  props.availability.copies = asPtr(spec.copies);
  props.availability.holds = asPtr(spec.holds);
  props.availability.headingText = theme.bodyText;
  props.availability.detailText = theme.bodyText;
  props.metadata = asPtr(spec.metadata);
  props.metadataLines = 3;
  props.descriptionHeading = asPtr(spec.descriptionHeading);
  props.description = asPtr(spec.description);
  props.descriptionPainter = &DetailsActivity::paintDescription;
  props.descriptionPainterUserData = this;
  props.headingText = theme.bodyText;
  props.bodyText = theme.bodyText;

  // Same outlined themed button as the OPDS publication page.
  const auto styleButton = [&theme](fui::ButtonProps& button) {
    button.styles = theme.button;
    button.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    button.styles.normal.borderWidth = 1;
    button.text = theme.bodyText;
    button.radius = theme.controlRadius;
    button.minTouchSize = theme.minTouchSize;
  };
  props.primary.label = asPtr(spec.actionLabel);
  props.primary.action = ACTION_PRIMARY;
  styleButton(props.primary);
  if (!spec.description.empty()) {
    props.more.label = tr(STR_DETAILS);
    props.more.action = ACTION_MORE;
    styleButton(props.more);
  }
  props.actionHeight = theme.rowHeight;

  fui::publicationPage(screen.frame(), screen.body(), props);
}

void DetailsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderUi();
  // The first paint only discovers the description area; loop() lays the
  // page out and repaints, so skip showing a frame with an empty body.
  if (needsLayout()) return;

  if (coverRect.width > 0 && coverRect.height > 0) {
    if (ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(spec.coverPath)) {
      RenderConfig cfg{coverRect.x, coverRect.y, coverRect.width, coverRect.height};
      decoder->decodeToFramebuffer(spec.coverPath, renderer, cfg);
    }
  }

  if (!pages.empty()) {
    const int fontId = SETTINGS.getReaderFontId();
    auto* fcm = renderer.getFontCacheManager();
    auto scope = fcm->createPrewarmScope();
    pages[0]->render(renderer, fontId, descriptionRect.x, descriptionRect.y);  // scan pass
    scope.endScanAndPrewarm();
    pages[0]->render(renderer, fontId, descriptionRect.x, descriptionRect.y);
  }

  const char* moreLabel = spec.description.empty() ? "" : tr(STR_DETAILS);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), spec.actionLabel.c_str(), "", moreLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
