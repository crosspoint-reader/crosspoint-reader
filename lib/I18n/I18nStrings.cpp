#include "I18nStrings.h"

namespace i18n_strings {

const char *const STRINGS_EN[] = {
    // Boot/Sleep
    "CrossPoint", "BOOTING", "SLEEPING", "Entering Sleep...",

    // Home Menu
    "Browse Files", "File Transfer", "Settings", "Calibre Library",
    "Continue Reading", "No open book", "Start reading below",

    // File Browser
    "Books", "No books found",

    // Reader
    "Select Chapter", "No chapters", "End of book", "Empty chapter",
    "Indexing...", "Memory error", "Page load error", "Empty file",
    "Out of bounds", "Loading...", "Failed to load XTC", "Failed to load TXT",
    "Failed to load EPUB", "SD card error",

    // Network
    "WiFi Networks", "No networks found", "%zu networks found", "Scanning...",
    "Connecting...", "Connected!", "Connection Failed", "Connection timeout",
    "Forget Network?",
    "Save password for next time?", "Remove saved password?",
    "Press OK to scan again", "Press any button to continue",
    "LEFT/RIGHT: Select | OK: Confirm", "How would you like to connect?",
    "Join a Network", "Create Hotspot", "Connect to an existing WiFi network",
    "Create a WiFi network others can join", "Starting Hotspot...",
    "Hotspot Mode", "Connect your device to this WiFi network",
    "Open this URL in your browser", "or http://",
    "or scan QR code with your phone:", "Calibre Wireless", "Calibre Web URL",
    "Connect as Wireless Device", "* = Encrypted | + = Saved", "MAC address:",
    "Checking WiFi...", "Enter WiFi Password", "Enter Text", "to ",

    // Calibre Wireless
    "Discovering Calibre...", "Connecting to ", "Connected to ",
    "Waiting for commands...", "(Connection failed, retrying)",
    "Calibre disconnected", "Waiting for transfer...",
    "If transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice plugin settings.",
    "Receiving: ", "Received: ", "Waiting for more...",
    "Failed to create file", "Password required", "Transfer interrupted",
    "1) Install CrossPoint Reader plugin", "2) Be on the same WiFi network",
    "3) In Calibre: \"Send to device\"", "Keep this screen open while sending",

    // Settings Categories
    "Display", "Reader", "Controls", "System",

    // Settings
    "Sleep Screen", "Sleep Screen Cover Mode", "Status Bar", "Hide Battery %",
    "Extra Paragraph Spacing", "Text Anti-Aliasing", "Short Power Button Click",
    "Reading Orientation", "Front Button Layout", "Side Button Layout (reader)",
    "Long-press Chapter Skip", "Reader Font Family", "External Reader Font", "Reader Font", "UI Font",
    "UI Font Size", "Reader Line Spacing",
    "ASCII Letter Spacing", "ASCII Digit Spacing", "CJK Spacing", "Color Mode",
    "Reader Screen Margin", "Reader Paragraph Alignment", "Hyphenation", "Time to Sleep",
    "Refresh Frequency", "Calibre Settings", "KOReader Sync", "Check for updates", "Language",
    "Select Wallpaper", "Clear Reading Cache",

    // Calibre Settings
    "Calibre",

    // KOReader Settings
    "Username", "Password", "Sync Server URL", "Document Matching", "Authenticate",
    "KOReader Username", "KOReader Password", "Filename", "Binary", "Set credentials first",

    // KOReader Auth
    "WiFi connection failed", "Authenticating...", "Successfully authenticated!",
    "KOReader Auth", "KOReader sync is ready to use", "Authentication Failed", "Done",

    // Clear Cache
    "This will clear all cached book data.", "All reading progress will be lost!",
    "Books will need to be re-indexed", "when opened again.",
    "Clearing cache...", "Cache Cleared", "items removed", "failed",
    "Failed to clear cache", "Check serial output for details",

    // Setting Values
    "Dark", "Light", "Custom", "Cover", "None", "Fit", "Crop", "No Progress",
    "Full", "Never", "In Reader", "Always", "Ignore", "Sleep", "Page Turn",
    "Portrait", "Landscape CW", "Inverted", "Landscape CCW",
    "Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght",
    "Prev/Next", "Next/Prev", "Bookerly", "Noto Sans", "Open Dyslexic", "Small",
    "Medium",
    "Large", "X Large", "Tight", "Normal", "Wide", "Justify", "Left", "Center",
    "Right", "1 min", "5 min", "10 min", "15 min", "30 min", "1 page",
    "5 pages", "10 pages", "15 pages", "30 pages",

    // OTA Update
    "Update", "Checking for update...", "New update available!",
    "Current Version: ", "New Version: ", "Updating...", "No update available",
    "Update failed", "Update complete",
    "Press and hold power button to turn back on",

    // Font Selection
    "External Font", "Built-in (Disabled)",

    // OPDS Browser
    "No entries found", "Downloading...", "Download failed", "Error:",
    "Unnamed", "No server URL configured", "Failed to fetch feed",
    "Failed to parse feed", "Network: ", "IP Address: ",
    "or scan QR code with your phone to connect to Wifi.",
    "Error: General failure", "Error: Network not found", "Error: Connection timeout",
    "SD card",

    // Buttons
    "\xC2\xAB Back", // « Back
    "\xC2\xAB Exit", // « Exit
    "\xC2\xAB Home", // « Home
    "\xC2\xAB Save", // « Save
    "Select", "Toggle", "Confirm", "Cancel", "Connect", "Open", "Download",
    "Retry", "Yes", "No", "ON", "OFF", "Set", "Not Set", "Left", "Right", "Up",
    "Down", "CAPS", "caps", "OK",

    // Languages
    "English", "Espa\xC3\xB1ol", "Italiano", "Svenska", "Fran\xC3\xA7\x61is",

    // Marker
    "[ON]",

    // Master branch specific additions
    "Sleep Screen Cover Filter", "Contrast",
    "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar",
    "UI Theme", "Classic", "Lyra",
    "Sunlight Fading Fix",
    "Remap Front Buttons", "OPDS Browser",
    "Cover + Custom",
    "Recents",
    "Recent Books",
    "No recent books",
    "Use Calibre wireless device transfers",
    "Forget network and remove saved password?",
    "Forget network",
    "Starting Calibre...",
    "Setup",
    "Status",
    "Clear",
    "Default",
    "Press a front button for each role",
    "Unassigned",
    "Already assigned",
    "Side button Up: Reset to default layout",
    "Side button Down: Cancel remapping",
    "Back (1st button)",
    "Confirm (2nd button)",
    "Left (3rd button)",
    "Right (4th button)",
    "Go to %",
    "Go Home",
    "Sync Progress",
    "Delete Book Cache",
    "Chapter: ",
    " pages  |  ",
    "Book: ",
    "shift",
    "SHIFT",
    "LOCK",
    "For Calibre, add /opds to your URL",
    "Left/Right: 1%  Up/Down: 10%",
    "Syncing time...",
    "Calculating document hash...",
    "Failed to calculate document hash",
    "Fetching remote progress...",
    "Uploading progress...",
    "No credentials configured",
    "Set up KOReader account in Settings",
    "Progress found!",
    "Remote:",
    "Local:",
    "  Page %d, %.2f%% overall",
    "  Page %d/%d, %.2f%% overall",
    "  From: %s",
    "Apply remote progress",
    "Upload local progress",
    "No remote progress found",
    "Upload current position?",
    "Progress uploaded!",
    "Sync failed",
    "Section ",
    "Upload",
};

const char *const STRINGS_ES[] = {
    // Boot/Sleep
    "CrossPoint", "BOOTING", "SLEEPING", "Entering Sleep...",

    // Home Menu
    "Browse Files", "File Transfer", "Settings", "Calibre Library",
    "Continue Reading", "No open book", "Start reading below",

    // File Browser
    "Books", "No books found",

    // Reader
    "Select Chapter", "No chapters", "End of book", "Empty chapter",
    "Indexing...", "Memory error", "Page load error", "Empty file",
    "Out of bounds", "Loading...", "Failed to load XTC", "Failed to load TXT",
    "Failed to load EPUB", "SD card error",

    // Network
    "WiFi Networks", "No networks found", "%zu networks found", "Scanning...",
    "Connecting...", "Connected!", "Connection Failed", "Connection timeout",
    "Forget Network?",
    "Save password for next time?", "Remove saved password?",
    "Press OK to scan again", "Press any button to continue",
    "LEFT/RIGHT: Select | OK: Confirm", "How would you like to connect?",
    "Join a Network", "Create Hotspot", "Connect to an existing WiFi network",
    "Create a WiFi network others can join", "Starting Hotspot...",
    "Hotspot Mode", "Connect your device to this WiFi network",
    "Open this URL in your browser", "or http://",
    "or scan QR code with your phone:", "Calibre Wireless", "Calibre Web URL",
    "Connect as Wireless Device", "* = Encrypted | + = Saved", "MAC address:",
    "Checking WiFi...", "Enter WiFi Password", "Enter Text", "to ",

    // Calibre Wireless
    "Discovering Calibre...", "Connecting to ", "Connected to ",
    "Waiting for commands...", "(Connection failed, retrying)",
    "Calibre disconnected", "Waiting for transfer...",
    "If transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice plugin settings.",
    "Receiving: ", "Received: ", "Waiting for more...",
    "Failed to create file", "Password required", "Transfer interrupted",
    "1) Install CrossPoint Reader plugin", "2) Be on the same WiFi network",
    "3) In Calibre: \"Send to device\"", "Keep this screen open while sending",

    // Settings Categories
    "Display", "Reader", "Controls", "System",

    // Settings
    "Sleep Screen", "Sleep Screen Cover Mode", "Status Bar", "Hide Battery %",
    "Extra Paragraph Spacing", "Text Anti-Aliasing", "Short Power Button Click",
    "Reading Orientation", "Front Button Layout", "Side Button Layout (reader)",
    "Long-press Chapter Skip", "Reader Font Family", "External Reader Font", "Reader Font", "UI Font",
    "UI Font Size", "Reader Line Spacing",
    "ASCII Letter Spacing", "ASCII Digit Spacing", "CJK Spacing", "Color Mode",
    "Reader Screen Margin", "Reader Paragraph Alignment", "Hyphenation", "Time to Sleep",
    "Refresh Frequency", "Calibre Settings", "KOReader Sync", "Check for updates", "Language",
    "Select Wallpaper", "Clear Reading Cache",

    // Calibre Settings
    "Calibre",

    // KOReader Settings
    "Username", "Password", "Sync Server URL", "Document Matching", "Authenticate",
    "KOReader Username", "KOReader Password", "Filename", "Binary", "Set credentials first",

    // KOReader Auth
    "WiFi connection failed", "Authenticating...", "Successfully authenticated!",
    "KOReader Auth", "KOReader sync is ready to use", "Authentication Failed", "Done",

    // Clear Cache
    "This will clear all cached book data.", "All reading progress will be lost!",
    "Books will need to be re-indexed", "when opened again.",
    "Clearing cache...", "Cache Cleared", "items removed", "failed",
    "Failed to clear cache", "Check serial output for details",

    // Setting Values
    "Dark", "Light", "Custom", "Cover", "None", "Fit", "Crop", "No Progress",
    "Full", "Never", "In Reader", "Always", "Ignore", "Sleep", "Page Turn",
    "Portrait", "Landscape CW", "Inverted", "Landscape CCW",
    "Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght",
    "Prev/Next", "Next/Prev", "Bookerly", "Noto Sans", "Open Dyslexic", "Small",
    "Medium",
    "Large", "X Large", "Tight", "Normal", "Wide", "Justify", "Left", "Center",
    "Right", "1 min", "5 min", "10 min", "15 min", "30 min", "1 page",
    "5 pages", "10 pages", "15 pages", "30 pages",

    // OTA Update
    "Update", "Checking for update...", "New update available!",
    "Current Version: ", "New Version: ", "Updating...", "No update available",
    "Update failed", "Update complete",
    "Press and hold power button to turn back on",

    // Font Selection
    "External Font", "Built-in (Disabled)",

    // OPDS Browser
    "No entries found", "Downloading...", "Download failed", "Error:",
    "Unnamed", "No server URL configured", "Failed to fetch feed",
    "Failed to parse feed", "Network: ", "IP Address: ",
    "or scan QR code with your phone to connect to Wifi.",
    "Error: General failure", "Error: Network not found", "Error: Connection timeout",
    "SD card",

    // Buttons
    "\xC2\xAB Back", // « Back
    "\xC2\xAB Exit", // « Exit
    "\xC2\xAB Home", // « Home
    "\xC2\xAB Save", // « Save
    "Select", "Toggle", "Confirm", "Cancel", "Connect", "Open", "Download",
    "Retry", "Yes", "No", "ON", "OFF", "Set", "Not Set", "Left", "Right", "Up",
    "Down", "CAPS", "caps", "OK",

    // Languages
    "English", "Espa\xC3\xB1ol", "Italiano", "Svenska", "Fran\xC3\xA7\x61is",

    // Marker
    "[ON]",

    // Master branch specific additions
    "Sleep Screen Cover Filter", "Contrast",
    "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar",
    "UI Theme", "Classic", "Lyra",
    "Sunlight Fading Fix",
    "Remap Front Buttons", "OPDS Browser",
    "Cover + Custom",
    "Recents",
    "Recent Books",
    "No recent books",
    "Use Calibre wireless device transfers",
    "Forget network and remove saved password?",
    "Forget network",
    "Starting Calibre...",
    "Setup",
    "Status",
    "Clear",
    "Default",
    "Press a front button for each role",
    "Unassigned",
    "Already assigned",
    "Side button Up: Reset to default layout",
    "Side button Down: Cancel remapping",
    "Back (1st button)",
    "Confirm (2nd button)",
    "Left (3rd button)",
    "Right (4th button)",
    "Go to %",
    "Go Home",
    "Sync Progress",
    "Delete Book Cache",
    "Chapter: ",
    " pages  |  ",
    "Book: ",
    "shift",
    "SHIFT",
    "LOCK",
    "For Calibre, add /opds to your URL",
    "Left/Right: 1%  Up/Down: 10%",
    "Syncing time...",
    "Calculating document hash...",
    "Failed to calculate document hash",
    "Fetching remote progress...",
    "Uploading progress...",
    "No credentials configured",
    "Set up KOReader account in Settings",
    "Progress found!",
    "Remote:",
    "Local:",
    "  Page %d, %.2f%% overall",
    "  Page %d/%d, %.2f%% overall",
    "  From: %s",
    "Apply remote progress",
    "Upload local progress",
    "No remote progress found",
    "Upload current position?",
    "Progress uploaded!",
    "Sync failed",
    "Section ",
    "Upload",
};

const char *const STRINGS_IT[] = {
    // Boot/Sleep
    "CrossPoint", "BOOTING", "SLEEPING", "Entering Sleep...",

    // Home Menu
    "Browse Files", "File Transfer", "Settings", "Calibre Library",
    "Continue Reading", "No open book", "Start reading below",

    // File Browser
    "Books", "No books found",

    // Reader
    "Select Chapter", "No chapters", "End of book", "Empty chapter",
    "Indexing...", "Memory error", "Page load error", "Empty file",
    "Out of bounds", "Loading...", "Failed to load XTC", "Failed to load TXT",
    "Failed to load EPUB", "SD card error",

    // Network
    "WiFi Networks", "No networks found", "%zu networks found", "Scanning...",
    "Connecting...", "Connected!", "Connection Failed", "Connection timeout",
    "Forget Network?",
    "Save password for next time?", "Remove saved password?",
    "Press OK to scan again", "Press any button to continue",
    "LEFT/RIGHT: Select | OK: Confirm", "How would you like to connect?",
    "Join a Network", "Create Hotspot", "Connect to an existing WiFi network",
    "Create a WiFi network others can join", "Starting Hotspot...",
    "Hotspot Mode", "Connect your device to this WiFi network",
    "Open this URL in your browser", "or http://",
    "or scan QR code with your phone:", "Calibre Wireless", "Calibre Web URL",
    "Connect as Wireless Device", "* = Encrypted | + = Saved", "MAC address:",
    "Checking WiFi...", "Enter WiFi Password", "Enter Text", "to ",

    // Calibre Wireless
    "Discovering Calibre...", "Connecting to ", "Connected to ",
    "Waiting for commands...", "(Connection failed, retrying)",
    "Calibre disconnected", "Waiting for transfer...",
    "If transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice plugin settings.",
    "Receiving: ", "Received: ", "Waiting for more...",
    "Failed to create file", "Password required", "Transfer interrupted",
    "1) Install CrossPoint Reader plugin", "2) Be on the same WiFi network",
    "3) In Calibre: \"Send to device\"", "Keep this screen open while sending",

    // Settings Categories
    "Display", "Reader", "Controls", "System",

    // Settings
    "Sleep Screen", "Sleep Screen Cover Mode", "Status Bar", "Hide Battery %",
    "Extra Paragraph Spacing", "Text Anti-Aliasing", "Short Power Button Click",
    "Reading Orientation", "Front Button Layout", "Side Button Layout (reader)",
    "Long-press Chapter Skip", "Reader Font Family", "External Reader Font", "Reader Font", "UI Font",
    "UI Font Size", "Reader Line Spacing",
    "ASCII Letter Spacing", "ASCII Digit Spacing", "CJK Spacing", "Color Mode",
    "Reader Screen Margin", "Reader Paragraph Alignment", "Hyphenation", "Time to Sleep",
    "Refresh Frequency", "Calibre Settings", "KOReader Sync", "Check for updates", "Language",
    "Select Wallpaper", "Clear Reading Cache",

    // Calibre Settings
    "Calibre",

    // KOReader Settings
    "Username", "Password", "Sync Server URL", "Document Matching", "Authenticate",
    "KOReader Username", "KOReader Password", "Filename", "Binary", "Set credentials first",

    // KOReader Auth
    "WiFi connection failed", "Authenticating...", "Successfully authenticated!",
    "KOReader Auth", "KOReader sync is ready to use", "Authentication Failed", "Done",

    // Clear Cache
    "This will clear all cached book data.", "All reading progress will be lost!",
    "Books will need to be re-indexed", "when opened again.",
    "Clearing cache...", "Cache Cleared", "items removed", "failed",
    "Failed to clear cache", "Check serial output for details",

    // Setting Values
    "Dark", "Light", "Custom", "Cover", "None", "Fit", "Crop", "No Progress",
    "Full", "Never", "In Reader", "Always", "Ignore", "Sleep", "Page Turn",
    "Portrait", "Landscape CW", "Inverted", "Landscape CCW",
    "Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght",
    "Prev/Next", "Next/Prev", "Bookerly", "Noto Sans", "Open Dyslexic", "Small",
    "Medium",
    "Large", "X Large", "Tight", "Normal", "Wide", "Justify", "Left", "Center",
    "Right", "1 min", "5 min", "10 min", "15 min", "30 min", "1 page",
    "5 pages", "10 pages", "15 pages", "30 pages",

    // OTA Update
    "Update", "Checking for update...", "New update available!",
    "Current Version: ", "New Version: ", "Updating...", "No update available",
    "Update failed", "Update complete",
    "Press and hold power button to turn back on",

    // Font Selection
    "External Font", "Built-in (Disabled)",

    // OPDS Browser
    "No entries found", "Downloading...", "Download failed", "Error:",
    "Unnamed", "No server URL configured", "Failed to fetch feed",
    "Failed to parse feed", "Network: ", "IP Address: ",
    "or scan QR code with your phone to connect to Wifi.",
    "Error: General failure", "Error: Network not found", "Error: Connection timeout",
    "SD card",

    // Buttons
    "\xC2\xAB Back", // « Back
    "\xC2\xAB Exit", // « Exit
    "\xC2\xAB Home", // « Home
    "\xC2\xAB Save", // « Save
    "Select", "Toggle", "Confirm", "Cancel", "Connect", "Open", "Download",
    "Retry", "Yes", "No", "ON", "OFF", "Set", "Not Set", "Left", "Right", "Up",
    "Down", "CAPS", "caps", "OK",

    // Languages
    "English", "Espa\xC3\xB1ol", "Italiano", "Svenska", "Fran\xC3\xA7\x61is",

    // Marker
    "[ON]",

    // Master branch specific additions
    "Sleep Screen Cover Filter", "Contrast",
    "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar",
    "UI Theme", "Classic", "Lyra",
    "Sunlight Fading Fix",
    "Remap Front Buttons", "OPDS Browser",
    "Cover + Custom",
    "Recents",
    "Recent Books",
    "No recent books",
    "Use Calibre wireless device transfers",
    "Forget network and remove saved password?",
    "Forget network",
    "Starting Calibre...",
    "Setup",
    "Status",
    "Clear",
    "Default",
    "Press a front button for each role",
    "Unassigned",
    "Already assigned",
    "Side button Up: Reset to default layout",
    "Side button Down: Cancel remapping",
    "Back (1st button)",
    "Confirm (2nd button)",
    "Left (3rd button)",
    "Right (4th button)",
    "Go to %",
    "Go Home",
    "Sync Progress",
    "Delete Book Cache",
    "Chapter: ",
    " pages  |  ",
    "Book: ",
    "shift",
    "SHIFT",
    "LOCK",
    "For Calibre, add /opds to your URL",
    "Left/Right: 1%  Up/Down: 10%",
    "Syncing time...",
    "Calculating document hash...",
    "Failed to calculate document hash",
    "Fetching remote progress...",
    "Uploading progress...",
    "No credentials configured",
    "Set up KOReader account in Settings",
    "Progress found!",
    "Remote:",
    "Local:",
    "  Page %d, %.2f%% overall",
    "  Page %d/%d, %.2f%% overall",
    "  From: %s",
    "Apply remote progress",
    "Upload local progress",
    "No remote progress found",
    "Upload current position?",
    "Progress uploaded!",
    "Sync failed",
    "Section ",
    "Upload",
};

const char *const STRINGS_SV[] = {
    // Boot/Sleep
    "CrossPoint", "BOOTING", "SLEEPING", "Entering Sleep...",

    // Home Menu
    "Browse Files", "File Transfer", "Settings", "Calibre Library",
    "Continue Reading", "No open book", "Start reading below",

    // File Browser
    "Books", "No books found",

    // Reader
    "Select Chapter", "No chapters", "End of book", "Empty chapter",
    "Indexing...", "Memory error", "Page load error", "Empty file",
    "Out of bounds", "Loading...", "Failed to load XTC", "Failed to load TXT",
    "Failed to load EPUB", "SD card error",

    // Network
    "WiFi Networks", "No networks found", "%zu networks found", "Scanning...",
    "Connecting...", "Connected!", "Connection Failed", "Connection timeout",
    "Forget Network?",
    "Save password for next time?", "Remove saved password?",
    "Press OK to scan again", "Press any button to continue",
    "LEFT/RIGHT: Select | OK: Confirm", "How would you like to connect?",
    "Join a Network", "Create Hotspot", "Connect to an existing WiFi network",
    "Create a WiFi network others can join", "Starting Hotspot...",
    "Hotspot Mode", "Connect your device to this WiFi network",
    "Open this URL in your browser", "or http://",
    "or scan QR code with your phone:", "Calibre Wireless", "Calibre Web URL",
    "Connect as Wireless Device", "* = Encrypted | + = Saved", "MAC address:",
    "Checking WiFi...", "Enter WiFi Password", "Enter Text", "to ",

    // Calibre Wireless
    "Discovering Calibre...", "Connecting to ", "Connected to ",
    "Waiting for commands...", "(Connection failed, retrying)",
    "Calibre disconnected", "Waiting for transfer...",
    "If transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice plugin settings.",
    "Receiving: ", "Received: ", "Waiting for more...",
    "Failed to create file", "Password required", "Transfer interrupted",
    "1) Install CrossPoint Reader plugin", "2) Be on the same WiFi network",
    "3) In Calibre: \"Send to device\"", "Keep this screen open while sending",

    // Settings Categories
    "Display", "Reader", "Controls", "System",

    // Settings
    "Sleep Screen", "Sleep Screen Cover Mode", "Status Bar", "Hide Battery %",
    "Extra Paragraph Spacing", "Text Anti-Aliasing", "Short Power Button Click",
    "Reading Orientation", "Front Button Layout", "Side Button Layout (reader)",
    "Long-press Chapter Skip", "Reader Font Family", "External Reader Font", "Reader Font", "UI Font",
    "UI Font Size", "Reader Line Spacing",
    "ASCII Letter Spacing", "ASCII Digit Spacing", "CJK Spacing", "Color Mode",
    "Reader Screen Margin", "Reader Paragraph Alignment", "Hyphenation", "Time to Sleep",
    "Refresh Frequency", "Calibre Settings", "KOReader Sync", "Check for updates", "Language",
    "Select Wallpaper", "Clear Reading Cache",

    // Calibre Settings
    "Calibre",

    // KOReader Settings
    "Username", "Password", "Sync Server URL", "Document Matching", "Authenticate",
    "KOReader Username", "KOReader Password", "Filename", "Binary", "Set credentials first",

    // KOReader Auth
    "WiFi connection failed", "Authenticating...", "Successfully authenticated!",
    "KOReader Auth", "KOReader sync is ready to use", "Authentication Failed", "Done",

    // Clear Cache
    "This will clear all cached book data.", "All reading progress will be lost!",
    "Books will need to be re-indexed", "when opened again.",
    "Clearing cache...", "Cache Cleared", "items removed", "failed",
    "Failed to clear cache", "Check serial output for details",

    // Setting Values
    "Dark", "Light", "Custom", "Cover", "None", "Fit", "Crop", "No Progress",
    "Full", "Never", "In Reader", "Always", "Ignore", "Sleep", "Page Turn",
    "Portrait", "Landscape CW", "Inverted", "Landscape CCW",
    "Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght",
    "Prev/Next", "Next/Prev", "Bookerly", "Noto Sans", "Open Dyslexic", "Small",
    "Medium",
    "Large", "X Large", "Tight", "Normal", "Wide", "Justify", "Left", "Center",
    "Right", "1 min", "5 min", "10 min", "15 min", "30 min", "1 page",
    "5 pages", "10 pages", "15 pages", "30 pages",

    // OTA Update
    "Update", "Checking for update...", "New update available!",
    "Current Version: ", "New Version: ", "Updating...", "No update available",
    "Update failed", "Update complete",
    "Press and hold power button to turn back on",

    // Font Selection
    "External Font", "Built-in (Disabled)",

    // OPDS Browser
    "No entries found", "Downloading...", "Download failed", "Error:",
    "Unnamed", "No server URL configured", "Failed to fetch feed",
    "Failed to parse feed", "Network: ", "IP Address: ",
    "or scan QR code with your phone to connect to Wifi.",
    "Error: General failure", "Error: Network not found", "Error: Connection timeout",
    "SD card",

    // Buttons
    "\xC2\xAB Back", // « Back
    "\xC2\xAB Exit", // « Exit
    "\xC2\xAB Home", // « Home
    "\xC2\xAB Save", // « Save
    "Select", "Toggle", "Confirm", "Cancel", "Connect", "Open", "Download",
    "Retry", "Yes", "No", "ON", "OFF", "Set", "Not Set", "Left", "Right", "Up",
    "Down", "CAPS", "caps", "OK",

    // Languages
    "English", "Espa\xC3\xB1ol", "Italiano", "Svenska", "Fran\xC3\xA7\x61is",

    // Marker
    "[ON]",

    // Master branch specific additions
    "Sleep Screen Cover Filter", "Contrast",
    "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar",
    "UI Theme", "Classic", "Lyra",
    "Sunlight Fading Fix",
    "Remap Front Buttons", "OPDS Browser",
    "Cover + Custom",
    "Recents",
    "Recent Books",
    "No recent books",
    "Use Calibre wireless device transfers",
    "Forget network and remove saved password?",
    "Forget network",
    "Starting Calibre...",
    "Setup",
    "Status",
    "Clear",
    "Default",
    "Press a front button for each role",
    "Unassigned",
    "Already assigned",
    "Side button Up: Reset to default layout",
    "Side button Down: Cancel remapping",
    "Back (1st button)",
    "Confirm (2nd button)",
    "Left (3rd button)",
    "Right (4th button)",
    "Go to %",
    "Go Home",
    "Sync Progress",
    "Delete Book Cache",
    "Chapter: ",
    " pages  |  ",
    "Book: ",
    "shift",
    "SHIFT",
    "LOCK",
    "For Calibre, add /opds to your URL",
    "Left/Right: 1%  Up/Down: 10%",
    "Syncing time...",
    "Calculating document hash...",
    "Failed to calculate document hash",
    "Fetching remote progress...",
    "Uploading progress...",
    "No credentials configured",
    "Set up KOReader account in Settings",
    "Progress found!",
    "Remote:",
    "Local:",
    "  Page %d, %.2f%% overall",
    "  Page %d/%d, %.2f%% overall",
    "  From: %s",
    "Apply remote progress",
    "Upload local progress",
    "No remote progress found",
    "Upload current position?",
    "Progress uploaded!",
    "Sync failed",
    "Section ",
    "Upload",
};

const char *const STRINGS_FR[] = {
    // Boot/Sleep
    "CrossPoint", "BOOTING", "SLEEPING", "Entering Sleep...",

    // Home Menu
    "Browse Files", "File Transfer", "Settings", "Calibre Library",
    "Continue Reading", "No open book", "Start reading below",

    // File Browser
    "Books", "No books found",

    // Reader
    "Select Chapter", "No chapters", "End of book", "Empty chapter",
    "Indexing...", "Memory error", "Page load error", "Empty file",
    "Out of bounds", "Loading...", "Failed to load XTC", "Failed to load TXT",
    "Failed to load EPUB", "SD card error",

    // Network
    "WiFi Networks", "No networks found", "%zu networks found", "Scanning...",
    "Connecting...", "Connected!", "Connection Failed", "Connection timeout",
    "Forget Network?",
    "Save password for next time?", "Remove saved password?",
    "Press OK to scan again", "Press any button to continue",
    "LEFT/RIGHT: Select | OK: Confirm", "How would you like to connect?",
    "Join a Network", "Create Hotspot", "Connect to an existing WiFi network",
    "Create a WiFi network others can join", "Starting Hotspot...",
    "Hotspot Mode", "Connect your device to this WiFi network",
    "Open this URL in your browser", "or http://",
    "or scan QR code with your phone:", "Calibre Wireless", "Calibre Web URL",
    "Connect as Wireless Device", "* = Encrypted | + = Saved", "MAC address:",
    "Checking WiFi...", "Enter WiFi Password", "Enter Text", "to ",

    // Calibre Wireless
    "Discovering Calibre...", "Connecting to ", "Connected to ",
    "Waiting for commands...", "(Connection failed, retrying)",
    "Calibre disconnected", "Waiting for transfer...",
    "If transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice plugin settings.",
    "Receiving: ", "Received: ", "Waiting for more...",
    "Failed to create file", "Password required", "Transfer interrupted",
    "1) Install CrossPoint Reader plugin", "2) Be on the same WiFi network",
    "3) In Calibre: \"Send to device\"", "Keep this screen open while sending",

    // Settings Categories
    "Display", "Reader", "Controls", "System",

    // Settings
    "Sleep Screen", "Sleep Screen Cover Mode", "Status Bar", "Hide Battery %",
    "Extra Paragraph Spacing", "Text Anti-Aliasing", "Short Power Button Click",
    "Reading Orientation", "Front Button Layout", "Side Button Layout (reader)",
    "Long-press Chapter Skip", "Reader Font Family", "External Reader Font", "Reader Font", "UI Font",
    "UI Font Size", "Reader Line Spacing",
    "ASCII Letter Spacing", "ASCII Digit Spacing", "CJK Spacing", "Color Mode",
    "Reader Screen Margin", "Reader Paragraph Alignment", "Hyphenation", "Time to Sleep",
    "Refresh Frequency", "Calibre Settings", "KOReader Sync", "Check for updates", "Language",
    "Select Wallpaper", "Clear Reading Cache",

    // Calibre Settings
    "Calibre",

    // KOReader Settings
    "Username", "Password", "Sync Server URL", "Document Matching", "Authenticate",
    "KOReader Username", "KOReader Password", "Filename", "Binary", "Set credentials first",

    // KOReader Auth
    "WiFi connection failed", "Authenticating...", "Successfully authenticated!",
    "KOReader Auth", "KOReader sync is ready to use", "Authentication Failed", "Done",

    // Clear Cache
    "This will clear all cached book data.", "All reading progress will be lost!",
    "Books will need to be re-indexed", "when opened again.",
    "Clearing cache...", "Cache Cleared", "items removed", "failed",
    "Failed to clear cache", "Check serial output for details",

    // Setting Values
    "Dark", "Light", "Custom", "Cover", "None", "Fit", "Crop", "No Progress",
    "Full", "Never", "In Reader", "Always", "Ignore", "Sleep", "Page Turn",
    "Portrait", "Landscape CW", "Inverted", "Landscape CCW",
    "Bck, Cnfrm, Lft, Rght", "Lft, Rght, Bck, Cnfrm", "Lft, Bck, Cnfrm, Rght",
    "Prev/Next", "Next/Prev", "Bookerly", "Noto Sans", "Open Dyslexic", "Small",
    "Medium",
    "Large", "X Large", "Tight", "Normal", "Wide", "Justify", "Left", "Center",
    "Right", "1 min", "5 min", "10 min", "15 min", "30 min", "1 page",
    "5 pages", "10 pages", "15 pages", "30 pages",

    // OTA Update
    "Update", "Checking for update...", "New update available!",
    "Current Version: ", "New Version: ", "Updating...", "No update available",
    "Update failed", "Update complete",
    "Press and hold power button to turn back on",

    // Font Selection
    "External Font", "Built-in (Disabled)",

    // OPDS Browser
    "No entries found", "Downloading...", "Download failed", "Error:",
    "Unnamed", "No server URL configured", "Failed to fetch feed",
    "Failed to parse feed", "Network: ", "IP Address: ",
    "or scan QR code with your phone to connect to Wifi.",
    "Error: General failure", "Error: Network not found", "Error: Connection timeout",
    "SD card",

    // Buttons
    "\xC2\xAB Back", // « Back
    "\xC2\xAB Exit", // « Exit
    "\xC2\xAB Home", // « Home
    "\xC2\xAB Save", // « Save
    "Select", "Toggle", "Confirm", "Cancel", "Connect", "Open", "Download",
    "Retry", "Yes", "No", "ON", "OFF", "Set", "Not Set", "Left", "Right", "Up",
    "Down", "CAPS", "caps", "OK",

    // Languages
    "English", "Espa\xC3\xB1ol", "Italiano", "Svenska", "Fran\xC3\xA7\x61is",

    // Marker
    "[ON]",

    // Master branch specific additions
    "Sleep Screen Cover Filter", "Contrast",
    "Full w/ Percentage", "Full w/ Book Bar", "Book Bar Only", "Full w/ Chapter Bar",
    "UI Theme", "Classic", "Lyra",
    "Sunlight Fading Fix",
    "Remap Front Buttons", "OPDS Browser",
    "Cover + Custom",
    "Recents",
    "Recent Books",
    "No recent books",
    "Use Calibre wireless device transfers",
    "Forget network and remove saved password?",
    "Forget network",
    "Starting Calibre...",
    "Setup",
    "Status",
    "Clear",
    "Default",
    "Press a front button for each role",
    "Unassigned",
    "Already assigned",
    "Side button Up: Reset to default layout",
    "Side button Down: Cancel remapping",
    "Back (1st button)",
    "Confirm (2nd button)",
    "Left (3rd button)",
    "Right (4th button)",
    "Go to %",
    "Go Home",
    "Sync Progress",
    "Delete Book Cache",
    "Chapter: ",
    " pages  |  ",
    "Book: ",
    "shift",
    "SHIFT",
    "LOCK",
    "For Calibre, add /opds to your URL",
    "Left/Right: 1%  Up/Down: 10%",
    "Syncing time...",
    "Calculating document hash...",
    "Failed to calculate document hash",
    "Fetching remote progress...",
    "Uploading progress...",
    "No credentials configured",
    "Set up KOReader account in Settings",
    "Progress found!",
    "Remote:",
    "Local:",
    "  Page %d, %.2f%% overall",
    "  Page %d/%d, %.2f%% overall",
    "  From: %s",
    "Apply remote progress",
    "Upload local progress",
    "No remote progress found",
    "Upload current position?",
    "Progress uploaded!",
    "Sync failed",
    "Section ",
    "Upload",
};

} // namespace i18n_strings
