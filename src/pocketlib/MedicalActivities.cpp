// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Compiled only into Pocket Library builds; stock envs see an empty unit.
#ifdef POCKET_LIBRARY

#include "MedicalActivities.h"

#include <Logging.h>
#include <Memory.h>

#include <cctype>
#include <string_view>

#include "ArticleActivity.h"
#include "LibraryActivities.h"
#include "PocketLibrary.h"
#include "SearchActivity.h"
#include "activities/ActivityResult.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

// Where a title is looked for: a fragment of the collection's key.
enum Source : uint8_t { MedlinePlus, MDWiki, Wikipedia, Wikibooks, kSources };
const char* const kSourceKeys[kSources] = {"medlineplus", "mdwiki", "wikipedia", "wikibooks"};

struct Candidate {
  Source source;
  const char* title;
};
struct Topic {
  const char* label;
  Candidate candidates[4];  // tried in order; title nullptr ends the list
};

// First Aid: what to do, from sources written as instructions. Each row is a
// MedlinePlus page (US National Library of Medicine: plain language, reviewed,
// public domain) named by its permanent address on medlineplus.gov, which
// opens at its "First Aid" section; else a chapter of the Wikibooks First Aid
// manual. A row neither answers is left out. Encyclopedia articles about the
// condition (Wikipedia, MDWiki) are deliberately not used here.
struct Aid {
  const char* label;
  const char* medline;       // path under medlineplus.gov/, or nullptr
  const char* medlineTitle;  // the page's own title, to check the address
  const char* wikibooks;     // chapter under First_Aid/, or nullptr
};
constexpr Aid kFirstAid[] = {
    {"Is it an emergency?", "ency/article/001927.htm", "Recognizing medical emergencies",
     "Emergency_First_Aid_&_Initial_Action_Steps"},
    {"CPR: adult or teen", "ency/article/000013.htm", "CPR - adult and child after onset of puberty", "CPR_summary"},
    {"CPR: child (1 to puberty)", "ency/article/000012.htm", "CPR - young child", "CPR_summary"},
    {"CPR: infant", "ency/article/000011.htm", "CPR - infant", "CPR_summary"},
    {"Choking: adult or child", "ency/article/000049.htm", "Choking - adult or child over 1 year", nullptr},
    {"Choking: infant", "ency/article/000048.htm", "Choking - infant under 1 year", nullptr},
    {"Choking: unconscious", "ency/article/000051.htm", "Choking - unconscious adult or child", nullptr},
    {"Unconscious person", "ency/article/000022.htm", "Unconsciousness - first aid", nullptr},
    {"Severe bleeding", "ency/article/000045.htm", "Bleeding", "External_Bleeding"},
    {"Shock", "ency/article/000039.htm", "Shock", "Shock"},
    {"Heart attack", "ency/article/000063.htm", "Heart attack first aid", "Heart_Attack_&_Angina"},
    {"Stroke", nullptr, nullptr, "Stroke_&_TIA"},
    {"Trouble breathing", "ency/article/000007.htm", "Breathing difficulties - first aid", nullptr},
    {"Severe allergic reaction", "ency/article/000844.htm", "Anaphylaxis", nullptr},
    {"Burns", "ency/article/000030.htm", "Burns", "Burns"},
    {"Poisoning", "ency/article/007579.htm", "Poisoning first aid", "Poisoning"},
    {"Head injury", "ency/article/000028.htm", "Head injury - first aid", nullptr},
    {"Neck or back injury", "ency/article/000029.htm", "Spinal injury", nullptr},
    {"Broken bones", "ency/article/000001.htm", "Broken bone", nullptr},
    {"Seizures", "ency/article/003200.htm", "Seizures", "Seizures"},
    {"Heat stroke and exhaustion", "ency/article/000056.htm", "Heat emergencies", nullptr},
    {"Hypothermia", "ency/article/000038.htm", "Hypothermia", "Cold-Related_Illness_&_Injury"},
    {"Frostbite", "ency/article/000057.htm", "Frostbite", "Cold-Related_Illness_&_Injury"},
    {"Electric shock", "ency/article/000053.htm", "Electrical injury", nullptr},
    {"Low blood sugar", "ency/patientinstructions/000085.htm", "Low blood sugar", nullptr},
    {"Cuts and wounds", "ency/article/000043.htm", "Cuts and puncture wounds", nullptr},
    {"Snake bites", "ency/article/000031.htm", "Snake bites", nullptr},
    {"Animal bites", "ency/patientinstructions/000734.htm", "Animal bites", nullptr},
    {"Insect bites and stings", "ency/article/000033.htm", "Insect bites and stings", nullptr},
    {"Sprains", "ency/article/000041.htm", "Sprains", nullptr},
    {"Eye injuries", "ency/article/000054.htm", "Eye emergencies", nullptr},
    {"Nosebleed", "ency/article/003106.htm", "Nosebleed", nullptr},
    {"Fainting", "ency/article/003092.htm", "Fainting", nullptr},
};

// Body systems and broad topics: MedlinePlus's own group pages, then the
// Wikipedia article on the system.
constexpr Topic kEncyclopedia[] = {
    {"Heart, blood and circulation",
     {{MedlinePlus, "Blood, Heart and Circulation"}, {Wikipedia, "Circulatory system"}}},
    {"Lungs and breathing", {{MedlinePlus, "Lungs and Breathing"}, {Wikipedia, "Respiratory system"}}},
    {"Brain and nerves", {{MedlinePlus, "Brain and Nerves"}, {Wikipedia, "Nervous system"}}},
    {"Digestive system", {{MedlinePlus, "Digestive System"}, {Wikipedia, "Human digestive system"}}},
    {"Bones, joints and muscles",
     {{MedlinePlus, "Bones, Joints and Muscles"}, {Wikipedia, "Human musculoskeletal system"}}},
    {"Skin, hair and nails", {{MedlinePlus, "Skin, Hair and Nails"}, {Wikipedia, "Integumentary system"}}},
    {"Kidneys and urinary system", {{MedlinePlus, "Kidneys and Urinary System"}, {Wikipedia, "Urinary system"}}},
    {"Hormones and glands", {{MedlinePlus, "Endocrine System"}, {Wikipedia, "Endocrine system"}}},
    {"Immune system", {{MedlinePlus, "Immune System"}, {Wikipedia, "Immune system"}}},
    {"Eyes and vision", {{MedlinePlus, "Eyes and Vision"}, {Wikipedia, "Visual system"}}},
    {"Ear, nose and throat", {{MedlinePlus, "Ear, Nose and Throat"}, {Wikipedia, "Otorhinolaryngology"}}},
    {"Mouth and teeth", {{MedlinePlus, "Mouth and Teeth"}, {Wikipedia, "Oral and maxillofacial pathology"}}},
    {"Infections", {{MedlinePlus, "Infections"}, {Wikipedia, "Infection"}}},
    {"Mental health", {{MedlinePlus, "Mental Health and Behavior"}, {Wikipedia, "Mental health"}}},
    {"Women's health", {{MedlinePlus, "Female Reproductive System"}, {Wikipedia, "Women's health"}}},
    {"Men's health", {{MedlinePlus, "Male Reproductive System"}, {Wikipedia, "Men's health"}}},
    {"Medicines", {{MedlinePlus, "Drugs, Herbs and Supplements"}, {Wikipedia, "Medication"}}},
};

const char* sourceName(Source s) {
  switch (s) {
    case MedlinePlus:
      return "MedlinePlus";
    case MDWiki:
      return "MDWiki";
    case Wikipedia:
      return "Wikipedia";
    default:
      return "Wikibooks";
  }
}

// Lowercase, trimmed: titles compare loosely ("Nosebleed " on the site).
std::string looseTitle(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.back() == ' ')) {
    if (s.front() == ' ') s.remove_prefix(1);
    if (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  }
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// The page at `path` (tried in the namespaces and host folders a scraped
// site or a wiki uses), when its title begins with `expect`: a wrong address
// then opens nothing rather than the wrong page. A title that is only the
// path (no <title> kept) is taken on trust.
bool findChecked(zim::Archive& archive, const char* const* prefixes, std::string_view path, std::string_view expect,
                 uint32_t& entry) {
  const std::string want = looseTitle(expect);
  for (char ns : {archive.contentNamespace(), 'A', 'C'}) {
    for (const char* const* p = prefixes; *p; p++) {
      const std::string full = std::string(*p) + std::string(path);
      zim::Entry e;
      if (archive.findByPath(ns, full, e) != zim::Error::None) continue;
      if (archive.resolve(e) != zim::Error::None) continue;
      const std::string got = looseTitle(e.title);
      if (e.title != e.path && got.compare(0, want.size(), want) != 0) {
        LOG_INF("PLIB", "First Aid: %s is \"%s\", not \"%s\"", full.c_str(), e.title.c_str(), want.c_str());
        return false;
      }
      entry = e.index;
      return true;
    }
  }
  return false;
}

constexpr const char* kMedlinePrefixes[] = {"medlineplus.gov/", "www.medlineplus.gov/", "", nullptr};
constexpr const char* kWikibooksPrefixes[] = {"First_Aid/", nullptr};

}  // namespace

CuratedListActivity::CuratedListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Kind kind)
    : UiListActivity("PocketCurated", renderer, mappedInput), kind_(kind) {}

void CuratedListActivity::onEnter() {
  auto& lib = pocketlib::Library::instance();
  if (lib.collections().empty()) lib.load();
  title_ = kind_ == Kind::FirstAid ? "First Aid" : "Medical Encyclopedia";
  resolve();
  UiListActivity::onEnter();
}

// Finds each row's article on the card, once per session: the card does not
// change while the reader is on, and the lookups cost dozens of reads.
void CuratedListActivity::resolve() {
  auto& lib = pocketlib::Library::instance();
  const auto& cols = lib.collections();
  struct Cached {
    bool done = false;
    size_t collections = 0;
    std::vector<Row> rows;
  };
  static Cached cache[2];
  Cached& c = cache[kind_ == Kind::FirstAid ? 0 : 1];
  if (c.done && c.collections == cols.size()) {
    rows_ = c.rows;
    fillItems();
    return;
  }
  int collectionOf[kSources];
  for (int s = 0; s < kSources; s++) {
    collectionOf[s] = -1;
    for (size_t i = 0; i < cols.size() && collectionOf[s] < 0; i++) {
      if (cols[i].key.find(kSourceKeys[s]) != std::string::npos) collectionOf[s] = static_cast<int>(i);
    }
  }

  rows_.clear();
  if (kind_ == Kind::FirstAid) {
    resolveFirstAid(collectionOf[MedlinePlus], collectionOf[Wikibooks]);
  } else {
    resolveTopics(collectionOf);
  }
  if (rows_.empty() || (kind_ == Kind::Encyclopedia && rows_.size() == 1)) {
    LOG_INF("PLIB", "%s: nothing found on the card", title_.c_str());
  }
  c = {true, cols.size(), rows_};
  fillItems();
}

void CuratedListActivity::fillItems() {
  items_.assign(rows_.size(), fui::ListItem{});
  for (size_t i = 0; i < rows_.size(); i++) {
    items_[i].label = rows_[i].label.c_str();
    items_[i].subtitle = rows_[i].subtitle.c_str();
    items_[i].actionValue = static_cast<int16_t>(i);
  }
}

// MedlinePlus by address, opened at its "First Aid" heading; else the
// Wikibooks chapter, from the top.
void CuratedListActivity::resolveFirstAid(int medline, int wikibooks) {
  auto& lib = pocketlib::Library::instance();
  zim::Archive* ml = medline >= 0 ? lib.open(static_cast<size_t>(medline)) : nullptr;
  zim::Archive* wb = wikibooks >= 0 ? lib.open(static_cast<size_t>(wikibooks)) : nullptr;
  for (const Aid& aid : kFirstAid) {
    uint32_t entry = 0;
    if (ml && aid.medline && findChecked(*ml, kMedlinePrefixes, aid.medline, aid.medlineTitle, entry)) {
      rows_.push_back({aid.label, std::string("MedlinePlus \xC2\xB7 ") + aid.medlineTitle, medline, entry, "First Aid"});
      continue;
    }
    if (wb && aid.wikibooks && findChecked(*wb, kWikibooksPrefixes, aid.wikibooks, "First Aid", entry)) {
      std::string chapter = aid.wikibooks;
      for (char& c : chapter)
        if (c == '_') c = ' ';
      rows_.push_back({aid.label, "Wikibooks First Aid \xC2\xB7 " + chapter, wikibooks, entry, {}});
    }
  }
}

// One exact-title lookup per candidate until one is on the card: a few index
// pages each, no article reads.
void CuratedListActivity::resolveTopics(const int* collectionOf) {
  auto& lib = pocketlib::Library::instance();
  rows_.push_back({"Search medical topics", "MedlinePlus and MDWiki", -1, 0, {}});
  std::vector<pocketlib::Library::Hit> hits;
  for (const Topic& topic : kEncyclopedia) {
    for (const Candidate& c : topic.candidates) {
      if (!c.title) break;
      const int col = collectionOf[c.source];
      if (col < 0) continue;
      lib.search(static_cast<size_t>(col), c.title, 1, hits);  // an exact match comes first
      const pocketlib::Library::Hit* exact = nullptr;
      for (const auto& h : hits) {
        if (h.exact) {
          exact = &h;
          break;
        }
      }
      if (!exact) continue;
      rows_.push_back({topic.label, std::string(sourceName(c.source)) + " \xC2\xB7 " + c.title, col, exact->entry, {}});
      break;
    }
  }
}

void CuratedListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.inputMask = fui::InputTouch;
  props.subtitleText = screen.theme().smallText;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  syncListViewport(screen, props);
  screen.list(props);
}

void CuratedListActivity::activateIndex(int index) {
  if (index < 0 || index >= static_cast<int>(rows_.size())) return;
  const Row& row = rows_[index];
  if (row.collection < 0) {
    auto search = makeUniqueNoThrow<SearchActivity>(renderer, mappedInput, -1, "Medical");
    if (search) startActivityForResult(std::move(search), [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  auto article =
      makeUniqueNoThrow<ArticleActivity>(renderer, mappedInput, static_cast<size_t>(row.collection), row.entry, row.landing);
  if (article) startActivityForResult(std::move(article), [this](const ActivityResult&) { requestUpdate(); });
}

#endif  // POCKET_LIBRARY
