#include "LuaHostApiDisplay.h"

#include "LuaAppDisplay.h"
#include "LuaHostApiContext.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdio>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {

void readHintField(lua_State* L, int tableIdx, const char* key, std::string& out) {
  lua_getfield(L, tableIdx, key);
  if (lua_isstring(L, -1)) {
    const char* value = lua_tostring(L, -1);
    if (value != nullptr) {
      out = value;
    }
  }
  lua_pop(L, 1);
}

void parseChromeOptions(lua_State* L, int tableIdx, LuaAppChromeState& chrome) {
  lua_getfield(L, tableIdx, "subtitle");
  if (lua_isstring(L, -1)) {
    const char* subtitle = lua_tostring(L, -1);
    if (subtitle != nullptr) {
      chrome.subtitle = subtitle;
    }
  }
  lua_pop(L, 1);

  lua_getfield(L, tableIdx, "hints");
  if (lua_istable(L, -1)) {
    const int hintsIdx = lua_gettop(L);
    chrome.hasHints = true;
    readHintField(L, hintsIdx, "back", chrome.hintBack);
    readHintField(L, hintsIdx, "confirm", chrome.hintConfirm);
    readHintField(L, hintsIdx, "left", chrome.hintLeft);
    readHintField(L, hintsIdx, "right", chrome.hintRight);
  }
  lua_pop(L, 1);
}

bool readBoardTable(lua_State* L, int tableIdx, uint8_t board[9][9]) {
  if (!lua_istable(L, tableIdx)) {
    return false;
  }
  for (int r = 1; r <= 9; ++r) {
    lua_pushnumber(L, r);
    lua_gettable(L, tableIdx);
    if (!lua_istable(L, -1)) {
      lua_pop(L, 1);
      return false;
    }
    const int rowIdx = lua_gettop(L);
    for (int c = 1; c <= 9; ++c) {
      lua_pushnumber(L, c);
      lua_gettable(L, rowIdx);
      if (!lua_isnumber(L, -1)) {
        lua_pop(L, 2);
        return false;
      }
      const int value = static_cast<int>(lua_tointeger(L, -1));
      board[r - 1][c - 1] = static_cast<uint8_t>(value < 0 ? 0 : (value > 9 ? 9 : value));
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
  return true;
}

bool readGivensTable(lua_State* L, int tableIdx, bool givens[9][9]) {
  if (!lua_istable(L, tableIdx)) {
    return false;
  }
  uint8_t givensBoard[9][9]{};
  if (!readBoardTable(L, tableIdx, givensBoard)) {
    return false;
  }
  for (int r = 0; r < 9; ++r) {
    for (int c = 0; c < 9; ++c) {
      givens[r][c] = givensBoard[r][c] != 0;
    }
  }
  return true;
}

void readConflictsTable(lua_State* L, int tableIdx, bool conflicts[9][9]) {
  if (!lua_istable(L, tableIdx)) {
    return;
  }
  lua_pushnil(L);
  while (lua_next(L, tableIdx) != 0) {
    if (lua_isstring(L, -2)) {
      const char* key = lua_tostring(L, -2);
      if (key != nullptr && lua_toboolean(L, -1)) {
        int r = 0;
        int c = 0;
        if (sscanf(key, "%d,%d", &r, &c) == 2 && r >= 1 && r <= 9 && c >= 1 && c <= 9) {
          conflicts[r - 1][c - 1] = true;
        }
      }
    }
    lua_pop(L, 1);
  }
}

int displayClear(lua_State* L) {
  (void)L;
  LuaAppDisplay::clear();
  return 0;
}

int displayText(lua_State* L) {
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const char* text = luaL_checkstring(L, 3);
  if (!LuaAppDisplay::addText(x, y, text, false)) {
    return luaL_error(L, "display entry limit reached");
  }
  return 0;
}

int displayCenter(lua_State* L) {
  const int y = static_cast<int>(luaL_checkinteger(L, 1));
  const char* text = luaL_checkstring(L, 2);
  if (!LuaAppDisplay::addText(0, y, text, true)) {
    return luaL_error(L, "display entry limit reached");
  }
  return 0;
}

int displayBmp(lua_State* L) {
  const char* path = luaL_checkstring(L, 1);
  const int x = static_cast<int>(luaL_checkinteger(L, 2));
  const int y = static_cast<int>(luaL_checkinteger(L, 3));
  const int maxW = lua_gettop(L) >= 4 ? static_cast<int>(luaL_checkinteger(L, 4)) : 0;
  const int maxH = lua_gettop(L) >= 5 ? static_cast<int>(luaL_checkinteger(L, 5)) : 0;

  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr || context->appId.empty()) {
    return luaL_error(L, "display unavailable");
  }

  std::string absolutePath;
  if (!buildAppBundlePath(context->appId, path, absolutePath)) {
    return luaL_error(L, "invalid asset path");
  }
  if (!Storage.exists(absolutePath.c_str())) {
    return luaL_error(L, "asset not found: %s", path);
  }

  if (!LuaAppDisplay::addBitmap(x, y, path, maxW, maxH)) {
    return luaL_error(L, "display bitmap limit reached");
  }
  return 0;
}

int displayFillRect(lua_State* L) {
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const int w = static_cast<int>(luaL_checkinteger(L, 3));
  const int h = static_cast<int>(luaL_checkinteger(L, 4));
  if (!LuaAppDisplay::addFillRect(x, y, w, h)) {
    return luaL_error(L, "display shape limit reached");
  }
  return 0;
}

int displayRect(lua_State* L) {
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const int w = static_cast<int>(luaL_checkinteger(L, 3));
  const int h = static_cast<int>(luaL_checkinteger(L, 4));
  const int lineW = lua_gettop(L) >= 5 ? static_cast<int>(luaL_checkinteger(L, 5)) : 1;
  if (!LuaAppDisplay::addRect(x, y, w, h, lineW)) {
    return luaL_error(L, "display shape limit reached");
  }
  return 0;
}

int displayLine(lua_State* L) {
  const int x1 = static_cast<int>(luaL_checkinteger(L, 1));
  const int y1 = static_cast<int>(luaL_checkinteger(L, 2));
  const int x2 = static_cast<int>(luaL_checkinteger(L, 3));
  const int y2 = static_cast<int>(luaL_checkinteger(L, 4));
  const int lineW = lua_gettop(L) >= 5 ? static_cast<int>(luaL_checkinteger(L, 5)) : 1;
  if (!LuaAppDisplay::addLine(x1, y1, x2, y2, lineW)) {
    return luaL_error(L, "display shape limit reached");
  }
  return 0;
}

int displayGrid(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  LuaAppGridState grid;
  grid.active = true;

  lua_getfield(L, 1, "ox");
  grid.ox = static_cast<int16_t>(luaL_optinteger(L, -1, 0));
  lua_pop(L, 1);

  lua_getfield(L, 1, "oy");
  grid.oy = static_cast<int16_t>(luaL_optinteger(L, -1, 0));
  lua_pop(L, 1);

  lua_getfield(L, 1, "cell");
  grid.cell = static_cast<int16_t>(luaL_checkinteger(L, -1));
  lua_pop(L, 1);

  lua_getfield(L, 1, "board");
  if (!readBoardTable(L, lua_gettop(L), grid.board)) {
    return luaL_error(L, "grid board must be a 9x9 table");
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "givens");
  if (!readGivensTable(L, lua_gettop(L), grid.givens)) {
    return luaL_error(L, "grid givens must be a 9x9 table");
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "conflicts");
  readConflictsTable(L, lua_gettop(L), grid.conflicts);
  lua_pop(L, 1);

  lua_getfield(L, 1, "cursor");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "r");
    grid.cursorR = static_cast<int>(luaL_optinteger(L, -1, 1));
    lua_pop(L, 1);
    lua_getfield(L, -1, "c");
    grid.cursorC = static_cast<int>(luaL_optinteger(L, -1, 1));
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  lua_getfield(L, 1, "entry_mode");
  grid.entryMode = lua_toboolean(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, 1, "entry_digit");
  grid.entryDigit = static_cast<int>(luaL_optinteger(L, -1, 0));
  lua_pop(L, 1);

  LuaAppDisplay::setGrid(grid);
  return 0;
}

int displayWidth(lua_State* L) {
  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr || context->renderer == nullptr) {
    return luaL_error(L, "display unavailable");
  }
  lua_pushinteger(L, context->renderer->getScreenWidth());
  return 1;
}

int displayHeight(lua_State* L) {
  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr || context->renderer == nullptr) {
    return luaL_error(L, "display unavailable");
  }
  lua_pushinteger(L, context->renderer->getScreenHeight());
  return 1;
}

int displayContentTop(lua_State* L) {
  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr) {
    return luaL_error(L, "display unavailable");
  }
  lua_pushinteger(L, context->contentTop);
  return 1;
}

int displayContentHeight(lua_State* L) {
  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr) {
    return luaL_error(L, "display unavailable");
  }
  lua_pushinteger(L, context->contentHeight);
  return 1;
}

int displayRefresh(lua_State* L) {
  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr || context->renderer == nullptr) {
    return luaL_error(L, "display unavailable");
  }
  if (context->fontId == 0) {
    return luaL_error(L, "display font not configured");
  }

  LuaAppChromeState chrome;
  if (lua_gettop(L) >= 1 && lua_istable(L, 1)) {
    parseChromeOptions(L, 1, chrome);
    LuaAppDisplay::setChrome(chrome);
  }

  LuaAppDisplay::paintWithChrome(*context->renderer, context->fontId, context->appId, context->displayName,
                                 context->contentTop, context->contentHeight);
  LuaAppDisplay::clear();
  return 0;
}

const luaL_Reg kDisplayFunctions[] = {
    {"clear", displayClear},
    {"text", displayText},
    {"center", displayCenter},
    {"bmp", displayBmp},
    {"fill_rect", displayFillRect},
    {"rect", displayRect},
    {"line", displayLine},
    {"grid", displayGrid},
    {"width", displayWidth},
    {"height", displayHeight},
    {"content_top", displayContentTop},
    {"content_height", displayContentHeight},
    {"refresh", displayRefresh},
    {nullptr, nullptr},
};

}  // namespace

void registerCpDisplayApi(lua_State* L) {
  luaL_newlib(L, kDisplayFunctions);
  lua_setfield(L, -2, "display");
}
