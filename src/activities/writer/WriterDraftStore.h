#pragma once

#include <string>

class WriterDraftStore {
 public:
  // Hard-coded paths for dev phase.
  static constexpr const char* DraftDir = "/writer";
  static constexpr const char* DraftPath = "/writer/draft.txt";

  bool ensureDraft();
  bool readDraft(std::string& out);
};
