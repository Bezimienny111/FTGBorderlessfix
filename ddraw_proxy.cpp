#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ddraw.h>
#include <winver.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
constexpr char proxyVersion[] = "1.2.0";
wchar_t logPath[MAX_PATH] = {};

void WriteLog(const char* format, ...) {
    if (logPath[0] == L'\0') {
        return;
    }
    DWORD previousError = GetLastError();
    char message[1536] = {};
    va_list arguments;
    va_start(arguments, format);
    vsnprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
    va_end(arguments);
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char line[1800] = {};
    sprintf_s(line, "[%04u-%02u-%02u %02u:%02u:%02u.%03u PID=%lu] %s\r\n",
              static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
              static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
              static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
              static_cast<unsigned>(time.wMilliseconds), GetCurrentProcessId(), message);
    HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        DWORD length = static_cast<DWORD>(std::strlen(line));
        if (!WriteFile(file, line, length, &written, nullptr) || written != length) {
            OutputDebugStringW(L"FTGWindow: could not write FTGWindow.log.\n");
        }
        CloseHandle(file);
    } else {
        OutputDebugStringW(L"FTGWindow: could not open FTGWindow.log beside the proxy DLL.\n");
    }
    SetLastError(previousError);
}

void LogPath(const char* label, const wchar_t* path) {
    char utf8[MAX_PATH * 4] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, _countof(utf8), nullptr, nullptr) != 0) {
        WriteLog("%s: %s", label, utf8);
    } else {
        WriteLog("%s: path conversion failed; error=%lu", label, GetLastError());
    }
}

bool ReadVersionResource(HMODULE module, VS_FIXEDFILEINFO& version) {
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16));
    if (resource == nullptr) {
        return false;
    }
    DWORD size = SizeofResource(module, resource);
    HGLOBAL loaded = LoadResource(module, resource);
    const BYTE* data = static_cast<const BYTE*>(LockResource(loaded));
    constexpr wchar_t key[] = L"VS_VERSION_INFO";
    constexpr SIZE_T valueOffset = (3 * sizeof(WORD) + sizeof(key) + 3) & ~SIZE_T(3);
    if (data == nullptr || size < valueOffset + sizeof(version)) {
        return false;
    }
    WORD length = 0;
    WORD valueLength = 0;
    std::memcpy(&length, data, sizeof(length));
    std::memcpy(&valueLength, data + sizeof(WORD), sizeof(valueLength));
    if (length > size || length < valueOffset + sizeof(version) ||
        valueLength < sizeof(version) ||
        std::memcmp(data + 3 * sizeof(WORD), key, sizeof(key)) != 0) {
        return false;
    }
    std::memcpy(&version, data + valueOffset, sizeof(version));
    return version.dwSignature == 0xFEEF04BD;
}

void InitializeLog(HINSTANCE instance) {
    wchar_t proxyPath[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(instance, proxyPath, _countof(proxyPath));
    if (length == 0 || length >= _countof(proxyPath)) {
        OutputDebugStringW(L"FTGWindow: cannot determine proxy path; file logging disabled.\n");
        return;
    }
    wcscpy_s(logPath, proxyPath);
    wchar_t* separator = std::wcsrchr(logPath, L'\\');
    if (separator == nullptr) {
        logPath[0] = L'\0';
        return;
    }
    separator[1] = L'\0';
    if (wcscat_s(logPath, L"FTGWindow.log") != 0) {
        logPath[0] = L'\0';
        return;
    }
    WriteLog("=== DLL_PROCESS_ATTACH: FTGWindow proxy %s (x86; built %s %s) ===",
             proxyVersion, __DATE__, __TIME__);
    LogPath("Proxy DLL", proxyPath);
    wchar_t executable[MAX_PATH] = {};
    length = GetModuleFileNameW(nullptr, executable, _countof(executable));
    if (length > 0 && length < _countof(executable)) {
        LogPath("Game executable", executable);
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (GetFileAttributesExW(executable, GetFileExInfoStandard, &attributes)) {
            ULARGE_INTEGER fileSize = {};
            fileSize.LowPart = attributes.nFileSizeLow;
            fileSize.HighPart = attributes.nFileSizeHigh;
            WriteLog("Game executable size=%llu bytes", static_cast<unsigned long long>(fileSize.QuadPart));
        }
    } else {
        WriteLog("Game executable path unavailable; error=%lu", GetLastError());
    }
    HMODULE game = GetModuleHandleW(nullptr);
    VS_FIXEDFILEINFO version = {};
    if (ReadVersionResource(game, version)) {
        WriteLog("Game file version=%u.%u.%u.%u; product version=%u.%u.%u.%u",
                 static_cast<unsigned>(HIWORD(version.dwFileVersionMS)),
                 static_cast<unsigned>(LOWORD(version.dwFileVersionMS)),
                 static_cast<unsigned>(HIWORD(version.dwFileVersionLS)),
                 static_cast<unsigned>(LOWORD(version.dwFileVersionLS)),
                 static_cast<unsigned>(HIWORD(version.dwProductVersionMS)),
                 static_cast<unsigned>(LOWORD(version.dwProductVersionMS)),
                 static_cast<unsigned>(HIWORD(version.dwProductVersionLS)),
                 static_cast<unsigned>(LOWORD(version.dwProductVersionLS)));
    } else {
        WriteLog("Game file version=unavailable (missing/invalid PE version resource)");
    }
    WriteLog("Game module base=%p; window-hook compatibility uses executable byte signatures", game);
}

using CreateWindowFunction = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int,
                                         int, int, HWND, HMENU, HINSTANCE, LPVOID);
using DirectDrawFunction = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);

struct Patch {
    DWORD rva;
    SIZE_T size;
    BYTE original[18];
    BYTE replacement[18];
};

constexpr Patch patches[] = {
    {0x22C46D, 16,
     {0x8b,0x35,0x1c,0xb1,0x65,0x00,0x6a,0x08,0xff,0xd6,0x6a,0x04,0x8b,0xf8,0xff,0xd6},
     {0x33,0xc0,0x33,0xff,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
    {0x22C48B, 3, {0x83,0xc2,0x04}, {0x83,0xc2,0x00}},
    {0x22C59F, 16,
     {0x8b,0x35,0x1c,0xb1,0x65,0x00,0x6a,0x08,0xff,0xd6,0x6a,0x04,0x8b,0xf8,0xff,0xd6},
     {0x33,0xc0,0x33,0xff,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
    {0x22C5BB, 3, {0x83,0xc3,0x04}, {0x83,0xc3,0x00}},
    {0x2378FA, 18,
     {0x6a,0x04,0xff,0x15,0x1c,0xb1,0x65,0x00,0x8b,0xf0,0x6a,0x08,0xff,0x15,0x1c,0xb1,0x65,0x00},
     {0x33,0xc0,0x33,0xf6,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
    {0x237932, 3, {0x83,0xc1,0x04}, {0x83,0xc1,0x00}},
};
constexpr BYTE windowSignature[] = {
    0x6a,0x04,0xff,0x15,0x1c,0xb1,0x65,0x00,0x8b,0x0d,0x00,0x41,0x6c,0x00,
    0x8d,0x54,0x01,0x08,0x52,0x6a,0x07,0xff,0x15,0x1c,0xb1,0x65,0x00,
    0x8b,0x0d,0xfc,0x40,0x6c,0x00,0x8d,0x14,0x41,0x52,0x6a,0x00,0x6a,0x00,
    0x68,0x00,0x00,0x00,0x10,0xa1,0xdc,0x05,0x6d,0x00,0x50,0x8b,0x0d,0xdc,0x05,0x6d,0x00,0x51,0x6a,0x00
};

constexpr Patch olderPatches[] = {
    {0x2275BD, 16,
     {0x8b,0x35,0x18,0x61,0x65,0x00,0x6a,0x08,0xff,0xd6,0x6a,0x04,0x8b,0xf8,0xff,0xd6},
     {0x33,0xc0,0x33,0xff,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
    {0x2275DB, 3, {0x83,0xc2,0x04}, {0x83,0xc2,0x00}},
    {0x2276EF, 16,
     {0x8b,0x35,0x18,0x61,0x65,0x00,0x6a,0x08,0xff,0xd6,0x6a,0x04,0x8b,0xf8,0xff,0xd6},
     {0x33,0xc0,0x33,0xff,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
    {0x22770B, 3, {0x83,0xc3,0x04}, {0x83,0xc3,0x00}},
    {0x232603, 18,
     {0x6a,0x04,0xff,0x15,0x18,0x61,0x65,0x00,0x8b,0xf0,0x6a,0x08,0xff,0x15,0x18,0x61,0x65,0x00},
     {0x33,0xc0,0x33,0xf6,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}},
    {0x23263B, 3, {0x83,0xc2,0x04}, {0x83,0xc2,0x00}},
};
constexpr BYTE olderWindowSignature[] = {
    0x6a,0x04,0xff,0x15,0x18,0x61,0x65,0x00,0x8b,0x0d,0xfc,0xe0,0x6b,0x00,
    0x8d,0x54,0x01,0x08,0x52,0x6a,0x07,0xff,0x15,0x18,0x61,0x65,0x00,
    0x8b,0x0d,0xf8,0xe0,0x6b,0x00,0x8d,0x14,0x41,0x52,0x6a,0x00,0x6a,0x00,
    0x68,0x00,0x00,0x00,0x10,0xa1,0x28,0xb9,0x6c,0x00,0x50,0x8b,0x0d,0x28,0xb9,0x6c,0x00,0x51,0x6a,0x00
};

struct BuildProfile {
    const char* name;
    const Patch* patches;
    SIZE_T patchCount;
    DWORD windowRva;
    const BYTE* windowBytes;
    SIZE_T windowSize;
    DWORD importRva;
    DWORD widthRva;
    DWORD heightRva;
    DWORD windowedRva;
    DWORD flagInstructionRva;
};

constexpr BuildProfile builds[] = {
    {"FTG newer (4ddc08b9)", patches, _countof(patches), 0x232787,
     windowSignature, sizeof(windowSignature), 0x25B0F4, 0x2C40FC, 0x2C4100, 0x49DE26, 0x232676},
    {"FTG older GOG (8b889c71)", olderPatches, _countof(olderPatches), 0x22D8B7,
     olderWindowSignature, sizeof(olderWindowSignature), 0x2560F0, 0x2BE0F8, 0x2BE0FC, 0x497E2B, 0x22D7A6},
};
const BuildProfile* activeBuild = &builds[0];

bool MatchesBuild(const BYTE* base, SIZE_T imageSize, const BuildProfile& build) {
    const auto fits = [imageSize](DWORD rva, SIZE_T size) {
        return rva < imageSize && size <= imageSize - rva;
    };
    if (!fits(build.windowRva, build.windowSize) || !fits(build.importRva, sizeof(void*)) ||
        !fits(build.widthRva, sizeof(int)) || !fits(build.heightRva, sizeof(int)) ||
        !fits(build.windowedRva, 1) || !fits(build.flagInstructionRva, 7) ||
        std::memcmp(base + build.windowRva, build.windowBytes, build.windowSize) != 0) {
        return false;
    }
    const BYTE flagPrefix[] = {0x0f,0xb6,0x05};
    DWORD flagAddress = 0;
    std::memcpy(&flagAddress, base + build.flagInstructionRva + 3, sizeof(flagAddress));
    if (std::memcmp(base + build.flagInstructionRva, flagPrefix, sizeof(flagPrefix)) != 0 ||
        flagAddress != 0x400000 + build.windowedRva) {
        return false;
    }
    for (SIZE_T index = 0; index < build.patchCount; ++index) {
        const auto& patch = build.patches[index];
        if (!fits(patch.rva, patch.size) ||
            std::memcmp(base + patch.rva, patch.original, patch.size) != 0) {
            return false;
        }
    }
    return true;
}

BYTE* gameBase = nullptr;
CreateWindowFunction originalCreateWindow = nullptr;
CreateWindowFunction* windowImport = nullptr;
INIT_ONCE settingsOnce = INIT_ONCE_STATIC_INIT;
INIT_ONCE drawOnce = INIT_ONCE_STATIC_INIT;
bool borderless = true;
bool patchesApplied = false;
DirectDrawFunction systemDirectDraw = nullptr;

bool MatchPatches(bool replacement) {
    for (SIZE_T index = 0; index < activeBuild->patchCount; ++index) {
        const auto& patch = activeBuild->patches[index];
        if (std::memcmp(gameBase + patch.rva,
                        replacement ? patch.replacement : patch.original, patch.size) != 0) {
            return false;
        }
    }
    return true;
}

bool WritePatches(bool apply) {
    if (!MatchPatches(!apply)) {
        WriteLog("Memory patches %s rejected: executable bytes do not match", apply ? "apply" : "restore");
        return false;
    }
    BYTE* start = gameBase + activeBuild->patches[0].rva;
    const auto& last = activeBuild->patches[activeBuild->patchCount - 1];
    SIZE_T size = last.rva + last.size - activeBuild->patches[0].rva;
    DWORD protection = 0;
    if (!VirtualProtect(start, size, PAGE_EXECUTE_READWRITE, &protection)) {
        WriteLog("Memory patches: VirtualProtect failed; error=%lu", GetLastError());
        return false;
    }
    for (SIZE_T index = 0; index < activeBuild->patchCount; ++index) {
        const auto& patch = activeBuild->patches[index];
        std::memcpy(gameBase + patch.rva, apply ? patch.replacement : patch.original, patch.size);
    }
    FlushInstructionCache(GetCurrentProcess(), start, size);
    DWORD ignored = 0;
    if (!VirtualProtect(start, size, protection, &ignored)) {
        WriteLog("Memory patches: protection restore failed; rolling back; error=%lu", GetLastError());
        for (SIZE_T index = 0; index < activeBuild->patchCount; ++index) {
            const auto& patch = activeBuild->patches[index];
            std::memcpy(gameBase + patch.rva, apply ? patch.original : patch.replacement, patch.size);
        }
        FlushInstructionCache(GetCurrentProcess(), start, size);
        VirtualProtect(start, size, protection, &ignored);
        return false;
    }
    patchesApplied = apply;
    WriteLog("Memory patches %s successfully", apply ? "applied" : "restored");
    return true;
}

BOOL CALLBACK ReadSettings(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, _countof(path));
    if (length == 0 || length >= _countof(path)) {
        borderless = false;
        WriteLog("Settings: executable path unavailable; borderless disabled");
        return TRUE;
    }
    wchar_t* separator = std::wcsrchr(path, L'\\');
    if (separator == nullptr) {
        borderless = false;
        WriteLog("Settings: executable directory unavailable; borderless disabled");
        return TRUE;
    }
    separator[1] = L'\0';
    if (wcscat_s(path, L"FTGWindow.ini") != 0) {
        borderless = false;
        WriteLog("Settings: INI path too long; borderless disabled");
        return TRUE;
    }
    borderless = GetPrivateProfileIntW(L"Window", L"Borderless", 1, path) == 1;
    LogPath("Settings INI", path);
    WriteLog("Settings: Borderless=%u (missing key/file defaults to 1)", static_cast<unsigned>(borderless));
    return TRUE;
}

HWND WINAPI CreateGameWindow(DWORD extendedStyle, LPCSTR className, LPCSTR title,
                            DWORD style, int left, int top, int width, int height,
                            HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    InitOnceExecuteOnce(&settingsOnce, ReadSettings, nullptr, nullptr);
    if (borderless && gameBase[activeBuild->windowedRva] != 0 &&
        (patchesApplied || WritePatches(true))) {
        width = *reinterpret_cast<const int*>(gameBase + activeBuild->widthRva);
        height = *reinterpret_cast<const int*>(gameBase + activeBuild->heightRva);
        left = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        top = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        style = WS_POPUP | WS_VISIBLE;
    }
    HWND window = originalCreateWindow(extendedStyle, className, title, style, left, top,
                                       width, height, parent, menu, instance, parameter);
    WriteLog("CreateWindowExA: windowed=%u borderless=%u patches=%u style=0x%08lX position=%d,%d size=%dx%d window=%p error=%lu",
             static_cast<unsigned>(gameBase[activeBuild->windowedRva] != 0), static_cast<unsigned>(borderless),
             static_cast<unsigned>(patchesApplied), style, left, top, width, height,
             window, window == nullptr ? GetLastError() : ERROR_SUCCESS);
    return window;
}

bool ReplaceWindowImport(CreateWindowFunction replacement) {
    DWORD protection = 0;
    if (!VirtualProtect(windowImport, sizeof(*windowImport), PAGE_READWRITE, &protection)) {
        WriteLog("Window import: VirtualProtect failed; error=%lu", GetLastError());
        return false;
    }
    CreateWindowFunction previous = *windowImport;
    *windowImport = replacement;
    DWORD ignored = 0;
    if (!VirtualProtect(windowImport, sizeof(*windowImport), protection, &ignored)) {
        WriteLog("Window import: protection restore failed; rolling back; error=%lu", GetLastError());
        *windowImport = previous;
        VirtualProtect(windowImport, sizeof(*windowImport), protection, &ignored);
        return false;
    }
    return true;
}

void InstallWindowHook() {
    gameBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<ULONG_PTR>(gameBase) != 0x400000) {
        WriteLog("Window hook disabled: unsupported executable base (expected 0x00400000)");
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) {
        WriteLog("Window hook disabled: unsupported DOS/PE header");
        return;
    }
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS32*>(gameBase + dos->e_lfanew);
    if (headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        OutputDebugStringW(L"FTGWindow: unsupported or already patched executable; window hook disabled.\n");
        WriteLog("Window hook disabled: unsupported or already patched executable; architecture/size/signature check failed");
        return;
    }
    activeBuild = nullptr;
    for (const auto& build : builds) {
        if (MatchesBuild(gameBase, headers->OptionalHeader.SizeOfImage, build)) {
            activeBuild = &build;
            break;
        }
    }
    if (activeBuild == nullptr) {
        WriteLog("Window hook disabled: unsupported or already patched executable; no build profile matched");
        return;
    }
    WriteLog("Executable profile matched: %s", activeBuild->name);
    windowImport = reinterpret_cast<CreateWindowFunction*>(gameBase + activeBuild->importRva);
    originalCreateWindow = *windowImport;
    const auto* expected = GetProcAddress(GetModuleHandleW(L"user32.dll"), "CreateWindowExA");
    if (reinterpret_cast<FARPROC>(originalCreateWindow) != expected) {
        WriteLog("Window hook disabled: CreateWindowExA import already replaced or unavailable");
        windowImport = nullptr;
        return;
    }
    if (!ReplaceWindowImport(CreateGameWindow)) {
        WriteLog("Window hook disabled: failed to replace CreateWindowExA import");
        windowImport = nullptr;
    } else {
        WriteLog("Window hook installed: CreateWindowExA import replaced; executable signatures matched");
    }
}

BOOL CALLBACK LoadSystemDirectDraw(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t systemPath[MAX_PATH] = {};
    UINT directoryLength = GetSystemDirectoryW(systemPath, _countof(systemPath));
    if (directoryLength == 0 || directoryLength >= _countof(systemPath) ||
        wcscat_s(systemPath, L"\\ddraw.dll") != 0) {
        WriteLog("System DirectDraw: system path unavailable; error=%lu", GetLastError());
        return TRUE;
    }
    HMODULE library = LoadLibraryExW(systemPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library == nullptr) {
        WriteLog("System DirectDraw: LoadLibraryExW failed; error=%lu", GetLastError());
        return TRUE;
    }
    FARPROC function = GetProcAddress(library, "DirectDrawCreate");
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(library, path, _countof(path));
    HMODULE pinned = nullptr;
    if (function != nullptr && length > 0 && length < _countof(path) &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, path, &pinned)) {
        systemDirectDraw = reinterpret_cast<DirectDrawFunction>(function);
        LogPath("System DirectDraw", path);
        WriteLog("System DirectDraw: DirectDrawCreate forwarding ready");
    } else {
        WriteLog("System DirectDraw: export lookup/path/module pin failed; error=%lu", GetLastError());
    }
    FreeLibrary(library);
    return TRUE;
}
}

extern "C" HRESULT WINAPI FTGDirectDrawCreate(GUID* guid, LPDIRECTDRAW* draw, IUnknown* outer) {
    InitOnceExecuteOnce(&drawOnce, LoadSystemDirectDraw, nullptr, nullptr);
    if (systemDirectDraw == nullptr) {
        WriteLog("DirectDrawCreate: forwarding unavailable; returning E_FAIL");
        if (draw != nullptr) {
            *draw = nullptr;
        }
        return E_FAIL;
    }
    HRESULT result = systemDirectDraw(guid, draw, outer);
    WriteLog("DirectDrawCreate: HRESULT=0x%08lX", static_cast<unsigned long>(result));
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeLog(instance);
        InstallWindowHook();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) {
            if (windowImport != nullptr && *windowImport == CreateGameWindow) {
                ReplaceWindowImport(originalCreateWindow);
            }
            if (patchesApplied) {
                WritePatches(false);
            }
        }
        WriteLog("=== DLL_PROCESS_DETACH: %s ===", reserved == nullptr ? "explicit unload" : "process shutdown");
    }
    return TRUE;
}

#ifdef FTG_WINDOW_PROXY_TEST

namespace {
struct CapturedWindow {
    DWORD style;
    int left;
    int top;
    int width;
    int height;
} captured = {};

HWND WINAPI CaptureWindow(DWORD, LPCSTR, LPCSTR, DWORD style, int left, int top,
                          int width, int height, HWND, HMENU, HINSTANCE, LPVOID) {
    captured = {style, left, top, width, height};
    return reinterpret_cast<HWND>(1);
}
}

int wmain(int argumentCount, wchar_t* arguments[]) {
    bool passed = true;
    for (const auto& build : builds) {
    activeBuild = &build;
    std::printf("Testing profile: %s\n", build.name);
    gameBase = static_cast<BYTE*>(VirtualAlloc(nullptr, 0x500000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (gameBase == nullptr) {
        return 1;
    }
    for (SIZE_T index = 0; index < build.patchCount; ++index) {
        const auto& patch = build.patches[index];
        std::memcpy(gameBase + patch.rva, patch.original, patch.size);
    }
    std::memcpy(gameBase + build.windowRva, build.windowBytes, build.windowSize);
    const BYTE flagPrefix[] = {0x0f,0xb6,0x05};
    std::memcpy(gameBase + build.flagInstructionRva, flagPrefix, sizeof(flagPrefix));
    DWORD flagAddress = 0x400000 + build.windowedRva;
    std::memcpy(gameBase + build.flagInstructionRva + 3, &flagAddress, sizeof(flagAddress));
    passed = MatchesBuild(gameBase, 0x500000, build) &&
             !MatchesBuild(gameBase, build.windowedRva, build) && passed;
    originalCreateWindow = CaptureWindow;
    InitOnceExecuteOnce(&settingsOnce, ReadSettings, nullptr, nullptr);
    passed = borderless && passed;
    *reinterpret_cast<int*>(gameBase + build.widthRva) = 800;
    *reinterpret_cast<int*>(gameBase + build.heightRva) = 600;
    gameBase[build.windowedRva] = 1;
    borderless = false;
    CreateGameWindow(0, "FTG", "FTG", 0x10000000, 0, 0, 808, 632, nullptr, nullptr, nullptr, nullptr);
    passed = passed && !patchesApplied && MatchPatches(false) &&
             captured.style == 0x10000000 && captured.width == 808 && captured.height == 632;
    std::puts(passed ? "PASS: Borderless=0 preserves framed window and code" : "FAIL: framed mode");
    borderless = true;
    gameBase[build.windowedRva] = 0;
    CreateGameWindow(0, "FTG", "FTG", 0x90000000, 0, 0, 2560, 1440, nullptr, nullptr, nullptr, nullptr);
    passed = passed && !patchesApplied && MatchPatches(false) &&
             captured.left == 0 && captured.top == 0 && captured.width == 2560 && captured.height == 1440;
    std::puts(passed ? "PASS: fullscreen bypasses borderless changes" : "FAIL: fullscreen");
    gameBase[build.windowedRva] = 1;
    CreateGameWindow(0, "FTG", "FTG", 0x10000000, 0, 0, 808, 632, nullptr, nullptr, nullptr, nullptr);
    passed = passed && patchesApplied && MatchPatches(true) && captured.style == 0x90000000 &&
             captured.width == 800 && captured.height == 600 &&
             captured.left == (GetSystemMetrics(SM_CXSCREEN) - 800) / 2 &&
             captured.top == (GetSystemMetrics(SM_CYSCREEN) - 600) / 2;
    std::puts(passed ? "PASS: borderless centers client size and updates drawing/cursor offsets" : "FAIL: borderless");
    passed = WritePatches(false) && passed && MatchPatches(false);
    gameBase[build.patches[0].rva] = 0;
    passed = passed && !WritePatches(true) && !patchesApplied && !MatchesBuild(gameBase, 0x500000, build);
    std::puts(passed ? "PASS: patches reversible; mismatched bytes rejected" : "FAIL: patch safety");
    VirtualFree(gameBase, 0, MEM_RELEASE);
    gameBase = nullptr;
    }
    VS_FIXEDFILEINFO version = {};
    passed = ReadVersionResource(GetModuleHandleW(L"kernel32.dll"), version) && passed;
    std::puts(passed ? "PASS: PE version resource read without version.dll" : "FAIL: version resource");
    if (argumentCount > 1) {
        HMODULE executable = LoadLibraryExW(arguments[1], nullptr,
                                            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        VS_FIXEDFILEINFO gameVersion = {};
        bool versionRead = executable != nullptr && ReadVersionResource(executable, gameVersion);
        if (executable != nullptr) {
            FreeLibrary(executable);
        }
        passed = passed && versionRead;
        if (versionRead) {
            std::printf("PASS: supplied EXE version=%u.%u.%u.%u; product=%u.%u.%u.%u (resource-only load)\n",
                        static_cast<unsigned>(HIWORD(gameVersion.dwFileVersionMS)),
                        static_cast<unsigned>(LOWORD(gameVersion.dwFileVersionMS)),
                        static_cast<unsigned>(HIWORD(gameVersion.dwFileVersionLS)),
                        static_cast<unsigned>(LOWORD(gameVersion.dwFileVersionLS)),
                        static_cast<unsigned>(HIWORD(gameVersion.dwProductVersionMS)),
                        static_cast<unsigned>(LOWORD(gameVersion.dwProductVersionMS)),
                        static_cast<unsigned>(HIWORD(gameVersion.dwProductVersionLS)),
                        static_cast<unsigned>(LOWORD(gameVersion.dwProductVersionLS)));
        } else {
            std::puts("FAIL: supplied EXE version resource");
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA logAttributes = {};
    DWORD logStart = GetFileAttributesExW(L"FTGWindow.log", GetFileExInfoStandard, &logAttributes)
                     ? logAttributes.nFileSizeLow : 0;
    HMODULE proxy = LoadLibraryW(L"ddraw.dll");
    if (proxy == nullptr) {
        return 1;
    }
    auto createDraw = reinterpret_cast<DirectDrawFunction>(GetProcAddress(proxy, "DirectDrawCreate"));
    LPDIRECTDRAW draw = nullptr;
    HRESULT result = createDraw != nullptr ? createDraw(nullptr, &draw, nullptr) : E_FAIL;
    passed = passed && SUCCEEDED(result) && draw != nullptr;
    if (draw != nullptr) {
        draw->Release();
    }
    FreeLibrary(proxy);
    std::puts(passed ? "PASS: built DLL forwards DirectDrawCreate to system DirectDraw" : "FAIL: proxy forwarding");
    HANDLE log = CreateFileW(L"FTGWindow.log", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    char session[8192] = {};
    DWORD read = 0;
    bool logRead = false;
    if (log != INVALID_HANDLE_VALUE) {
        if (SetFilePointer(log, static_cast<LONG>(logStart), nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
            logRead = ReadFile(log, session, sizeof(session) - 1, &read, nullptr) != FALSE;
        }
        CloseHandle(log);
    }
    passed = passed && logRead && read > 0 && std::strstr(session, "DLL_PROCESS_ATTACH") != nullptr &&
             std::strstr(session, proxyVersion) != nullptr && std::strstr(session, "Game executable:") != nullptr &&
             std::strstr(session, "Game file version=") != nullptr &&
             std::strstr(session, "Window hook disabled: unsupported") != nullptr &&
             std::strstr(session, "System DirectDraw: DirectDrawCreate forwarding ready") != nullptr &&
             std::strstr(session, "DirectDrawCreate: HRESULT=0x00000000") != nullptr &&
             std::strstr(session, "DLL_PROCESS_DETACH") != nullptr;
    std::puts(passed ? "PASS: fresh DLL session logs version, host, hook rejection, forwarding and unload" : "FAIL: startup log");
    return passed ? 0 : 1;
}
#endif