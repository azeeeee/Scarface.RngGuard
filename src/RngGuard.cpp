// Scarface - RandomRange guard + crash logger (ASI plugin)
//
// NOTE: this plugin, including the crash analysis and every runtime patch,
// was produced entirely by an AI assistant.
//
// Built for scarface.exe 1.0.0.2 (time stamp 0x455CAD51, image base 0x400000).
//
// What it does:
//   1) Hooks the engine random-in-range helper at RVA 0x1330 (VA 0x401330)
//      and reproduces it exactly, EXCEPT that a zero-length range
//      (hi - lo + 1 == 0) no longer raises 0xC0000094 (integer divide by zero).
//      In that case it returns 'lo' and writes a line to Scarface.RngGuard.log.
//   2) Installs a vectored exception handler that logs access violations and
//      divide-by-zero faults whose faulting instruction is inside scarface.exe,
//      together with the faulting pointer and a stack trace (RVAs), so the
//      calling subsystem can be identified.
//
// No file in the game directory is modified. Nothing is written to the exe.
//
// Build: see build.bat (32-bit MSVC).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <intrin.h>

// ---------------------------------------------------------------------------
// Addresses for scarface.exe 1.0.0.2
// ---------------------------------------------------------------------------
static const uintptr_t EXE_BASE      = 0x00400000u;
static const uintptr_t EXE_END       = 0x009FC000u; // image base + SizeOfImage
static const uintptr_t RANGE_FN_VA   = 0x00401330u; // RandomRange(lo, hi)
static const uintptr_t RNG_STATE_PTR = 0x00831660u; // uint32_t* (two-word state)

// Original prologue bytes we expect at 0x401330: mov ecx, [0x831660]
static const unsigned char kPrologue[6] = { 0x8B, 0x0D, 0x60, 0x16, 0x83, 0x00 };

// ---------------------------------------------------------------------------
// Configuration (Scarface.RngGuard.ini next to the exe)
// ---------------------------------------------------------------------------
struct Config
{
    int guards;          // EnableGuards
    int logging;         // EnableLogging
    int maxKB;           // LogMaxSizeKB (0 = unlimited)
    int logBadRanges;    // LogBadRanges
    int logInvalidLists; // LogInvalidLists
    int logCrashes;      // LogCrashes
    int logStack;        // LogStackTraces
};

static Config g_cfg = { 1, 1, 1024, 1, 1, 1, 1 };
static char   g_iniPath[MAX_PATH];

#define INI_SECTION "General"
#define INI_ADVANCED "Advanced"

static int IniInt2(const char* sec1, const char* sec2, const char* key, int def)
{
    const int kMissing = -9999;
    int v = (int)GetPrivateProfileIntA(sec1, key, kMissing, g_iniPath);
    if (v != kMissing) return v;
    if (sec2)
    {
        v = (int)GetPrivateProfileIntA(sec2, key, kMissing, g_iniPath);
        if (v != kMissing) return v;
    }
    return def;
}

static void WriteDefaultIni(void)
{
    if (GetFileAttributesA(g_iniPath) != INVALID_FILE_ATTRIBUTES)
        return; // already there

    static const char kDefaultIni[] =
        ";=============================================================================\r\n"
        ";  Scarface.RngGuard.asi\r\n"
        ";\r\n"
        ";  Only one setting matters for most people:\r\n"
        ";\r\n"
        ";      Logging=1   -> write Scarface.RngGuard.log next to scarface.exe\r\n"
        ";      Logging=0   -> do not create or write any log file\r\n"
        ";=============================================================================\r\n"
        "\r\n"
        "[" INI_SECTION "]\r\n"
        "Logging=1\r\n"
        "\r\n"
        ";-----------------------------------------------------------------------------\r\n"
        "; The rest can normally be left as-is.\r\n"
        ";-----------------------------------------------------------------------------\r\n"
        "[" INI_ADVANCED "]\r\n"
        "; 0 = disable the crash guards (not recommended)\r\n"
        "Guards=1\r\n"
        "\r\n"
        "; Log size limit in KB. When reached, the current log is renamed to\r\n"
        "; Scarface.RngGuard.log.old and a new log is started. 0 = no limit.\r\n"
        "LogMaxSizeKB=1024\r\n"
        "\r\n"
        "; Individual log categories (only used when Logging=1)\r\n"
        "LogBadRanges=1\r\n"
        "LogInvalidLists=1\r\n"
        "LogCrashes=1\r\n"
        "LogStackTraces=1\r\n";

    HANDLE h = CreateFileA(g_iniPath, GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(h, kDefaultIni, (DWORD)(sizeof(kDefaultIni) - 1), &written, NULL);
        CloseHandle(h);
    }
}

static void LoadConfig(void)
{
    // Simple switch first: [General] Logging=1/0
    int logging = IniInt2(INI_SECTION, NULL, "Logging", -1);
    if (logging < 0) // legacy [Settings] EnableLogging
        logging = IniInt2("Settings", NULL, "EnableLogging", 1);
    g_cfg.logging = logging;

    int guards = IniInt2(INI_ADVANCED, NULL, "Guards", -1);
    if (guards < 0) // legacy [Settings] EnableGuards
        guards = IniInt2("Settings", NULL, "EnableGuards", 1);
    g_cfg.guards = guards;

    g_cfg.maxKB           = IniInt2(INI_ADVANCED, "Settings", "LogMaxSizeKB", 1024);
    g_cfg.logBadRanges    = IniInt2(INI_ADVANCED, "Settings", "LogBadRanges", 1);
    g_cfg.logInvalidLists = IniInt2(INI_ADVANCED, "Settings", "LogInvalidLists", 1);
    g_cfg.logCrashes      = IniInt2(INI_ADVANCED, "Settings", "LogCrashes", 1);
    g_cfg.logStack        = IniInt2(INI_ADVANCED, "Settings", "LogStackTraces", 1);

    if (g_cfg.maxKB < 0)     g_cfg.maxKB = 0;
    if (g_cfg.maxKB > 65536) g_cfg.maxKB = 65536;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_cs;
static bool             g_csReady = false;
static char             g_logPath[MAX_PATH];
static volatile LONG    g_inLog = 0;

static void BuildPaths(void)
{
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (slash) *(slash + 1) = 0; else exePath[0] = 0;
    _snprintf_s(g_logPath, sizeof(g_logPath), _TRUNCATE,
                "%sScarface.RngGuard.log", exePath);
    _snprintf_s(g_iniPath, sizeof(g_iniPath), _TRUNCATE,
                "%sScarface.RngGuard.ini", exePath);
}

// If the game folder is not writable (e.g. Program Files without elevation),
// move the log to %LOCALAPPDATA%\ScarfaceRngGuard\ so logging still works.
static void EnsureWritableLogDir(void)
{
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        CloseHandle(h);
        return;
    }

    char local[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH) == 0)
        return;

    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\ScarfaceRngGuard", local);
    CreateDirectoryA(dir, NULL);
    _snprintf_s(g_logPath, sizeof(g_logPath), _TRUNCATE,
                "%s\\Scarface.RngGuard.log", dir);
}

// Rename the log to .old once it exceeds LogMaxSizeKB (keeps disk use bounded).
static void RotateLogIfNeeded(void)
{
    if (g_cfg.maxKB <= 0)
        return;

    HANDLE h = CreateFileA(g_logPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER size;
    size.QuadPart = 0;
    BOOL ok = GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (!ok)
        return;

    if (size.QuadPart < (LONGLONG)g_cfg.maxKB * 1024)
        return;

    char oldPath[MAX_PATH];
    _snprintf_s(oldPath, sizeof(oldPath), _TRUNCATE, "%s.old", g_logPath);
    DeleteFileA(oldPath);
    MoveFileExA(g_logPath, oldPath, MOVEFILE_REPLACE_EXISTING);

    // Leave a marker in the new file so a truncated history is obvious.
    HANDLE n = CreateFileA(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (n != INVALID_HANDLE_VALUE)
    {
        static const char kMarker[] =
            "[----] previous log rotated to Scarface.RngGuard.log.old ----\r\n";
        DWORD written = 0;
        WriteFile(n, kMarker, (DWORD)(sizeof(kMarker) - 1), &written, NULL);
        CloseHandle(n);
    }
}

static void LogRaw(const char* text)
{
    if (!g_cfg.logging)
        return;
    if (InterlockedExchange(&g_inLog, 1) != 0)
        return; // avoid recursion (e.g. AV inside logging)

    if (g_csReady) EnterCriticalSection(&g_cs);

    RotateLogIfNeeded();

    // Raw Win32 file I/O: no CRT locks, safe to use from an exception handler.
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        char line[4096];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
                            st.wYear, st.wMonth, st.wDay,
                            st.wHour, st.wMinute, st.wSecond, text);
        if (n > 0)
        {
            DWORD written = 0;
            WriteFile(h, line, (DWORD)n, &written, NULL);
        }
        CloseHandle(h);
    }

    if (g_csReady) LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_inLog, 0);
}

static const char* ModuleNameFor(const void* addr, uintptr_t* outRva)
{
    static char name[MAX_PATH];
    HMODULE h = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &h))
    {
        GetModuleFileNameA(h, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        strcpy_s(name, sizeof(name), base);
        if (outRva) *outRva = (uintptr_t)addr - (uintptr_t)h;
        return name;
    }
    if (outRva) *outRva = (uintptr_t)addr;
    return "?";
}

typedef USHORT (WINAPI* pRtlCaptureStackBackTrace)(ULONG, ULONG, PVOID*, PULONG);
static pRtlCaptureStackBackTrace g_capture = NULL;

// Append a stack trace (max 24 frames) to 'out'. Frames outside the game exe
// are still shown with their module+RVA, which helps identify engine/D3D paths.
static void AppendStack(char* out, size_t outSize, int skip)
{
    out[0] = 0;
    if (!g_capture) return;

    void* frames[24];
    USHORT n = g_capture((ULONG)skip, 24, frames, NULL);
    size_t used = 0;
    for (USHORT i = 0; i < n; i++)
    {
        uintptr_t rva = 0;
        const char* mod = ModuleNameFor(frames[i], &rva);
        int w = _snprintf_s(out + used, outSize - used, _TRUNCATE,
                            "\n      #%02u %s+0x%X", i, mod, (unsigned)rva);
        if (w <= 0) break;
        used += (size_t)w;
        if (used >= outSize - 1) break;
    }
}

// ---------------------------------------------------------------------------
// Bad-range logging (deduplicated per call site)
// ---------------------------------------------------------------------------
struct BadRangeSite { uintptr_t caller; unsigned count; };
static BadRangeSite g_sites[32];
static volatile LONG g_siteCount = 0;
static volatile LONG g_badTotal = 0;

static void LogBadRange(int lo, int hi, void* retAddr)
{
    if (!g_cfg.logging || !g_cfg.logBadRanges)
        return;

    LONG total = InterlockedIncrement(&g_badTotal);
    uintptr_t caller = (uintptr_t)retAddr;

    // Find / create a site entry.
    BadRangeSite* site = NULL;
    LONG n = g_siteCount;
    for (LONG i = 0; i < n; i++)
        if (g_sites[i].caller == caller) { site = &g_sites[i]; break; }

    if (!site && n < (LONG)(sizeof(g_sites) / sizeof(g_sites[0])))
    {
        site = &g_sites[InterlockedIncrement(&g_siteCount) - 1];
        site->caller = caller;
        site->count = 0;
    }
    if (site && ++site->count > 20)
        return; // already described this site often enough

    char buf[2048];
    const char* mod = "scarface.exe";
    uintptr_t rva = caller - EXE_BASE;
    if (caller < EXE_BASE || caller >= EXE_END)
    {
        static char other[MAX_PATH];
        rva = 0;
        const char* m = ModuleNameFor(retAddr, &rva);
        strcpy_s(other, sizeof(other), m);
        mod = other;
    }

    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "BAD RANGE  lo=%d  hi=%d  caller=%s+0x%X  (total=%ld)",
                lo, hi, mod, (unsigned)rva, total);
    size_t len = strlen(buf);
    if (g_cfg.logStack)
        AppendStack(buf + len, sizeof(buf) - len, 3);
    LogRaw(buf);
}

// ---------------------------------------------------------------------------
// RandomRange replacement
//   The original computes:
//       s0 = (s0 & 0xFFFF) * 0x9069 + (s0 >> 16)
//       s1 = (s1 & 0xFFFF) * 0x4650 + (s1 >> 16)
//       value = (s0 << 16) + (s1 & 0xFFFF)
//       return lo + value % (hi - lo + 1)
//   which faults when (hi - lo + 1) == 0.
// ---------------------------------------------------------------------------
extern "C" __declspec(noinline) int __cdecl RandomRangeHook(int lo, int hi)
{
    uint32_t* st = *(uint32_t**)RNG_STATE_PTR;

    if (!st)
    {
        LogBadRange(lo, hi, _ReturnAddress());
        return lo;
    }

    uint32_t s0 = st[0];
    uint32_t s1 = st[1];

    s0 = (uint32_t)(((s0 & 0xFFFFu) * 0x9069u) + (s0 >> 16));
    s1 = (uint32_t)(((s1 & 0xFFFFu) * 0x4650u) + (s1 >> 16));
    st[0] = s0;
    st[1] = s1;

    uint32_t value = (uint32_t)((s0 << 16) + (s1 & 0xFFFFu));
    uint32_t range = (uint32_t)hi - (uint32_t)lo + 1u;

    if (range == 0u)
    {
        // Would be a divide-by-zero. Keep the state advanced (as the original
        // would have) and pick the low bound.
        LogBadRange(lo, hi, _ReturnAddress());
        return lo;
    }

    return (int)((uint32_t)lo + (value % range));
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------
static bool PatchBytes(void* dst, const void* src, size_t len)
{
    DWORD oldProt = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProt))
        return false;
    memcpy(dst, src, len);
    VirtualProtect(dst, len, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

static void InstallHook(void)
{
    HMODULE self = GetModuleHandleA(NULL);
    if ((uintptr_t)self != EXE_BASE)
    {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "NOT INSTALLED: main module at 0x%p, expected 0x%p",
                    (void*)(uintptr_t)self, (void*)EXE_BASE);
        LogRaw(buf);
        return;
    }

    unsigned char* fn = (unsigned char*)RANGE_FN_VA;
    if (memcmp(fn, kPrologue, sizeof(kPrologue)) != 0)
    {
        LogRaw("NOT INSTALLED: RandomRange prologue mismatch (wrong exe version?)");
        return;
    }

    unsigned char stub[5];
    stub[0] = 0xE9; // jmp rel32
    int32_t rel = (int32_t)((uintptr_t)&RandomRangeHook - (RANGE_FN_VA + 5));
    memcpy(stub + 1, &rel, 4);

    if (PatchBytes(fn, stub, sizeof(stub)))
        LogRaw("RandomRange guard installed at scarface.exe+0x1330");
    else
        LogRaw("NOT INSTALLED: VirtualProtect failed on scarface.exe+0x1330");
}

// ---------------------------------------------------------------------------
// WorldAmbienceSpawnObject candidate-pick NULL guards
//
// 0x401330's caller at 0x20471C computes  hi = (total - excluded) - 1.
// When every candidate is excluded, hi == -1 and the pick function returns
// NULL (0x204640 -> 0x204744). Both call sites then dereference it:
//
//   0x205A10  mov [esi+0x94],eax
//   0x205A16  mov edx,[eax+4]     <-- 0xC0000005 read of 0x4 when eax==0
//
//   0x205A6B  mov [esi+0x94],eax
//   0x205A71  mov ecx,[eax+4]     <-- same
//
// We replace each 6-byte store with a jump to a small stub which skips the
// selection entirely (and clears the "have selection" flag) when eax == NULL.
// ---------------------------------------------------------------------------
static const uintptr_t SITE_A      = 0x00605A10; // mov [esi+0x94],eax  (6 bytes)
static const uintptr_t SITE_B      = 0x00605A6B; // mov [esi+0x94],eax  (6 bytes)
static const uintptr_t CONT_A      = 0x00605A19; // mov [esi+0x84],edx
static const uintptr_t CONT_B      = 0x00605A74; // mov [esi+0x84],ecx
static const uintptr_t EPILOGUE_A  = 0x00605AA3; // pop edi; pop esi; ret 8
static const uintptr_t EPILOGUE_B  = 0x00605A82; // pop edi; pop esi; ret 8
static const size_t    STUB_BYTES  = 40;

static bool Rel32Fits(uintptr_t next, uintptr_t target)
{
    int64_t d = (int64_t)target - (int64_t)next;
    return d >= INT32_MIN && d <= INT32_MAX;
}

static bool StubFits(uintptr_t base)
{
    return Rel32Fits(SITE_A + 5, base) &&
           Rel32Fits(SITE_B + 5, base + STUB_BYTES) &&
           Rel32Fits(base + 18, CONT_A) &&
           Rel32Fits(base + 40, EPILOGUE_A) &&
           Rel32Fits(base + STUB_BYTES + 18, CONT_B) &&
           Rel32Fits(base + STUB_BYTES + 40, EPILOGUE_B);
}

// Emit: test eax,eax / jz null / store / load / jmp cont / null: clear+flag / jmp epilogue
static void EmitGuard(unsigned char* p, bool useEdx, uintptr_t cont, uintptr_t epilogue)
{
    size_t o = 0;
    p[o++] = 0x85; p[o++] = 0xC0;                          // test eax,eax
    p[o++] = 0x74; p[o++] = 0x0E;                          // jz +14 (-> offset 18)
    p[o++] = 0x89; p[o++] = 0x86; p[o++] = 0x94;           // mov [esi+0x94],eax
    p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0x8B; p[o++] = useEdx ? 0x50 : 0x48; p[o++] = 0x04; // mov edx/ecx,[eax+4]
    p[o++] = 0xE9;                                         // jmp cont
    int32_t r = (int32_t)((intptr_t)cont - (intptr_t)(p + o + 4));
    memcpy(p + o, &r, 4); o += 4;

    // null:
    p[o++] = 0xC7; p[o++] = 0x86;                          // mov dword [esi+0x94],0
    p[o++] = 0x94; p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0x80; p[o++] = 0xA6;                          // and byte [esi+0xB3],0xFE
    p[o++] = 0xB3; p[o++] = 0x00; p[o++] = 0x00; p[o++] = 0x00;
    p[o++] = 0xFE;
    p[o++] = 0xE9;                                         // jmp epilogue
    r = (int32_t)((intptr_t)epilogue - (intptr_t)(p + o + 4));
    memcpy(p + o, &r, 4); o += 4;
}

static void InstallNullGuards(void)
{
    if ((uintptr_t)GetModuleHandleA(NULL) != EXE_BASE)
        return; // not the game process; do not touch those addresses

    const unsigned char expect[6] = { 0x89, 0x86, 0x94, 0x00, 0x00, 0x00 };
    if (memcmp((void*)SITE_A, expect, 6) != 0 ||
        memcmp((void*)SITE_B, expect, 6) != 0)
    {
        LogRaw("NOT INSTALLED: ambience pick guard byte mismatch");
        return;
    }

    // Prefer a static buffer inside this module; fall back to a near allocation.
    static __declspec(align(16)) unsigned char buf[128];
    unsigned char* stub = NULL;

    if (StubFits((uintptr_t)buf))
    {
        DWORD old = 0;
        if (VirtualProtect(buf, sizeof(buf), PAGE_EXECUTE_READWRITE, &old))
            stub = buf;
    }
    if (!stub)
    {
        const uintptr_t hints[] = { 0x30000000u, 0x20000000u, 0x50000000u,
                                    0x10000000u, 0x60000000u, 0x08000000u };
        for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]) && !stub; i++)
        {
            void* p = VirtualAlloc((void*)hints[i], 0x1000,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p && StubFits((uintptr_t)p)) stub = (unsigned char*)p;
            else if (p) VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    if (!stub)
    {
        LogRaw("NOT INSTALLED: no reachable memory for ambience pick guard");
        return;
    }

    EmitGuard(stub, true,  CONT_A, EPILOGUE_A);
    EmitGuard(stub + STUB_BYTES, false, CONT_B, EPILOGUE_B);
    FlushInstructionCache(GetCurrentProcess(), stub, STUB_BYTES * 2);

    unsigned char jmp6[6];
    jmp6[0] = 0xE9;
    jmp6[5] = 0x90;
    int32_t r = (int32_t)((intptr_t)stub - (intptr_t)(SITE_A + 5));
    memcpy(jmp6 + 1, &r, 4);
    bool okA = PatchBytes((void*)SITE_A, jmp6, 6);

    r = (int32_t)((intptr_t)(stub + STUB_BYTES) - (intptr_t)(SITE_B + 5));
    memcpy(jmp6 + 1, &r, 4);
    bool okB = PatchBytes((void*)SITE_B, jmp6, 6);

    LogRaw(okA && okB
               ? "Ambience pick NULL guards installed at scarface.exe+0x205A10/+0x205A6B"
               : "NOT INSTALLED: ambience pick guard patch failed");
}

// ---------------------------------------------------------------------------
// Candidate-picker input validation
//
// Even with the caller guards, 0x204640 can be entered with a corrupt
// candidate array (observed: begin == NULL, end == heap pointer), which faults
// at 0x204671  mov ecx,[edi].  Validate the array at function entry and return
// NULL (the callers already handle NULL) if it is not sane.
// ---------------------------------------------------------------------------
static const uintptr_t PICK_FN     = 0x00604640; // VA (RVA 0x204640)
static const uintptr_t PICK_RESUME = 0x00604645; // first instruction after the 5-byte prologue

static void LogBadPicker(const char* why, uint32_t begin, uint32_t end)
{
    if (!g_cfg.logging || !g_cfg.logInvalidLists)
        return;

    static volatile LONG count = 0;
    if (InterlockedIncrement(&count) > 20)
        return;
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "INVALID PICKER LIST (%s) begin=0x%X end=0x%X -> spawn skipped",
                why, (unsigned)begin, (unsigned)end);
    LogRaw(buf);
}

static bool RangeReadable(uintptr_t addr, size_t size)
{
    uintptr_t cur = addr, last = addr + size;
    if (last < cur) return false;
    while (cur < last)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)cur, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (regionEnd <= cur) return false;
        cur = regionEnd;
    }
    return true;
}

static int ValidatePicker(void* self)
{
    if (!self) { LogBadPicker("null selector", 0, 0); return 0; }
    if (!RangeReadable((uintptr_t)self, 0x28))
    { LogBadPicker("unreadable selector", 0, 0); return 0; }

    uint32_t begin = *(uint32_t*)((char*)self + 0x20);
    uint32_t end   = *(uint32_t*)((char*)self + 0x24);

    if (begin == 0)              { LogBadPicker("null begin", begin, end); return 0; }
    if (begin > end)             { LogBadPicker("begin>end", begin, end); return 0; }

    uint32_t bytes = end - begin;
    if ((bytes & 3u) != 0 || bytes > 0x40000u)
    { LogBadPicker("bad size", begin, end); return 0; }

    if (!RangeReadable(begin, bytes))
    { LogBadPicker("unreadable", begin, end); return 0; }

    return 1;
}

static bool PickerStubFits(uintptr_t base)
{
    return Rel32Fits(PICK_FN + 5, base) &&
           Rel32Fits(base + 22, base + 32) &&
           Rel32Fits(base + 32 + 10, PICK_RESUME);
}

// Third caller of the same picker, at RVA 0x1FB1B8:
//   push 0
//   call 0x604640
//   test byte [eax+0x1F], bl     <-- faults if the picker returned NULL
// It appends the result to one of two vectors and continues its loop at
// 0x1FB259.  If the picker returns NULL we skip that iteration instead.
static const uintptr_t SITE_C = 0x005FB1B8; // call 0x604640
static const uintptr_t CONT_C = 0x005FB259; // add edi,1  (loop continue)

static void InstallThirdCallerGuard(void)
{
    if ((uintptr_t)GetModuleHandleA(NULL) != EXE_BASE)
        return;

    // Expect: E8 rel32  -> 0x604640
    const unsigned char* p = (const unsigned char*)SITE_C;
    int32_t rel = 0;
    memcpy(&rel, p + 1, 4);
    if (p[0] != 0xE8 || (uintptr_t)(SITE_C + 5 + rel) != PICK_FN)
    {
        LogRaw("NOT INSTALLED: third picker call site mismatch");
        return;
    }

    static __declspec(align(16)) unsigned char buf[128];
    unsigned char* s = NULL;
    if (Rel32Fits(SITE_C + 5, (uintptr_t)buf))
    {
        DWORD old = 0;
        if (VirtualProtect(buf, sizeof(buf), PAGE_EXECUTE_READWRITE, &old))
            s = buf;
    }
    if (!s)
    {
        const uintptr_t hints[] = { 0x30000000u, 0x20000000u, 0x50000000u,
                                    0x10000000u, 0x60000000u, 0x08000000u };
        for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]) && !s; i++)
        {
            void* q = VirtualAlloc((void*)hints[i], 0x1000,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (q && Rel32Fits(SITE_C + 5, (uintptr_t)q)) s = (unsigned char*)q;
            else if (q) VirtualFree(q, 0, MEM_RELEASE);
        }
    }
    if (!s)
    {
        LogRaw("NOT INSTALLED: no reachable memory for third picker guard");
        return;
    }

    size_t o = 0;
    s[o++] = 0xFF; s[o++] = 0x74; s[o++] = 0x24; s[o++] = 0x04; // push dword [esp+4]
    s[o++] = 0xB8;                                              // mov eax, 0x40604640
    uint32_t pick = (uint32_t)PICK_FN; memcpy(s + o, &pick, 4); o += 4;
    s[o++] = 0xFF; s[o++] = 0xD0;                               // call eax
    s[o++] = 0x8B; s[o++] = 0x14; s[o++] = 0x24;                // mov edx,[esp]  (return addr)
    s[o++] = 0x85; s[o++] = 0xC0;                               // test eax,eax
    s[o++] = 0x74; s[o++] = 0x08;                               // jz null
    s[o++] = 0x83; s[o++] = 0xC4; s[o++] = 0x08;                // add esp,8
    s[o++] = 0xFF; s[o++] = 0xE2;                               // jmp edx
    s[o++] = 0x90; s[o++] = 0x90; s[o++] = 0x90;                // pad
    s[o++] = 0x83; s[o++] = 0xC4; s[o++] = 0x08;                // null: add esp,8
    s[o++] = 0xB8;                                              // mov eax, 0x5FB259
    uint32_t cont = (uint32_t)CONT_C; memcpy(s + o, &cont, 4); o += 4;
    s[o++] = 0xFF; s[o++] = 0xE0;                               // jmp eax

    FlushInstructionCache(GetCurrentProcess(), s, o);

    unsigned char call5[5];
    call5[0] = 0xE8;
    rel = (int32_t)((intptr_t)s - (intptr_t)(SITE_C + 5));
    memcpy(call5 + 1, &rel, 4);

    LogRaw(PatchBytes((void*)SITE_C, call5, 5)
               ? "Third picker caller guard installed at scarface.exe+0x1FB1B8"
               : "NOT INSTALLED: third picker caller patch failed");
}

static void InstallPickerGuard(void)
{
    if ((uintptr_t)GetModuleHandleA(NULL) != EXE_BASE)
        return;

    const unsigned char expect[5] = { 0x83, 0xEC, 0x14, 0x53, 0x55 }; // sub esp,14; push ebx; push ebp
    if (memcmp((void*)PICK_FN, expect, 5) != 0)
    {
        const unsigned char* a = (const unsigned char*)PICK_FN;
        char buf[160];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "NOT INSTALLED: candidate picker prologue mismatch at 0x%X "
                    "got %02X %02X %02X %02X %02X",
                    (unsigned)PICK_FN, a[0], a[1], a[2], a[3], a[4]);
        LogRaw(buf);
        return;
    }

    static __declspec(align(16)) unsigned char buf[128];
    unsigned char* base = NULL;
    if (PickerStubFits((uintptr_t)buf))
    {
        DWORD old = 0;
        if (VirtualProtect(buf, sizeof(buf), PAGE_EXECUTE_READWRITE, &old))
            base = buf;
    }
    if (!base)
    {
        const uintptr_t hints[] = { 0x30000000u, 0x20000000u, 0x50000000u,
                                    0x10000000u, 0x60000000u, 0x08000000u };
        for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]) && !base; i++)
        {
            void* p = VirtualAlloc((void*)hints[i], 0x1000,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p && PickerStubFits((uintptr_t)p)) base = (unsigned char*)p;
            else if (p) VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    if (!base)
    {
        LogRaw("NOT INSTALLED: no reachable memory for candidate picker guard");
        return;
    }

    unsigned char* w = base;        // wrapper (27 bytes)
    unsigned char* t = base + 32;   // trampoline (10 bytes)
    memset(base, 0, 48);

    w[0] = 0x51; w[1] = 0x51;                       // push ecx; push ecx
    w[2] = 0xB8;                                    // mov eax, <ValidatePicker>
    uint32_t fn = (uint32_t)(uintptr_t)&ValidatePicker;
    memcpy(w + 3, &fn, 4);
    w[7] = 0xFF; w[8] = 0xD0;                       // call eax
    w[9] = 0x83; w[10] = 0xC4; w[11] = 0x04;        // add esp,4
    w[12] = 0x59;                                   // pop ecx
    w[13] = 0x85; w[14] = 0xC0;                     // test eax,eax
    w[15] = 0x74; w[16] = 0x05;                     // jz invalid
    w[17] = 0xE9;                                   // jmp trampoline
    int32_t r = (int32_t)((intptr_t)t - (intptr_t)(w + 22));
    memcpy(w + 18, &r, 4);
    w[22] = 0x33; w[23] = 0xC0;                     // invalid: xor eax,eax
    w[24] = 0xC2; w[25] = 0x04; w[26] = 0x00;       // ret 4

    t[0] = 0x83; t[1] = 0xEC; t[2] = 0x14;          // sub esp,0x14
    t[3] = 0x53;                                    // push ebx
    t[4] = 0x55;                                    // push ebp
    t[5] = 0xE9;                                    // jmp 0x40204645
    r = (int32_t)((intptr_t)PICK_RESUME - (intptr_t)(t + 10));
    memcpy(t + 6, &r, 4);

    FlushInstructionCache(GetCurrentProcess(), base, 48);

    unsigned char jmp5[5];
    jmp5[0] = 0xE9;
    r = (int32_t)((intptr_t)w - (intptr_t)(PICK_FN + 5));
    memcpy(jmp5 + 1, &r, 4);

    LogRaw(PatchBytes((void*)PICK_FN, jmp5, 5)
               ? "Candidate picker guard installed at scarface.exe+0x204640"
               : "NOT INSTALLED: candidate picker patch failed");
}

// ---------------------------------------------------------------------------
// Crash logging (first-chance, does not change exception handling)
// ---------------------------------------------------------------------------
struct SeenCrash { DWORD code; uintptr_t rva; unsigned count; };
static SeenCrash g_seen[16];
static volatile LONG g_seenCount = 0;
static volatile LONG g_inHandler = 0;

static LONG CALLBACK OnException(PEXCEPTION_POINTERS ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;

    if (!g_cfg.logging || !g_cfg.logCrashes)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t eip = (uintptr_t)ep->ContextRecord->Eip;
    if (eip < EXE_BASE || eip >= EXE_END)
        return EXCEPTION_CONTINUE_SEARCH;   // ignore faults in other modules

    if (InterlockedExchange(&g_inHandler, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t rva = eip - EXE_BASE;
    LONG n = g_seenCount;
    unsigned hit = 0;
    for (LONG i = 0; i < n; i++)
        if (g_seen[i].code == code && g_seen[i].rva == rva)
        {
            hit = ++g_seen[i].count;
            break;
        }
    if (!hit && n < (LONG)(sizeof(g_seen) / sizeof(g_seen[0])))
    {
        LONG idx = InterlockedIncrement(&g_seenCount) - 1;
        g_seen[idx].code = code;
        g_seen[idx].rva = rva;
        g_seen[idx].count = 1;
    }

    if (hit > 50)
    {
        InterlockedExchange(&g_inHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    char buf[4096];
    const char* what = (code == EXCEPTION_ACCESS_VIOLATION)
                           ? "ACCESS VIOLATION" : "DIVIDE BY ZERO";

    int w;
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        ULONG_PTR op  = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR adr = ep->ExceptionRecord->ExceptionInformation[1];
        const char* opn = (op == 0) ? "read" : (op == 1) ? "write" : "execute";
        w = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "CRASH(first-chance) %s at scarface.exe+0x%X  %s of 0x%X  "
                        "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
                        "ebp=%08X esp=%08X",
                        what, (unsigned)rva, opn, (unsigned)adr,
                        (unsigned)ep->ContextRecord->Eax,
                        (unsigned)ep->ContextRecord->Ebx,
                        (unsigned)ep->ContextRecord->Ecx,
                        (unsigned)ep->ContextRecord->Edx,
                        (unsigned)ep->ContextRecord->Esi,
                        (unsigned)ep->ContextRecord->Edi,
                        (unsigned)ep->ContextRecord->Ebp,
                        (unsigned)ep->ContextRecord->Esp);
    }
    else
    {
        w = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "CRASH(first-chance) %s at scarface.exe+0x%X  "
                        "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
                        "ebp=%08X esp=%08X",
                        what, (unsigned)rva,
                        (unsigned)ep->ContextRecord->Eax,
                        (unsigned)ep->ContextRecord->Ebx,
                        (unsigned)ep->ContextRecord->Ecx,
                        (unsigned)ep->ContextRecord->Edx,
                        (unsigned)ep->ContextRecord->Esi,
                        (unsigned)ep->ContextRecord->Edi,
                        (unsigned)ep->ContextRecord->Ebp,
                        (unsigned)ep->ContextRecord->Esp);
    }

    if (w > 0)
    {
        size_t len = strlen(buf);
        if (g_cfg.logStack)
            AppendStack(buf + len, sizeof(buf) - len, 1);
        LogRaw(buf);
    }

    InterlockedExchange(&g_inHandler, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
        g_csReady = true;

        BuildPaths();
        WriteDefaultIni();
        LoadConfig();
        if (g_cfg.logging)
            EnsureWritableLogDir();

        HMODULE nt = GetModuleHandleA("ntdll.dll");
        if (nt)
            g_capture = (pRtlCaptureStackBackTrace)GetProcAddress(
                             nt, "RtlCaptureStackBackTrace");

        char hello[256];
        _snprintf_s(hello, sizeof(hello), _TRUNCATE,
                    "RngGuard loaded (guards=%d logging=%d cap=%dKB badRanges=%d "
                    "invalidLists=%d crashes=%d stacks=%d)",
                    g_cfg.guards, g_cfg.logging, g_cfg.maxKB, g_cfg.logBadRanges,
                    g_cfg.logInvalidLists, g_cfg.logCrashes, g_cfg.logStack);
        LogRaw(hello);

        if (g_cfg.guards)
        {
            InstallHook();
            InstallNullGuards();
            InstallPickerGuard();
            InstallThirdCallerGuard();
        }
        else
        {
            LogRaw("Guards disabled by Scarface.RngGuard.ini");
        }

        AddVectoredExceptionHandler(1, OnException);
    }
    return TRUE;
}
