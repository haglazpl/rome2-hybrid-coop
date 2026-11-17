#include "hook.h"

#define MAX_FRAMES 256
typedef struct { int n; Frame f[MAX_FRAMES]; } FrameStack;

static DWORD g_tls = TLS_OUT_OF_INDEXES;
static u8 *g_code, *g_code_end, *g_code_pos;
static u8 *g_leave_stub;
CRITICAL_SECTION g_lock;      /* one lock for all callbacks (Frida also ran them one at a time) */

static u8 *code_alloc(uptr n) {
  if (g_code_pos + n > g_code_end) return NULL;
  u8 *p = g_code_pos; g_code_pos += (n + 15) & ~15u; return p;
}

static FrameStack *frames(void) {
  FrameStack *s = (FrameStack *)TlsGetValue(g_tls);
  if (!s) {
    s = (FrameStack *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FrameStack));
    TlsSetValue(g_tls, s);
  }
  return s;
}

/* called from the per-hook stub: cdecl int enter_dispatch(Hook*, Ctx*) */
static int __cdecl enter_dispatch(Hook *h, Ctx *c) {
  if (h->on_leave) {
    FrameStack *s = frames();
    if (!s || s->n >= MAX_FRAMES) {            /* should never happen; run without the leave callback */
      Frame tmp; mset(&tmp, 0, sizeof tmp); tmp.h = h; tmp.ra = *(u32 *)c->esp;
      int r = 0;
      if (h->on_enter) { EnterCriticalSection(&g_lock); r = h->on_enter(h, c, &tmp); LeaveCriticalSection(&g_lock); }
      return r;
    }
    Frame *f = &s->f[s->n++];                    /* reserve first: on_enter may call hooked game functions */
    mset(f, 0, sizeof *f); f->h = h; f->ra = *(u32 *)c->esp;
    int skip = 0;
    if (h->on_enter) { EnterCriticalSection(&g_lock); skip = h->on_enter(h, c, f); LeaveCriticalSection(&g_lock); }
    if (skip) { s->n--; return 1; }
    *(u32 *)c->esp = (u32)g_leave_stub;          /* return through the leave stub (like Frida's onLeave) */
    return 0;
  }
  Frame tmp; mset(&tmp, 0, sizeof tmp); tmp.h = h; tmp.ra = *(u32 *)c->esp;
  int r = 0;
  if (h->on_enter) { EnterCriticalSection(&g_lock); r = h->on_enter(h, c, &tmp); LeaveCriticalSection(&g_lock); }
  return r;
}

/* called from the shared leave stub: the slot above the context receives the real return address */
static void __cdecl leave_dispatch(Ctx *c) {
  FrameStack *s = frames();
  Frame *f = &s->f[--s->n];
  if (f->h->on_leave) { EnterCriticalSection(&g_lock); f->h->on_leave(f->h, c, f); LeaveCriticalSection(&g_lock); }
  *(u32 *)((u8 *)c + sizeof(Ctx)) = f->ra;
}

static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void rel32(u8 *at, u32 target) { put32(at, target - ((u32)at + 4)); }

int hook_engine_init(void) {
  if (g_code) return 1;
  InitializeCriticalSection(&g_lock);
  g_tls = TlsAlloc();
  if (g_tls == TLS_OUT_OF_INDEXES) return 0;
  g_code = (u8 *)VirtualAlloc(NULL, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!g_code) return 0;
  g_code_pos = g_code; g_code_end = g_code + 0x4000;
  /* leave stub: push 0 / pushad / pushfd / cld / push esp / call leave_dispatch / add esp,4 / popfd / popad / ret */
  u8 *p = g_leave_stub = code_alloc(32);
  *p++ = 0x6A; *p++ = 0x00;
  *p++ = 0x60; *p++ = 0x9C; *p++ = 0xFC; *p++ = 0x54;
  *p++ = 0xE8; rel32(p, (u32)leave_dispatch); p += 4;
  *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;
  *p++ = 0x9D; *p++ = 0x61; *p++ = 0xC3;
  return 1;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

int hook_verify(Hook *h, u32 base) {
  const u8 *live = (const u8 *)(h->va - 0x10000000u + base);
  u8 exp[32];
  for (int i = 0; i < h->len; i++) exp[i] = (u8)(hexval(h->expect[2 * i]) * 16 + hexval(h->expect[2 * i + 1]));
  if (h->reloc >= 0) {                         /* absolute address inside: compare after relocation */
    u32 v = exp[h->reloc] | exp[h->reloc + 1] << 8 | exp[h->reloc + 2] << 16 | (u32)exp[h->reloc + 3] << 24;
    put32(exp + h->reloc, v - 0x10000000u + base);
  }
  for (int i = 0; i < h->len; i++) if (live[i] != exp[i]) return 0;
  return 1;
}

int hook_install(Hook *h, u32 base) {
  u8 *target = (u8 *)(h->va - 0x10000000u + base);
  u8 *p = h->stub = code_alloc(64 + h->len);
  if (!p) return 0;
  *p++ = 0x60; *p++ = 0x9C; *p++ = 0xFC; *p++ = 0x54;                 /* pushad, pushfd, cld, push esp (ctx) */
  *p++ = 0x68; put32(p, (u32)h); p += 4;                              /* push hook */
  *p++ = 0xE8; rel32(p, (u32)enter_dispatch); p += 4;                 /* call enter_dispatch */
  *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;                              /* add esp, 8 */
  *p++ = 0x85; *p++ = 0xC0;                                           /* test eax, eax */
  *p++ = 0x74; *p++ = 0x05;                                           /* jz continue */
  *p++ = 0x9D; *p++ = 0x61; *p++ = 0xC2; *p++ = (u8)h->skip_ret; *p++ = (u8)(h->skip_ret >> 8);   /* skip: ret N */
  *p++ = 0x9D; *p++ = 0x61;                                           /* continue: popfd, popad */
  mcpy(p, target, h->len); p += h->len;                               /* original instructions */
  *p++ = 0xE9; rel32(p, (u32)(target + h->len)); p += 4;              /* jmp back */

  DWORD old;
  if (!VirtualProtect(target, h->len, PAGE_EXECUTE_READWRITE, &old)) return 0;
  u8 patch[16];
  patch[0] = 0xE9;
  put32(patch + 1, (u32)h->stub - ((u32)target + 5));
  for (int i = 5; i < h->len; i++) patch[i] = 0x90;
  mcpy(target, patch, h->len);
  VirtualProtect(target, h->len, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, h->len);
  h->installed = 1;
  return 1;
}
