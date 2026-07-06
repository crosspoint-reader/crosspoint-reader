#include "LuaHostApiUi.h"

#include "LuaAppDisplay.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {

int uiList(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);

  LuaAppListState list;
  list.active = true;

  lua_getfield(L, 1, "selected");
  list.selected = static_cast<int>(luaL_optinteger(L, -1, 1));
  lua_pop(L, 1);

  lua_getfield(L, 1, "items");
  if (!lua_istable(L, -1)) {
    return luaL_error(L, "list items must be a table");
  }

  const int itemsIdx = lua_gettop(L);
  const int count = static_cast<int>(lua_rawlen(L, itemsIdx));
  if (count > static_cast<int>(LuaAppDisplay::kMaxListItems)) {
    return luaL_error(L, "list item limit reached");
  }

  for (int i = 1; i <= count; ++i) {
    lua_rawgeti(L, itemsIdx, i);
    const char* label = luaL_checkstring(L, -1);
    list.items.emplace_back(label);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  LuaAppDisplay::setList(list);
  return 0;
}

int uiPopup(lua_State* L) {
  const char* message = luaL_checkstring(L, 1);
  LuaAppDisplay::setPopup(message);
  return 0;
}

const luaL_Reg kUiFunctions[] = {
    {"list", uiList},
    {"popup", uiPopup},
    {nullptr, nullptr},
};

}  // namespace

void registerCpUiApi(lua_State* L) {
  luaL_newlib(L, kUiFunctions);
  lua_setfield(L, -2, "ui");
}
