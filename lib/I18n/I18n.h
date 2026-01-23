#pragma once

#include <cstdint>

/**
 * Internationalization (i18n) system for CrossPoint Reader
 * Supports English and Chinese UI languages
 */

// String IDs - organized by category
enum class StrId : uint16_t {
  // === Boot/Sleep ===
  CROSSPOINT, // "CrossPoint"
  BOOTING,    // "BOOTING" / "启动中"
  SLEEPING,   // "SLEEPING" / "休眠中"
  ENTERING_SLEEP, // "Entering Sleep..." / "进入休眠..."

  // === Home Menu ===
  BROWSE_FILES,     // "Browse Files" / "浏览文件"
  FILE_TRANSFER,    // "File Transfer" / "文件传输"
  SETTINGS_TITLE,   // "Settings" / "设置"
  CALIBRE_LIBRARY,  // "Calibre Library" / "Calibre书库"
  CONTINUE_READING, // "Continue Reading" / "继续阅读"
  NO_OPEN_BOOK,     // "No open book" / "无打开的书籍"
  START_READING,    // "Start reading below" / "从下方开始阅读"

  // === File Browser ===
  BOOKS,          // "Books" / "书籍"
  NO_BOOKS_FOUND, // "No books found" / "未找到书籍"

  // === Reader ===
  SELECT_CHAPTER,  // "Select Chapter" / "选择章节"
  NO_CHAPTERS,     // "No chapters" / "无章节"
  END_OF_BOOK,     // "End of book" / "已到书末"
  EMPTY_CHAPTER,   // "Empty chapter" / "空章节"
  INDEXING,        // "Indexing..." / "索引中..."
  MEMORY_ERROR,    // "Memory error" / "内存错误"
  PAGE_LOAD_ERROR, // "Page load error" / "页面加载错误"
  EMPTY_FILE,      // "Empty file" / "空文件"
  OUT_OF_BOUNDS,   // "Out of bounds" / "超出范围"
  LOADING,         // "Loading..." / "加载中..."
  LOAD_XTC_FAILED, // "Failed to load XTC" / "加载XTC失败"
  LOAD_TXT_FAILED, // "Failed to load TXT" / "加载TXT失败"
  LOAD_EPUB_FAILED, // "Failed to load EPUB" / "加载EPUB失败"
  SD_CARD_ERROR,    // "SD card error" / "SD卡错误"

  // === Network ===
  WIFI_NETWORKS,      // "WiFi Networks" / "WiFi网络"
  NO_NETWORKS,        // "No networks found" / "未找到网络"
  NETWORKS_FOUND,     // "%zu networks found" / "找到%zu个网络"
  SCANNING,           // "Scanning..." / "扫描中..."
  CONNECTING,         // "Connecting..." / "连接中..."
  CONNECTED,          // "Connected!" / "已连接!"
  CONNECTION_FAILED,  // "Connection Failed" / "连接失败"
  CONNECTION_TIMEOUT, // "Connection timeout" / "连接超时"
  FORGET_NETWORK,     // "Forget Network?" / "忘记网络?"
  SAVE_PASSWORD,      // "Save password for next time?" / "保存密码?"
  REMOVE_PASSWORD,    // "Remove saved password?" / "删除已保存密码?"
  PRESS_OK_SCAN,      // "Press OK to scan again" / "按确定重新扫描"
  PRESS_ANY_CONTINUE, // "Press any button to continue" / "按任意键继续"
  SELECT_HINT,  // "LEFT/RIGHT: Select | OK: Confirm" / "左/右:选择 | 确定:确认"
  HOW_CONNECT,  // "How would you like to connect?" / "选择连接方式"
  JOIN_NETWORK, // "Join a Network" / "加入网络"
  CREATE_HOTSPOT, // "Create Hotspot" / "创建热点"
  JOIN_DESC,    // "Connect to an existing WiFi network" / "连接到现有WiFi网络"
  HOTSPOT_DESC, // "Create a WiFi network others can join" /
                // "创建WiFi网络供他人连接"
  STARTING_HOTSPOT,  // "Starting Hotspot..." / "启动热点中..."
  HOTSPOT_MODE,      // "Hotspot Mode" / "热点模式"
  CONNECT_WIFI_HINT, // "Connect your device to this WiFi network" /
                     // "将设备连接到此WiFi"
  OPEN_URL_HINT,     // "Open this URL in your browser" / "在浏览器中打开此URL"
  OR_HTTP_PREFIX,    // "or http://" / "或 http://"
  SCAN_QR_HINT, // "or scan QR code with your phone:" / "或用手机扫描二维码:"
  CALIBRE_WIRELESS, // "Calibre Wireless" / "Calibre无线连接"
  CALIBRE_WEB_URL,  // "Calibre Web URL" / "Calibre Web地址"
  CONNECT_WIRELESS, // "Connect as Wireless Device" / "作为无线设备连接"
  NETWORK_LEGEND,   // "* = Encrypted | + = Saved" / "* = 加密 | + = 已保存"
  MAC_ADDRESS,      // "MAC address:" / "MAC地址:"
  CHECKING_WIFI,    // "Checking WiFi..." / "检查WiFi..."
  ENTER_WIFI_PASSWORD, // "Enter WiFi Password" / "输入WiFi密码"
  ENTER_TEXT,       // "Enter Text" / "输入文字"

  // === Calibre Wireless ===
  CALIBRE_DISCOVERING,          // "Discovering Calibre..." / "正在搜索Calibre..."
  CALIBRE_CONNECTING_TO,        // "Connecting to " / "正在连接到 "
  CALIBRE_CONNECTED_TO,         // "Connected to " / "已连接到 "
  CALIBRE_WAITING_COMMANDS,     // "Waiting for commands..." / "等待指令中..."
  CONNECTION_FAILED_RETRYING,   // "(Connection failed, retrying)" / "(连接失败，重试中)"
  CALIBRE_DISCONNECTED,         // "Calibre disconnected" / "Calibre已断开"
  CALIBRE_WAITING_TRANSFER,     // "Waiting for transfer..." / "等待传输中..."
  CALIBRE_TRANSFER_HINT,        // Transfer hint text
  CALIBRE_RECEIVING,            // "Receiving: " / "正在接收: "
  CALIBRE_RECEIVED,             // "Received: " / "已接收: "
  CALIBRE_WAITING_MORE,         // "Waiting for more..." / "等待更多内容..."
  CALIBRE_FAILED_CREATE_FILE,   // "Failed to create file" / "创建文件失败"
  CALIBRE_PASSWORD_REQUIRED,    // "Password required" / "需要密码"
  CALIBRE_TRANSFER_INTERRUPTED, // "Transfer interrupted" / "传输中断"

  // === Settings Categories ===
  CAT_DISPLAY,      // "Display" / "显示"
  CAT_READER,       // "Reader" / "阅读"
  CAT_CONTROLS,     // "Controls" / "控制"
  CAT_SYSTEM,       // "System" / "系统"

  // === Settings ===
  SLEEP_SCREEN,     // "Sleep Screen" / "休眠屏幕"
  SLEEP_COVER_MODE, // "Sleep Screen Cover Mode" / "封面显示模式"
  STATUS_BAR,       // "Status Bar" / "状态栏"
  HIDE_BATTERY,     // "Hide Battery %" / "隐藏电量百分比"
  EXTRA_SPACING,    // "Extra Paragraph Spacing" / "段落额外间距"
  TEXT_AA,          // "Text Anti-Aliasing" / "文字抗锯齿"
  SHORT_PWR_BTN,    // "Short Power Button Click" / "电源键短按"
  ORIENTATION,      // "Reading Orientation" / "阅读方向"
  FRONT_BTN_LAYOUT, // "Front Button Layout" / "前置按钮布局"
  SIDE_BTN_LAYOUT,  // "Side Button Layout (reader)" / "侧边按钮布局"
  LONG_PRESS_SKIP,  // "Long-press Chapter Skip" / "长按跳转章节"
  FONT_FAMILY,      // "Reader Font Family" / "阅读字体"
  EXT_READER_FONT,  // "External Reader Font" / "阅读器字体"
  EXT_CHINESE_FONT, // "Reader Font" / "阅读器字体"
  EXT_UI_FONT,      // "External UI Font" / "UI字体"
  FONT_SIZE,        // "Reader Font Size" / "字体大小"
  LINE_SPACING,     // "Reader Line Spacing" / "行间距"
  ASCII_LETTER_SPACING, // "ASCII Letter Spacing" / "ASCII 字母间距"
  ASCII_DIGIT_SPACING,  // "ASCII Digit Spacing" / "ASCII 数字间距"
  CJK_SPACING,          // "CJK Spacing" / "汉字间距"
  COLOR_MODE,       // "Color Mode" / "颜色模式"
  SCREEN_MARGIN,    // "Reader Screen Margin" / "页面边距"
  PARA_ALIGNMENT,   // "Reader Paragraph Alignment" / "段落对齐"
  HYPHENATION,      // "Hyphenation" / "连字符"
  TIME_TO_SLEEP,    // "Time to Sleep" / "休眠时间"
  REFRESH_FREQ,     // "Refresh Frequency" / "刷新频率"
  CALIBRE_SETTINGS, // "Calibre Settings" / "Calibre设置"
  KOREADER_SYNC,    // "KOReader Sync" / "KOReader同步"
  CHECK_UPDATES,    // "Check for updates" / "检查更新"
  LANGUAGE,         // "Language" / "语言"
  SELECT_WALLPAPER, // "Select Wallpaper" / "选择壁纸"
  CLEAR_READING_CACHE, // "Clear Reading Cache" / "清理阅读缓存"

  // === Calibre Settings ===
  CALIBRE, // "Calibre" / "Calibre"

  // === KOReader Settings ===
  USERNAME,            // "Username" / "用户名"
  PASSWORD,            // "Password" / "密码"
  SYNC_SERVER_URL,     // "Sync Server URL" / "同步服务器地址"
  DOCUMENT_MATCHING,   // "Document Matching" / "文档匹配"
  AUTHENTICATE,        // "Authenticate" / "认证"
  KOREADER_USERNAME,   // "KOReader Username" / "KOReader用户名"
  KOREADER_PASSWORD,   // "KOReader Password" / "KOReader密码"
  FILENAME,            // "Filename" / "文件名"
  BINARY,              // "Binary" / "二进制"
  SET_CREDENTIALS_FIRST, // "Set credentials first" / "请先设置凭据"

  // === KOReader Auth ===
  WIFI_CONN_FAILED,  // "WiFi connection failed" / "WiFi连接失败"
  AUTHENTICATING,    // "Authenticating..." / "认证中..."
  AUTH_SUCCESS,      // "Successfully authenticated!" / "认证成功!"
  KOREADER_AUTH,     // "KOReader Auth" / "KOReader认证"
  SYNC_READY,        // "KOReader sync is ready to use" / "KOReader同步已就绪"
  AUTH_FAILED,       // "Authentication Failed" / "认证失败"
  DONE,              // "Done" / "完成"

  // === Clear Cache ===
  CLEAR_CACHE_WARNING_1, // "This will clear all cached book data." / "这将清除所有缓存的书籍数据。"
  CLEAR_CACHE_WARNING_2, // "All reading progress will be lost!" / "所有阅读进度将丢失！"
  CLEAR_CACHE_WARNING_3, // "Books will need to be re-indexed" / "书籍需要重新索引"
  CLEAR_CACHE_WARNING_4, // "when opened again." / "当再次打开时。"
  CLEARING_CACHE,        // "Clearing cache..." / "清理缓存中..."
  CACHE_CLEARED,         // "Cache Cleared" / "缓存已清理"
  ITEMS_REMOVED,         // "items removed" / "项已删除"
  FAILED_LOWER,          // "failed" / "失败"
  CLEAR_CACHE_FAILED,    // "Failed to clear cache" / "清理缓存失败"
  CHECK_SERIAL_OUTPUT,   // "Check serial output for details" / "查看串口输出了解详情"

  // Setting Values
  DARK,          // "Dark" / "深色"
  LIGHT,         // "Light" / "浅色"
  CUSTOM,        // "Custom" / "自定义"
  COVER,         // "Cover" / "封面"
  NONE,          // "None" / "无"
  FIT,           // "Fit" / "适应"
  CROP,          // "Crop" / "裁剪"
  NO_PROGRESS,   // "No Progress" / "无进度"
  FULL,          // "Full" / "完整"
  NEVER,         // "Never" / "从不"
  IN_READER,     // "In Reader" / "阅读时"
  ALWAYS,        // "Always" / "始终"
  IGNORE,        // "Ignore" / "忽略"
  SLEEP,         // "Sleep" / "休眠"
  PAGE_TURN,     // "Page Turn" / "翻页"
  PORTRAIT,      // "Portrait" / "竖屏"
  LANDSCAPE_CW,  // "Landscape CW" / "横屏顺时针"
  INVERTED,      // "Inverted" / "倒置"
  LANDSCAPE_CCW, // "Landscape CCW" / "横屏逆时针"
  FRONT_LAYOUT_BCLR, // "Bck, Cnfrm, Lft, Rght" / "返回, 确认, 左, 右"
  FRONT_LAYOUT_LRBC, // "Lft, Rght, Bck, Cnfrm" / "左, 右, 返回, 确认"
  FRONT_LAYOUT_LBCR, // "Lft, Bck, Cnfrm, Rght" / "左, 返回, 确认, 右"
  PREV_NEXT,     // "Prev/Next" / "上一页/下一页"
  NEXT_PREV,     // "Next/Prev" / "下一页/上一页"
  BOOKERLY,      // "Bookerly"
  NOTO_SANS,     // "Noto Sans"
  OPEN_DYSLEXIC, // "Open Dyslexic"
  SMALL,         // "Small" / "小"
  MEDIUM,        // "Medium" / "中"
  LARGE,         // "Large" / "大"
  X_LARGE,       // "X Large" / "特大"
  TIGHT,         // "Tight" / "紧凑"
  NORMAL,        // "Normal" / "正常"
  WIDE,          // "Wide" / "宽松"
  JUSTIFY,       // "Justify" / "两端对齐"
  LEFT,          // "Left" / "左对齐"
  CENTER,        // "Center" / "居中"
  RIGHT,         // "Right" / "右对齐"
  MIN_1,         // "1 min" / "1分钟"
  MIN_5,         // "5 min" / "5分钟"
  MIN_10,        // "10 min" / "10分钟"
  MIN_15,        // "15 min" / "15分钟"
  MIN_30,        // "30 min" / "30分钟"
  PAGES_1,       // "1 page" / "1页"
  PAGES_5,       // "5 pages" / "5页"
  PAGES_10,      // "10 pages" / "10页"
  PAGES_15,      // "15 pages" / "15页"
  PAGES_30,      // "30 pages" / "30页"

  // === OTA Update ===
  UPDATE,          // "Update" / "更新"
  CHECKING_UPDATE, // "Checking for update..." / "检查更新中..."
  NEW_UPDATE,      // "New update available!" / "有新版本可用!"
  CURRENT_VERSION, // "Current Version: " / "当前版本: "
  NEW_VERSION,     // "New Version: " / "新版本: "
  UPDATING,        // "Updating..." / "更新中..."
  NO_UPDATE,       // "No update available" / "已是最新版本"
  UPDATE_FAILED,   // "Update failed" / "更新失败"
  UPDATE_COMPLETE, // "Update complete" / "更新完成"
  POWER_ON_HINT,   // "Press and hold power button to turn back on" /
                   // "长按电源键开机"

  // === Font Selection ===
  EXTERNAL_FONT,    // "External Font" / "外置字体"
  BUILTIN_DISABLED, // "Built-in (Disabled)" / "内置(已禁用)"

  // === OPDS Browser ===
  NO_ENTRIES,        // "No entries found" / "无条目"
  DOWNLOADING,       // "Downloading..." / "下载中..."
  DOWNLOAD_FAILED,   // "Download failed" / "下载失败"
  ERROR,             // "Error:" / "错误:"
  UNNAMED,           // "Unnamed" / "未命名"
  NO_SERVER_URL,     // "No server URL configured" / "未配置服务器地址"
  FETCH_FEED_FAILED, // "Failed to fetch feed" / "获取订阅失败"
  PARSE_FEED_FAILED, // "Failed to parse feed" / "解析订阅失败"
  NETWORK_PREFIX,    // "Network: " / "网络: "
  IP_ADDRESS_PREFIX, // "IP Address: " / "IP地址: "
  SCAN_QR_WIFI_HINT, // "or scan QR code with your phone to connect to Wifi." /
                     // "或用手机扫描二维码连接WiFi"

  // === Buttons ===
  BACK,    // "« Back" / "« 返回"
  EXIT,    // "« Exit" / "« 退出"
  HOME,    // "« Home" / "« 主页"
  SAVE,    // "« Save" / "« 保存"
  SELECT,  // "Select" / "选择"
  TOGGLE,  // "Toggle" / "切换"
  CONFIRM, // "Confirm" / "确定"
  CANCEL,  // "Cancel" / "取消"
  CONNECT, // "Connect" / "连接"
  OPEN,    // "Open" / "打开"
  DOWNLOAD, // "Download" / "下载"
  RETRY,   // "Retry" / "重试"
  YES,     // "Yes" / "是"
  NO,      // "No" / "否"
  ON,      // "ON" / "开"
  OFF,     // "OFF" / "关"
  SET,     // "Set" / "已设置"
  NOT_SET, // "Not Set" / "未设置"
  DIR_LEFT,  // "Left" / "左"
  DIR_RIGHT, // "Right" / "右"
  DIR_UP,    // "Up" / "上"
  DIR_DOWN,  // "Down" / "下"
  CAPS_ON,   // "CAPS" / "大写"
  CAPS_OFF,  // "caps" / "小写"
  OK_BUTTON, // "OK" / "确定"

  // === Languages ===
  ENGLISH,          // "English"
  CHINESE_SIMPLIFIED,  // "简体中文"
  JAPANESE,         // "日本語"

  // Sentinel - must be last
  _COUNT
};

// Language enum
enum class Language : uint8_t {
  ENGLISH = 0,
  CHINESE_SIMPLIFIED = 1,
  JAPANESE = 2,
  _COUNT
};

class I18n {
public:
  static I18n &getInstance();

  // Disable copy
  I18n(const I18n &) = delete;
  I18n &operator=(const I18n &) = delete;

  /**
   * Get localized string by ID
   */
  const char *get(StrId id) const;

  /**
   * Shorthand operator for get()
   */
  const char *operator[](StrId id) const { return get(id); }

  /**
   * Get/Set current language
   */
  Language getLanguage() const { return _language; }
  void setLanguage(Language lang);

  /**
   * Save/Load language setting
   */
  void saveSettings();
  void loadSettings();

  /**
   * Get all unique characters used in a specific language
   * Returns a sorted string of unique characters
   */
  static const char *getCharacterSet(Language lang);

  // String arrays (public for static_assert access)
  static const char *const STRINGS_EN[];
  static const char *const STRINGS_ZH[];
  static const char *const STRINGS_JA[];

private:
  I18n() : _language(Language::ENGLISH) {}

  Language _language;
};

// Convenience macros
#define TR(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
