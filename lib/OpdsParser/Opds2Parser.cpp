#include "Opds2Parser.h"

#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace OpdsLimits;

Opds2Parser::Opds2Parser()
    : parser(JsonCallbacks{this, &sOnKey, &sOnString, &sOnNumber, &sOnBool, nullptr, &sOnObjectStart, &sOnObjectEnd,
                           &sOnArrayStart, &sOnArrayEnd}) {
  entries.reserve(ENTRY_STORAGE_CAPACITY);
}

size_t Opds2Parser::write(const uint8_t c) { return write(&c, 1); }

size_t Opds2Parser::write(const uint8_t* data, const size_t length) {
  if (errorOccured) return length;
  parser.feed(reinterpret_cast<const char*>(data), length);
  if (parser.hasError()) {
    errorOccured = true;
    LOG_DBG("OPDS2", "JSON parse error");
  }
  return length;
}

void Opds2Parser::flush() {
  // End of input: a body that ends before the root object closes is a
  // truncated feed, not a shorter one — reject it rather than presenting the
  // partial catalog as complete.
  if (!sawRoot || depth != 0) {
    LOG_DBG("OPDS2", "Incomplete JSON document");
    errorOccured = true;
    return;
  }
  // Servers often expose the same categories twice: a bare top-level
  // `navigation` list plus `groups` that showcase each category with preview
  // publications. Drop a top-level navigation entry when a group covers it
  // (its title matches a group heading, or its href matches a group's "see
  // all" self link). Unrelated navigation stays, but moves below the groups:
  // the richer preview sections lead the screen.
  if (!sawGroups || topNavIndices.empty()) return;
  std::vector<OpdsEntry> keptNav;
  keptNav.reserve(topNavIndices.size());
  for (const uint16_t index : topNavIndices) {
    const OpdsEntry& nav = entries[index];
    bool duplicate = false;
    for (const auto& entry : entries) {
      if ((!entry.heading.empty() && entry.heading == nav.title) ||
          (entry.id == OPDS_SEE_ALL_ID && entry.href == nav.href)) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) keptNav.push_back(entries[index]);
  }
  for (auto it = topNavIndices.rbegin(); it != topNavIndices.rend(); ++it) {
    entries.erase(entries.begin() + *it);
  }
  for (auto& nav : keptNav) {
    entries.push_back(std::move(nav));
  }
  topNavIndices.clear();
}

bool Opds2Parser::error() const { return errorOccured || parser.hasError(); }

void Opds2Parser::assignBounded(std::string& target, const char* value, const size_t len, const size_t maxLen) {
  target.assign(value, len < maxLen ? len : maxLen);
}

void Opds2Parser::resetLink() {
  link.href.clear();
  link.title.clear();
  link.relSearch = link.relNext = link.relPrev = link.relFirst = link.relLast = link.relSelf = false;
  link.acqRank = -1;
  link.typeEpub = false;
  link.templated = false;
  link.numberOfItems = -1;
}

void Opds2Parser::applyRel(const char* rel) {
  if (strcmp(rel, "search") == 0) link.relSearch = true;
  if (strcmp(rel, "next") == 0) link.relNext = true;
  if (strcmp(rel, "previous") == 0 || strcmp(rel, "prev") == 0) link.relPrev = true;
  if (strcmp(rel, "first") == 0) link.relFirst = true;
  if (strcmp(rel, "last") == 0) link.relLast = true;
  if (strcmp(rel, "self") == 0) link.relSelf = true;
  const int rank = opdsAcquisitionRank(rel);
  if (rank > link.acqRank) link.acqRank = rank;
}

// ---- scope machine ----

Opds2Parser::Scope Opds2Parser::scopeForChild(const Scope parent, const bool isObject) const {
  switch (parent) {
    case Scope::FEED:
      if (isObject && strcmp(pendingKey, "metadata") == 0) return Scope::FEED_META;
      if (!isObject) {
        if (strcmp(pendingKey, "links") == 0) return Scope::FEED_LINKS;
        if (strcmp(pendingKey, "navigation") == 0) return Scope::NAV;
        if (strcmp(pendingKey, "publications") == 0) return Scope::PUBS;
        if (strcmp(pendingKey, "groups") == 0) return Scope::GROUPS;
        if (strcmp(pendingKey, "facets") == 0) return Scope::FACETS;
      }
      return Scope::SKIP;
    case Scope::FEED_META:
      if (isObject && strcmp(pendingKey, "title") == 0) return Scope::FEED_TITLE;
      return Scope::SKIP;
    case Scope::GROUP:
      if (isObject && strcmp(pendingKey, "metadata") == 0) return Scope::GROUP_META;
      if (!isObject) {
        if (strcmp(pendingKey, "links") == 0) return Scope::GROUP_LINKS;
        if (strcmp(pendingKey, "navigation") == 0) return Scope::NAV;
        if (strcmp(pendingKey, "publications") == 0) return Scope::PUBS;
      }
      return Scope::SKIP;
    case Scope::FEED_LINKS:
      return isObject ? Scope::FEED_LINK : Scope::SKIP;
    case Scope::GROUP_LINKS:
      return isObject ? Scope::GROUP_LINK : Scope::SKIP;
    case Scope::FEED_LINK:
    case Scope::PUB_LINK:
    case Scope::GROUP_LINK:
      if (!isObject && strcmp(pendingKey, "rel") == 0) return Scope::LINK_REL;
      return Scope::SKIP;  // properties, alternate, children
    case Scope::FACET_LINK:
      if (!isObject && strcmp(pendingKey, "rel") == 0) return Scope::LINK_REL;
      if (isObject && strcmp(pendingKey, "properties") == 0) return Scope::FACET_PROPS;
      return Scope::SKIP;
    case Scope::NAV:
      return isObject ? Scope::NAV_LINK : Scope::SKIP;
    case Scope::PUBS:
      return isObject ? Scope::PUB : Scope::SKIP;
    case Scope::PUB:
      if (isObject && strcmp(pendingKey, "metadata") == 0) return Scope::PUB_META;
      if (!isObject && strcmp(pendingKey, "links") == 0) return Scope::PUB_LINKS;
      return Scope::SKIP;  // images, reading order, resources
    case Scope::PUB_META:
      if (strcmp(pendingKey, "author") == 0) return isObject ? Scope::AUTHOR : Scope::AUTHOR_ARR;
      if (isObject && strcmp(pendingKey, "title") == 0) return Scope::PUB_TITLE;
      return Scope::SKIP;  // belongsTo, subject, other contributors
    case Scope::AUTHOR_ARR:
      return isObject ? Scope::AUTHOR : Scope::SKIP;
    case Scope::PUB_LINKS:
      return isObject ? Scope::PUB_LINK : Scope::SKIP;
    case Scope::GROUPS:
      return isObject ? Scope::GROUP : Scope::SKIP;
    case Scope::FACETS:
      return isObject ? Scope::FACET : Scope::SKIP;
    case Scope::FACET:
      if (isObject && strcmp(pendingKey, "metadata") == 0) return Scope::FACET_META;
      if (!isObject && strcmp(pendingKey, "links") == 0) return Scope::FACET_LINKS;
      return Scope::SKIP;
    case Scope::FACET_LINKS:
      return isObject ? Scope::FACET_LINK : Scope::SKIP;
    default:
      return Scope::SKIP;
  }
}

void Opds2Parser::beginGroup() {
  inGroup = true;
  sawGroups = true;
  groupTitle.clear();
  groupSelfHref.clear();
  groupStartIndex = entries.size();
}

void Opds2Parser::endGroup() {
  // Field order in the group object is not guaranteed, so the heading and the
  // "see all" entry are applied only once the whole group has been read.
  if (!groupSelfHref.empty() && entries.size() < MAX_ENTRIES) {
    OpdsEntry seeAll;
    seeAll.type = OpdsEntryType::NAVIGATION;
    seeAll.id = OPDS_SEE_ALL_ID;
    seeAll.href = std::move(groupSelfHref);
    entries.push_back(std::move(seeAll));
  }
  if (!groupTitle.empty() && entries.size() > groupStartIndex) {
    entries[groupStartIndex].heading = std::move(groupTitle);
  }
  inGroup = false;
  groupTitle.clear();
  groupSelfHref.clear();
}

void Opds2Parser::onContainerStart(const bool isObject) {
  if (depth >= MAX_DEPTH) {
    errorOccured = true;
    return;
  }
  Scope next;
  if (!sawRoot) {
    sawRoot = true;
    next = isObject ? Scope::FEED : Scope::SKIP;
  } else {
    next = scopeForChild(current(), isObject);
  }
  switch (next) {
    case Scope::FEED_LINK:
    case Scope::NAV_LINK:
    case Scope::PUB_LINK:
    case Scope::GROUP_LINK:
    case Scope::FACET_LINK:
      resetLink();
      break;
    case Scope::PUB:
      currentEntry = OpdsEntry{};
      pubAcqRank = -1;
      pubHasPlainEpub = false;
      break;
    case Scope::GROUP:
      beginGroup();
      break;
    case Scope::FACET:
      facetTitle.clear();
      facetStartIndex = facetEntries.size();
      break;
    default:
      break;
  }
  stack[depth++] = next;
  pendingKey[0] = '\0';
}

void Opds2Parser::onContainerEnd() {
  if (depth == 0) return;
  const Scope closed = stack[--depth];
  pendingKey[0] = '\0';
  switch (closed) {
    case Scope::FEED_LINK:
      commitFeedLink();
      break;
    case Scope::NAV_LINK:
      commitNavLink();
      break;
    case Scope::PUB_LINK:
      commitPubLink();
      break;
    case Scope::GROUP_LINK:
      commitGroupLink();
      break;
    case Scope::FACET_LINK:
      commitFacetLink();
      break;
    case Scope::PUB:
      commitPublication();
      break;
    case Scope::GROUP:
      endGroup();
      break;
    case Scope::FACET:
      if (!facetTitle.empty() && facetEntries.size() > facetStartIndex) {
        facetEntries[facetStartIndex].heading = std::move(facetTitle);
      }
      facetTitle.clear();
      break;
    default:
      break;
  }
}

void Opds2Parser::onStringValue(const char* value, const size_t len) {
  switch (current()) {
    case Scope::FEED_LINK:
    case Scope::NAV_LINK:
    case Scope::PUB_LINK:
    case Scope::GROUP_LINK:
    case Scope::FACET_LINK:
      if (strcmp(pendingKey, "href") == 0) {
        assignBounded(link.href, value, len, MAX_HREF_CHARS);
      } else if (strcmp(pendingKey, "title") == 0) {
        assignBounded(link.title, value, len, MAX_TITLE_CHARS);
      } else if (strcmp(pendingKey, "rel") == 0) {
        applyRel(value);
      } else if (strcmp(pendingKey, "type") == 0) {
        if (strcmp(value, "application/epub+zip") == 0) link.typeEpub = true;
      }
      break;
    case Scope::LINK_REL:
      applyRel(value);
      break;
    case Scope::FEED_META:
      if (strcmp(pendingKey, "title") == 0) assignBounded(feedTitle, value, len, MAX_TITLE_CHARS);
      break;
    case Scope::FEED_TITLE:
      // Localized feed title object: take the first translation.
      if (feedTitle.empty()) assignBounded(feedTitle, value, len, MAX_TITLE_CHARS);
      break;
    case Scope::GROUP_META:
      if (strcmp(pendingKey, "title") == 0) assignBounded(groupTitle, value, len, MAX_TITLE_CHARS);
      break;
    case Scope::FACET_META:
      if (strcmp(pendingKey, "title") == 0) assignBounded(facetTitle, value, len, MAX_TITLE_CHARS);
      break;
    case Scope::PUB_META:
      if (strcmp(pendingKey, "title") == 0) {
        assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
      } else if (strcmp(pendingKey, "author") == 0) {
        if (currentEntry.author.empty()) assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      } else if (strcmp(pendingKey, "identifier") == 0) {
        assignBounded(currentEntry.id, value, len, MAX_ID_CHARS);
      }
      break;
    case Scope::PUB_TITLE:
      // Localized title object: take the first translation.
      if (currentEntry.title.empty()) assignBounded(currentEntry.title, value, len, MAX_TITLE_CHARS);
      break;
    case Scope::AUTHOR:
      if (strcmp(pendingKey, "name") == 0 && currentEntry.author.empty()) {
        assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      }
      break;
    case Scope::AUTHOR_ARR:
      // Array of contributor name strings: take the first.
      if (currentEntry.author.empty()) assignBounded(currentEntry.author, value, len, MAX_AUTHOR_CHARS);
      break;
    default:
      break;
  }
}

void Opds2Parser::onNumberValue(const char* value) {
  switch (current()) {
    case Scope::FEED_META:
      if (strcmp(pendingKey, "numberOfItems") == 0) {
        numberOfItems = static_cast<uint32_t>(strtoul(value, nullptr, 10));
      } else if (strcmp(pendingKey, "itemsPerPage") == 0) {
        itemsPerPage = static_cast<uint32_t>(strtoul(value, nullptr, 10));
      } else if (strcmp(pendingKey, "currentPage") == 0) {
        currentPage = static_cast<uint32_t>(strtoul(value, nullptr, 10));
      }
      break;
    case Scope::FACET_PROPS:
      if (strcmp(pendingKey, "numberOfItems") == 0) {
        link.numberOfItems = static_cast<int32_t>(strtol(value, nullptr, 10));
      }
      break;
    default:
      break;
  }
}

// ---- commits ----

void Opds2Parser::commitFeedLink() {
  if (link.href.empty()) return;
  if (link.relSearch && (link.templated || link.href.find('{') != std::string::npos)) {
    if (searchTemplate.empty() && link.href.size() <= MAX_SEARCH_TEMPLATE_CHARS) searchTemplate = link.href;
    return;
  }
  if (link.relNext && nextPageUrl.empty()) nextPageUrl = link.href;
  if (link.relPrev && prevPageUrl.empty()) prevPageUrl = link.href;
  if (link.relFirst && firstPageUrl.empty()) firstPageUrl = link.href;
  if (link.relLast && lastPageUrl.empty()) lastPageUrl = link.href;
}

void Opds2Parser::commitNavLink() {
  if (link.href.empty() || link.title.empty()) return;
  if (entries.size() >= MAX_ENTRIES) {
    feedTruncated = true;
    return;
  }
  // The stack still holds ... FEED/GROUP > NAV at this point (NAV_LINK was
  // just popped); only top-level navigation participates in group dedup.
  if (depth >= 2 && stack[depth - 2] == Scope::FEED) {
    topNavIndices.push_back(static_cast<uint16_t>(entries.size()));
  }
  OpdsEntry entry;
  entry.type = OpdsEntryType::NAVIGATION;
  entry.title = std::move(link.title);
  entry.href = std::move(link.href);
  entries.push_back(std::move(entry));
}

void Opds2Parser::commitGroupLink() {
  if (link.relSelf && groupSelfHref.empty()) groupSelfHref = std::move(link.href);
}

void Opds2Parser::commitFacetLink() {
  if (link.href.empty() || link.title.empty()) return;
  if (facetEntries.size() >= MAX_FACET_ENTRIES) return;
  OpdsEntry entry;
  entry.type = OpdsEntryType::NAVIGATION;
  entry.title = std::move(link.title);
  entry.href = std::move(link.href);
  if (link.numberOfItems >= 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld", static_cast<long>(link.numberOfItems));
    entry.detail = buf;
  }
  facetEntries.push_back(std::move(entry));
}

void Opds2Parser::commitPubLink() {
  if (link.href.empty() || !link.typeEpub || link.acqRank < 0) return;
  // Prefer higher-ranked acquisitions (open-access over borrow); at equal
  // rank prefer a plain EPUB path over derived formats (kepub etc.).
  const bool isPlainEpub =
      link.href.find(".epub") != std::string::npos || link.href.find("/epub/") != std::string::npos;
  if (currentEntry.href.empty() || link.acqRank > pubAcqRank ||
      (link.acqRank == pubAcqRank && isPlainEpub && !pubHasPlainEpub)) {
    currentEntry.href = std::move(link.href);
    pubAcqRank = link.acqRank;
    pubHasPlainEpub = isPlainEpub;
  }
}

void Opds2Parser::commitPublication() {
  if (currentEntry.title.empty() || currentEntry.href.empty()) return;
  if (entries.size() >= MAX_ENTRIES) {
    feedTruncated = true;
    return;
  }
  currentEntry.type = OpdsEntryType::BOOK;
  entries.push_back(std::move(currentEntry));
  currentEntry = OpdsEntry{};
}

// ---- static callbacks ----

void Opds2Parser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<Opds2Parser*>(ctx);
  const size_t max = sizeof(self->pendingKey) - 1;
  if (len > max) len = max;
  memcpy(self->pendingKey, key, len);
  self->pendingKey[len] = '\0';
}

void Opds2Parser::sOnString(void* ctx, const char* value, const size_t len) {
  static_cast<Opds2Parser*>(ctx)->onStringValue(value, len);
}

void Opds2Parser::sOnNumber(void* ctx, const char* value, size_t) {
  static_cast<Opds2Parser*>(ctx)->onNumberValue(value);
}

void Opds2Parser::sOnBool(void* ctx, const bool value) {
  auto* self = static_cast<Opds2Parser*>(ctx);
  const Scope scope = self->current();
  if ((scope == Scope::FEED_LINK || scope == Scope::PUB_LINK) && strcmp(self->pendingKey, "templated") == 0) {
    self->link.templated = value;
  }
}

void Opds2Parser::sOnObjectStart(void* ctx) { static_cast<Opds2Parser*>(ctx)->onContainerStart(true); }
void Opds2Parser::sOnObjectEnd(void* ctx) { static_cast<Opds2Parser*>(ctx)->onContainerEnd(); }
void Opds2Parser::sOnArrayStart(void* ctx) { static_cast<Opds2Parser*>(ctx)->onContainerStart(false); }
void Opds2Parser::sOnArrayEnd(void* ctx) { static_cast<Opds2Parser*>(ctx)->onContainerEnd(); }
