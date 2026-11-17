/* Linux i386 test of the hook engine: fake Win32 API, asm test targets with game-like prologues. */
#define DLLIMPORT
#include "../src/core/hook.h"
static long sys(long n, long a, long b, long c, long d, long e, long f) {
  long r; __asm__ volatile("push %%ebp; mov %7,%%ebp; int $0x80; pop %%ebp" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "m"(f) : "memory"); return r; }
static void wr(const char *s) { sys(4, 1, (long)s, slen(s), 0, 0, 0); }
static void wrn(const char *s, u32 v) { char b[16]; int i = 15; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); wr(s); wr(b + i); wr("\n"); }
static u8 heap[1 << 20]; static u32 hp;
/* fake Win32 */
DWORD WINAPI TlsAlloc(void) { return 1; }
static void *tlsv; void *WINAPI TlsGetValue(DWORD i) { return tlsv; } BOOL WINAPI TlsSetValue(DWORD i, void *v) { tlsv = v; return 1; }
HANDLE WINAPI GetProcessHeap(void) { return 0; }
void *WINAPI HeapAlloc(HANDLE h, DWORD f, uptr n) { void *p = heap + hp; hp += (n + 15) & ~15u; mset(p, 0, n); return p; }
void *WINAPI VirtualAlloc(void *a, uptr n, DWORD t, DWORD p) { return (void *)sys(192, 0, n, 7, 0x22, -1, 0); }
BOOL WINAPI VirtualProtect(void *a, uptr n, DWORD p, DWORD *old) { u32 s = (u32)a & ~0xfffu; sys(125, s, ((u32)a + n - s + 0xfff) & ~0xfffu, 7, 0, 0, 0); *old = 0x20; return 1; }
BOOL WINAPI FlushInstructionCache(HANDLE h, const void *a, uptr n) { return 1; }
HANDLE WINAPI GetCurrentProcess(void) { return 0; }
void WINAPI InitializeCriticalSection(CRITICAL_SECTION *c) {}
static int depth; void WINAPI EnterCriticalSection(CRITICAL_SECTION *c) { depth++; } void WINAPI LeaveCriticalSection(CRITICAL_SECTION *c) { depth--; }
void *memset(void *d, int v, uptr n) { volatile u8 *a = d; while (n--) *a++ = (u8)v; return d; }
void *memcpy(void *d, const void *s, uptr n) { volatile u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }

/* targets (asm). thiscall: ecx=this, 2 stack args, returns this+a+b, ret 8; prologue like advance_turn */
__asm__(".text\n.globl t_this\nt_this:\n sub $0x2c,%esp\n push %ebx\n push %ebp\n mov 0x38(%esp),%eax\n add 0x3c(%esp),%eax\n add %ecx,%eax\n pop %ebp\n pop %ebx\n add $0x2c,%esp\n ret $8\n"
/* recursive cdecl: f(n) = n==0 ? 0 : n + f(n-1); prologue push ebx; push esi; mov esi,ecx; push edi */
".globl t_rec\nt_rec:\n push %ebx\n push %esi\n mov %ecx,%esi\n push %edi\n mov 0x10(%esp),%ebx\n xor %eax,%eax\n test %ebx,%ebx\n jz 1f\n lea -1(%ebx),%eax\n push %eax\n call t_rec\n add $4,%esp\n add %ebx,%eax\n1:\n pop %edi\n pop %esi\n pop %ebx\n ret\n"
/* mid-function: computes eax = arg*2 then at probe point 'add $8,%esp; test %al,%al' style (5 bytes: 83 c4 08 84 c0) */
/* 'exception': t_thrower (hooked with leave) never returns - it jumps back into t_outer like a C++ catch */
".globl t_outer\nt_outer:\n push %ebp\n mov %esp,%ebp\n push %ebx\n push %esi\n mov %esp,saved_esp\n call t_thrower\n mov $1,%eax\n jmp 2f\n"
"t_catch:\n mov saved_esp,%esp\n mov $7,%eax\n2:\n pop %esi\n pop %ebx\n pop %ebp\n ret\n"
".globl t_thrower\nt_thrower:\n sub $0x2c,%esp\n push %ebx\n push %ebp\n jmp t_catch\n"
".globl t_mid\nt_mid:\n mov 4(%esp),%eax\n add %eax,%eax\n push $0x1111\n push $0x2222\n.globl t_mid_probe\nt_mid_probe:\n add $8,%esp\n test %al,%al\n ret\n");
u32 __attribute__((thiscall)) t_this(u32 self, u32 a, u32 b);
u32 t_rec(u32 n); u32 t_mid(u32 x); extern u8 t_mid_probe[]; u32 t_outer(void); void t_thrower(void); u32 saved_esp;

static int n_enter, n_leave, skip_this;
static int this_enter(Hook *h, Ctx *c, Frame *f) { n_enter++; f->d[0] = arg(c, 0); f->d[1] = c->ecx; return skip_this ? (c->eax = 777, 1) : 0; }
static void this_leave(Hook *h, Ctx *c, Frame *f) { n_leave++; if (f->d[0] == 5) c->eax += 1000; }
static int rec_enter(Hook *h, Ctx *c, Frame *f) { f->d[0] = arg(c, 0); return 0; }
static u32 rec_seen[32]; static int nrec;
static void rec_leave(Hook *h, Ctx *c, Frame *f) { rec_seen[nrec++] = f->d[0] * 1000 + c->eax; }
static u32 mid_esp4;
static int mid_probe(Hook *h, Ctx *c, Frame *f) { mid_esp4 = at_esp(c, 4); if ((c->eax & 0xff) == 0x14) c->eax = 0x99; return 0; }

static int n_outer_leave, n_thrower_leave;
static void outer_leave(Hook *h, Ctx *c, Frame *f) { n_outer_leave++; }
static void thrower_leave(Hook *h, Ctx *c, Frame *f) { n_thrower_leave++; }
static Hook H[] = {
  {"this", 0, 5, "83ec2c5355", -1, 8, this_enter, this_leave},
  {"rec", 0, 5, "535689ce57", -1, 0, rec_enter, rec_leave},
  {"mid", 0, 5, "83c40884c0", -1, 0, mid_probe, 0},
  {"outer", 0, 5, "5589e55356", -1, 0, 0, outer_leave},
  {"thrower", 0, 5, "83ec2c5355", -1, 0, 0, thrower_leave},
};
static int fails;
static void check(const char *what, u32 got, u32 exp) { wr(got == exp ? "ok   " : "FAIL "); wr(what); wrn(" = ", got); if (got != exp) fails++; }
void _start(void) {
  H[0].va = (u32)t_this; H[1].va = (u32)t_rec; H[2].va = (u32)t_mid_probe; H[3].va = (u32)t_outer; H[4].va = (u32)t_thrower;
  check("engine init", hook_engine_init(), 1);
  for (int i = 0; i < 5; i++) { check("verify", hook_verify(&H[i], 0x10000000), 1); check("install", hook_install(&H[i], 0x10000000), 1); }
  check("thiscall plain", t_this(100, 1, 2), 103);
  check("thiscall leave +1000", t_this(100, 5, 2), 1107);
  check("enter count", n_enter, 2); check("leave count", n_leave, 2);
  skip_this = 1; check("thiscall skipped (ret 8, eax=777)", t_this(100, 5, 2), 777); skip_this = 0;
  check("after skip still ok", t_this(1, 1, 1), 3);
  check("recursion f(4)", t_rec(4), 10);
  check("recursion leave order", rec_seen[0], 0); check("rec leave last", rec_seen[4], 4010); check("rec frames", nrec, 5);
  check("mid normal", t_mid(3), 6); check("mid esp+4 (pushed 0x1111)", mid_esp4, 0x1111);
  check("mid eax change", t_mid(10), 0x99);
  check("abandoned call: outer returns", t_outer(), 7);
  check("outer leave ran once", n_outer_leave, 1); check("thrower leave never", n_thrower_leave, 0);
  check("after abandon: thiscall ok", t_this(2, 3, 4), 9);
  check("after abandon: recursion ok", t_rec(3), 6);
  check("abandoned again, nested in thiscall-free path", t_outer(), 7); check("outer leaves", n_outer_leave, 2);
  check("frame stack empty", ((u32 *)tlsv)[0], 0);
  check("lock balanced", depth, 0);
  wrn("fails=", fails);
  sys(1, fails, 0, 0, 0, 0, 0);
}
