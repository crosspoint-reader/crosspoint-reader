#include "PageExporter.h"

#include <SDCardManager.h>

#include <string>

namespace {
constexpr const char* EXPORTS_DIR = "/clippings";
constexpr const char* TAG = "PEX";
}  // namespace

std::string PageExporter::getExportPath(const std::string& bookPath) {
  // Extract filename from path (e.g. "/Books/My Book.epub" -> "My Book")
  std::string filename;
  const auto lastSlash = bookPath.rfind('/');
  if (lastSlash != std::string::npos) {
    filename = bookPath.substr(lastSlash + 1);
  } else {
    filename = bookPath;
  }
  // Strip the original extension
  const auto lastDot = filename.rfind('.');
  if (lastDot != std::string::npos) {
    filename.resize(lastDot);
  }
  if (filename.empty()) {
    filename = "untitled";
  }
  return std::string(EXPORTS_DIR) + "/" + filename + ".md";
}

bool PageExporter::writeHeader(FsFile& file, const std::string& bookTitle, const std::string& bookAuthor) {
  // YAML frontmatter
  std::string header = "---\n";
  header += "title: \"" + bookTitle + "\"\n";
  if (!bookAuthor.empty()) {
    header += "author: \"" + bookAuthor + "\"\n";
  }
  header += "---\n\n";

  // Markdown title heading
  header += "# " + bookTitle;
  if (!bookAuthor.empty()) {
    header += " \xe2\x80\x94 " + bookAuthor;  // em-dash UTF-8
  }
  header += "\n";

  return file.write(reinterpret_cast<const uint8_t*>(header.c_str()), header.size()) == header.size();
}

bool PageExporter::writePassage(FsFile& file, const std::vector<CapturedPage>& pages) {
  if (pages.empty()) {
    return true;
  }

  std::string entry;
  std::string lastChapter;

  for (const auto& page : pages) {
    // Insert a new chapter heading when the chapter changes
    if (page.chapterTitle != lastChapter) {
      entry += "\n## " + page.chapterTitle + " | " + std::to_string(page.bookPercent) + "% of book | " +
               std::to_string(page.chapterPercent) + "% of chapter\n";
      lastChapter = page.chapterTitle;
    }

    entry += page.pageText;
    entry += "\n\n";
  }

  // Visual separator between captures for readability
  entry += "---\n";

  return file.write(reinterpret_cast<const uint8_t*>(entry.c_str()), entry.size()) == entry.size();
}

bool PageExporter::exportPassage(const std::string& bookPath, const std::string& bookTitle,
                                 const std::string& bookAuthor, const std::vector<CapturedPage>& pages) {
  if (pages.empty()) {
    return false;
  }

  SdMan.mkdir(EXPORTS_DIR);

  const std::string path = getExportPath(bookPath);

  // Check if file already exists (to decide whether to write header)
  const bool isNew = !SdMan.exists(path.c_str());

  // Open in append mode
  FsFile file = SdMan.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    Serial.printf("[%lu] [%s] Failed to open export file: %s\n", millis(), TAG, path.c_str());
    return false;
  }

  bool ok = true;
  if (isNew) {
    ok = writeHeader(file, bookTitle, bookAuthor);
  }
  if (ok) {
    ok = writePassage(file, pages);
  }

  file.close();

  if (ok) {
    Serial.printf("[%lu] [%s] Passage exported to %s (%d pages)\n", millis(), TAG, path.c_str(),
                  static_cast<int>(pages.size()));
  } else {
    Serial.printf("[%lu] [%s] Failed to write passage\n", millis(), TAG);
  }
  return ok;
}
