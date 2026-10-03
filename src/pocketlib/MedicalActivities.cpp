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

// Emergencies, most urgent first. MedlinePlus (US National Library of
// Medicine: plain language, reviewed, public domain) first; then MDWiki or
// Wikipedia. Titles are as the sites name them; a row whose titles are all
// missing from the card is left out.
constexpr Topic kFirstAid[] = {
    {"First aid basics", {{Wikibooks, "First Aid"}, {MedlinePlus, "First Aid"}, {Wikipedia, "First aid"}}},
    {"CPR",
     {{MedlinePlus, "CPR - adult and child after onset of puberty"},
      {MedlinePlus, "CPR"},
      {MDWiki, "Cardiopulmonary resuscitation"},
      {Wikipedia, "Cardiopulmonary resuscitation"}}},
    {"Choking", {{MedlinePlus, "Choking - adult or child over 1 year"}, {MDWiki, "Choking"}, {Wikipedia, "Choking"}}},
    {"Severe bleeding", {{MedlinePlus, "Bleeding"}, {Wikipedia, "Bleeding"}}},
    {"Shock", {{MedlinePlus, "Shock"}, {Wikipedia, "Shock (circulatory)"}}},
    {"Heart attack",
     {{MedlinePlus, "Heart attack first aid"}, {MedlinePlus, "Heart attack"}, {Wikipedia, "Myocardial infarction"}}},
    {"Stroke", {{MedlinePlus, "Stroke"}, {MDWiki, "Stroke"}, {Wikipedia, "Stroke"}}},
    {"Severe allergic reaction",
     {{MedlinePlus, "Anaphylaxis"}, {MedlinePlus, "Allergic reactions"}, {Wikipedia, "Anaphylaxis"}}},
    {"Burns", {{MedlinePlus, "Burns"}, {Wikipedia, "Burn"}}},
    {"Broken bones", {{MedlinePlus, "Broken bone"}, {Wikipedia, "Bone fracture"}}},
    {"Head injury", {{MedlinePlus, "Head injury - first aid"}, {Wikipedia, "Head injury"}}},
    {"Poisoning", {{MedlinePlus, "Poisoning first aid"}, {MedlinePlus, "Poisoning"}, {Wikipedia, "Poisoning"}}},
    {"Seizures", {{MedlinePlus, "Seizures"}, {Wikipedia, "Epileptic seizure"}}},
    {"Heat stroke and exhaustion", {{MedlinePlus, "Heat emergencies"}, {Wikipedia, "Heat stroke"}}},
    {"Hypothermia", {{MedlinePlus, "Hypothermia"}, {Wikipedia, "Hypothermia"}}},
    {"Frostbite", {{MedlinePlus, "Frostbite"}, {Wikipedia, "Frostbite"}}},
    {"Drowning", {{MedlinePlus, "Near drowning"}, {Wikipedia, "Drowning"}}},
    {"Electric shock", {{MedlinePlus, "Electrical injury"}, {Wikipedia, "Electrical injury"}}},
    {"Low blood sugar", {{MedlinePlus, "Low blood sugar"}, {Wikipedia, "Hypoglycemia"}}},
    {"Cuts and wounds", {{MedlinePlus, "Cuts and puncture wounds"}, {Wikipedia, "Wound"}}},
    {"Bites and stings", {{MedlinePlus, "Insect bites and stings"}, {Wikipedia, "Insect bites and stings"}}},
    {"Snake bites", {{MedlinePlus, "Snake bites"}, {Wikipedia, "Snakebite"}}},
    {"Animal bites", {{MedlinePlus, "Animal bites"}, {Wikipedia, "Dog bite"}}},
    {"Sprains and strains", {{MedlinePlus, "Sprains"}, {Wikipedia, "Sprain"}}},
    {"Eye injuries", {{MedlinePlus, "Eye emergencies"}, {Wikipedia, "Eye injury"}}},
    {"Nosebleed", {{MedlinePlus, "Nosebleed"}, {Wikipedia, "Nosebleed"}}},
    {"Fainting", {{MedlinePlus, "Fainting"}, {Wikipedia, "Syncope (medicine)"}}},
    {"Dehydration", {{MedlinePlus, "Dehydration"}, {Wikipedia, "Dehydration"}}},
    {"Recovery position", {{MDWiki, "Recovery position"}, {Wikipedia, "Recovery position"}}},
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

// One exact-title lookup per candidate until one is on the card: a few index
// pages each, no article reads.
void CuratedListActivity::resolve() {
  auto& lib = pocketlib::Library::instance();
  const auto& cols = lib.collections();
  int collectionOf[kSources];
  for (int s = 0; s < kSources; s++) {
    collectionOf[s] = -1;
    for (size_t i = 0; i < cols.size() && collectionOf[s] < 0; i++) {
      if (cols[i].key.find(kSourceKeys[s]) != std::string::npos) collectionOf[s] = static_cast<int>(i);
    }
  }

  rows_.clear();
  if (kind_ == Kind::Encyclopedia) {
    rows_.push_back({"Search medical topics", "MedlinePlus and MDWiki", -1, 0});
  }
  const Topic* topics = kind_ == Kind::FirstAid ? kFirstAid : kEncyclopedia;
  const size_t count = kind_ == Kind::FirstAid ? std::size(kFirstAid) : std::size(kEncyclopedia);
  std::vector<pocketlib::Library::Hit> hits;
  for (size_t t = 0; t < count; t++) {
    for (const Candidate& c : topics[t].candidates) {
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
      rows_.push_back({topics[t].label, std::string(sourceName(c.source)) + " \xC2\xB7 " + c.title, col, exact->entry});
      break;
    }
  }
  if (rows_.empty() || (kind_ == Kind::Encyclopedia && rows_.size() == 1 && collectionOf[MedlinePlus] < 0)) {
    LOG_INF("PLIB", "%s: nothing found on the card", title_.c_str());
  }

  items_.assign(rows_.size(), fui::ListItem{});
  for (size_t i = 0; i < rows_.size(); i++) {
    items_[i].label = rows_[i].label.c_str();
    items_[i].subtitle = rows_[i].subtitle.c_str();
    items_[i].actionValue = static_cast<int16_t>(i);
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
      makeUniqueNoThrow<ArticleActivity>(renderer, mappedInput, static_cast<size_t>(row.collection), row.entry);
  if (article) startActivityForResult(std::move(article), [this](const ActivityResult&) { requestUpdate(); });
}

#endif  // POCKET_LIBRARY
