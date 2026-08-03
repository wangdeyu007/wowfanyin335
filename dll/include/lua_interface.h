#pragma once

#include <string>
#include <cstdint>
#include <cstddef>

// Lua C API function pointers.
// WoW 3.3.5.12340 uses __cdecl, NOT __fastcall like 1.12 did. Verified by
// disassembling UnitXP (0x60EA60): it reads L from [ebp+8] and the caller
// cleans the stack with `add esp,N`. See dll/include/offsets_335.h.
typedef int(__cdecl* LUA_CFUNCTION)(void* L);
typedef void(__cdecl* LUA_PUSHSTRING)(void* L, const char* s);
typedef void(__cdecl* LUA_PUSHBOOLEAN)(void* L, int boolean_value);
typedef void(__cdecl* LUA_PUSHNUMBER)(void* L, double n);
typedef void(__cdecl* LUA_PUSHNIL)(void* L);
// 3.3.5 exports the 3-argument lua_tolstring; lua_tostring is only a macro.
typedef const char* (__cdecl* LUA_TOLSTRING)(void* L, int index, size_t* len);
typedef double(__cdecl* LUA_TONUMBER)(void* L, int index);
typedef int(__cdecl* LUA_TOBOOLEAN)(void* L, int index);
typedef int(__cdecl* LUA_GETTOP)(void* L);
typedef int(__cdecl* LUA_ISNUMBER)(void* L, int index);
typedef int(__cdecl* LUA_ISSTRING)(void* L, int index);

// Main WoWTranslate command handler (internal hook function).
// Must match the hooked UnitXP's own convention.
int __cdecl detoured_UnitXP(void* L);

// Lua interface functions
bool InitializeLuaInterface();
void CleanupLuaInterface();

// Helper functions for Lua interaction
void lua_pushstring(void* L, const std::string& str);
void lua_pushboolean(void* L, bool value);
void lua_pushnumber(void* L, double value);
void lua_pushnil(void* L);
std::string lua_tostring(void* L, int index);
double lua_tonumber(void* L, int index);
bool lua_toboolean(void* L, int index);
int lua_gettop(void* L);
bool lua_isnumber(void* L, int index);
bool lua_isstring(void* L, int index);
