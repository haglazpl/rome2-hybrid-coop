/* Hybrid co-op loader: a stand-in for dinput8.dll placed in the ROME II folder.
   1. Forwards DirectInput to the real Windows dinput8.dll (the game only needs DirectInput8Create).
   2. If the Hybrid Co-op mod is enabled in the launcher (its pack is listed in used_mods.txt), it extracts
      hybrid_coop\hybrid_coop.dll from that pack into %LOCALAPPDATA%\RomeHybridCoop\bin and loads it -
      ONLY if its Ed25519 signature (hybrid_coop\hybrid_coop.dll.sig, made by the project's GitHub build)
      is valid. Any other DLL is refused.
   Mod disabled -> nothing else happens (normal game). */
#include "../common/win.h"
#include "monocypher-ed25519.h"
#include "pubkey.h"

#ifndef HC_VERSION
#define HC_VERSION "dev"
#endif
#define CORE_IN_PACK "hybrid_coop\\hybrid_coop.dll"
#define SIG_IN_PACK "hybrid_coop\\hybrid_coop.dll.sig"

/* ---------------------------------------------------------------- real dinput8 */
static HMODULE g_real;
static void *real(const char *name) {
  if (!g_real) {
    WCHAR p[MAX_PATH]; DWORD n = GetSystemDirectoryW(p, MAX_PATH);
    if (!n || n > MAX_PATH - 20) return NULL;
    wcat(p, L"\\dinput8.dll");
    g_real = LoadLibraryW(p);
    if (!g_real) return NULL;
  }
  return GetProcAddress(g_real, name);
}
typedef i32(WINAPI *DI8Create)(void *, DWORD, const void *, void **, void *);
typedef i32(WINAPI *Fn0)(void);
typedef i32(WINAPI *Fn3)(const void *, const void *, void **);
typedef void *(WINAPI *FnP)(void);
#define E_FAIL ((i32)0x80004005)
i32 WINAPI DirectInput8Create(void *inst, DWORD ver, const void *riid, void **out, void *outer) {
  DI8Create f = (DI8Create)real("DirectInput8Create"); return f ? f(inst, ver, riid, out, outer) : E_FAIL; }
i32 WINAPI DllCanUnloadNow(void) { Fn0 f = (Fn0)real("DllCanUnloadNow"); return f ? f() : 1; }
i32 WINAPI DllGetClassObject(const void *a, const void *b, void **c) { Fn3 f = (Fn3)real("DllGetClassObject"); return f ? f(a, b, c) : E_FAIL; }
i32 WINAPI DllRegisterServer(void) { Fn0 f = (Fn0)real("DllRegisterServer"); return f ? f() : E_FAIL; }
i32 WINAPI DllUnregisterServer(void) { Fn0 f = (Fn0)real("DllUnregisterServer"); return f ? f() : E_FAIL; }
void *WINAPI GetdfDIJoystick(void) { FnP f = (FnP)real("GetdfDIJoystick"); return f ? f() : NULL; }

/* ---------------------------------------------------------------- log */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static WCHAR g_dir[MAX_PATH];
static void logs(const char *s) {
  if (g_log == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME t; GetLocalTime(&t);
  char b[600]; int n = wsprintfA(b, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  uptr m = slen(s); if (m > 560) m = 560;
  mcpy(b + n, s, m); n += (int)m; b[n++] = '\r'; b[n++] = '\n';
  DWORD w; WriteFile(g_log, b, (DWORD)n, &w, NULL);
}
static void wtoa(char *d, const WCHAR *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i] < 128 ? (char)s[i] : '?'; d[i] = 0; }
#define LOG(...) do { char _b[560]; wsprintfA(_b, __VA_ARGS__); logs(_b); } while (0)

/* ---------------------------------------------------------------- files */
static u8 *read_all(const WCHAR *path, DWORD *size) {
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  DWORD n = GetFileSize(h, NULL);
  u8 *b = (n && n < 0x10000000) ? (u8 *)HeapAlloc(GetProcessHeap(), 0, n + 2) : NULL;
  DWORD got = 0;
  if (b && (!ReadFile(h, b, n, &got, NULL) || got != n)) { HeapFree(GetProcessHeap(), 0, b); b = NULL; }
  CloseHandle(h);
  if (b) { b[n] = 0; b[n + 1] = 0; *size = n; }
  return b;
}
static int exists(const WCHAR *p) { HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); if (h == INVALID_HANDLE_VALUE) return 0; CloseHandle(h); return 1; }
static int is_abs(const WCHAR *p) { return (p[0] && p[1] == ':') || (p[0] == '\\' && p[1] == '\\'); }

/* read a file of a pack (PFH4/PFH5). returns heap buffer or NULL */
static u8 *pack_find(const WCHAR *packPath, const char *want, DWORD *outSize) {
  HANDLE h = CreateFileW(packPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  u8 *res = NULL, hdr[28]; DWORD got = 0;
  if (!ReadFile(h, hdr, 28, &got, NULL) || got != 28 || hdr[0] != 'P' || hdr[1] != 'F' || hdr[2] != 'H' || (hdr[3] != '4' && hdr[3] != '5')) goto done;
  u32 flags = *(u32 *)(hdr + 4), pidxSize = *(u32 *)(hdr + 12), count = *(u32 *)(hdr + 16), idxSize = *(u32 *)(hdr + 20);
  if (flags & 0x80) goto done;                               /* encrypted index: not ours */
  u32 hdrSize = 28 + ((flags & 0x100) ? 256 : 0);            /* extended header (not used by ROME II mods) */
  if (idxSize == 0 || idxSize > 0x4000000) goto done;
  u8 *idx = (u8 *)HeapAlloc(GetProcessHeap(), 0, idxSize);
  if (!idx) goto done;
  SetFilePointer(h, (i32)(hdrSize + pidxSize), NULL, FILE_BEGIN);
  if (!ReadFile(h, idx, idxSize, &got, NULL) || got != idxSize) { HeapFree(GetProcessHeap(), 0, idx); goto done; }
  u32 o = 0, dataOff = hdrSize + pidxSize + idxSize;
  for (u32 i = 0; i < count && o + 4 < idxSize; i++) {
    u32 size = *(u32 *)(idx + o); o += 4;
    if (flags & 0x40) o += 4;                                /* per-file timestamp */
    if (hdr[3] == '5' && (flags & 0x40) == 0) {}             /* PFH5 without timestamps: same layout */
    const char *name = (const char *)(idx + o);
    uptr nl = 0; while (o + nl < idxSize && name[nl]) nl++;
    int match = 1; const char *a = name, *b = want;
    while (*b) { char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x += 32; if (x == '/') x = '\\'; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) { match = 0; break; } a++; b++; }
    if (match && *a) match = 0;
    if (match && size < 0x4000000) {
      res = (u8 *)HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
      SetFilePointer(h, (i32)dataOff, NULL, FILE_BEGIN);
      if (res && (!ReadFile(h, res, size, &got, NULL) || got != size)) { HeapFree(GetProcessHeap(), 0, res); res = NULL; }
      if (res) *outSize = size;
      break;
    }
    dataOff += size; o += (u32)nl + 1;
  }
  HeapFree(GetProcessHeap(), 0, idx);
done:
  CloseHandle(h);
  return res;
}

/* ---------------------------------------------------------------- used_mods.txt */
static void game_dir(WCHAR *d) {
  GetModuleFileNameW(NULL, d, MAX_PATH);
  int i = (int)wlen(d); while (i > 0 && d[i - 1] != '\\' && d[i - 1] != '/') i--; d[i ? i - 1 : 0] = 0;
}
/* mods file named on the command line (the launcher passes e.g. used_mods.txt;), default used_mods.txt */
static void mods_file(const WCHAR *gd, WCHAR *out) {
  const WCHAR *c = GetCommandLineW();
  WCHAR tok[MAX_PATH]; out[0] = 0;
  while (*c) {
    while (*c == ' ' || *c == '\t') c++;
    int q = 0, n = 0;
    while (*c && (q || (*c != ' ' && *c != '\t'))) { if (*c == '"') q = !q; else if (n < MAX_PATH - 1) tok[n++] = *c; c++; }
    tok[n] = 0;
    while (n && (tok[n - 1] == ';')) tok[--n] = 0;
    if (n > 4 && wlower(tok[n - 4]) == '.' && wlower(tok[n - 3]) == 't' && wlower(tok[n - 2]) == 'x' && wlower(tok[n - 1]) == 't') {
      if (is_abs(tok)) wcpy(out, tok); else { wcpy(out, gd); wcat(out, L"\\"); wcat(out, tok); }
    }
  }
  if (!out[0] || !exists(out)) { wcpy(out, gd); wcat(out, L"\\used_mods.txt"); }
}

static int signature_ok(const u8 *dll, DWORD size, const u8 *sig) {
  return crypto_ed25519_check(sig, HYBRID_COOP_PUBKEY, dll, size) == 0;
}

static int extract_and_load(const u8 *dll, DWORD size, const u8 *sig) {
  WCHAR bin[MAX_PATH], tmp[MAX_PATH], dst[MAX_PATH];
  wcpy(bin, g_dir); wcat(bin, L"\\bin"); CreateDirectoryW(bin, NULL);
  wcpy(dst, bin); wcat(dst, L"\\hybrid_coop_core.dll");
  wcpy(tmp, bin); wcat(tmp, L"\\hybrid_coop_core.tmp");
  HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD w = 0; WriteFile(h, dll, size, &w, NULL); CloseHandle(h);
    if (w != size || !MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING)) { LOG("could not replace the core dll (in use by another game window?) - using the existing copy"); DeleteFileW(tmp); }
  }
  /* check the file that will really be loaded (it may be an older copy if replacing failed). The handle stays open
     without write/delete sharing until the DLL is loaded, so nothing can swap the file between check and load. */
  HANDLE lock = CreateFileW(dst, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (lock == INVALID_HANDLE_VALUE) { LOG("cannot open the core dll - not loaded"); return 0; }
  DWORD dsz = GetFileSize(lock, NULL), got = 0;
  u8 *disk = (dsz == size) ? (u8 *)HeapAlloc(GetProcessHeap(), 0, dsz) : NULL;
  int ok_disk = disk && ReadFile(lock, disk, dsz, &got, NULL) && got == dsz && signature_ok(disk, dsz, sig);
  if (disk) HeapFree(GetProcessHeap(), 0, disk);
  if (!ok_disk) { CloseHandle(lock); LOG("the core dll on disk does not match the signed one - not loaded"); return 0; }
  HMODULE m = LoadLibraryW(dst);
  CloseHandle(lock);
  if (!m) { LOG("LoadLibrary of the core dll failed"); return 0; }
  typedef int(__cdecl * StartFn)(void);
  StartFn start = (StartFn)GetProcAddress(m, "HybridCoopStart");
  if (!start) { LOG("core dll has no HybridCoopStart"); return 0; }
  int ok = start();
  LOG("core started: %s (details in logs\\hybrid_*.log)", ok ? "OK" : "DISABLED");
  return ok;
}

static void run(void) {
  WCHAR la[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
  wcpy(g_dir, la); wcat(g_dir, L"\\RomeHybridCoop"); CreateDirectoryW(g_dir, NULL);
  WCHAR lp[MAX_PATH]; wcpy(lp, g_dir); wcat(lp, L"\\loader.log");
  g_log = CreateFileW(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  LOG("Hybrid Co-op loader %s (loads only the signed co-op DLL)", HC_VERSION);

  WCHAR gd[MAX_PATH], mf[MAX_PATH]; char a[300];
  game_dir(gd); mods_file(gd, mf);
  wtoa(a, mf, 300); LOG("mods file: %s", a);
  DWORD size = 0; u8 *txt = read_all(mf, &size);
  if (!txt) { LOG("no mods file - mod not enabled, normal game"); return; }

  /* parse: add_working_directory "X"; and mod "Y"; - paths are UTF-8 (letters like ł or é in folder names) */
  if (size >= 3 && txt[0] == 0xEF && txt[1] == 0xBB && txt[2] == 0xBF) { txt[0] = txt[1] = txt[2] = ' '; }
  static WCHAR wdirs[16][MAX_PATH]; int nw = 0;
  static WCHAR mods[64][MAX_PATH]; int nm = 0;
  for (char *p = (char *)txt; *p;) {
    char *line = p; while (*p && *p != '\n') p++; if (*p) *p++ = 0;
    while (*line == ' ' || *line == '\t' || *line == '\r') line++;
    int isW = sprefix(line, "add_working_directory"), isM = sprefix(line, "mod ") || sprefix(line, "mod\t");
    if (!isW && !isM) continue;
    char *q1 = line; while (*q1 && *q1 != '"') q1++; if (!*q1) continue; q1++;
    char *q2 = q1; while (*q2 && *q2 != '"') q2++; *q2 = 0;
    WCHAR *dst = isW ? (nw < 16 ? wdirs[nw++] : NULL) : (nm < 64 ? mods[nm++] : NULL);
    if (!dst) continue;
    int n = MultiByteToWideChar(CP_UTF8, 8 /* MB_ERR_INVALID_CHARS */, q1, -1, dst, MAX_PATH);
    if (n <= 0) { int i = 0; for (; q1[i] && i < MAX_PATH - 1; i++) dst[i] = (WCHAR)(u8)q1[i]; dst[i] = 0; }   /* not UTF-8: as bytes */
  }
  LOG("enabled mods: %d, extra mod folders: %d", nm, nw);

  for (int m = 0; m < nm; m++) {
    /* candidate locations of this pack: working directories, game data folder, absolute path */
    static WCHAR cand[20][MAX_PATH]; int nc = 0;
    if (is_abs(mods[m])) wcpy(cand[nc++], mods[m]);
    for (int w = 0; w < nw && nc < 19; w++) { wcpy(cand[nc], wdirs[w]); wcat(cand[nc], L"\\"); wcat(cand[nc], mods[m]); nc++; }
    wcpy(cand[nc], gd); wcat(cand[nc], L"\\data\\"); wcat(cand[nc], mods[m]); nc++;
    for (int c = 0; c < nc; c++) {
      if (!exists(cand[c])) continue;
      DWORD dsz = 0; u8 *dll = pack_find(cand[c], CORE_IN_PACK, &dsz);
      if (!dll) break;                                       /* found the pack, it is not ours */
      wtoa(a, cand[c], 300);
      DWORD ssz = 0; u8 *sig = pack_find(cand[c], SIG_IN_PACK, &ssz);
      if (!sig || ssz != 64 || !signature_ok(dll, dsz, sig)) {
        LOG("REFUSED: %s contains a co-op DLL without a valid signature - not loaded", a);
      } else {
        LOG("Hybrid Co-op pack: %s (core %u bytes, signature OK)", a, (u32)dsz);
        extract_and_load(dll, dsz, sig);
      }
      if (sig) HeapFree(GetProcessHeap(), 0, sig);
      HeapFree(GetProcessHeap(), 0, dll);
      return;
    }
  }
  LOG("Hybrid Co-op is not enabled in the launcher - normal game");
}

static DWORD WINAPI wait_and_run(void *p) {
  (void)p;
  for (int i = 0; i < 600 && !GetModuleHandleW(L"empire.retail.dll"); i++) Sleep(50);
  if (GetModuleHandleW(L"empire.retail.dll")) run();
  return 0;
}

BOOL WINAPI DllMain(HMODULE h, DWORD reason, void *res) {
  (void)res;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    /* only in the game: Rome2.exe */
    WCHAR exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    int i = (int)wlen(exe); while (i > 0 && exe[i - 1] != '\\') i--;
    if (!wieq(exe + i, L"Rome2.exe")) return TRUE;
    /* normally empire.retail.dll (which imports us) is already mapped: hook now, before any game code runs */
    if (GetModuleHandleW(L"empire.retail.dll")) run();
    else CreateThread(NULL, 0, wait_and_run, NULL, 0, NULL);
  }
  return TRUE;
}

