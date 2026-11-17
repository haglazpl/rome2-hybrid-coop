/* Minimal Win32 declarations (no Windows SDK in the build environment). 32-bit only. */
#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;
typedef unsigned int uptr;
typedef int BOOL;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef void *HMODULE;
typedef unsigned short WCHAR;

#define WINAPI __stdcall
#ifndef DLLIMPORT
#define DLLIMPORT __declspec(dllimport)
#endif
#define TRUE 1
#define FALSE 0
#ifndef NULL
#define NULL ((void *)0)
#endif
#define INVALID_HANDLE_VALUE ((HANDLE)(i32)-1)
#define MAX_PATH 260

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_EXECUTE_READWRITE 0x40

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_BEGIN 0
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 1
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
#define HEAP_ZERO_MEMORY 8
#define MB_ICONASTERISK 0x40

typedef struct { u16 wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
typedef struct { void *DebugInfo; i32 LockCount, RecursionCount; HANDLE OwningThread, LockSemaphore; uptr SpinCount; } CRITICAL_SECTION;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
  DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
  WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;
#define FILE_ATTRIBUTE_DIRECTORY 0x10

DLLIMPORT HMODULE WINAPI GetModuleHandleW(const WCHAR *);
DLLIMPORT HMODULE WINAPI LoadLibraryW(const WCHAR *);
DLLIMPORT void *WINAPI GetProcAddress(HMODULE, const char *);
DLLIMPORT DWORD WINAPI GetModuleFileNameW(HMODULE, WCHAR *, DWORD);
DLLIMPORT BOOL WINAPI DisableThreadLibraryCalls(HMODULE);
DLLIMPORT DWORD WINAPI GetSystemDirectoryW(WCHAR *, DWORD);
DLLIMPORT void *WINAPI VirtualAlloc(void *, uptr, DWORD, DWORD);
DLLIMPORT BOOL WINAPI VirtualProtect(void *, uptr, DWORD, DWORD *);
DLLIMPORT BOOL WINAPI FlushInstructionCache(HANDLE, const void *, uptr);
DLLIMPORT HANDLE WINAPI GetCurrentProcess(void);
DLLIMPORT DWORD WINAPI TlsAlloc(void);
DLLIMPORT void *WINAPI TlsGetValue(DWORD);
DLLIMPORT BOOL WINAPI TlsSetValue(DWORD, void *);
DLLIMPORT HANDLE WINAPI GetProcessHeap(void);
DLLIMPORT void *WINAPI HeapAlloc(HANDLE, DWORD, uptr);
DLLIMPORT BOOL WINAPI HeapFree(HANDLE, DWORD, void *);
DLLIMPORT HANDLE WINAPI CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
DLLIMPORT BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
DLLIMPORT BOOL WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
DLLIMPORT DWORD WINAPI SetFilePointer(HANDLE, i32, i32 *, DWORD);
DLLIMPORT DWORD WINAPI GetFileSize(HANDLE, DWORD *);
DLLIMPORT BOOL WINAPI CloseHandle(HANDLE);
DLLIMPORT BOOL WINAPI CreateDirectoryW(const WCHAR *, void *);
DLLIMPORT BOOL WINAPI MoveFileExW(const WCHAR *, const WCHAR *, DWORD);
DLLIMPORT BOOL WINAPI DeleteFileW(const WCHAR *);
DLLIMPORT DWORD WINAPI GetEnvironmentVariableW(const WCHAR *, WCHAR *, DWORD);
DLLIMPORT void WINAPI GetLocalTime(SYSTEMTIME *);
DLLIMPORT DWORD WINAPI GetTickCount(void);
DLLIMPORT void WINAPI InitializeCriticalSection(CRITICAL_SECTION *);
DLLIMPORT void WINAPI EnterCriticalSection(CRITICAL_SECTION *);
DLLIMPORT void WINAPI LeaveCriticalSection(CRITICAL_SECTION *);
DLLIMPORT const WCHAR *WINAPI GetCommandLineW(void);
DLLIMPORT HANDLE WINAPI FindFirstFileW(const WCHAR *, WIN32_FIND_DATAW *);
DLLIMPORT BOOL WINAPI FindNextFileW(HANDLE, WIN32_FIND_DATAW *);
DLLIMPORT BOOL WINAPI FindClose(HANDLE);
DLLIMPORT DWORD WINAPI GetCurrentThreadId(void);
DLLIMPORT void WINAPI OutputDebugStringA(const char *);
DLLIMPORT HANDLE WINAPI CreateThread(void *, uptr, DWORD(WINAPI *)(void *), void *, DWORD, DWORD *);
DLLIMPORT void WINAPI Sleep(DWORD);
DLLIMPORT int WINAPI MultiByteToWideChar(u32, DWORD, const char *, int, WCHAR *, int);
#define CP_UTF8 65001
/* user32 */
DLLIMPORT int __cdecl wsprintfA(char *, const char *, ...);
DLLIMPORT BOOL WINAPI MessageBeep(u32);

/* freestanding helpers (no CRT) */
static inline void *mcpy(void *d, const void *s, uptr n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
static inline void mset(void *d, int v, uptr n) { u8 *a = d; while (n--) *a++ = (u8)v; }
static inline uptr slen(const char *s) { uptr n = 0; while (s[n]) n++; return n; }
static inline uptr wlen(const WCHAR *s) { uptr n = 0; while (s[n]) n++; return n; }
static inline int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static inline int sprefix(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static inline void wcat(WCHAR *d, const WCHAR *s) { d += wlen(d); while ((*d++ = *s++)) {} }
static inline void wcpy(WCHAR *d, const WCHAR *s) { while ((*d++ = *s++)) {} }
static inline WCHAR wlower(WCHAR c) { return (c >= 'A' && c <= 'Z') ? (WCHAR)(c + 32) : c; }
static inline int wieq(const WCHAR *a, const WCHAR *b) { while (*a && wlower(*a) == wlower(*b)) { a++; b++; } return wlower(*a) == wlower(*b); }
