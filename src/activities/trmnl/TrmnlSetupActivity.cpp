#include "TrmnlSetupActivity.h"
#include "trmnl/TrmnlService.h"
#include "fontIds.h"
#include "components/UITheme.h"
#include <WiFi.h>
#include <HalStorage.h>

TrmnlSetupActivity::TrmnlSetupActivity(GfxRenderer& renderer, MappedInputManager& inputManager, std::function<void()> onBack)
    : Activity("TrmnlSetup", renderer, inputManager), onBack(onBack) {}

void TrmnlSetupActivity::onEnter() {
    Activity::onEnter();
    TrmnlService::loadConfig();
    displayingImage = false;
    requestUpdate();
}

void TrmnlSetupActivity::onExit() {
    Activity::onExit();
}

void TrmnlSetupActivity::loop() {
    if (displayingImage) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
            displayingImage = false;
            statusMessage = "";
            requestUpdate();
        }
        return;
    }

    buttonNavigator.onNext([this] {
        selectedIndex = ButtonNavigator::nextIndex(selectedIndex, 3);
        requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
        selectedIndex = ButtonNavigator::previousIndex(selectedIndex, 3);
        requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        onBack();
        return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        switch (selectedIndex) {
            case 0: toggleEnabled(); break;
            case 1: performRegistration(); break;
            case 2: testDownload(); break;
        }
    }
}

void TrmnlSetupActivity::toggleEnabled() {
    auto& config = TrmnlService::getConfig();
    config.enabled = !config.enabled;
    TrmnlService::saveConfig();
    requestUpdate();
}

void TrmnlSetupActivity::performRegistration() {
    statusMessage = "Connecting WiFi...";
    requestUpdate();
    renderer.displayBuffer(); // Force update

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        WiFi.begin();
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 20) {
            delay(500);
            retries++;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        statusMessage = "Registering...";
        requestUpdate();
        renderer.displayBuffer();
        
        if (TrmnlService::registerDevice()) {
            statusMessage = "Registration Success!";
        } else {
            statusMessage = "Registration Failed.";
        }
    } else {
        statusMessage = "WiFi Failed.";
    }
    requestUpdate();
}

void TrmnlSetupActivity::testDownload() {
    statusMessage = "Downloading...";
    requestUpdate();
    renderer.displayBuffer();

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        WiFi.begin();
        delay(2000);
    }

    if (TrmnlService::refreshScreen()) {
        statusMessage = "Download Success!";
        displayingImage = true;
    } else {
        statusMessage = "Download Failed.";
    }
    requestUpdate();
}

static void drawBmpFromFile(GfxRenderer& renderer, const char* filename, int x, int y) {
    FsFile file;
    if (!Storage.openFileForRead("TRMNL", filename, file)) {
        renderer.drawCenteredText(UI_10_FONT_ID, 100, "File not found");
        return;
    }

    uint8_t header[54];
    if (file.read(header, 54) != 54) { file.close(); return; }

    if (header[0] != 'B' || header[1] != 'M') { file.close(); return; }

    uint32_t dataOffset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
    int32_t width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
    int32_t height = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
    uint16_t bpp = header[28] | (header[29] << 8);
    
    if (bpp != 1) {
        file.close();
        renderer.drawCenteredText(UI_10_FONT_ID, 100, "Unsupported BMP BPP");
        return;
    }

    bool flipY = true;
    if (height < 0) {
        height = -height;
        flipY = false;
    }

    int rowSize = (width * bpp + 31) / 32 * 4;
    uint8_t* lineBuffer = new uint8_t[rowSize];

    file.seek(dataOffset);

    for (int row = 0; row < height; row++) {
        file.read(lineBuffer, rowSize);
        int drawY = y + (flipY ? (height - 1 - row) : row);
        
        for (int col = 0; col < width; col++) {
            int byteIdx = col / 8;
            int bitIdx = 7 - (col % 8);
            bool pixel = (lineBuffer[byteIdx] >> bitIdx) & 1;
            renderer.drawPixel(x + col, drawY, pixel ? 1 : 0);
        }
    }

    delete[] lineBuffer;
    file.close();
}

void TrmnlSetupActivity::render(Activity::RenderLock&& lock) {
    renderer.clearScreen();

    if (displayingImage) {
        drawBmpFromFile(renderer, "/sleep.bmp", 0, 0);
        GUI.drawButtonHints(renderer, "Back", "", "", "");
        renderer.displayBuffer();
        return;
    }
    
    renderer.drawCenteredText(UI_12_FONT_ID, 40, "TRMNL Setup", true, EpdFontFamily::BOLD);
    
    auto& config = TrmnlService::getConfig();
    std::string mac = "MAC: " + TrmnlService::getMacAddress();
    renderer.drawCenteredText(UI_10_FONT_ID, 80, mac.c_str());
    
    std::string url = "Server: " + config.serverUrl;
    renderer.drawCenteredText(SMALL_FONT_ID, 100, url.c_str());

    int y = 150;
    int h = 40;
    int w = renderer.getScreenWidth();
    
    std::string enableStr = std::string("Enable Service: ") + (config.enabled ? "ON" : "OFF");
    if (selectedIndex == 0) renderer.fillRect(20, y - 5, w - 40, h);
    renderer.drawText(UI_10_FONT_ID, 40, y, enableStr.c_str(), selectedIndex != 0);
    
    if (selectedIndex == 1) renderer.fillRect(20, y + h - 5, w - 40, h);
    renderer.drawText(UI_10_FONT_ID, 40, y + h, "Register Device", selectedIndex != 1);
    
    if (selectedIndex == 2) renderer.fillRect(20, y + h * 2 - 5, w - 40, h);
    renderer.drawText(UI_10_FONT_ID, 40, y + h * 2, "Test Download", selectedIndex != 2);

    if (!statusMessage.empty()) {
        renderer.drawCenteredText(UI_10_FONT_ID, 400, statusMessage.c_str(), true, EpdFontFamily::BOLD);
    }

    GUI.drawButtonHints(renderer, "Back", "Select", "Up", "Down");
    renderer.displayBuffer();
}
