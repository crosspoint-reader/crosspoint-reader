#pragma once

#include <Epub/Page.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/UiAppHost.h"

// Content for the shared details page (fui::publicationPage layout).
struct DetailsSpec {
  std::string header;       // header band title
  std::string title;        // bold hero title
  std::string author;       // hero line under the title
  std::string source;       // hero line under the author (e.g. repository)
  std::string metadata;     // short facts (version, status, ...)
  std::string description;  // plain text, or HTML when descriptionHtml
  bool descriptionHtml = false;
  std::string actionLabel;  // primary button; confirming finishes with a non-cancelled result
};

// Title, metadata and the start of the description, with the primary action
// and More (full paged description). HTML descriptions render through the
// dictionary's EPUB-engine layout; when that fails they fall back to plain text.
class DetailsActivity final : public Activity, private UiAppHost {
 public:
  DetailsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, DetailsSpec spec);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void detailsScreen(UiScreen& screen, void* user);
  static void onPrimaryEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onMoreEvent(const freeink::ui::ActionEvent& event, void* user);
  static bool paintDescription(freeink::ui::DrawTarget& target, freeink::ui::Rect rect,
                               const freeink::ui::PublicationPageProps& props, void* user);
  void buildScreen(UiScreen& screen);
  bool needsLayout() const;
  void layoutDescription();
  void confirm();
  void cancel();
  void openMore();

  DetailsSpec spec;
  // Description area the layout reserved, recorded by paintDescription.
  freeink::ui::Rect descriptionRect{};
  freeink::ui::Rect laidOutRect{};
  // First styled page of an HTML description; empty until laid out.
  std::vector<std::unique_ptr<Page>> pages;
};
