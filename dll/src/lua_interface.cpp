// lua_interface.cpp - Lua interface for WoWTranslate
// Handles DLL communication and async translation via UnitXP hook

#include <windows.h>
#include <string>
#include <sstream>
#include <vector>

#ifdef MINHOOK_AVAILABLE
#include "MinHook.h"
#endif

#include "../include/lua_interface.h"
#include "../include/translator_core.h"
#include "../include/logging.h"
#include "../include/utils.h"

// Minimum Lua stack depth for commands that take N args (excluding the
// "WoWTranslate" and subcommand strings at indices 1-2).
//   set_baidu_key:  Index 3 = appid, 4 = secret  -> need gettop >= 4
#define NEED_ARGS(n) do { if (lua_gettop(L) < (n)) { \
    lua_pushstring(L, "error|not enough arguments"); return 1; } } while(0)

using namespace std;

// Function pointer types are declared once in lua_interface.h.
// Addresses are for WoW 3.3.5.12340; every one was verified by offline
// disassembly of the client rather than copied from an offset table.
// See dll/include/offsets_335.h for the per-function evidence.
static auto p_lua_pushstring = reinterpret_cast<LUA_PUSHSTRING>(0x84E350);
static auto p_lua_pushboolean = reinterpret_cast<LUA_PUSHBOOLEAN>(0x84E4D0);
static auto p_lua_pushnumber = reinterpret_cast<LUA_PUSHNUMBER>(0x84E2A0);
static auto p_lua_pushnil = reinterpret_cast<LUA_PUSHNIL>(0x84E280);
static auto p_lua_tolstring = reinterpret_cast<LUA_TOLSTRING>(0x84E0E0);
static auto p_lua_tonumber = reinterpret_cast<LUA_TONUMBER>(0x84E030);
static auto p_lua_toboolean = reinterpret_cast<LUA_TOBOOLEAN>(0x84E0B0);
static auto p_lua_gettop = reinterpret_cast<LUA_GETTOP>(0x84DBD0);
static auto p_lua_isnumber = reinterpret_cast<LUA_ISNUMBER>(0x84DF20);
static auto p_lua_isstring = reinterpret_cast<LUA_ISSTRING>(0x84DF60);

// Hook target - UnitXP, located via its FrameScript registry entry at 0xAD22E8
static auto p_UnitXP = reinterpret_cast<LUA_CFUNCTION>(0x60EA60);
static LUA_CFUNCTION p_original_UnitXP = nullptr;

// State tracking
static bool g_initialized = false;

void lua_pushstring(void* L, const string& str) {
    if (p_lua_pushstring && L) {
        p_lua_pushstring(L, str.c_str());
    }
}

void lua_pushboolean(void* L, bool value) {
    if (p_lua_pushboolean && L) {
        p_lua_pushboolean(L, value ? 1 : 0);
    }
}

void lua_pushnumber(void* L, double value) {
    if (p_lua_pushnumber && L) {
        p_lua_pushnumber(L, value);
    }
}

void lua_pushnil(void* L) {
    if (p_lua_pushnil && L) {
        p_lua_pushnil(L);
    }
}

string lua_tostring(void* L, int index) {
    if (!p_lua_tolstring || !L) return "";
    const char* ptr = p_lua_tolstring(L, index, nullptr);
    return ptr ? string(ptr) : "";
}

double lua_tonumber(void* L, int index) {
    if (!p_lua_tonumber || !L) return 0.0;
    return p_lua_tonumber(L, index);
}

bool lua_toboolean(void* L, int index) {
    if (!p_lua_toboolean || !L) return false;
    return p_lua_toboolean(L, index) != 0;
}

int lua_gettop(void* L) {
    if (!p_lua_gettop || !L) return 0;
    return p_lua_gettop(L);
}

bool lua_isnumber(void* L, int index) {
    if (!p_lua_isnumber || !L) return false;
    return p_lua_isnumber(L, index) != 0;
}

bool lua_isstring(void* L, int index) {
    if (!p_lua_isstring || !L) return false;
    return p_lua_isstring(L, index) != 0;
}

// Main WoWTranslate command handler
// Commands:
//   UnitXP("WoWTranslate", "ping") -> "pong"
//   UnitXP("WoWTranslate", "translate_async", requestId, text, [sourceLang], [targetLang]) -> "ok" or "error|..."
//   UnitXP("WoWTranslate", "poll") -> "requestId|translation|error" or ""
//   UnitXP("WoWTranslate", "status") -> status string
//   UnitXP("WoWTranslate", "translate", text, [sourceLang], [targetLang]) -> translated text or "error|..."
int __cdecl detoured_UnitXP(void* L) {
    try {
        if (lua_gettop(L) >= 1) {
            string cmd{ lua_tostring(L, 1) };

            // Check if this is a WoWTranslate command
            if (cmd == "WoWTranslate") {
                LOG_DEBUG("WoWTranslate command intercepted");

                if (lua_gettop(L) >= 2) {
                    string subcmd{ lua_tostring(L, 2) };

                    // PING - Check if DLL is loaded
                    if (subcmd == "ping") {
                        lua_pushstring(L, "pong");
                        LOG_DEBUG("Ping -> Pong");
                        return 1;
                    }

                    // VERSION - Get version string
                    else if (subcmd == "version") {
                        lua_pushstring(L, "WoWTranslate v1.5-335.6 - TranSmart + Baidu fallback");
                        return 1;
                    }

                    // STATUS - Get current status
                    else if (subcmd == "status") {
                        string status = "WoWTranslate: DLL Active, Translator ";
                        status += (g_translator && g_translator->IsInitialized()) ? "Ready" : "Not Ready";
                        if (g_translator) {
                            status += ", Pending: " + to_string(g_translator->GetPendingCount());
                        }
                        lua_pushstring(L, status);
                        return 1;
                    }

                    // TRANSLATE_ASYNC - Queue async translation request
                    // Args: requestId, text, [sourceLang], [targetLang]
                    // Optional language params default to zh->en for backward compatibility
                    else if (subcmd == "translate_async") {
                        if (lua_gettop(L) >= 4) {
                            string requestId{ lua_tostring(L, 3) };
                            string text{ lua_tostring(L, 4) };

                            // Optional language parameters (default zh->en for backward compat)
                            string sourceLang = "zh";
                            string targetLang = "en";
                            if (lua_gettop(L) >= 6) {
                                sourceLang = lua_tostring(L, 5);
                                targetLang = lua_tostring(L, 6);
                            }

                            if (!g_translator || !g_translator->IsInitialized()) {
                                lua_pushstring(L, "error|translator not initialized");
                                return 1;
                            }

                            if (text.empty()) {
                                lua_pushstring(L, "error|empty text");
                                return 1;
                            }

                            if (g_translator->TranslateAsync(requestId, text, sourceLang, targetLang)) {
                                lua_pushstring(L, "ok");
                                LOG_DEBUG("Async translation queued: " + requestId + " (" + sourceLang + " -> " + targetLang + ")");
                            } else {
                                lua_pushstring(L, "error|failed to queue request");
                            }
                            return 1;
                        }
                        lua_pushstring(L, "error|requestId and text required");
                        return 1;
                    }

                    // POLL - Poll for completed translation
                    // Returns: "requestId|translation|error" or ""
                    else if (subcmd == "poll") {
                        if (!g_translator) {
                            lua_pushstring(L, "");
                            return 1;
                        }

                        string requestId, translation, error;
                        if (g_translator->PollResult(requestId, translation, error)) {
                            // Format: requestId|translation|error
                            string result = requestId + "|" + translation + "|" + error;
                            lua_pushstring(L, result);
                            LOG_DEBUG("Poll returned: " + requestId);
                        } else {
                            lua_pushstring(L, "");
                        }
                        return 1;
                    }

                    // TRANSLATE (synchronous) - For testing
                    // Args: text, [sourceLang], [targetLang]
                    else if (subcmd == "translate") {
                        if (lua_gettop(L) >= 3) {
                            string text{ lua_tostring(L, 3) };

                            // Optional language parameters (default zh->en for backward compat)
                            string sourceLang = "zh";
                            string targetLang = "en";
                            if (lua_gettop(L) >= 5) {
                                sourceLang = lua_tostring(L, 4);
                                targetLang = lua_tostring(L, 5);
                            }

                            if (!g_translator || !g_translator->IsInitialized()) {
                                lua_pushstring(L, "error|translator not initialized");
                                return 1;
                            }

                            string result;
                            TranslationResult tr = g_translator->TranslateText(text, result, sourceLang, targetLang);

                            if (tr == TranslationResult::SUCCESS) {
                                lua_pushstring(L, result);
                                LOG_DEBUG("Sync translation: " + text + " -> " + result);
                            } else {
                                string error = "error|";
                                switch (tr) {
                                    case TranslationResult::NETWORK_ERROR: error += "network error"; break;
                                    case TranslationResult::API_ERROR: error += "API error"; break;
                                    case TranslationResult::ENCODING_ERROR: error += "encoding error"; break;
                                    case TranslationResult::TIMEOUT_ERROR: error += "timeout"; break;
                                    case TranslationResult::INVALID_PARAMS: error += "invalid parameters"; break;
                                    default: error += "unknown error"; break;
                                }
                                lua_pushstring(L, error);
                            }
                            return 1;
                        }
                        lua_pushstring(L, "error|text required");
                        return 1;
                    }

                    // SET BAIDU API KEY - Store Baidu Translate credentials from Lua config
                    // Args: appid, secret
                    else if (subcmd == "set_baidu_key") {
                        NEED_ARGS(4);
                        if (!g_translator) {
                            lua_pushstring(L, "error|translator not initialized");
                            return 1;
                        }
                        string appid{ lua_tostring(L, 3) };
                        string secret{ lua_tostring(L, 4) };
                        g_translator->SetBaiduKey(appid, secret);
                        lua_pushstring(L, "ok");
                        LOG_INFO("Baidu API key configured from Lua config");
                        return 1;
                    }

                    // GET STATUS - Extended status with Baidu key info
                    else if (subcmd == "status_ext") {
                        string status = "WoWTranslate: DLL Active, Translator ";
                        status += (g_translator && g_translator->IsInitialized()) ? "Ready" : "Not Ready";
                        if (g_translator) {
                            status += ", Pending: " + to_string(g_translator->GetPendingCount());
                        }
                        lua_pushstring(L, status);
                        return 1;
                    }

                    else {
                        string error = "error|unknown command: " + subcmd;
                        lua_pushstring(L, error);
                        return 1;
                    }
                } else {
                    lua_pushstring(L, "error|no subcommand specified");
                    return 1;
                }
            }
        }

        // Not our command - call original UnitXP if available
        if (p_original_UnitXP) {
            return p_original_UnitXP(L);
        }

        // Fallback - return 0 if no original function
        return 0;

    } catch (const exception& e) {
        string error = "WoWTranslate Exception: " + string(e.what());
        LOG_ERROR(error);
        lua_pushstring(L, ("error|" + string(e.what())).c_str());
        return 1;
    } catch (...) {
        string error = "WoWTranslate Unknown Exception";
        LOG_ERROR(error);
        lua_pushstring(L, "error|unknown exception");
        return 1;
    }
}

// Initialize the Lua interface by hooking UnitXP
bool InitializeLuaInterface() {
    if (g_initialized) {
        LOG_WARNING("Lua interface already initialized");
        return true;
    }

    LOG_INFO("Initializing WoWTranslate Lua interface...");

#ifdef MINHOOK_AVAILABLE
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        LOG_ERROR("Failed to initialize MinHook");
        return false;
    }

    // Hook the UnitXP function with our handler
    if (MH_CreateHook(reinterpret_cast<LPVOID>(p_UnitXP),
                      reinterpret_cast<LPVOID>(detoured_UnitXP),
                      reinterpret_cast<LPVOID*>(&p_original_UnitXP)) != MH_OK) {
        LOG_ERROR("Failed to create hook for UnitXP function");
        return false;
    }

    if (MH_EnableHook(reinterpret_cast<LPVOID>(p_UnitXP)) != MH_OK) {
        LOG_ERROR("Failed to enable hook for UnitXP function");
        return false;
    }

    LOG_INFO("Successfully hooked UnitXP function");
#else
    LOG_WARNING("MinHook not available - hooks not installed");
#endif

    g_initialized = true;
    LOG_INFO("WoWTranslate Lua interface initialization complete");
    return true;
}

// Cleanup the Lua interface
void CleanupLuaInterface() {
    if (!g_initialized) {
        return;
    }

    LOG_INFO("Cleaning up WoWTranslate Lua interface...");

#ifdef MINHOOK_AVAILABLE
    // Disable and remove hook
    MH_DisableHook(reinterpret_cast<LPVOID>(p_UnitXP));
    MH_RemoveHook(reinterpret_cast<LPVOID>(p_UnitXP));
    MH_Uninitialize();
#endif

    g_initialized = false;
    LOG_INFO("WoWTranslate Lua interface cleanup complete");
}
