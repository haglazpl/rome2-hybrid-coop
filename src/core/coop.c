/* Hybrid co-op for Total War: ROME II - native port of probe.js v22 (Frida).
   Loaded into Rome2.exe by the loader (dinput8.dll). Hooks empire.retail.dll (version-checked). */
#include "hook.h"
#include "ccq_names.h"

#ifndef HC_VERSION
#define HC_VERSION "dev"            /* set by tools/build.sh from the release tag */
#endif
#define VERSION_TEXT "Hybrid Co-op core " HC_VERSION
#define EMPIRE_TIMESTAMP 0x69ce2041u
#define IMG 0x10000000u

static u32 BASE;
#define A(va) ((u32)(va) - IMG + BASE)
#define S(p) ((u32)(p) - BASE + IMG)

/* ---- engine layout ---- */
enum { ctrl_model = 0x58, ctrl_local_faction = 0x110, model_world = 0x1b98, world_current = 0x3c, world_phase = 0x68,
       faction_is_human = 0x730, faction_dead = 0x840 };
enum {
  FN_can_act_now_ret_ui = 0x111bbddc, FN_order_ui_turn_check_ret = 0x1110743e,
  FN_owns_check_ret_action_gate = 0x1077e250, FN_may_end_turn_ret_ui = 0x111bbd61, FN_may_end_turn_ret_exec = 0x1082c15a,
  FN_model_tick_ret_ui = 0x106c961f, FN_end_turn_faction = 0x1077d560,
  FN_diplo_after_deal_gate_ret = 0x1116ae55,
};
static const u32 diplomacy_ui_gate_rets[] = { 0x11195f38, 0x1116ccfc };
static const u32 attack_exec_turn_check_rets[] = { 0x10828342, 0x10828418 };
static const u32 is_turn_sim_rets[] = { 0x10803d35, 0x10808098, 0x109221aa };

/* ---------------------------------------------------------------- memory helpers */
static inline u32 rd(u32 p, u32 off) { return p < 0x10000 ? 0 : *(u32 *)(p + off); }
static inline u8 rd8(u32 p, u32 off) { return p < 0x10000 ? 0 : *(u8 *)(p + off); }
static inline i32 rds(u32 p, u32 off) { return p < 0x10000 ? -999 : *(i32 *)(p + off); }
static inline u16 rd16(u32 p, u32 off) { return *(u16 *)(p + off); }

/* ---------------------------------------------------------------- log */
static HANDLE g_logf = INVALID_HANDLE_VALUE;
static WCHAR g_dir[MAX_PATH];
static struct { u32 h, t; } g_recent[1024];

static u32 fnv(const char *s) { u32 h = 2166136261u; while (*s) { h ^= (u8)*s++; h *= 16777619u; } return h; }
static int contains(const char *s, const char *w) { uptr n = slen(w); for (; *s; s++) if (sprefix(s, w)) return 1; (void)n; return 0; }

static void log_raw(const char *msg) {
  if (g_logf == INVALID_HANDLE_VALUE) return;
  SYSTEMTIME t; GetLocalTime(&t);
  char line[1200];
  int n = wsprintfA(line, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  uptr m = slen(msg); if (m > 1100) m = 1100;
  mcpy(line + n, msg, m); n += (int)m; line[n++] = '\r'; line[n++] = '\n';
  DWORD w; WriteFile(g_logf, line, (DWORD)n, &w, NULL);
}
/* identical message at most once per 2 s (diplomacy messages always) - like probe.js */
static void logs(const char *msg) {
  if (!contains(msg, "DIPLOMACY")) {
    u32 h = fnv(msg), now = GetTickCount(), i = h & 1023;
    if (g_recent[i].h == h && now - g_recent[i].t < 2000) return;
    g_recent[i].h = h; g_recent[i].t = now;
  }
  log_raw(msg);
}
#define LOG(...) do { char _b[1100]; wsprintfA(_b, __VA_ARGS__); logs(_b); } while (0)

/* 16-bit id of this build, sent inside every command tag: both players must run the same build, otherwise the
   two simulations would follow different rules (desync). A different id from the other PC = warning. */
static u16 build_id(void) { u32 h = fnv(HC_VERSION); u16 v = (u16)(h ^ (h >> 16)); return v ? v : 1; }
static u32 g_otherBuild;            /* first different build id seen in a command from the other PC (0 = none) */

/* ---------------------------------------------------------------- game state */
static u32 ctrl, model;
static u32 world(void) { return model ? rd(model, model_world) : 0; }
static u32 current(void) { u32 w = world(); return w ? rd(w, world_current) : 0; }
static i32 phase(void) { u32 w = world(); return w ? rds(w, world_phase) : -1; }
static u32 local(void) { return ctrl ? rd(ctrl, ctrl_local_faction) : 0; }
static int isHuman(u32 f) { return f && rd8(f, faction_is_human) == 1; }

static struct { int active; u32 real, loc; } uiSwap;
static struct { u32 active, target, last; u32 dropped[8]; int ndropped; u32 graceUntil, lastEnd; } diplo;
static struct { int active; u32 saved, actor, w; } moveHold;
static struct { int active, move; u32 saved, actor, w; } execSwap;
static u32 ready[8]; static int nready;
static u32 autoEndedFor;
static int absorbNextMayEndTurn, inAutoEnd;

static int readyHas(u32 f) { for (int i = 0; i < nready; i++) if (ready[i] == f) return 1; return 0; }
static void readyAdd(u32 f) { if (!readyHas(f) && nready < 8) ready[nready++] = f; }
static int droppedHas(u32 f) { for (int i = 0; i < diplo.ndropped; i++) if (diplo.dropped[i] == f) return 1; return 0; }
static void droppedDel(u32 f) { for (int i = 0; i < diplo.ndropped; i++) if (diplo.dropped[i] == f) { diplo.dropped[i] = diplo.dropped[--diplo.ndropped]; return; } }
static void droppedAdd(u32 f) { if (!droppedHas(f) && diplo.ndropped < 8) diplo.dropped[diplo.ndropped++] = f; }

/* labels: faction pointer -> LOCAL / OTHER_HUMAN / AI#n */
static struct { u32 f; char tag[16]; } g_names[512]; static int g_nnames;
static char g_lbl[8][48]; static int g_lbli;
static const char *label(u32 f) {
  char *b = g_lbl[g_lbli++ & 7];
  if (!f) { wsprintfA(b, "null"); return b; }
  int i; for (i = 0; i < g_nnames; i++) if (g_names[i].f == f) break;
  if (i == g_nnames && g_nnames < 512) {
    g_names[i].f = f; g_nnames++;
    if (local() && f == local()) wsprintfA(g_names[i].tag, "LOCAL");
    else if (isHuman(f)) wsprintfA(g_names[i].tag, "OTHER_HUMAN");
    else wsprintfA(g_names[i].tag, "AI#%d", i);
  }
  if (i < 512 && local() && f == local() && !seq(g_names[i].tag, "LOCAL")) wsprintfA(g_names[i].tag, "LOCAL");
  wsprintfA(b, "%s(0x%x)", i < 512 ? g_names[i].tag : "?", f);
  return b;
}
static u32 realCurrent(void) { return uiSwap.active ? uiSwap.real : current(); }
static char g_state[160];
static const char *state(void) {
  wsprintfA(g_state, "cur=%s%s phase=%d local=%s", label(realCurrent()), uiSwap.active ? "[ui:LOCAL]" : "", phase(), label(local()));
  return g_state;
}

static void setCtrl(u32 c, const char *where) {
  if (!c || c == ctrl) return;
  if (!rd(c, ctrl_model)) return;
  ctrl = c; model = rd(ctrl, ctrl_model);
  LOG("CONTROLLER found via %s ctrl=0x%x model=0x%x %s", where, ctrl, model, state());
}

/* command type hashes */
static struct { u32 h; const char *name; } g_hash[CCQ_GLOBALS_N]; static int g_nhash;
static void loadHashes(void) {
  g_nhash = 0;
  for (int i = 0; i < CCQ_GLOBALS_N; i++) { u32 h = *(u32 *)A(CCQ_GLOBALS[i].va); if (h) { g_hash[g_nhash].h = h; g_hash[g_nhash].name = CCQ_GLOBALS[i].name; g_nhash++; } }
}
static const char *hashName(u32 h) { for (int i = 0; i < g_nhash; i++) if (g_hash[i].h == h) return g_hash[i].name; return ""; }

/* ---------------------------------------------------------------- turn helpers */
static int humansAlive(u32 w, u32 *out, int max) {
  i32 n = rds(w, 0x50); u32 arr = rd(w, 0x54); int k = 0;
  for (i32 i = 0; i < n && i < 2000; i++) { u32 f = rd(arr, i * 4); if (isHuman(f) && rd8(f, faction_dead) == 0) { if (k < max) out[k] = f; k++; } }
  return k;
}
static int humansAliveOk(u32 w) { u32 tmp[8]; return humansAlive(w, tmp, 8) >= 2; }
static int laterHumanExists(u32 w, u32 cur) {
  i32 n = rds(w, 0x50); u32 arr = rd(w, 0x54); int seen = 0;
  for (i32 i = 0; i < n && i < 2000; i++) {
    u32 f = rd(arr, i * 4);
    if (f == cur) { seen = 1; continue; }
    if (seen && isHuman(f) && rd8(f, faction_dead) == 0) return 1;
  }
  return 0;
}
static int slotOf(u32 w, u32 f) { i32 n = rds(w, 0x50); u32 arr = rd(w, 0x54); for (i32 i = 0; i < n && i < 2000; i++) if (rd(arr, i * 4) == f) return i; return -1; }

static int localReady(void) { u32 l = local(); return l && readyHas(l); }
static u32 shownNegotiator(void) {
  if (diplo.active) return diplo.active;
  if (!diplo.last) return 0;
  if ((i32)(GetTickCount() - diplo.graceUntil) < 0) return diplo.last;
  return 0;
}
static int otherNegotiating(void) { u32 l = local(); return diplo.active && l && diplo.active != l; }
static int otherHumansTurn(void) {
  u32 cur = current(), l = local();
  return l && isHuman(l) && cur && cur != l && isHuman(cur) && phase() == 1 && !localReady() && shownNegotiator() == 0;
}

/* ---------------------------------------------------------------- status file for the in-game End Turn text */
static char g_lastStatus[96]; static int g_friendsReadyPrev = -1; static u32 g_lastStatusTick;
static void write_status(const char *line) {
  WCHAR tmp[MAX_PATH], dst[MAX_PATH];
  wcpy(tmp, g_dir); wcat(tmp, L"\\status.tmp"); wcpy(dst, g_dir); wcat(dst, L"\\status.txt");
  HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD wr; WriteFile(h, line, (DWORD)slen(line), &wr, NULL); WriteFile(h, "\n", 1, &wr, NULL); CloseHandle(h);
  MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING);
}
static void emitStatus(void) {
  u32 now = GetTickCount();
  if (now - g_lastStatusTick < 500) return;
  g_lastStatusTick = now;
  u32 w = world(), l = local();
  if (!w || !l) return;
  u32 hs[8]; int n = humansAlive(w, hs, 8); if (n > 8) n = 8;
  u32 cur = realCurrent();
  int shared = n >= 2 && isHuman(cur) && phase() == 1;
  int nf = 0, nfr = 0;
  for (int i = 0; i < n; i++) if (hs[i] != l) { nf++; if (readyHas(hs[i])) nfr++; }
  int friendsReady = nf > 0 && nfr == nf;
  char line[96];
  wsprintfA(line, "stage=%s;you=%d;friend=%d;diplo=%s;warn=%s", shared ? "shared" : (isHuman(cur) ? "human" : "ai"),
            readyHas(l) ? 1 : 0, friendsReady ? 1 : 0, !diplo.active ? "" : (diplo.active == l ? "you" : "friend"),
            g_otherBuild ? "version" : "");
  if (!seq(line, g_lastStatus)) {
    mcpy(g_lastStatus, line, slen(line) + 1);
    write_status(line);
    LOG("STATUS %s", line);
  }
  if (shared && friendsReady && g_friendsReadyPrev == 0) MessageBeep(MB_ICONASTERISK);    /* friend pressed End Turn */
  g_friendsReadyPrev = shared ? friendsReady : -1;
}

/* ================================================================ hooks */
static const char *g_lastWindow = "";

/* ---- turn changes + per-turn reset ---- */
static int advance_turn_enter(Hook *h, Ctx *c, Frame *f) { (void)h; model = c->ecx; f->d[0] = current(); return 0; }
static void advance_turn_leave(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)c;
  u32 before = f->d[0], after = current();
  if (isHuman(before) || isHuman(after)) LOG("TURN %s -> %s phase=%d", label(before), label(after), phase());
  nready = 0;
  diplo.active = 0; diplo.target = 0; diplo.ndropped = 0;
  if (autoEndedFor && current() != autoEndedFor) autoEndedFor = 0;
}

static int end_turn_faction_enter(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)f; if (!model) model = c->ecx;
  LOG("END_TURN_PROCESS faction=%s forced=%d %s", label(arg(c, 0)), arg(c, 1) & 0xff, state());
  return 0;
}
static int local_turn_start_ui_enter(Hook *h, Ctx *c, Frame *f) { (void)h; (void)f; setCtrl(c->ecx, "turn_ui"); LOG("LOCAL_TURN_START_UI %s", state()); return 0; }

/* ---- main UI gate: "is it the local player's turn?" ---- */
static int lastLocalTurn = -1; static u32 forcedCount, diploLockLogs;
static u32 g_uiCallers[256]; static int g_nuiCallers;
static int ui_is_local_turn_enter(Hook *h, Ctx *c, Frame *f) { (void)h; (void)f; setCtrl(rd(c->ecx, 4), "ui_is_local_turn"); return 0; }
static void ui_is_local_turn_leave(Hook *h, Ctx *c, Frame *f) {
  (void)h;
  int v = c->eax & 0xff; u32 caller = S(f->ra);
  int seen = 0; for (int i = 0; i < g_nuiCallers; i++) if (g_uiCallers[i] == caller) { seen = 1; break; }
  if (!seen) { if (g_nuiCallers < 256) g_uiCallers[g_nuiCallers++] = caller; LOG("UI_GATE new caller 0x%x answer=%d", caller, v); }
  if (caller == FN_diplo_after_deal_gate_ret && diplo.active && diplo.target) {
    /* human-human deal answered -> close the diplomacy screen instead of the game's reset/re-open */
    c->eax = 0;
    LOG("DIPLOMACY human deal done: closing screen instead of re-opening (%s with %s)", label(diplo.active), label(diplo.target));
    return;
  }
  int isDiploGate = caller == diplomacy_ui_gate_rets[0] || caller == diplomacy_ui_gate_rets[1];
  if (isDiploGate && otherNegotiating() && v != 0) {
    c->eax = 0; v = 0;
    if (diploLockLogs++ < 20) LOG("DIPLOMACY locked: %s is negotiating", label(diplo.active));
  } else if (v == 0 && otherHumansTurn()) {
    c->eax = 1; v = 1;
    if (forcedCount++ % 500 == 0) LOG("UNLOCK ui_is_local_turn forced=1 (x%u) %s", forcedCount, state());
  }
  if (v != lastLocalTurn) { lastLocalTurn = v; LOG("UI_IS_LOCAL_TURN=%d %s", v, state()); }
}

/* ---- partner's diplomacy screen closes when the other human ends the negotiation ---- */
static int diplo_end_listener_enter(Hook *h, Ctx *c, Frame *f) {
  (void)h;
  f->d[0] = 0;
  u32 lst = c->ecx, cb = rd(lst, 0x2c), neg = rd(lst, 0x30);
  if (!cb || !neg) return 0;
  u32 owner = rd(neg, 0x18), l = local();
  if (!owner || !l || owner == l || !isHuman(owner)) return 0;
  f->d[0] = cb; f->d[1] = *(u8 *)(cb + 0xe1);
  *(u8 *)(cb + 0xe1) = 1;
  LOG("DIPLOMACY partner screen: closing (negotiation by %s ended, flag was %d)", label(owner), f->d[1]);
  return 0;
}
static void diplo_end_listener_leave(Hook *h, Ctx *c, Frame *f) { (void)h; (void)c; if (f->d[0]) *(u8 *)(f->d[0] + 0xe1) = (u8)f->d[1]; }

/* ---- UI "may I act" ---- */
static int lastUi = -1;
static int ui_can_act_enter(Hook *h, Ctx *c, Frame *f) { (void)h; (void)f; setCtrl(rd(c->ecx, 4), "ui_can_act"); return 0; }
static void ui_can_act_leave(Hook *h, Ctx *c, Frame *f) { (void)h; (void)f; int v = c->eax & 0xff; if (v != lastUi) { lastUi = v; LOG("UI_CAN_ACT=%d %s", v, state()); } }

static u32 unlockCount;
static int can_act_now_enter(Hook *h, Ctx *c, Frame *f) { (void)h; f->d[0] = arg(c, 0); return 0; }
static void can_act_now_leave(Hook *h, Ctx *c, Frame *f) {
  (void)h;
  if (S(f->ra) != FN_can_act_now_ret_ui || (c->eax & 0xff) != 0) return;
  u32 l = local();
  if (!l || f->d[0] != l || !otherHumansTurn()) return;
  c->eax = 1;
  if (unlockCount++ % 200 == 0) LOG("UNLOCK forced UI_CAN_ACT for LOCAL during %s turn (x%u)", label(current()), unlockCount);
}

/* ---- movement/order cursor (UI only) ---- */
static u32 orderForced;
static int order_ui_faction_check_after(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)f;
  u32 uiFaction = at_esp(c, 4);
  if ((c->eax & 0xff) != 0) return 0;
  if (uiFaction != local() || !otherHumansTurn()) return 0;
  c->eax = 1;
  if (orderForced++ % 500 == 0) LOG("UNLOCK order cursor faction check (x%u) %s", orderForced, state());
  return 0;
}

/* ---- character action gate (simulation, deterministic) ---- */
static u32 actionGateForced;
static int owns_check_enter(Hook *h, Ctx *c, Frame *f) { (void)h; f->d[0] = arg(c, 0); f->d[1] = c->ecx; f->d[2] = c->esi; return 0; }
static void owns_check_leave(Hook *h, Ctx *c, Frame *f) {
  (void)h;
  if (S(f->ra) != FN_owns_check_ret_action_gate || (c->eax & 0xff) != 0) return;
  u32 owner = rd(rd(f->d[0], 0x1dc), 0), w = rd(f->d[2], model_world);
  i32 ph = w ? rds(w, world_phase) : -1;
  if (isHuman(owner) && isHuman(f->d[1]) && ph == 1) {
    c->eax = 1;
    if (actionGateForced++ % 50 == 0) LOG("UNLOCK action gate owner=%s cur=%s (x%u)", label(owner), label(f->d[1]), actionGateForced);
  }
}

/* ---- merged human turns: END_TURN executor = READY ---- */
static int endturn_exec_after_decode(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)f;
  u32 fac = at_esp(c, 4), mdl = at_esp(c, 0x14), w = rd(mdl, model_world);
  if (!w) return 0;
  u32 cur = rd(w, world_current); i32 ph = rds(w, world_phase);
  u32 hs[8]; int n = humansAlive(w, hs, 8); if (n > 8) n = 8;
  if (n < 2 || !isHuman(fac) || !isHuman(cur) || ph != 1) return 0;
  readyAdd(fac);
  int nr = 0; for (int i = 0; i < n; i++) if (readyHas(hs[i])) nr++;
  LOG("MERGE READY %s (%d/%d)", label(fac), nr, n);
  *(u8 *)(c->esp + 8) = 1;                              /* forced: no end-turn warning popups */
  if (nr == n) {
    *(u32 *)(c->esp + 4) = cur; c->eax = cur;            /* end the CURRENT (last) human's turn */
    nready = 0;
    LOG("MERGE all humans ready -> ending shared turn (%s)", label(cur));
  } else absorbNextMayEndTurn = 1;                      /* make this END_TURN do nothing */
  return 0;
}
static int may_end_turn_enter(Hook *h, Ctx *c, Frame *f) { (void)h; f->d[0] = arg(c, 0); return 0; }
static void may_end_turn_leave(Hook *h, Ctx *c, Frame *f) {
  (void)h;
  u32 ra = S(f->ra);
  if (ra == FN_may_end_turn_ret_exec) { if (absorbNextMayEndTurn) { absorbNextMayEndTurn = 0; c->eax = 0; } return; }
  if (ra == FN_may_end_turn_ret_ui && (c->eax & 0xff) == 0 && f->d[0] == local() && otherHumansTurn()) c->eax = 1;
}
static int endturn_button_turn_check_after(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)f;
  setCtrl(rd(c->edi, 4), "end_turn_button");
  LOG("END_TURN button pressed %s", state());
  if ((c->eax & 0xff) != 0) return 0;
  if (at_esp(c, 4) == local() && otherHumansTurn()) c->eax = 1;
  return 0;
}
static int endturn_button_fill_faction_after(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)f;
  u32 l = local();
  if (!l || !isHuman(l)) return 0;
  u32 *slot = (u32 *)(c->esp + 0x18);
  if (*slot != l) { *slot = l; LOG("MERGE End Turn pressed by %s (sent as ready)", label(l)); }
  return 0;
}

/* ---- acting-faction architecture: (a) tag outgoing commands ---- */
#define TAG 0x48594231u
#define TAG_LEN 15
static u32 tagged;
static u32 g_seenOther[256]; static int g_nseenOther;
static int cmd_begin_write_enter(Hook *h, Ctx *c, Frame *f) {
  (void)h;
  if (g_nhash < 150) loadHashes();
  u32 buf = arg(c, 0), hh = arg(c, 1);
  /* log */
  const char *name = hashName(hh);
  if (!*name) {
    u32 k = hh ^ (S(f->ra) * 2654435761u); int seen = 0;
    for (int i = 0; i < g_nseenOther; i++) if (g_seenOther[i] == k) { seen = 1; break; }
    if (!seen) { if (g_nseenOther < 256) g_seenOther[g_nseenOther++] = k; if (hh != TAG) LOG("SEND_OTHER type=0x%x caller=0x%x", hh, S(f->ra)); }
  } else LOG("SEND %s caller=0x%x %s", name, S(f->ra), state());
  /* tag */
  if (!ctrl || hh == TAG) return 0;
  u32 outbox = rd(ctrl, 0x5c);
  if (!outbox || buf != outbox + 4) return 0;          /* only the local player's command buffer */
  u32 l = local(), w = world();
  if (!l || !w || !isHuman(l) || !humansAliveOk(w)) return 0;
  int slot = slotOf(w, l);
  if (slot < 0) return 0;
  u32 len = *(u32 *)(buf + 0x5000);
  if (len + TAG_LEN + 0x100 > 0x5000) return 0;
  u8 *r = (u8 *)(buf + len);
  *(u16 *)r = TAG_LEN; r[2] = 0x48; r[3] = 0x59; r[4] = 0x42; r[5] = 0x31;
  r[6] = 0; *(u16 *)(r + 7) = (u16)slot; *(u16 *)(r + 9) = build_id(); *(u32 *)(r + 11) = rd(l, 4);
  *(u32 *)(buf + 0x5000) = len + TAG_LEN;
  if (tagged++ < 3) LOG("TAG outgoing command as %s slot=%d", label(l), slot);
  return 0;
}

/* ---- (b) execute tagged commands as the sending faction ---- */
static u32 taggedActor(u32 batch, u32 off, u32 w, u32 *build) {
  u32 total = *(u32 *)(batch + 0x5004);
  if (total > 0x5000 || off > total) return 0;
  u32 o = 0; i32 prev = -1;
  while (o < off) { u16 len = rd16(batch, 4 + o); if (len < 7) return 0; prev = (i32)o; o += len; }
  if (o != off || prev < 0) return 0;
  u32 r = batch + 4 + (u32)prev;
  if (rd16(r, 0) != TAG_LEN || rd8(r, 2) != 0x48 || rd8(r, 3) != 0x59 || rd8(r, 4) != 0x42 || rd8(r, 5) != 0x31) return 0;
  u32 slot = rd16(r, 7), id = rd(r, 11);
  *build = rd16(r, 9);
  i32 n = rds(w, 0x50); u32 arr = rd(w, 0x54);
  if ((i32)slot < n) { u32 fa = rd(arr, slot * 4); if (fa && rd(fa, 4) == id) return fa; }
  for (i32 i = 0; i < n && i < 2000; i++) { u32 fa = rd(arr, i * 4); if (fa && rd(fa, 4) == id) return fa; }
  return 0;
}
static void __cdecl noop_executor(void *cmd, void *mdl) { (void)cmd; (void)mdl; }
static void restoreUiSwap(u32 w, const char *why) {
  if (!uiSwap.active) return;
  uiSwap.active = 0;
  u32 *cw = (u32 *)(w + world_current);
  if (*cw == uiSwap.loc) *cw = uiSwap.real; else LOG("UI swap: current changed outside a tick (%s)", why);
}
static void endMoveHold(const char *why) {
  if (!moveHold.active) return;
  moveHold.active = 0;
  u32 *cw = (u32 *)(moveHold.w + world_current);
  if (*cw == moveHold.actor) *cw = moveHold.saved; else LOG("HOLD: current changed, not restored (%s)", why);
}
static u32 execSwaps, holdLogs;
static const char *g_execName = ""; static int g_execLog;   /* CMD log line written in exec_post */

static int exec_pre(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)f;
  g_execLog = 0;
  u32 batch = at_esp(c, 0x14), off = at_esp(c, 0x24), mdl = at_esp(c, 0x40);
  u32 w = rd(mdl, model_world);
  if (!w) return 0;
  if (uiSwap.active) restoreUiSwap(w, "exec");                /* never simulate with the UI value */
  endMoveHold("next command");
  u32 hh = c->esi;
  if (hh == TAG) return 0;
  const char *name = hashName(hh);
  g_execName = name; g_execLog = 1;
  u32 cur = rd(w, world_current);
  u32 tagBuild = 0;
  u32 tg = taggedActor(batch, off, w, &tagBuild);
  if (tg && tagBuild != build_id() && !g_otherBuild) {
    g_otherBuild = tagBuild ? tagBuild : 0xffff;
    LOG("VERSION MISMATCH: %s runs a different Hybrid Co-op build (id 0x%04x, this PC: %s id 0x%04x) - "
        "update the mod on both PCs, the games may go out of sync", label(tg), tagBuild, HC_VERSION, build_id());
  }
  u32 actor = tg ? tg : cur;
  int shared = isHuman(cur) && rds(w, world_phase) == 1 && humansAliveOk(w);
  u32 negotiator = diplo.active;
  int isDiplo = sprefix(name, "DIPLOMACY_");
  /* diplomacy lock (deterministic: command stream only) */
  if (tg && isDiplo && !seq(name, "DIPLOMACY_END_NEGOTIATION") && isHuman(actor) && shared && diplo.active &&
      diplo.active != actor && (seq(name, "DIPLOMACY_BEGIN_NEGOTIATION") || !diplo.target || diplo.target != actor)) {
    c->eax = (u32)noop_executor;
    if (seq(name, "DIPLOMACY_BEGIN_NEGOTIATION")) droppedAdd(actor);
    char t[64]; t[0] = 0; if (diplo.target) wsprintfA(t, " with %s", label(diplo.target));
    LOG("DIPLOMACY: dropped %s of %s - %s is negotiating%s", name, label(actor), label(diplo.active), t);
    g_execLog = 0;
    return 0;
  }
  if (seq(name, "DIPLOMACY_BEGIN_NEGOTIATION") && isHuman(actor)) {
    diplo.active = actor; diplo.target = 0;
    u32 rec = batch + 4 + off; u16 len = rd16(rec, 0);
    u32 hs[8]; int n = humansAlive(w, hs, 8); if (n > 8) n = 8;
    for (int k = 0; k < n; k++) {
      if (hs[k] == actor) continue;
      u32 id = rd(hs[k], 4);
      for (u32 i = 7; i + 4 <= len; i++) if (*(u32 *)(rec + i) == id) { diplo.target = hs[k]; break; }
    }
    char hex[140]; int p = 0;
    for (u32 i = 7; i < len && i < 64; i++) p += wsprintfA(hex + p, "%02x", rd8(rec, i));
    hex[p] = 0;
    LOG("DIPLOMACY begin by %s payload=%s -> partner %s%s", label(actor), hex, diplo.target ? label(diplo.target) : "AI", diplo.target ? " (human)" : "");
  } else if (seq(name, "DIPLOMACY_END_NEGOTIATION")) {
    if (droppedHas(actor)) droppedDel(actor);
    else if (diplo.active) {
      diplo.last = diplo.active; diplo.graceUntil = GetTickCount() + 2500; diplo.lastEnd = GetTickCount();
      diplo.active = 0; diplo.target = 0;
    }
  }
  if (seq(name, "END_TURN")) return 0;                        /* END_TURN keeps the real current (READY logic) */
  u32 execAs = actor;
  if (isDiplo && !seq(name, "DIPLOMACY_BEGIN_NEGOTIATION") && negotiator && isHuman(negotiator)) execAs = negotiator;
  else if (!tg) return 0;
  if (!shared || execAs == cur || !isHuman(execAs) || rd8(execAs, faction_dead) != 0) return 0;
  execSwap.active = 1; execSwap.saved = cur; execSwap.actor = execAs; execSwap.w = w;
  execSwap.move = seq(name, "CHARACTER_LOCOMOTE_TO") || seq(name, "ARMY_LEAVE_GARRISON") || isDiplo;
  *(u32 *)(w + world_current) = execAs;
  if (isDiplo) LOG("DIPLOMACY exec %s as %s", name, label(execAs));
  else if (execSwaps++ % 100 == 0) LOG("ACT as %s (%s) x%u", label(execAs), *name ? name : "?", execSwaps);
  return 0;
}
static int exec_post(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)c; (void)f;
  if (g_execLog) { g_execLog = 0; LOG("CMD %s %s", *g_execName ? g_execName : "?", state()); }
  if (!execSwap.active) return 0;
  execSwap.active = 0;
  if (execSwap.move) {                                        /* keep 'current' until the end of the tick (camera fix) */
    moveHold.active = 1; moveHold.saved = execSwap.saved; moveHold.actor = execSwap.actor; moveHold.w = execSwap.w;
    if (holdLogs++ < 10) LOG("HOLD current=%s until end of tick", label(execSwap.actor));
    return 0;
  }
  u32 *cw = (u32 *)(execSwap.w + world_current);
  if (*cw == execSwap.actor) *cw = execSwap.saved; else LOG("ACT: current changed during command, not restored");
  return 0;
}

/* ---- (c) UI swap around every simulation tick, (d) deterministic auto-end ---- */
typedef void(__thiscall *EndTurnFn)(u32 model, u32 faction, u32 forced);
static int model_tick_enter(Hook *h, Ctx *c, Frame *f) {
  (void)h;
  f->d[0] = S(f->ra) == FN_model_tick_ret_ui;
  if (f->d[0]) setCtrl(c->edi, "model_tick");
  u32 mdl = c->ecx, w = rd(mdl, model_world);
  if (!w) return 0;
  if (!model) model = mdl;
  endMoveHold("tick start");
  restoreUiSwap(w, "tick");
  u32 cur = rd(w, world_current);
  if (isHuman(cur) && rds(w, world_phase) == 1 && autoEndedFor != cur && laterHumanExists(w, cur)) {
    autoEndedFor = cur;
    LOG("MERGE auto-ending turn of %s (another human plays later this round)", label(cur));
    inAutoEnd = 1;
    ((EndTurnFn)A(FN_end_turn_faction))(mdl, cur, 1);
    inAutoEnd = 0;
  }
  return 0;
}
static void model_tick_leave(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)c;
  endMoveHold("tick end");
  if (!f->d[0] || !ctrl) return;
  u32 w = world();
  if (!w) return;
  emitStatus();
  u32 cur = rd(w, world_current), l = local();
  int open = l && isHuman(l) && rd8(l, faction_dead) == 0 && isHuman(cur) && cur != l && rds(w, world_phase) == 1 &&
             humansAliveOk(w) && !localReady() && !otherNegotiating();
  u32 shown = shownNegotiator();
  int negotiating = shown && isHuman(shown) && isHuman(cur) && rds(w, world_phase) == 1 && humansAliveOk(w);
  u32 target = negotiating ? shown : (open ? l : 0);
  const char *st = negotiating ? "DIPLOMACY" : (open ? "OPEN (UI acts as LOCAL)" : (localReady() ? "closed (you are READY - waiting)" : "closed"));
  if (!seq(st, g_lastWindow)) { g_lastWindow = st; LOG("WINDOW %s%s%s cur=%s", st, negotiating ? " UI acts as " : "", negotiating ? label(shown) : "", label(cur)); }
  if (!target || target == cur) return;
  uiSwap.active = 1; uiSwap.real = cur; uiSwap.loc = target;
  *(u32 *)(w + world_current) = target;
}
static int turn_begin_enter(Hook *h, Ctx *c, Frame *f) { (void)h; f->d[0] = c->ecx; f->d[1] = (u32)rds(c->ecx, world_phase); return 0; }
static void turn_begin_leave(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)c;
  u32 w = f->d[0], cur = rd(w, world_current);
  if ((i32)f->d[1] != 0 || rds(w, world_phase) != 1) return;
  if (!isHuman(cur) || autoEndedFor == cur) return;
  if (laterHumanExists(w, cur)) LOG("MERGE turn of %s started, will auto-end next tick", label(cur));
}
/* (e) the auto-end must not put the LOCAL player's UI into "my turn is over" mode: skip UI handler 0x106c6110 */
static u32 uiTurnEndSkipped;
static int ui_turn_end_enter(Hook *h, Ctx *c, Frame *f) {
  (void)h; (void)f;
  u32 fac = arg(c, 0);
  if (inAutoEnd && fac && fac == local()) {
    if (uiTurnEndSkipped++ < 50) LOG("MERGE auto-end: skipped UI \"turn ended\" handler for %s", label(fac));
    return 1;                                                 /* ret 4 */
  }
  return 0;
}

/* ---- turn checks ---- */
static int is_turn_enter(Hook *h, Ctx *c, Frame *f) { (void)h; f->d[0] = arg(c, 0); if (!model) model = c->ecx; return 0; }
static void is_turn_common_leave(Ctx *c, Frame *f, int unblocked) {
  u32 fa = f->d[0], ra = S(f->ra);
  if (!unblocked && (c->eax & 0xff) == 0) {
    for (int i = 0; i < 3; i++) if (ra == is_turn_sim_rets[i]) {
      if (isHuman(fa) && isHuman(current()) && phase() == 1) { c->eax = 1; LOG("UNLOCK politics/dilemma turn check for %s", label(fa)); return; }
    }
  }
  if (unblocked && (c->eax & 0xff) == 0) {
    if (ra == FN_order_ui_turn_check_ret && fa == local() && otherHumansTurn()) {
      c->eax = 1;
      if (orderForced++ % 500 == 0) LOG("UNLOCK order cursor turn check (x%u) %s", orderForced, state());
      return;
    }
    if (ra == attack_exec_turn_check_rets[0] || ra == attack_exec_turn_check_rets[1]) {
      if (isHuman(fa) && isHuman(current()) && phase() == 1) { c->eax = 1; LOG("UNLOCK attack executor turn check for %s %s", label(fa), state()); return; }
    }
  }
  if (!fa || fa == current() || !isHuman(fa)) return;
  if (ra == FN_may_end_turn_ret_ui) return;
  LOG("CHECK %s(%s)=%d caller=0x%x %s", unblocked ? "is_turn_unblocked" : "is_turn", label(fa), c->eax & 0xff, ra, state());
}
static void is_turn_leave(Hook *h, Ctx *c, Frame *f) { (void)h; is_turn_common_leave(c, f, 0); }
static void is_turn_unblocked_leave(Hook *h, Ctx *c, Frame *f) { (void)h; is_turn_common_leave(c, f, 1); }

/* ================================================================ hook table (bytes verified against the game) */
static Hook HOOKS[] = {
  {"advance_turn", 0x10779cd0, 5, "83ec2c5355", -1, 0, advance_turn_enter, advance_turn_leave},
  {"end_turn_faction", 0x1077d560, 6, "81ecc0000000", -1, 0, end_turn_faction_enter, 0},
  {"local_turn_start_ui", 0x106c2be0, 6, "81eca0080000", -1, 0, local_turn_start_ui_enter, 0},
  {"ui_is_local_turn", 0x111c3730, 6, "8b49048b4158", -1, 0, ui_is_local_turn_enter, ui_is_local_turn_leave},
  {"diplo_end_listener", 0x111da2b0, 7, "578bf9837f2c00", -1, 0, diplo_end_listener_enter, diplo_end_listener_leave},
  {"ui_can_act", 0x111bbdb0, 5, "83ec105356", -1, 0, ui_can_act_enter, ui_can_act_leave},
  {"can_act_now", 0x1077e390, 5, "53568bf157", -1, 0, can_act_now_enter, can_act_now_leave},
  {"owns_check", 0x107b95b0, 10, "8b4424048b80dc010000", -1, 0, owns_check_enter, owns_check_leave},
  {"may_end_turn", 0x1078eba0, 6, "6a00ff742408", -1, 0, may_end_turn_enter, may_end_turn_leave},
  {"is_turn", 0x1077e150, 5, "807c240800", -1, 0, is_turn_enter, is_turn_leave},
  {"is_turn_unblocked", 0x1078ebb0, 8, "568bf157ff74240c", -1, 0, is_turn_enter, is_turn_unblocked_leave},
  {"model_tick", 0x107934b0, 6, "558bec83e4f8", -1, 0, model_tick_enter, model_tick_leave},
  {"turn_begin", 0x107d8f80, 6, "83ec14568bf1", -1, 0, turn_begin_enter, turn_begin_leave},
  {"cmd_begin_write", 0x101d3550, 5, "8b44240856", -1, 0, cmd_begin_write_enter, 0},
  {"ui_turn_end", 0x106c6110, 5, "5355568be9", -1, 4, ui_turn_end_enter, 0},
  {"order_ui_faction_check_after", 0x111073f4, 5, "83c40884c0", -1, 0, order_ui_faction_check_after, 0},
  {"endturn_exec_after_decode", 0x1082c14e, 6, "8b7424148bce", -1, 0, endturn_exec_after_decode, 0},
  {"endturn_button_turn_check_after", 0x111bf36f, 5, "83c40884c0", -1, 0, endturn_button_turn_check_after, 0},
  {"endturn_button_fill_faction_after", 0x111bf3d7, 7, "885c241c8b715c", -1, 0, endturn_button_fill_faction_after, 0},
  {"exec_pre", 0x10714db1, 8, "ff7424408d4c241c", -1, 0, exec_pre, 0},
  {"exec_post", 0x10714dbc, 6, "8b1dc831fd11", 2, 0, exec_post, 0},
};
#define NHOOKS ((int)(sizeof HOOKS / sizeof HOOKS[0]))

/* ================================================================ init */
/* keep only the newest logs (names contain the date, so name order = age) */
#define KEEP_LOGS 20
static void prune_logs(const WCHAR *dir) {
  static WCHAR names[256][64]; int n = 0;
  WCHAR pat[MAX_PATH]; wcpy(pat, dir); wcat(pat, L"\\hybrid_*.log");
  WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do { if (n < 256 && wlen(fd.cFileName) < 64) wcpy(names[n++], fd.cFileName); } while (FindNextFileW(h, &fd));
  FindClose(h);
  for (int i = 1; i < n; i++)                          /* insertion sort, oldest first */
    for (int j = i; j > 0; j--) {
      const WCHAR *a = names[j - 1], *b = names[j]; int k = 0;
      while (a[k] && a[k] == b[k]) k++;
      if (a[k] <= b[k]) break;
      WCHAR t[64]; wcpy(t, names[j]); wcpy(names[j], names[j - 1]); wcpy(names[j - 1], t);
    }
  for (int i = 0; i + KEEP_LOGS - 1 < n; i++) { WCHAR p[MAX_PATH]; wcpy(p, dir); wcat(p, L"\\"); wcat(p, names[i]); DeleteFileW(p); }
}

static void open_log(void) {
  WCHAR la[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
  wcpy(g_dir, la); wcat(g_dir, L"\\RomeHybridCoop"); CreateDirectoryW(g_dir, NULL);
  WCHAR p[MAX_PATH]; wcpy(p, g_dir); wcat(p, L"\\logs"); CreateDirectoryW(p, NULL);
  prune_logs(p);
  SYSTEMTIME t; GetLocalTime(&t);
  char nm[64]; wsprintfA(nm, "\\hybrid_%04d%02d%02d_%02d%02d%02d.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  WCHAR wn[64]; int i = 0; for (; nm[i]; i++) wn[i] = (WCHAR)nm[i]; wn[i] = 0;
  wcat(p, wn);
  g_logf = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int g_started;
__declspec(dllexport) int __cdecl HybridCoopStart(void) {
  if (g_started) return 1;
  g_started = 1;
  open_log();
  write_status("stage=off");
  LOG("%s starting (build id 0x%04x)", VERSION_TEXT, build_id());
  u32 b = (u32)GetModuleHandleW(L"empire.retail.dll");
  if (!b) { LOG("empire.retail.dll not loaded - mod disabled"); return 0; }
  u32 pe = b + *(u32 *)(b + 0x3c), ts = *(u32 *)(pe + 8);
  if (ts != EMPIRE_TIMESTAMP) { LOG("unsupported game version (empire.retail.dll timestamp 0x%x, expected 0x%x) - mod disabled", ts, EMPIRE_TIMESTAMP); return 0; }
  BASE = b;
  for (int i = 0; i < NHOOKS; i++)
    if (!hook_verify(&HOOKS[i], BASE)) { LOG("code at %s (0x%x) is not as expected - mod disabled", HOOKS[i].name, HOOKS[i].va); return 0; }
  if (!hook_engine_init()) { LOG("hook engine init failed - mod disabled"); return 0; }
  loadHashes();
  int ok = 0;
  for (int i = 0; i < NHOOKS; i++) { if (hook_install(&HOOKS[i], BASE)) ok++; else LOG("hook %s FAILED", HOOKS[i].name); }
  LOG("READY empire.retail.dll base=0x%x hooks=%d/%d hashes=%d", BASE, ok, NHOOKS, g_nhash);
  return ok == NHOOKS;
}

BOOL WINAPI DllMain(HMODULE h, DWORD reason, void *res) {
  (void)res;
  if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
  if (reason == DLL_PROCESS_DETACH && g_logf != INVALID_HANDLE_VALUE) { write_status("stage=off"); CloseHandle(g_logf); }
  return TRUE;
}

