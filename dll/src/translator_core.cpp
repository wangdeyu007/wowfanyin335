// translator_core.cpp - Translation functionality for WoWTranslate
// Sends POST requests to transmart.qq.com /api/imt (Tencent TranSmart free tier).
// No API key required; reachable from mainland China without VPN.

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <codecvt>
#include <locale>
#include <vector>
#include <cstdio>

#include "../include/translator_core.h"
#include "../include/logging.h"
#include "../include/utils.h"

using namespace std;

// UTF-8 codepoint encoder (file-scope for use in multiple places)
static string ConvertCodepointToUTF8(unsigned int codepoint) {
    string result;
    if (codepoint <= 0x7F) {
        result += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        result += static_cast<char>(0xC0 | (codepoint >> 6));
        result += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        result += static_cast<char>(0xE0 | (codepoint >> 12));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        result += static_cast<char>(0xF0 | (codepoint >> 18));
        result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return result;
}

// Simple JSON parser for proxy server responses
class SimpleJsonParser {
public:
    static string extractField(const string& json, const string& fieldName) {
        string searchKey = "\"" + fieldName + "\"";
        size_t keyPos = json.find(searchKey);
        if (keyPos == string::npos) {
            return "";
        }

        size_t colonPos = json.find(":", keyPos + searchKey.length());
        if (colonPos == string::npos) {
            return "";
        }

        size_t start = colonPos + 1;
        while (start < json.length() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\n' || json[start] == '\r')) {
            start++;
        }

        if (start >= json.length()) {
            return "";
        }

        // Check if it's a string value (starts with quote)
        if (json[start] == '"') {
            start++;
            size_t end = start;
            while (end < json.length() && json[end] != '"') {
                if (json[end] == '\\' && end + 1 < json.length()) {
                    end += 2; // Skip escaped character
                } else {
                    end++;
                }
            }
            return unescapeJson(json.substr(start, end - start));
        }

        // It's a number or boolean
        size_t end = start;
        while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != '\n') {
            end++;
        }
        string value = json.substr(start, end - start);
        // Trim whitespace
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
            value.pop_back();
        }
        return value;
    }

    static double extractNumber(const string& json, const string& fieldName) {
        string value = extractField(json, fieldName);
        if (value.empty()) return -1;
        try {
            return stod(value);
        } catch (...) {
            return -1;
        }
    }

private:
    static string unescapeJson(const string& input) {
        string result = input;
        size_t pos = 0;

        // Unescape basic characters
        while ((pos = result.find("\\\"", pos)) != string::npos) {
            result.replace(pos, 2, "\"");
            pos += 1;
        }
        pos = 0;
        while ((pos = result.find("\\\\", pos)) != string::npos) {
            result.replace(pos, 2, "\\");
            pos += 1;
        }
        pos = 0;
        while ((pos = result.find("\\n", pos)) != string::npos) {
            result.replace(pos, 2, "\n");
            pos += 1;
        }
        pos = 0;
        while ((pos = result.find("\\r", pos)) != string::npos) {
            result.replace(pos, 2, "\r");
            pos += 1;
        }
        pos = 0;
        while ((pos = result.find("\\t", pos)) != string::npos) {
            result.replace(pos, 2, "\t");
            pos += 1;
        }

        // Handle Unicode escape sequences \uXXXX
        pos = 0;
        while ((pos = result.find("\\u", pos)) != string::npos) {
            if (pos + 5 < result.length()) {
                string hexStr = result.substr(pos + 2, 4);
                try {
                    unsigned int codepoint = stoul(hexStr, nullptr, 16);
                    string utf8_char = ConvertCodepointToUTF8(codepoint);
                    result.replace(pos, 6, utf8_char);
                    pos += utf8_char.length();
                } catch (...) {
                    pos += 6;
                }
            } else {
                break;
            }
        }

        return result;
    }
};

// Global variables
unique_ptr<TranslationClient> g_translator = nullptr;
char g_translation_buffer[4096] = {0};
char g_error_buffer[256] = {0};

TranslationClient::TranslationClient()
    : hSession(nullptr), hConnect(nullptr), initialized(false), running(false) {
}

TranslationClient::~TranslationClient() {
    Cleanup();
}

bool TranslationClient::Initialize() {
    if (initialized) Cleanup();

    const std::string host = "transmart.qq.com";
    const int port = 443;

    LOG_INFO("Initializing TranSmart (Tencent) translation client");

    // 3.3.5's WinHTTP with WINHTTP_ACCESS_TYPE_DEFAULT_PROXY reads the
    // netsh winhttp config (empty by default → direct connection), NOT
    // the IE/WinINET system proxy. Clash/v2ray set the system proxy via
    // the IE registry keys, so we have to read them ourselves.
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ieConfig = {};
    BOOL gotIEConfig = WinHttpGetIEProxyConfigForCurrentUser(&ieConfig);
    BOOL hasIEProxy  = gotIEConfig
                       && ieConfig.lpszProxy
                       && ieConfig.lpszProxy[0] != L'\0';

    if (hasIEProxy) {
        std::wstring proxyW(ieConfig.lpszProxy);
        std::string  proxyA(proxyW.begin(), proxyW.end());  // proxy URLs are ASCII
        LOG_INFO("Using IE system proxy: " + proxyA);
        hSession = WinHttpOpen(L"WoWTranslate/1.0",
                               WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                               ieConfig.lpszProxy,
                               ieConfig.lpszProxyBypass ? ieConfig.lpszProxyBypass
                                                        : WINHTTP_NO_PROXY_BYPASS,
                               0);
    } else {
        LOG_INFO("No IE system proxy configured, connecting directly");
        hSession = WinHttpOpen(L"WoWTranslate/1.0",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS,
                               0);
    }

    // WinHttpGetIEProxyConfigForCurrentUser allocates these with GlobalAlloc;
    // must free or we leak per Initialize() call.
    if (ieConfig.lpszAutoConfigUrl) GlobalFree(ieConfig.lpszAutoConfigUrl);
    if (ieConfig.lpszProxy)         GlobalFree(ieConfig.lpszProxy);
    if (ieConfig.lpszProxyBypass)   GlobalFree(ieConfig.lpszProxyBypass);

    if (!hSession) {
        LOG_ERROR("Failed to initialize WinHTTP session");
        return false;
    }

    // 8-second timeouts so a blocked/unreachable endpoint fails fast
    WinHttpSetTimeouts(hSession, 8000, 8000, 8000, 8000);

    wstring wHost(host.begin(), host.end());
    hConnect = WinHttpConnect(hSession, wHost.c_str(),
                              static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        LOG_ERROR("Failed to connect to transmart.qq.com");
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        return false;
    }

    running = true;
    workerThread = thread(&TranslationClient::WorkerThreadFunc, this);
    initialized = true;
    LOG_INFO("TranSmart translation client initialized");
    return true;
}


void TranslationClient::Cleanup() {
    // Stop worker thread
    if (running) {
        running = false;
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }

    if (hConnect) {
        WinHttpCloseHandle(hConnect);
        hConnect = nullptr;
    }

    if (hSession) {
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
    }

    cache.clear();
    initialized = false;
    LOG_INFO("Translation client cleanup complete");
}

string TranslationClient::UrlEncode(const string& text) {
    ostringstream encoded;
    encoded.fill('0');
    encoded << hex;

    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << uppercase;
            encoded << '%' << setw(2) << static_cast<int>(c);
            encoded << nouppercase;
        }
    }

    return encoded.str();
}

string TranslationClient::GenerateCacheKey(const string& text, const string& sourceLang, const string& targetLang) {
    return sourceLang + "->" + targetLang + ":" + text;
}

void TranslationClient::CleanExpiredCache() {
    DWORD currentTime = GetTickCount();
    auto it = cache.begin();

    while (it != cache.end()) {
        if (currentTime - it->second.timestamp > CACHE_EXPIRY_MS) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }

    if (cache.size() > MAX_CACHE_SIZE) {
        size_t removeCount = cache.size() - MAX_CACHE_SIZE / 2;
        for (size_t i = 0; i < removeCount && !cache.empty(); ++i) {
            cache.erase(cache.begin());
        }
    }
}

string TranslationClient::MapLangCode(const string& lang) {
    if (lang == "zh") return "zh-CN";
    // ja, ko, ru, en are already valid Google lang codes
    return lang;
}

string TranslationClient::HttpsGet(const string& path) {
    if (!hConnect) return "";

    wstring wPath(path.begin(), path.end());

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", wPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest) {
        LOG_ERROR("Failed to open GET request");
        return "";
    }

    // Identify as a browser to avoid 403s
    wstring headers = L"User-Agent: Mozilla/5.0\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD);

    BOOL result = WinHttpSendRequest(hRequest,
                                     WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     nullptr, 0, 0, 0);

    string response;
    if (result && WinHttpReceiveResponse(hRequest, nullptr)) {
        // Read HTTP status code before consuming the body.
        // A 429 response has an unparseable body; surface it explicitly
        // so the Lua side can trigger immediate backoff rather than treating
        // it as a generic API parse failure.
        DWORD statusCode = 0;
        DWORD statusLen  = sizeof(DWORD);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode, &statusLen,
            WINHTTP_NO_HEADER_INDEX);
        if (statusCode == 429) {
            LOG_WARNING("Rate limited by Google (HTTP 429)");
            WinHttpCloseHandle(hRequest);
            return "HTTP_429";
        }

        DWORD bytesAvailable = 0;
        char buffer[8192];
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable)
               && bytesAvailable > 0) {
            DWORD bytesRead = 0;
            DWORD bytesToRead = min(bytesAvailable, (DWORD)(sizeof(buffer) - 1));
            if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
                buffer[bytesRead] = '\0';
                response += string(buffer, bytesRead);
            } else {
                break;
            }
        }
    } else {
        LOG_ERROR("GET request failed: " + to_string(GetLastError()));
    }

    WinHttpCloseHandle(hRequest);
    return response;
}

// POST application/json over the existing secure connection, return body.
string TranslationClient::HttpsPostJson(const string& path, const string& body) {
    if (!hConnect) return "";

    wstring wPath(path.begin(), path.end());

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hRequest) {
        LOG_ERROR("Failed to open POST request");
        return "";
    }

    // TranSmart rejects requests that don't look like they came from
    // transmart.qq.com — must set Origin/Referer/User-Agent.
    wstring headers = L"Content-Type: application/json\r\n";
    headers += L"Origin: https://transmart.qq.com\r\n";
    headers += L"Referer: https://transmart.qq.com/\r\n";
    headers += L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
               L"AppleWebKit/537.36 (KHTML, like Gecko) "
               L"Chrome/120.0.0.0 Safari/537.36\r\n";
    WinHttpAddRequestHeaders(hRequest, headers.c_str(), (DWORD)-1,
                             WINHTTP_ADDREQ_FLAG_ADD);

    BOOL result = WinHttpSendRequest(hRequest,
                                     WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     (LPVOID)body.c_str(),
                                     (DWORD)body.length(),
                                     (DWORD)body.length(),
                                     0);

    string response;
    if (result && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusLen  = sizeof(DWORD);
        WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode, &statusLen,
            WINHTTP_NO_HEADER_INDEX);
        if (statusCode == 429) {
            LOG_WARNING("Rate limited by TranSmart (HTTP 429)");
            WinHttpCloseHandle(hRequest);
            return "HTTP_429";
        }

        DWORD bytesAvailable = 0;
        char buffer[8192];
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable)
               && bytesAvailable > 0) {
            DWORD bytesRead = 0;
            DWORD bytesToRead = min(bytesAvailable, (DWORD)(sizeof(buffer) - 1));
            if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
                buffer[bytesRead] = '\0';
                response += string(buffer, bytesRead);
            } else {
                break;
            }
        }
    } else {
        LOG_ERROR("POST request failed: " + to_string(GetLastError()));
    }

    WinHttpCloseHandle(hRequest);
    return response;
}

string TranslationClient::ParseTranSmartResponse(const string& json) {
    // Response shape:
    //   { "header": {...}, "auto_translation": [ { "translated_text_list": ["..."] } ] }
    // Pull the first string out of translated_text_list.
    string key = "\"translated_text_list\"";
    size_t keyPos = json.find(key);
    if (keyPos == string::npos) return "";

    size_t arrStart = json.find('[', keyPos);
    if (arrStart == string::npos) return "";

    size_t pos = json.find('"', arrStart);
    if (pos == string::npos) return "";
    pos++;  // skip opening "

    string segment;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case '"':  segment += '"';  break;
                case '\\': segment += '\\'; break;
                case 'n':  segment += '\n'; break;
                case 'r':  segment += '\r'; break;
                case 't':  segment += '\t'; break;
                case 'u': {
                    if (pos + 4 < json.size()) {
                        string hex = json.substr(pos + 1, 4);
                        try {
                            unsigned int cp = stoul(hex, nullptr, 16);
                            segment += ConvertCodepointToUTF8(cp);
                            pos += 4;
                        } catch (...) {}
                    }
                    break;
                }
                default: segment += json[pos]; break;
            }
        } else {
            segment += json[pos];
        }
        pos++;
    }
    return segment;
}

string TranslationClient::ParseGoogleFreeResponse(const string& json) {
    string result;

    // Find the start of the sentence array: [[[
    size_t pos = json.find("[[[");
    if (pos == string::npos) return "";
    pos += 3; // now at opening " of first translated segment

    // Upper bound: the ]] that closes the outer sentence array
    size_t sentencesEnd = json.find("]]", pos);

    while (pos < json.size()) {
        if (json[pos] != '"') break;
        pos++; // skip opening "

        string segment;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                switch (json[pos]) {
                    case '"':  segment += '"';  break;
                    case '\\': segment += '\\'; break;
                    case 'n':  segment += '\n'; break;
                    case 'r':  segment += '\r'; break;
                    case 't':  segment += '\t'; break;
                    case 'u': {
                        if (pos + 4 < json.size()) {
                            string hex = json.substr(pos + 1, 4);
                            try {
                                unsigned int cp = stoul(hex, nullptr, 16);
                                segment += ConvertCodepointToUTF8(cp);
                                pos += 4;
                            } catch (...) {}
                        }
                        break;
                    }
                    default: segment += json[pos]; break;
                }
            } else {
                segment += json[pos];
            }
            pos++;
        }
        result += segment;
        if (pos < json.size()) pos++; // skip closing "

        // Find next inner array: ,["  within the sentence array bounds
        size_t nextInner = json.find(",[\"", pos);
        if (nextInner == string::npos ||
            (sentencesEnd != string::npos && nextInner > sentencesEnd)) break;
        pos = nextInner + 2; // point at the opening " of next segment
    }

    return result;
}

TranslationResult TranslationClient::TranslateText(const string& text, string& result,
                                                    const string& sourceLang,
                                                    const string& targetLang) {
    if (!initialized) return TranslationResult::INVALID_PARAMS;
    if (text.empty())  return TranslationResult::INVALID_PARAMS;

    // DLL-side cache check
    string cacheKey = GenerateCacheKey(text, sourceLang, targetLang);
    auto cacheIt = cache.find(cacheKey);
    if (cacheIt != cache.end() &&
        (GetTickCount() - cacheIt->second.timestamp) < CACHE_EXPIRY_MS) {
        result = cacheIt->second.translation;
        LOG_DEBUG("Cache hit: " + text.substr(0, 50));
        return TranslationResult::SUCCESS;
    }

    CleanExpiredCache();

    // Build TranSmart POST body. Language codes are the short forms TranSmart
    // accepts: "zh", "en", "ru", "ja", "ko", or "auto" for source.
    string sl = sourceLang;
    string tl = targetLang;
    if (sl == "zh-CN") sl = "zh";
    if (sl == "zh-TW") sl = "zh-TW";
    if (tl == "zh-CN") tl = "zh";

    // JSON-escape the input text (only the chars that break the body literal)
    string escaped;
    escaped.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n";  break;
            case '\r': escaped += "\\r";  break;
            case '\t': escaped += "\\t";  break;
            default:   escaped += (char)c; break;
        }
    }

    string body = string("{\"header\":{\"fn\":\"auto_translation\",")
                  + "\"client_key\":\"desktop-chrome\"},"
                  + "\"type\":\"Plain\",\"model_category\":\"normal\","
                  + "\"source\":{\"lang_code\":\"" + sl
                  + "\",\"text_list\":[\"" + escaped + "\"]},"
                  + "\"target\":{\"lang_code\":\"" + tl + "\"}}";

    LOG_DEBUG("POST /api/imt body=" + body.substr(0, min<size_t>(body.size(), 200)));

    string response = HttpsPostJson("/api/imt", body);

    if (response == "HTTP_429") {
        return TranslationResult::RATE_LIMITED;
    }

    if (response.empty()) {
        LOG_ERROR("Empty response from TranSmart");
        return TranslationResult::NETWORK_ERROR;
    }

    LOG_DEBUG("Response: " + response.substr(0, 200));

    string translation = ParseTranSmartResponse(response);

    if (translation.empty()) {
        LOG_ERROR("Failed to parse TranSmart response: " + response.substr(0, 200));
        return TranslationResult::API_ERROR;
    }

    cache[cacheKey] = CacheEntry(translation);
    result = translation;
    LOG_DEBUG("Translated: " + text.substr(0, 30) + " -> " + translation.substr(0, 50));
    return TranslationResult::SUCCESS;
}

// Queue async translation request
bool TranslationClient::TranslateAsync(const string& requestId, const string& text,
                                       const string& sourceLang, const string& targetLang) {
    if (!initialized || !running) {
        return false;
    }

    lock_guard<mutex> lock(requestMutex);
    requestQueue.push(AsyncRequest(requestId, text, sourceLang, targetLang));
    LOG_DEBUG("Async request queued: " + requestId + " (" + sourceLang + " -> " + targetLang + ")");
    return true;
}

// Poll for completed translation
bool TranslationClient::PollResult(string& requestId, string& translation, string& error) {
    lock_guard<mutex> lock(resultMutex);

    if (resultQueue.empty()) {
        return false;
    }

    AsyncResult result = resultQueue.front();
    resultQueue.pop();

    requestId = result.requestId;
    translation = result.translation;
    error = result.error;

    return true;
}

// Get count of pending requests
size_t TranslationClient::GetPendingCount() {
    lock_guard<mutex> lock(requestMutex);
    return requestQueue.size();
}

// Worker thread for async translations
void TranslationClient::WorkerThreadFunc() {
    LOG_INFO("Worker thread started");

    while (running) {
        AsyncRequest request;
        bool hasRequest = false;

        {
            lock_guard<mutex> lock(requestMutex);
            if (!requestQueue.empty()) {
                request = requestQueue.front();
                requestQueue.pop();
                hasRequest = true;
            }
        }

        if (hasRequest) {
            LOG_DEBUG("Processing async request: " + request.requestId);

            string translation;
            string error;

            TranslationResult tr = TranslateText(request.text, translation,
                                                  request.sourceLang, request.targetLang);

            if (tr != TranslationResult::SUCCESS) {
                switch (tr) {
                    case TranslationResult::NETWORK_ERROR:  error = "network error"; break;
                    case TranslationResult::API_ERROR:      error = "API error";     break;
                    case TranslationResult::ENCODING_ERROR: error = "encoding error"; break;
                    case TranslationResult::TIMEOUT_ERROR:  error = "timeout";       break;
                    case TranslationResult::RATE_LIMITED:   error = "rate limited";  break;
                    default:                                error = "unknown error";  break;
                }
                translation = "";
            }

            {
                lock_guard<mutex> lock(resultMutex);
                resultQueue.push(AsyncResult(request.requestId, translation, error));
            }

            LOG_DEBUG("Async request completed: " + request.requestId);
        } else {
            Sleep(50);
        }
    }

    LOG_INFO("Worker thread stopped");
}
