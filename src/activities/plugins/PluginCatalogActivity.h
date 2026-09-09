#pragma once
#include <I18n.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "activities/CatalogActivity.h"
#include "network/HttpDownloader.h"
#include "util/PluginHttp.h"

class HalFile;
namespace freeink {
class SecureHttpClient;
}

// Picker entry; an empty manifestPath marks an inert browser-only plugin.
struct PluginRef {
  std::string name;          // folder name
  std::string title;         // from device.json or manifest.json (falls back to name)
  std::string description;   // one-line summary, if provided
  std::string manifestPath;  // device.json path, "" for a web-only plugin
};

// Rescan plugin roots on demand; metadata is owned by the picker.
std::vector<PluginRef> discoverPlugins();

// True if a plugin.js or device.json is installed; reads no manifests.
bool anyPluginInstalled();

// Manifest-driven picker, catalog, download, and sign-in screens.
// See docs/sd-plugins.md for the device.json schema.
class PluginCatalogActivity final : public CatalogActivity {
 public:
  // showOpds adds the OPDS row; rootMode returns Home instead of popping to Settings.
  // Out-of-line construction/destruction needs the complete SecureHttpClient type.
  explicit PluginCatalogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool showOpds = false,
                                 bool rootMode = false);
  ~PluginCatalogActivity() override;

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  struct Manifest {
    // Shared on-SD token and flat {cfg.KEY} configuration.
    std::string tokenFile, tokenPath;
    std::string configFile;
    // JSON uses dotted field paths; XML uses element/attribute selectors.
    std::string browseFormat;
    // Browse request: templates may use {token}, {cfg.KEY}, {page}, {limit}.
    pluginhttp::RequestSpec browseReq;
    std::string itemsPath;  // JSON: dotted path to the item array; "" = response root
    // JSON field paths (dotted); XML field selectors ("elem", "elem@attr", "@attr").
    std::string titlePath, authorPath, idPath, urlPath;
    // Compare this item field against the installed manifest version for badges.
    std::string versionPath;
    bool tracksInstalls() const { return !versionPath.empty(); }
    int pageSize = 8;
    // Named JSON views override the browse URL/body; Back returns to their picker.
    struct BrowseList {
      std::string title, url, body;
    };
    std::vector<BrowseList> browseLists;
    // JSON search overrides URL/body independently; empty fields reuse browse.
    std::string searchUrl, searchBody;
    bool hasSearch() const { return (!searchUrl.empty() || !searchBody.empty()) && browseFormat != "xml"; }
    // XML list options:
    std::string xmlItem;                     // local-name of the repeating element (required)
    std::string xmlContainer;                // local-name whose presence marks a navigable folder
    bool xmlSkipSelf = false;                // drop the entry whose url equals the request url
    bool xmlResolveUrls = false;             // resolve url field against the request origin
    std::vector<std::string> xmlExtensions;  // allowed file extensions ("" = all)

    bool isXmlList() const { return browseFormat == "xml"; }
    // Empty dlUrlPath means a direct URL; otherwise resolve it through an API hop.
    pluginhttp::RequestSpec downloadReq;
    std::string dlUrlPath;
    // Optional HTTP Basic credentials for the file GET; templates.
    std::string dlUser, dlPass;
    std::string destDir, filenameTpl;
    // Bundle item fields: base URL and relative paths, installed under destDir/subdir.
    std::string bundleBasePath, bundleFilesPath, bundleSubdir;
    bool isBundle() const { return !bundleFilesPath.empty(); }
    // Optional sidecar written after a successful download; templates may use
    // {id}, {title}, {md5} (MD5 of the destination path).
    std::string sidecarPath, sidecarBody;
    // Both grants write the shared token file; device_code also shows a QR/code.
    std::string authType;  // "device_code" (default) or "password"
    pluginhttp::RequestSpec authReq, pollReq;
    std::string authCodePath, authVerifyPath, authDeviceCodePath;
    std::string authIntervalPath, authExpiresPath, authTokenPath, authErrorPath;

    bool hasDeviceCode() const { return authType == "device_code" && !authReq.url.empty() && !pollReq.url.empty(); }
    bool hasPasswordGrant() const { return authType == "password" && !authReq.url.empty(); }
  };

  struct Item {
    std::string title, author, id, url;
    std::string version;  // catalog version (tracksInstalls only)
    std::string status;   // computed install/update badge shown in the row value
    bool isDir = false;   // a container/folder (navigable), not a downloadable file
    // Bundle download only: base URL + relative file paths for this item.
    std::string base;
    std::vector<std::string> files;
  };

  std::string manifestPath;  // empty while the picker is showing
  std::string catalogTitle;
  Manifest manifest;
  std::vector<PluginRef> installedPlugins;
  bool showOpds = false;
  bool rootMode = false;
  int pickerReturnRow = 0;  // picker row to reselect after leaving a catalog
  std::vector<Item> items;
  // Row buffer over items/browseLists plus the synthetic pager rows; rebuilt
  // lazily on the render task whenever rowsDirty (items or state changed).
  std::vector<freeink::ui::ListItem> rowItems;
  bool rowsDirty = true;
  std::string token;
  pluginhttp::Headers config;  // {cfg.KEY} values
  int page = 1;
  bool hasMore = false;
  int currentList = -1;  // index into manifest.browseLists; -1 = none/default
  std::string searchQuery;
  bool searchActive = false;
  // Reuse browse TLS; release in the picker and before a single-file download.
  std::unique_ptr<freeink::SecureHttpClient> session;
  // XML-list folder navigation: current container URL and the trail back out.
  std::string browseCurrentUrl;
  std::vector<std::string> browseHistory;
  // Device-code sign-in state
  std::string authUserCode, authVerifyUrl, authDeviceCode;
  unsigned long authIntervalMs = 5000;
  unsigned long authNextPollMs = 0;
  unsigned long authDeadlineMs = 0;
  // QR placement measured by buildScreen (AUTH state); drawn as a raw-renderer
  // overlay in render() after the app has painted.
  freeink::ui::Rect authQrRect{};

  void enterPluginPicker();
  void enterCatalog();
  void exitCatalog();
  bool loadManifest();
  bool loadToken();
  void loadConfig();
  bool saveToken(const std::string& value);
  // Show named lists when present, otherwise fetch the first page.
  void startBrowse() override;
  void retryBrowse() override;
  bool hasSearch() const override { return manifest.hasSearch(); }
  void performSearch(const std::string& query) override;
  void downloadFinished(bool cancelled) override;
  // Header label for the browsing screen (list title / search / page suffix).
  std::string browsingHeaderLabel() const;
  // JSON pagination adds rows before/after the current items.
  bool prevRowVisible() const;
  bool nextRowVisible() const;
  // Zero outside list states disables the base list protocol.
  int rowCount() const;
  int listCount() const override { return rowCount(); }
  // Row dispatch: pager rows page, picker rows pick, item rows open/download.
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void drawFooter() override;
  void rebuildRowItems();
  // Items (and the interaction table indexing them) are about to be replaced:
  // stop routing and mark the row buffer for rebuild.
  void releaseRows();
  void buildAuthScreen(UiScreen& screen);
  void buildBrowsingScreen(UiScreen& screen);
  // Browse url/body with the selected browse list's overrides applied.
  const std::string& activeBrowseUrl() const;
  const std::string& activeBrowseBody() const;
  // Copies `headers` with `substituted()` applied to each value.
  pluginhttp::Headers substitutedHeaders(const pluginhttp::Headers& headers, const Item* item) const;
  void fetchPage(int newPage);
  // Read installed versions once per page, outside the render path.
  void computeInstallStatus();
  bool parseXmlList();
  bool parseBrowseResponse();
  void activateItem(int itemIndex);  // XML list: navigate into a folder, else download
  void downloadItem(int itemIndex);
  HttpDownloader::DownloadError downloadBundle(const Item& item);
  HttpDownloader::DownloadError downloadBook(const Item& item);
  void beginAuth();
  void pollAuth();
  bool refreshCredentialToken();  // password grant: mint a token from config creds
  // Load credentials and stream a JSON/XML browse response to the SD temp file.
  // Retries a password grant once on 401/403; displays failures and returns false.
  bool fetchBrowseResponse();
  int apiRequest(const pluginhttp::RequestSpec& req, String& out);
  pluginhttp::RequestSpec substitutedRequest(const pluginhttp::RequestSpec& req, const Item* item = nullptr) const;
  std::string substituted(std::string tpl, const Item* item) const;
};
