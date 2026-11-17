/* Tiny x86 hook engine (Frida-like): entry hooks with optional 'leave' callback, mid-function probes.
   Register context is saved/restored around the C callbacks, so callbacks may read and change registers. */
#pragma once
#include "../common/win.h"

typedef struct Ctx { u32 eflags, edi, esi, ebp, esp, ebx, edx, ecx, eax; } Ctx;   /* esp = value at the hook point */

struct Hook;
typedef struct Frame { struct Hook *h; u32 ra; u32 d[6]; } Frame;   /* per call (enter -> leave) */
typedef int (*EnterFn)(struct Hook *, Ctx *, Frame *);              /* return 1 = skip the original (ret N, eax from ctx) */
typedef void (*LeaveFn)(struct Hook *, Ctx *, Frame *);

typedef struct Hook {
  const char *name;
  u32 va;                 /* static address (image base 0x10000000) */
  u8 len;                 /* bytes replaced (whole instructions, no relative branches, verified offline) */
  const char *expect;     /* expected original bytes as hex */
  i32 reloc;              /* offset of a 4-byte absolute address inside the bytes (-1 = none) */
  u16 skip_ret;           /* ret N used when on_enter returns 1 */
  EnterFn on_enter;
  LeaveFn on_leave;
  u8 *stub;
  int installed;
} Hook;

static inline u32 arg(Ctx *c, int i) { return *(u32 *)(c->esp + 4 + 4 * i); }   /* at function entry */
static inline u32 at_esp(Ctx *c, int off) { return *(u32 *)(c->esp + off); }   /* mid-function */

int hook_engine_init(void);
int hook_verify(Hook *h, u32 base);
int hook_install(Hook *h, u32 base);
