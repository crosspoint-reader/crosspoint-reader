#pragma once

#include <string>

// Appends looked-up words to /Highlights/vocab.md — one "## word" heading
// followed by the definition text, so the file reads as a running vocabulary
// list. Lives alongside the highlight markdown (same folder) so the existing
// sync picks it up without extra plumbing.
namespace VocabStore {

// Exposed so consumers (e.g. the Koofr upload) can find the file without
// duplicating the path.
constexpr const char* VOCAB_FILE_PATH = "/Highlights/vocab.md";

// Entries are appended verbatim; the same word looked up twice is written
// twice. Callers that can cheaply tell it is a repeat should skip the call.
bool save(const std::string& word, const std::string& definition);

}  // namespace VocabStore
