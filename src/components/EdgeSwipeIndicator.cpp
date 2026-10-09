#include "EdgeSwipeIndicator.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <Logging.h>
#include <Memory.h>

#include "UITheme.h"
#include "icons/edgeSwipeIcons.h"

void EdgeSwipeIndicator::begin() {
  underlay = makeUniqueNoThrow<uint8_t[]>(edge_swipe::Config::SNAPSHOT_BYTES);
  if (!underlay) LOG_ERR("EDGE", "OOM: indicator snapshot");
}

void EdgeSwipeIndicator::pageChanged() {
  visible = false;
  pending = false;
  idleAt = 0;
}

uint32_t EdgeSwipeIndicator::serviceDelay(uint32_t now) const {
  if (pending) {
    const auto elapsed = now - refreshedAt;
    return elapsed < edge_swipe::Config::MIN_REFRESH_MS ? edge_swipe::Config::MIN_REFRESH_MS - elapsed : 0;
  }
  if (idleAt) {
    const auto elapsed = now - idleAt;
    return elapsed < edge_swipe::Config::CLEANUP_MS ? edge_swipe::Config::CLEANUP_MS - elapsed : 0;
  }
  return NO_SERVICE;
}

void EdgeSwipeIndicator::clear(const GfxRenderer& renderer, bool cleanup) {
  if (!renderer.prepareBwOverlay()) {
    pageChanged();
    return;
  }
  // Each draw restores the write framebuffer immediately; it already holds
  // clean page pixels. Keep the panel baseline until the cleanup refresh.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  ++refreshes;
  if (refreshes > edge_swipe::Config::MAX_REFRESHES) exhausted = true;
  visible = false;
  refreshedAt = millis();
  hasRefreshed = true;
  idleAt = 0;
  if (cleanup || exhausted)
    LOG_DBG("EDGE", "cleanup contact=%lu", static_cast<unsigned long>(contact));
  else
    LOG_DBG("EDGE", "contact=%lu hint=hidden refresh=%u", static_cast<unsigned long>(contact), refreshes);
}

void EdgeSwipeIndicator::render(const GfxRenderer& renderer, const Input& input, uint32_t now) {
  using namespace edge_swipe;
  if (!underlay) return;
  if (visible && displayGeneration != renderer.getDisplayGeneration()) pageChanged();
  pending = false;
  if (visible && orientation != static_cast<uint8_t>(renderer.getOrientation())) {
    // Orientation changes are accompanied by a page redraw. Never restore old
    // pixels into the new orientation.
    pageChanged();
    return;
  }
  if (hasRefreshed && now - refreshedAt < Config::MIN_REFRESH_MS) {
    pending = true;
    return;
  }
  if (contact != input.state.contact) {
    if (visible) {
      clear(renderer, true);
      return;
    }
    contact = input.state.contact;
    refreshes = 0;
    exhausted = false;
    committed = false;
    idleAt = 0;
  }
  committed = committed || input.state.committed;
  const bool show = input.enabled && input.state.tracking && input.state.stage != Stage::Idle && !exhausted;
  if (!show) {
    if (!visible) return;
    // Navigation paints over a committed indicator. Only a commit that did
    // not navigate reaches this quiet cleanup after 200 ms.
    if (!input.state.tracking && committed) {
      if (!idleAt) idleAt = now;
      if (now - idleAt < Config::CLEANUP_MS) return;
    }
    clear(renderer, !input.state.tracking);
    return;
  }
  idleAt = 0;
  if (visible) return;
  if (refreshes >= Config::MAX_REFRESHES) {
    exhausted = true;
    return;
  }
  if (input.orientation != static_cast<uint8_t>(renderer.getOrientation())) return;
  if (!renderer.prepareBwOverlay()) return;
  const int width = Config::pixels(Config::TAB_WIDTH_MM, input.dpi);
  const int depth = Config::pixels(Config::TAB_DEPTH_MM, input.dpi);
  const int margin = Config::pixels(Config::MARGIN_MM, input.dpi);
  const int sw = renderer.getScreenWidth(), sh = renderer.getScreenHeight();
  const bool horizontal = input.state.edge == Edge::Top || input.state.edge == Edge::Bottom;
  int top = 0, right = 0, bottom = 0, left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int low = (horizontal ? left : top) + width / 2 + margin;
  const int high = (horizontal ? sw - right : sh - bottom) - (width - width / 2) - margin;
  if (low > high) return;
  const int position = std::clamp(input.state.position, low, high);
  Rect full;
  const freeink::Icon* icon = nullptr;
  switch (input.state.edge) {
    case Edge::Top:
      full = Rect(position - width / 2, top, width, depth);
      icon = Frontlight.present() ? &icon_edge_light_24 : &icon_edge_menu_24;
      break;
    case Edge::Bottom:
      full = Rect(position - width / 2, sh - bottom - depth, width, depth);
      icon = &icon_edge_home_24;
      break;
    case Edge::Left:
      full = Rect(left, position - width / 2, depth, width);
      icon = &icon_edge_back_24;
      break;
    case Edge::None:
      return;
  }
  region = Rect(std::max(0, full.x - margin), std::max(0, full.y - margin),
                std::min(sw, full.x + full.width + margin) - std::max(0, full.x - margin),
                std::min(sh, full.y + full.height + margin) - std::max(0, full.y - margin));
  if (!renderer.copyRegionToBuffer(region.x, region.y, region.width, region.height, underlay.get(),
                                   Config::SNAPSHOT_BYTES))
    return;
  GUI.drawEdgeSwipeTab(renderer, full, input.state.edge, *icon);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  renderer.copyBufferToRegion(region.x, region.y, region.width, region.height, underlay.get(), Config::SNAPSHOT_BYTES);
  visible = true;
  orientation = input.orientation;
  displayGeneration = renderer.getDisplayGeneration();
  refreshedAt = millis();
  hasRefreshed = true;
  ++refreshes;
  LOG_DBG("EDGE", "contact=%lu hint=shown refresh=%u rect=%d,%d %dx%d", static_cast<unsigned long>(contact), refreshes,
          region.x, region.y, region.width, region.height);
}
