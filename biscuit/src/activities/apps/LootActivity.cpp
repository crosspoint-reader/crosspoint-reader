#include "LootActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void LootActivity::onEnter() {
    Activity::onEnter();
    state = OVERVIEW;
    overviewIndex = 0;
    countFiles();
    requestUpdate();
}

void LootActivity::onExit() {
    Activity::onExit();
}

// ---------------------------------------------------------------------------
// Data loading
// ---------------------------------------------------------------------------

void LootActivity::countFiles() {
    // Count handshakes
    handshakeCount = 0;
    {
        HalFile dir = Storage.open("/biscuit/loot/handshakes");
        if (dir) {
            HalFile entry;
            while ((entry = dir.openNextFile())) {
                if (!entry.isDirectory()) handshakeCount++;
                entry.close();
            }
            dir.close();
        }
    }

    // Count PCAP files
    pcapCount = 0;
    {
        HalFile dir = Storage.open("/biscuit/pcap");
        if (dir) {
            HalFile entry;
            while ((entry = dir.openNextFile())) {
                if (!entry.isDirectory()) pcapCount++;
                entry.close();
            }
            dir.close();
        }
    }

    // Count BLE dumps
    bleDumpCount = 0;
    {
        HalFile dir = Storage.open("/biscuit/loot/ble");
        if (dir) {
            HalFile entry;
            while ((entry = dir.openNextFile())) {
                if (!entry.isDirectory()) bleDumpCount++;
                entry.close();
            }
            dir.close();
        }
    }

    // Count credential lines (excluding header)
    credentialCount = 0;
    {
        HalFile file = Storage.open("/biscuit/creds.csv");
        if (file) {
            bool firstLine = true;
            int ch;
            bool onNewLine = true;
            while ((ch = file.read()) >= 0) {
                if (ch == '\n') {
                    if (!onNewLine) {
                        if (firstLine) {
                            firstLine = false;
                        } else {
                            credentialCount++;
                        }
                    }
                    onNewLine = true;
                } else if (ch != '\r') {
                    onNewLine = false;
                }
            }
            // Handle last line without trailing newline
            if (!onNewLine && !firstLine) {
                credentialCount++;
            }
            file.close();
        }
    }
}

void LootActivity::loadHandshakes() {
    fileCount = 0;
    HalFile dir = Storage.open("/biscuit/loot/handshakes");
    if (!dir) return;
    HalFile entry;
    while ((entry = dir.openNextFile()) && fileCount < MAX_FILES) {
        if (!entry.isDirectory()) {
            entry.getName(fileNames[fileCount], 31);
            fileNames[fileCount][31] = '\0';
            fileCount++;
        }
        entry.close();
    }
    dir.close();
}

void LootActivity::loadPcapFiles() {
    fileCount = 0;
    HalFile dir = Storage.open("/biscuit/pcap");
    if (!dir) return;
    HalFile entry;
    while ((entry = dir.openNextFile()) && fileCount < MAX_FILES) {
        if (!entry.isDirectory()) {
            entry.getName(fileNames[fileCount], 31);
            fileNames[fileCount][31] = '\0';
            fileCount++;
        }
        entry.close();
    }
    dir.close();
}

void LootActivity::loadBleDumps() {
    fileCount = 0;
    HalFile dir = Storage.open("/biscuit/loot/ble");
    if (!dir) return;
    HalFile entry;
    while ((entry = dir.openNextFile()) && fileCount < MAX_FILES) {
        if (!entry.isDirectory()) {
            entry.getName(fileNames[fileCount], 31);
            fileNames[fileCount][31] = '\0';
            fileCount++;
        }
        entry.close();
    }
    dir.close();
}

void LootActivity::loadCredentials() {
    credCount = 0;

    // Use readFile to get the whole content (bounded by available RAM, but creds.csv is small)
    String content = Storage.readFile("/biscuit/creds.csv");
    if (content.length() == 0) return;

    const int len = static_cast<int>(content.length());
    int lineStart = 0;
    bool firstLine = true;

    for (int i = 0; i <= len && credCount < MAX_FILES; i++) {
        if (i == len || content[i] == '\n') {
            int lineEnd = i;
            // Strip trailing \r
            if (lineEnd > lineStart && content[lineEnd - 1] == '\r') lineEnd--;

            if (lineEnd > lineStart) {
                if (firstLine) {
                    firstLine = false;
                } else {
                    Credential& cr = creds[credCount];
                    cr.timestamp[0] = '\0';
                    cr.ssid[0] = '\0';
                    cr.username[0] = '\0';
                    cr.password[0] = '\0';

                    int fieldStart = lineStart;
                    int field = 0;

                    for (int j = lineStart; j <= lineEnd; j++) {
                        if (j == lineEnd || content[j] == ',') {
                            int flen = j - fieldStart;
                            switch (field) {
                                case 0: {
                                    int copy = flen < 19 ? flen : 19;
                                    memcpy(cr.timestamp, content.c_str() + fieldStart, copy);
                                    cr.timestamp[copy] = '\0';
                                    break;
                                }
                                case 1: {
                                    int copy = flen < 32 ? flen : 32;
                                    memcpy(cr.ssid, content.c_str() + fieldStart, copy);
                                    cr.ssid[copy] = '\0';
                                    break;
                                }
                                case 2: {
                                    int copy = flen < 32 ? flen : 32;
                                    memcpy(cr.username, content.c_str() + fieldStart, copy);
                                    cr.username[copy] = '\0';
                                    break;
                                }
                                case 3: {
                                    int copy = flen < 32 ? flen : 32;
                                    memcpy(cr.password, content.c_str() + fieldStart, copy);
                                    cr.password[copy] = '\0';
                                    break;
                                }
                                default: break;
                            }
                            field++;
                            fieldStart = j + 1;
                        }
                    }

                    if (field >= 2) credCount++;
                }
            }
            lineStart = i + 1;
        }
    }
}

void LootActivity::loadFileDetail(int index) {
    if (index < 0 || index >= fileCount) {
        snprintf(detailBuf, sizeof(detailBuf), "(no file selected)");
        return;
    }

    detailScroll = 0;
    detailAction = VIEW;

    if (state == ITEM_DETAIL && listCategory == CREDENTIALS) {
        // Credential detail — show full entry
        if (index < credCount) {
            const Credential& cr = creds[index];
            snprintf(detailBuf, sizeof(detailBuf),
                     "SSID: %s\nUser: %s\nPass: %s\nTime: %s",
                     cr.ssid[0] ? cr.ssid : "(empty)",
                     cr.username[0] ? cr.username : "(empty)",
                     cr.password[0] ? cr.password : "(empty)",
                     cr.timestamp[0] ? cr.timestamp : "(unknown)");
        } else {
            snprintf(detailBuf, sizeof(detailBuf), "(credential not found)");
        }
        return;
    }

    // Build path based on current list category
    char path[96];
    if (listCategory == HANDSHAKES) {
        snprintf(path, sizeof(path), "/biscuit/loot/handshakes/%s", fileNames[index]);
    } else if (listCategory == PCAP_FILES) {
        snprintf(path, sizeof(path), "/biscuit/pcap/%s", fileNames[index]);
    } else if (listCategory == BLE_DUMPS) {
        snprintf(path, sizeof(path), "/biscuit/loot/ble/%s", fileNames[index]);
    } else {
        snprintf(detailBuf, sizeof(detailBuf), "(unknown category)");
        return;
    }

    HalFile file = Storage.open(path);
    if (!file) {
        snprintf(detailBuf, sizeof(detailBuf), "Cannot open:\n%s", fileNames[index]);
        return;
    }

    size_t fsize = file.size();
    file.close();

    if (listCategory == HANDSHAKES) {
        // Show filename, size, completeness hint
        // .hccapx files are 393 bytes per handshake when complete
        const char* status = (fsize >= 393) ? "Complete" : "Partial";
        snprintf(detailBuf, sizeof(detailBuf),
                 "File: %s\nSize: %u bytes\nStatus: %s",
                 fileNames[index], (unsigned)fsize, status);
    } else if (listCategory == PCAP_FILES) {
        // Rough packet estimate: average Ethernet frame ~100 bytes, minus 24-byte pcap global header
        int packetEst = fsize > 24 ? (int)((fsize - 24) / 100) : 0;
        snprintf(detailBuf, sizeof(detailBuf),
                 "File: %s\nSize: %u bytes\n~%d packets",
                 fileNames[index], (unsigned)fsize, packetEst);
    } else {
        snprintf(detailBuf, sizeof(detailBuf),
                 "File: %s\nSize: %u bytes",
                 fileNames[index], (unsigned)fsize);
    }
}

void LootActivity::exportSummaryReport() {
    Storage.mkdir("/biscuit/loot");

    HalFile f = Storage.open("/biscuit/loot/report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return;

    f.println("=== LOOT SUMMARY REPORT ===");
    char line[64];
    snprintf(line, sizeof(line), "Handshakes : %d", handshakeCount);
    f.println(line);
    snprintf(line, sizeof(line), "Credentials: %d", credentialCount);
    f.println(line);
    snprintf(line, sizeof(line), "PCAP Files : %d", pcapCount);
    f.println(line);
    snprintf(line, sizeof(line), "BLE Dumps  : %d", bleDumpCount);
    f.println(line);
    snprintf(line, sizeof(line), "Total      : %d", handshakeCount + credentialCount + pcapCount + bleDumpCount);
    f.println(line);

    f.flush();
    f.close();
}

void LootActivity::executeDetailAction() {
    if (fileIndex < 0 || fileIndex >= fileCount) return;

    if (detailAction == VIEW) {
        // Already displaying content — no further action needed
        return;
    }

    if (detailAction == DELETE) {
        char path[96];
        if (listCategory == HANDSHAKES) {
            snprintf(path, sizeof(path), "/biscuit/loot/handshakes/%s", fileNames[fileIndex]);
        } else if (listCategory == PCAP_FILES) {
            snprintf(path, sizeof(path), "/biscuit/pcap/%s", fileNames[fileIndex]);
        } else if (listCategory == BLE_DUMPS) {
            snprintf(path, sizeof(path), "/biscuit/loot/ble/%s", fileNames[fileIndex]);
        } else {
            return;
        }
        Storage.remove(path);
        // Reload the list and go back
        if (listCategory == HANDSHAKES) loadHandshakes();
        else if (listCategory == PCAP_FILES) loadPcapFiles();
        else if (listCategory == BLE_DUMPS) loadBleDumps();
        fileIndex = 0;
        state = listCategory;
        countFiles();
        requestUpdate();
        return;
    }

    if (detailAction == EXPORT && listCategory == HANDSHAKES) {
        // Copy handshake to hashcat directory
        char srcPath[96];
        char dstPath[96];
        snprintf(srcPath, sizeof(srcPath), "/biscuit/loot/handshakes/%s", fileNames[fileIndex]);
        snprintf(dstPath, sizeof(dstPath), "/biscuit/loot/hashcat/%s", fileNames[fileIndex]);
        Storage.mkdir("/biscuit/loot/hashcat");

        HalFile src = Storage.open(srcPath);
        if (src) {
            HalFile dst = Storage.open(dstPath, O_WRITE | O_CREAT | O_TRUNC);
            if (dst) {
                char buf[256];
                int n;
                while ((n = src.read(buf, sizeof(buf))) > 0) {
                    dst.write(buf, n);
                }
                dst.flush();
                dst.close();
            }
            src.close();
        }
        snprintf(detailBuf, sizeof(detailBuf), "Exported to:\n/biscuit/loot/hashcat/\n%s", fileNames[fileIndex]);
        requestUpdate();
        return;
    }

    if (detailAction == EXPORT && listCategory == CREDENTIALS) {
        // Build WiFi QR string for the credential
        if (fileIndex < credCount) {
            const Credential& cr = creds[fileIndex];
            snprintf(detailBuf, sizeof(detailBuf),
                     "WIFI:T:WPA;\nS:%s;\nP:%s;;",
                     cr.ssid, cr.password);
        }
        requestUpdate();
        return;
    }
}

// ---------------------------------------------------------------------------
// Loop — button handling
// ---------------------------------------------------------------------------

void LootActivity::loop() {
    switch (state) {
        case OVERVIEW: {
            buttonNavigator.onNext([this] {
                overviewIndex = ButtonNavigator::nextIndex(overviewIndex, CATEGORY_COUNT);
                requestUpdate();
            });
            buttonNavigator.onPrevious([this] {
                overviewIndex = ButtonNavigator::previousIndex(overviewIndex, CATEGORY_COUNT);
                requestUpdate();
            });

            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                fileIndex = 0;
                switch (overviewIndex) {
                    case 0:
                        loadHandshakes();
                        listCategory = HANDSHAKES;
                        state = HANDSHAKES;
                        break;
                    case 1:
                        loadCredentials();
                        listCategory = CREDENTIALS;
                        state = CREDENTIALS;
                        break;
                    case 2:
                        loadPcapFiles();
                        listCategory = PCAP_FILES;
                        state = PCAP_FILES;
                        break;
                    case 3:
                        loadBleDumps();
                        listCategory = BLE_DUMPS;
                        state = BLE_DUMPS;
                        break;
                    default: break;
                }
                requestUpdate();
            }

            if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
                exportSummaryReport();
                // Show brief confirmation by re-rendering with a popup — done via detailBuf trick
                snprintf(detailBuf, sizeof(detailBuf), "Report saved to\n/biscuit/loot/report.txt");
                requestUpdate();
            }

            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                finish();
            }
            break;
        }

        case HANDSHAKES:
        case PCAP_FILES:
        case BLE_DUMPS: {
            buttonNavigator.onNext([this] {
                if (fileCount > 0) {
                    fileIndex = ButtonNavigator::nextIndex(fileIndex, fileCount);
                    requestUpdate();
                }
            });
            buttonNavigator.onPrevious([this] {
                if (fileCount > 0) {
                    fileIndex = ButtonNavigator::previousIndex(fileIndex, fileCount);
                    requestUpdate();
                }
            });

            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                if (fileCount > 0) {
                    loadFileDetail(fileIndex);
                    state = ITEM_DETAIL;
                    requestUpdate();
                }
            }

            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                detailBuf[0] = '\0';
                state = OVERVIEW;
                requestUpdate();
            }
            break;
        }

        case CREDENTIALS: {
            buttonNavigator.onNext([this] {
                if (credCount > 0) {
                    fileIndex = ButtonNavigator::nextIndex(fileIndex, credCount);
                    requestUpdate();
                }
            });
            buttonNavigator.onPrevious([this] {
                if (credCount > 0) {
                    fileIndex = ButtonNavigator::previousIndex(fileIndex, credCount);
                    requestUpdate();
                }
            });

            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                if (credCount > 0) {
                    loadFileDetail(fileIndex);
                    state = ITEM_DETAIL;
                    requestUpdate();
                }
            }

            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                detailBuf[0] = '\0';
                state = OVERVIEW;
                requestUpdate();
            }
            break;
        }

        case ITEM_DETAIL: {
            if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
                // Cycle through actions: VIEW -> EXPORT -> DELETE -> EXPORT
                // (skip VIEW on wrap — content is already displayed)
                switch (detailAction) {
                    case VIEW:   detailAction = EXPORT; break;
                    case EXPORT: detailAction = DELETE; break;
                    case DELETE: detailAction = EXPORT; break;
                }
                requestUpdate();
            }

            if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
                executeDetailAction();
            }

            if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
                state = listCategory;
                requestUpdate();
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------

void LootActivity::renderOverview() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Loot - Captured Data");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    // Category counts
    const int counts[CATEGORY_COUNT] = {handshakeCount, credentialCount, pcapCount, bleDumpCount};
    static const char* const labels[CATEGORY_COUNT] = {"Handshakes", "Credentials", "PCAP Files", "BLE Dumps"};

    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight},
        CATEGORY_COUNT, overviewIndex,
        [&](int i) -> std::string {
            return labels[i];
        },
        [&](int i) -> std::string {
            char sub[16];
            snprintf(sub, sizeof(sub), "[%d]", counts[i]);
            return sub;
        });

    // Total line
    int total = handshakeCount + credentialCount + pcapCount + bleDumpCount;
    char totalBuf[48];
    snprintf(totalBuf, sizeof(totalBuf), "Total captured items: %d", total);
    renderer.drawCenteredText(SMALL_FONT_ID,
        pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 18,
        totalBuf);

    // Show confirmation message if report was just exported (detailBuf is non-empty)
    if (detailBuf[0] != '\0') {
        GUI.drawPopup(renderer, detailBuf);
    }

    const auto btnLabels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, btnLabels.btn1, btnLabels.btn2, btnLabels.btn3, btnLabels.btn4);
    GUI.drawSideButtonHints(renderer, "Report", "");
}

void LootActivity::renderFileList(const char* title) const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, title);

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (fileCount == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "No files found");
    } else {
        GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight},
            fileCount, fileIndex,
            [this](int i) -> std::string {
                return fileNames[i];
            });
    }

    const auto btnLabels = mappedInput.mapLabels("Back", "Detail", "^", "v");
    GUI.drawButtonHints(renderer, btnLabels.btn1, btnLabels.btn2, btnLabels.btn3, btnLabels.btn4);
}

void LootActivity::renderCredentialList() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    char subtitle[24];
    snprintf(subtitle, sizeof(subtitle), "%d entries", credCount);
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Credentials", subtitle);

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (credCount == 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, "No credentials found");
    } else {
        GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight},
            credCount, fileIndex,
            [this](int i) -> std::string {
                return creds[i].ssid[0] ? creds[i].ssid : "(no ssid)";
            },
            [this](int i) -> std::string {
                char sub[48];
                const char* user = creds[i].username[0] ? creds[i].username : "(no user)";
                snprintf(sub, sizeof(sub), "%s  ****", user);
                return sub;
            });
    }

    const auto btnLabels = mappedInput.mapLabels("Back", "View", "^", "v");
    GUI.drawButtonHints(renderer, btnLabels.btn1, btnLabels.btn2, btnLabels.btn3, btnLabels.btn4);
}

void LootActivity::renderDetail() const {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Detail");

    const int leftPad = metrics.contentSidePadding;
    int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 16;
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int maxWidth = pageWidth - 2 * leftPad;

    // Render detailBuf with newline wrapping
    const char* p = detailBuf;
    char lineBuf[64];
    while (*p && y < pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing) {
        // Find end of this logical line
        const char* nl = p;
        while (*nl && *nl != '\n') nl++;
        int segLen = (int)(nl - p);
        if (segLen >= (int)sizeof(lineBuf)) segLen = (int)sizeof(lineBuf) - 1;
        memcpy(lineBuf, p, segLen);
        lineBuf[segLen] = '\0';

        // Word-wrap within the segment using renderer text width
        // Simple approach: render as-is (the detail lines are short by design)
        renderer.drawText(UI_10_FONT_ID, leftPad, y, lineBuf);
        y += lineH + 4;

        p = (*nl == '\n') ? nl + 1 : nl;
        if (!*p) break;
    }
    (void)maxWidth;  // available for future word-wrap use

    // Action selector bar
    static const char* const actionLabels[] = {"View", "Export", "Delete"};
    char actionBuf[48];
    snprintf(actionBuf, sizeof(actionBuf), "Action: [%s]", actionLabels[detailAction]);
    renderer.drawCenteredText(SMALL_FONT_ID,
        pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 20,
        actionBuf);

    static const char* const actionLabelsExec[] = {"", "Export", "Delete"};
    const char* execLabel = (detailAction < 3) ? actionLabelsExec[detailAction] : "";
    const auto btnLabels = mappedInput.mapLabels("Back", "Next", "", "");
    GUI.drawButtonHints(renderer, btnLabels.btn1, btnLabels.btn2, btnLabels.btn3, btnLabels.btn4);
    GUI.drawSideButtonHints(renderer, execLabel, "");
}

// ---------------------------------------------------------------------------
// Render dispatch
// ---------------------------------------------------------------------------

void LootActivity::render(RenderLock&&) {
    renderer.clearScreen();

    switch (state) {
        case OVERVIEW:    renderOverview(); break;
        case HANDSHAKES:  renderFileList("Handshakes"); break;
        case PCAP_FILES:  renderFileList("PCAP Files"); break;
        case BLE_DUMPS:   renderFileList("BLE Dumps"); break;
        case CREDENTIALS: renderCredentialList(); break;
        case ITEM_DETAIL: renderDetail(); break;
    }

    renderer.displayBuffer();
}
