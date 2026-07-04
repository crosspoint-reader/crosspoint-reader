#include "LuaHostApiInput.h"

#include "LuaEngine.h"
#include "LuaHostApiContext.h"
#include "LuaHostApiSys.h"

#include <MappedInputManager.h>

#include "LuaAppIdle.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <Arduino.h>

namespace {

const char* pollMappedButton(MappedInputManager& input) {
  input.update();
  if (input.wasPressed(MappedInputManager::Button::Back)) {
    return "back";
  }
  if (input.wasPressed(MappedInputManager::Button::Confirm)) {
    return "confirm";
  }
  if (input.wasPressed(MappedInputManager::Button::Up)) {
    return "up";
  }
  if (input.wasPressed(MappedInputManager::Button::Down)) {
    return "down";
  }
  if (input.wasPressed(MappedInputManager::Button::Left)) {
    return "left";
  }
  if (input.wasPressed(MappedInputManager::Button::Right)) {
    return "right";
  }
  return "none";
}

int inputPoll(lua_State* L) {
  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr || context->mappedInput == nullptr) {
    return luaL_error(L, "input unavailable");
  }
  lua_pushstring(L, pollMappedButton(*context->mappedInput));
  return 1;
}

int inputWait(lua_State* L) {
  LuaHostApiContext* context = getActiveHostApiContext();
  if (context == nullptr || context->mappedInput == nullptr) {
    return luaL_error(L, "input unavailable");
  }

  for (;;) {
    const char* button = pollMappedButton(*context->mappedInput);
    if (button[0] != 'n') {
      lua_pushstring(L, button);
      return 1;
    }
    if (LuaEngine* engine = getActiveLuaEngine()) {
      if (engine->exitRequested()) {
        lua_pushstring(L, "back");
        return 1;
      }
    }
    crosspointLuaIdleTick();
    delay(10);
  }
}

const luaL_Reg kInputFunctions[] = {
    {"poll", inputPoll},
    {"wait", inputWait},
    {nullptr, nullptr},
};

}  // namespace

void registerCpInputApi(lua_State* L) {
  luaL_newlib(L, kInputFunctions);
  lua_setfield(L, -2, "input");
}
