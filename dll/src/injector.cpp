// injector.cpp - Standalone DLL injector for WoW 3.3.5.12340
//
// 1.12 clients get this for free: VanillaFixes / the patched WoW.exe reads
// dlls.txt at startup. The 3.3.5 client has no such feature (its Wow.exe does
// not even contain the string "dlls.txt"), so injection has to be done from
// outside. This launcher does the minimum: find Wow.exe, LoadLibraryW our DLL
// into it, done. The DLL installs its own hooks from DLL_PROCESS_ATTACH.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#include <string>

namespace {

const wchar_t* kDefaultDll = L"WoWTranslate.dll";
const wchar_t* kDefaultProcess = L"Wow.exe";
const int kWaitSeconds = 120;

std::wstring DirectoryOfThisExe() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path{ buf };
    size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
}

bool EqualsNoCase(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

DWORD FindProcessId(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (EqualsNoCase(entry.szExeFile, exeName)) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return pid;
}

// Injecting twice would make MinHook chain a hook onto its own trampoline,
// which corrupts the UnitXP dispatch. Refuse instead.
bool AlreadyInjected(DWORD pid, const wchar_t* dllName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snap, &entry)) {
        do {
            if (EqualsNoCase(entry.szModule, dllName)) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        wprintf(L"[错误] 打不开进程 (pid=%lu, GetLastError=%lu)。请用管理员身份运行。\n",
                pid, GetLastError());
        return false;
    }

    bool ok = false;
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!remote) {
        wprintf(L"[错误] VirtualAllocEx 失败 (%lu)\n", GetLastError());
    } else if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        wprintf(L"[错误] WriteProcessMemory 失败 (%lu)\n", GetLastError());
    } else {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(proc, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (!thread) {
            wprintf(L"[错误] CreateRemoteThread 失败 (%lu)\n", GetLastError());
        } else {
            WaitForSingleObject(thread, INFINITE);
            DWORD moduleHandle = 0;
            GetExitCodeThread(thread, &moduleHandle);
            CloseHandle(thread);
            if (moduleHandle == 0) {
                wprintf(L"[错误] LoadLibraryW 在游戏进程里返回 NULL。\n"
                        L"       常见原因:DLL 是 64 位的(必须 32 位),或缺少依赖。\n");
            } else {
                ok = true;
            }
        }
    }

    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    std::wstring dllPath = argc > 1 ? argv[1] : DirectoryOfThisExe() + kDefaultDll;
    const wchar_t* processName = argc > 2 ? argv[2] : kDefaultProcess;

    wchar_t fullDllPath[MAX_PATH]{};
    if (!GetFullPathNameW(dllPath.c_str(), MAX_PATH, fullDllPath, nullptr) ||
        GetFileAttributesW(fullDllPath) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[错误] 找不到 DLL: %ls\n"
                L"       把 WoWTranslate.dll 和本程序放在同一个目录,或用参数指定路径。\n",
                dllPath.c_str());
        return 1;
    }

    wprintf(L"WoWTranslate 注入器 (WoW 3.3.5.12340)\n");
    wprintf(L"  DLL : %ls\n", fullDllPath);
    wprintf(L"  目标: %ls\n\n", processName);

    DWORD pid = 0;
    for (int i = 0; i < kWaitSeconds && pid == 0; ++i) {
        pid = FindProcessId(processName);
        if (pid == 0) {
            if (i == 0) wprintf(L"等待游戏启动中(最多 %d 秒)…\n", kWaitSeconds);
            Sleep(1000);
        }
    }

    if (pid == 0) {
        wprintf(L"[错误] 没等到 %ls。请先启动游戏,再运行本程序。\n", processName);
        return 1;
    }

    wprintf(L"找到进程 pid=%lu\n", pid);

    if (AlreadyInjected(pid, kDefaultDll)) {
        wprintf(L"[跳过] %ls 已经注入过了,不能重复注入。\n", kDefaultDll);
        return 0;
    }

    if (!Inject(pid, fullDllPath)) {
        return 1;
    }

    wprintf(L"[成功] 已注入。进游戏后用 /wt ping 验证。\n");
    return 0;
}
