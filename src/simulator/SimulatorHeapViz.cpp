#ifdef SIMULATOR

#include "SimulatorHeapViz.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

namespace SimulatorHeapViz {
namespace {

constexpr std::size_t kCellPx = 3;
constexpr std::size_t kMarginPx = 24;
constexpr std::size_t kPanelWidthPx = 840;

std::mutex gMutex;
bool gEnabled = false;
bool gFailureLogged = false;
std::string gOutputDir;
std::size_t gLastPeakUsedBytes = 0;
std::size_t gLastMinLargestFreeBlockBytes = static_cast<std::size_t>(-1);
std::atomic<std::uint64_t> gSequence{0};

bool ensureDirectoryExists(const char* rawPath) {
  std::string normalized(rawPath ? rawPath : "");
  while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
  if (normalized.empty()) {
    std::lock_guard<std::mutex> lock(gMutex);
    gEnabled = false;
    gOutputDir.clear();
    return false;
  }

  std::string partial;
  if (normalized[0] == '/') partial = "/";
  std::size_t pos = normalized[0] == '/' ? 1 : 0;
  bool ok = true;
  int lastErrno = 0;
  while (pos <= normalized.size()) {
    const std::size_t next = normalized.find('/', pos);
    const std::size_t end = next == std::string::npos ? normalized.size() : next;
    const std::string segment = normalized.substr(pos, end - pos);
    if (!segment.empty()) {
      if (!partial.empty() && partial.back() != '/') partial.push_back('/');
      partial += segment;
      if (::mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST) {
        ok = false;
        lastErrno = errno;
        break;
      }
    }
    if (next == std::string::npos) break;
    pos = next + 1;
  }

  std::lock_guard<std::mutex> lock(gMutex);
  if (!ok) {
    if (!gFailureLogged) {
      std::fprintf(stderr, "[SIM] failed to create CROSSPOINT_SIM_HEAP_VIZ=%s errno=%d; disabling SVG dumps\n", rawPath,
                   lastErrno);
      gFailureLogged = true;
    }
    gEnabled = false;
    gOutputDir.clear();
    return false;
  }

  gEnabled = true;
  gFailureLogged = false;
  gOutputDir = normalized;
  gLastPeakUsedBytes = 0;
  gLastMinLargestFreeBlockBytes = static_cast<std::size_t>(-1);
  gSequence.store(0);
  return true;
}

std::size_t bytesPerRowForArena(const std::size_t arenaBytes) {
  const double root = std::sqrt(static_cast<double>(arenaBytes));
  return std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(root)));
}

std::string escapeXml(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&apos;";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::string jsonStringLiteral(const std::string& text) {
  std::string out = "\"";
  out.reserve(text.size() + 2);
  for (const char ch : text) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
}

std::string sanitizeToken(const char* text) {
  std::string out;
  if (!text) return "manual";
  for (const char* p = text; *p; ++p) {
    const char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  return out.empty() ? "manual" : out;
}

std::string timestampPrefix() {
  std::time_t now = std::time(nullptr);
  std::tm localTime = {};
  localtime_r(&now, &localTime);
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%y%m%d_%H%M%S", &localTime) == 0) return "000000_000000";
  return buffer;
}

std::string colorForSite(const std::vector<AllocationSnapshot>& allocations, const std::uint64_t siteHash,
                         const std::vector<std::uint64_t>& rankedTopSites) {
  (void)allocations;
  static constexpr const char* kTopPalette[8] = {
      "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
      "#9467bd", "#8c564b", "#e377c2", "#7f7f7f",
  };
  static constexpr const char* kFallbackPalette[6] = {
      "#9ecae1", "#fdd0a2", "#a1d99b", "#fdae6b", "#bcbddc", "#c7e9c0",
  };

  for (std::size_t i = 0; i < rankedTopSites.size(); ++i) {
    if (rankedTopSites[i] == siteHash) return kTopPalette[i];
  }
  return kFallbackPalette[siteHash % 6U];
}

std::string darkenHexColor(const std::string& hex, const double factor) {
  if (hex.size() != 7 || hex[0] != '#') return hex;
  auto channel = [&](const int idx) {
    const int value = std::stoi(hex.substr(static_cast<std::size_t>(idx), 2), nullptr, 16);
    const int shaded = std::max(0, std::min(255, static_cast<int>(std::lround(value * factor))));
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", shaded);
    return std::string(buf);
  };
  return "#" + channel(1) + channel(3) + channel(5);
}

std::vector<std::uint64_t> rankTopSites(const Snapshot& snapshot) {
  struct SiteTotal {
    std::uint64_t siteHash = 0;
    std::size_t totalBytes = 0;
  };

  std::vector<SiteTotal> totals;
  for (const auto& allocation : snapshot.allocations) {
    auto it = std::find_if(totals.begin(), totals.end(),
                           [&](const SiteTotal& total) { return total.siteHash == allocation.siteHash; });
    if (it == totals.end()) {
      totals.push_back({allocation.siteHash, allocation.requestedBytes});
    } else {
      it->totalBytes += allocation.requestedBytes;
    }
  }

  std::sort(totals.begin(), totals.end(), [](const SiteTotal& a, const SiteTotal& b) { return a.totalBytes > b.totalBytes; });
  std::vector<std::uint64_t> ranked;
  const std::size_t count = std::min<std::size_t>(8, totals.size());
  ranked.reserve(count);
  for (std::size_t i = 0; i < count; ++i) ranked.push_back(totals[i].siteHash);
  return ranked;
}

std::string renderSvg(const Snapshot& snapshot, const char* reason) {
  const std::size_t bytesPerRow = bytesPerRowForArena(snapshot.arenaBytes);
  const std::size_t rowCount = (snapshot.arenaBytes + bytesPerRow - 1U) / bytesPerRow;
  const std::size_t arenaWidthPx = bytesPerRow * kCellPx;
  const std::size_t arenaHeightPx = rowCount * kCellPx;
  const std::size_t svgWidthPx = arenaWidthPx + kPanelWidthPx + (kMarginPx * 3U);
  const std::size_t svgHeightPx = std::max<std::size_t>(arenaHeightPx + (kMarginPx * 2U), 640U);
  const std::vector<std::uint64_t> rankedTopSites = rankTopSites(snapshot);
  const std::string defaultDetail = snapshot.contextText.empty() ? "Hover an allocation, click to pin details."
                                                                 : snapshot.contextText;
  const std::string contextTitle = snapshot.contextTitle.empty() ? (reason ? reason : "manual") : snapshot.contextTitle;
  std::unordered_map<std::string, std::string> textToId;
  std::vector<std::pair<std::string, std::string>> textEntries;
  auto textIdFor = [&](const std::string& text) {
    const auto it = textToId.find(text);
    if (it != textToId.end()) return it->second;
    const std::string id = "t" + std::to_string(textEntries.size());
    textToId.emplace(text, id);
    textEntries.emplace_back(id, text);
    return id;
  };
  std::unordered_map<std::string, std::string> detailToId;
  std::vector<std::pair<std::string, std::string>> detailEntries;
  auto detailIdFor = [&](const std::string& detail) {
    const auto it = detailToId.find(detail);
    if (it != detailToId.end()) return it->second;
    const std::string id = "d" + std::to_string(detailEntries.size());
    detailToId.emplace(detail, id);
    detailEntries.emplace_back(id, detail);
    return id;
  };
  const std::string defaultDetailId = detailIdFor(defaultDetail);

  std::ostringstream oss;
  oss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << svgWidthPx << "\" height=\"" << svgHeightPx
      << "\" viewBox=\"0 0 " << svgWidthPx << " " << svgHeightPx << "\">\n";
  oss << "<style>"
         "text{font-family:Menlo,Consolas,monospace;fill:#111827}"
         ".meta{font-size:14px}.panel{fill:#f8fafc;stroke:#cbd5e1}.arena{fill:#ffffff;stroke:#94a3b8;stroke-width:2}"
         ".interactive{cursor:pointer}.alloc-seg{stroke:#111827;stroke-opacity:.35;stroke-width:.5}"
         ".grid{stroke:#e5e7eb;stroke-width:.5}.detail-wrap{height:100%;overflow:auto}"
         ".detail{font:12px/1.35 Menlo,Consolas,monospace;white-space:pre-wrap;margin:0;padding:0;color:#111827;"
         "overflow-wrap:anywhere;min-height:100%}"
         "</style>\n";
  oss << "<rect width=\"100%\" height=\"100%\" fill=\"#eef2f7\"/>\n";
  oss << "<rect class=\"arena\" x=\"" << kMarginPx << "\" y=\"" << kMarginPx << "\" width=\"" << arenaWidthPx
      << "\" height=\"" << arenaHeightPx << "\"/>\n";

  auto drawReservedRange = [&](const std::size_t startByte, const std::size_t lengthBytes, const char* fill,
                               const std::string& title, const std::string& detail) {
    const std::string detailId = detailIdFor(detail);
    const std::size_t endByte = std::min(snapshot.arenaBytes, startByte + lengthBytes);
    oss << "<g class=\"interactive\" data-detail-id=\"" << detailId
        << "\" onclick=\"selectAlloc(evt)\" onmousemove=\"previewAlloc(evt)\" onmouseleave=\"clearPreview()\">";
    oss << "<title>" << escapeXml(title) << "</title>";
    std::size_t cursor = startByte;
    while (cursor < endByte) {
      const std::size_t row = cursor / bytesPerRow;
      const std::size_t rowStart = row * bytesPerRow;
      const std::size_t nextRow = (row + 1U) * bytesPerRow;
      const std::size_t segmentEnd = std::min(endByte, nextRow);
      const std::size_t col = cursor - rowStart;
      const std::size_t span = segmentEnd - cursor;
      oss << "<rect x=\"" << (kMarginPx + col * kCellPx) << "\" y=\"" << (kMarginPx + row * kCellPx)
          << "\" width=\"" << (span * kCellPx) << "\" height=\"" << kCellPx << "\" fill=\"" << fill
          << "\" stroke=\"none\"/>\n";
      cursor = segmentEnd;
    }
    oss << "</g>\n";
  };

  if (snapshot.controlBytes > 0) {
    std::ostringstream detail;
    detail << "TLSF control structure\n";
    detail << "offset=0 bytes\n";
    detail << "size=" << snapshot.controlBytes << " bytes\n";
    detail << "Allocator metadata: FL/SL bitmaps, free-list heads, control block.";
    drawReservedRange(0, snapshot.controlBytes, "#cbd5e1", "tlsf control", detail.str());
  }
  if (snapshot.sentinelBytes > 0 && snapshot.sentinelBytes <= snapshot.arenaBytes) {
    std::ostringstream detail;
    detail << "TLSF terminal sentinel\n";
    detail << "offset=" << (snapshot.arenaBytes - snapshot.sentinelBytes) << " bytes\n";
    detail << "size=" << snapshot.sentinelBytes << " bytes\n";
    detail << "Non-allocable end-of-pool sentinel bookkeeping.";
    drawReservedRange(snapshot.arenaBytes - snapshot.sentinelBytes, snapshot.sentinelBytes, "#94a3b8",
                      "tlsf sentinel", detail.str());
  }

  const std::size_t gridStep = bytesPerRow > 256 ? 64 : 32;
  for (std::size_t x = gridStep; x < bytesPerRow; x += gridStep) {
    oss << "<line class=\"grid\" x1=\"" << (kMarginPx + x * kCellPx) << "\" y1=\"" << kMarginPx << "\" x2=\""
        << (kMarginPx + x * kCellPx) << "\" y2=\"" << (kMarginPx + arenaHeightPx) << "\"/>\n";
  }
  for (std::size_t row = gridStep; row < rowCount; row += gridStep) {
    oss << "<line class=\"grid\" x1=\"" << kMarginPx << "\" y1=\"" << (kMarginPx + row * kCellPx) << "\" x2=\""
        << (kMarginPx + arenaWidthPx) << "\" y2=\"" << (kMarginPx + row * kCellPx) << "\"/>\n";
  }

  for (std::size_t index = 0; index < snapshot.allocations.size(); ++index) {
    const AllocationSnapshot& allocation = snapshot.allocations[index];
    const std::string fill = colorForSite(snapshot.allocations, allocation.siteHash, rankedTopSites);
    const std::string headerFill = darkenHexColor(fill, 0.72);
    const std::string stackId = textIdFor(allocation.stackText);
    const std::string headerJson = escapeXml(jsonStringLiteral(allocation.detailHeaderText));

    const std::size_t headerStart = allocation.offset >= allocation.headerBytes ? allocation.offset - allocation.headerBytes : 0;
    const std::size_t payloadStart = allocation.offset;
    const std::size_t payloadEnd = allocation.offset + allocation.payloadBytes;
    const std::size_t totalEnd = payloadEnd;

    oss << "<g class=\"interactive\" data-stack-id=\"" << stackId << "\" data-header-json=\"" << headerJson
        << "\" onclick=\"selectAlloc(evt)\" onmousemove=\"previewAlloc(evt)\" onmouseleave=\"clearPreview()\">";
    oss << "<title>" << escapeXml(allocation.titleText) << "</title>";
    std::size_t cursor = headerStart;
    while (cursor < totalEnd) {
      const std::size_t row = cursor / bytesPerRow;
      const std::size_t rowStart = row * bytesPerRow;
      const std::size_t rowEnd = (row + 1U) * bytesPerRow;
      const std::size_t headerSegStart = std::max(cursor, headerStart);
      const std::size_t headerSegEnd = std::min(rowEnd, payloadStart);
      if (headerSegStart < headerSegEnd) {
        const std::size_t col = headerSegStart - rowStart;
        const std::size_t span = headerSegEnd - headerSegStart;
        oss << "<rect class=\"alloc-seg\" id=\"alloc" << index << "\" x=\"" << (kMarginPx + col * kCellPx) << "\" y=\""
            << (kMarginPx + row * kCellPx) << "\" width=\"" << (span * kCellPx) << "\" height=\"" << kCellPx
            << "\" fill=\"" << headerFill << "\"/>\n";
      }

      const std::size_t payloadSegStart = std::max(cursor, payloadStart);
      const std::size_t payloadSegEnd = std::min(rowEnd, payloadEnd);
      if (payloadSegStart < payloadSegEnd) {
        const std::size_t col = payloadSegStart - rowStart;
        const std::size_t span = payloadSegEnd - payloadSegStart;
        oss << "<rect class=\"alloc-seg\" id=\"alloc" << index << "\" x=\"" << (kMarginPx + col * kCellPx) << "\" y=\""
            << (kMarginPx + row * kCellPx) << "\" width=\"" << (span * kCellPx) << "\" height=\"" << kCellPx
            << "\" fill=\"" << fill << "\"/>\n";
      }

      cursor = rowEnd;
    }
    oss << "</g>\n";
  }

  const std::size_t panelX = kMarginPx * 2U + arenaWidthPx;
  oss << "<rect class=\"panel\" x=\"" << panelX << "\" y=\"" << kMarginPx << "\" width=\"" << kPanelWidthPx
      << "\" height=\"" << (svgHeightPx - kMarginPx * 2U) << "\" rx=\"8\"/>\n";
  oss << "<text class=\"meta\" x=\"" << (panelX + 16) << "\" y=\"" << (kMarginPx + 28) << "\">CrossPoint heap viz</text>\n";
  oss << "<text class=\"meta\" x=\"" << (panelX + 16) << "\" y=\"" << (kMarginPx + 54) << "\">reason: "
      << escapeXml(contextTitle) << "</text>\n";
  oss << "<text class=\"meta\" x=\"" << (panelX + 16) << "\" y=\"" << (kMarginPx + 80) << "\">arena="
      << snapshot.arenaBytes << " free=" << snapshot.freeBytes << " peak=" << snapshot.peakUsedBytes << "</text>\n";
  oss << "<text class=\"meta\" x=\"" << (panelX + 16) << "\" y=\"" << (kMarginPx + 106) << "\">min_free="
      << snapshot.minFreeBytes << " largest_free=" << snapshot.largestFreeBlockBytes << " frag="
      << snapshot.fragmentationPercent << "%</text>\n";
  oss << "<text class=\"meta\" x=\"" << (panelX + 16) << "\" y=\"" << (kMarginPx + 132) << "\">tlsf_control="
      << snapshot.controlBytes << " sentinel=" << snapshot.sentinelBytes << "</text>\n";
  oss << "<foreignObject x=\"" << (panelX + 16) << "\" y=\"" << (kMarginPx + 152) << "\" width=\""
      << (kPanelWidthPx - 32) << "\" height=\"" << (svgHeightPx - (kMarginPx * 2U) - 168) << "\">"
      << "<div xmlns=\"http://www.w3.org/1999/xhtml\" class=\"detail-wrap\">"
      << "<pre class=\"detail\" id=\"detail\" data-default-detail-id=\"" << defaultDetailId << "\">"
      << escapeXml(defaultDetail) << "</pre>"
      << "</div></foreignObject>\n";
  oss << "<metadata id=\"details-json\"><![CDATA[{";
  for (std::size_t i = 0; i < detailEntries.size(); ++i) {
    if (i != 0) oss << ",";
    oss << jsonStringLiteral(detailEntries[i].first) << ":" << jsonStringLiteral(detailEntries[i].second);
  }
  oss << "}]]></metadata>\n";
  oss << "<metadata id=\"stacks-json\"><![CDATA[{";
  for (std::size_t i = 0; i < textEntries.size(); ++i) {
    if (i != 0) oss << ",";
    oss << jsonStringLiteral(textEntries[i].first) << ":" << jsonStringLiteral(textEntries[i].second);
  }
  oss << "}]]></metadata>\n";
  oss << "<script><![CDATA[\n"
         "let pinned='';\n"
         "const detailsById=JSON.parse(document.getElementById('details-json').textContent);\n"
         "const stacksById=JSON.parse(document.getElementById('stacks-json').textContent);\n"
         "function setDetail(t){document.getElementById('detail').textContent=t;}\n"
         "function defaultDetail(){return detailsById[document.getElementById('detail').getAttribute('data-default-detail-id')];}\n"
         "function interactiveTarget(node){while(node&&node.getAttribute&&!node.getAttribute('data-detail-id')&&!node.getAttribute('data-stack-id')){node=node.parentNode;}return node;}\n"
         "function detailsFor(target){const node=interactiveTarget(target);if(!node){return defaultDetail();}"
         "if(node.getAttribute('data-detail-id')){return detailsById[node.getAttribute('data-detail-id')];}"
         "const header=JSON.parse(node.getAttribute('data-header-json'));const stack=stacksById[node.getAttribute('data-stack-id')]||'stack unavailable';"
         "return header+'\\n'+stack;}\n"
         "function previewAlloc(evt){if(!pinned){setDetail(detailsFor(evt.target));}}\n"
         "function clearPreview(){if(!pinned){setDetail(defaultDetail());}}\n"
         "function selectAlloc(evt){const d=detailsFor(evt.target);pinned=(pinned===d)?'':d;"
         "setDetail(pinned||defaultDetail());}\n"
         "]]></script>\n";
  oss << "</svg>\n";
  return oss.str();
}

}  // namespace

void configureFromEnv() {
  const char* raw = std::getenv("CROSSPOINT_SIM_HEAP_VIZ");
  if (!raw || !*raw) {
    std::lock_guard<std::mutex> lock(gMutex);
    gEnabled = false;
    gOutputDir.clear();
    gFailureLogged = false;
    gLastPeakUsedBytes = 0;
    gLastMinLargestFreeBlockBytes = static_cast<std::size_t>(-1);
    gSequence.store(0);
    return;
  }
  ensureDirectoryExists(raw);
}

bool enabled() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gEnabled;
}

unsigned updateThresholdState(const std::size_t peakUsedBytes, const std::size_t largestFreeBlockBytes) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (!gEnabled) return kNoThresholdChange;

  unsigned flags = kNoThresholdChange;
  if (peakUsedBytes > gLastPeakUsedBytes) {
    gLastPeakUsedBytes = peakUsedBytes;
    flags |= kPeakUsedIncreased;
  }
  if (largestFreeBlockBytes < gLastMinLargestFreeBlockBytes) {
    gLastMinLargestFreeBlockBytes = largestFreeBlockBytes;
    flags |= kLargestFreeBlockDecreased;
  }
  return flags;
}

bool writeSnapshot(const Snapshot& snapshot, const char* reason) {
  std::string outputDir;
  std::uint64_t seq = 0;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gEnabled || gOutputDir.empty()) return false;
    outputDir = gOutputDir;
    seq = gSequence.fetch_add(1);
  }

  char suffix[96];
  std::snprintf(suffix, sizeof(suffix), "%s_%06llu_%s.svg", timestampPrefix().c_str(),
                static_cast<unsigned long long>(seq), sanitizeToken(reason).c_str());
  const std::string filePath = outputDir + "/" + suffix;
  const std::string svg = renderSvg(snapshot, reason);

  FILE* fp = std::fopen(filePath.c_str(), "wb");
  if (!fp) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gFailureLogged) {
      std::fprintf(stderr, "[SIM] failed to write heap SVG %s errno=%d; disabling SVG dumps\n", filePath.c_str(), errno);
      gFailureLogged = true;
    }
    gEnabled = false;
    gOutputDir.clear();
    return false;
  }

  const std::size_t written = std::fwrite(svg.data(), 1, svg.size(), fp);
  std::fclose(fp);
  return written == svg.size();
}

#ifdef SIMULATOR_HEAP_TESTING
std::string outputDirForTests() {
  std::lock_guard<std::mutex> lock(gMutex);
  return gOutputDir;
}
#endif

}  // namespace SimulatorHeapViz

#endif
