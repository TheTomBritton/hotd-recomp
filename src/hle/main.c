/*
 * The House of the Dead - the portable host, for Linux, macOS and the browser.
 *
 * The same lifted C as the Windows build (src/recomp/gen), on pcrecomp's
 * runtime/win32hle instead of runtime/native32: every Win32 import the game
 * makes is answered by win32hle's own implementation (files, the window,
 * DirectDraw, DirectSound and DirectInput on SDL2), so no Windows and no
 * Wine. In a browser (Emscripten) the game runs on a pthread and the page
 * presents its frames and plays its sound (src/web/web.c, the page).
 *
 * What is here is what is specific to this game: where the image goes, the
 * command line, the hooks the lift calls (hotd_longjmp and the online
 * play patches), the fault report, and the scripted input a headless run
 * is driven with. The game's own answers are in hotd.c.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <SDL2/SDL.h>

#include "win32hle.h"
#include "pe_loader.h"
#include "host.h"
#include "hotd.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "web.h"
#endif

extern const uint32_t hotd_entry_va;              /* recomp_dispatch.c */
extern const uint32_t hotd_exe_stamp;             /* recomp_dispatch.c: the build the lift is of */
extern const uint32_t hotd_setjmp_resume[][2];    /* recomp_dispatch.c: (function VA, resume VA) */
extern const uint32_t recomp_dispatch_count;
extern int win32hle_trace;
extern int hle_filetrace_on;
extern int g_input_trace;

static const char *g_guest_args;                       /* --guest-args: switches appended to the game's command line */
static int g_headless, g_fullscreen = -1;
static const char *g_scale;
static unsigned g_watchdog_s;

/* ---- what the lift calls into --------------------------------------------- */

/* A function the catalog missed, reached through a pointer: the run ends
 * naming it, and tools/seed_from_log.py puts it in the next lift. */
void recomp_not_lifted(uint32_t va) {
    fprintf(stderr, "ICALL: unresolved VA 0x%08X (not lifted; seed it with tools/seed_from_log.py)\n", va);
    hle_exit(44);
}

/* Watcom's longjmp. The game runs its tasks under a setjmp in sub_0043E354;
 * a task ends with longjmp(buf, 3), or 2 to run again. run_lift.py patches
 * the first instruction of the guest's longjmp (0x00579783) to come here,
 * with eax the jmp_buf and edx the value. Watcom's buffer: ebx ecx edx esi
 * edi ebp at 0..0x14, the return address at 0x18 (a lifted call pushes a
 * dummy, so the landing pad's own address is used), esp at 0x1C. */
static unsigned g_longjmps;
void hotd_longjmp(void) {
    uint32_t jb = g_eax, val = g_edx ? g_edx : 1;
    uint32_t esp = MEM32(jb + 0x1C);
    recomp_ehframe_t *f = recomp_eh_owner(esp);
    uint32_t resume = 0;
    for (int i = 0; f && hotd_setjmp_resume[i][0]; i++)
        if (hotd_setjmp_resume[i][0] == f->func) resume = hotd_setjmp_resume[i][1];
    g_longjmps++;
    if (g_longjmps <= 8 || (g_longjmps & 0x3FF) == 0)
        fprintf(stderr, "[longjmp] #%u buf %08X val %u esp %08X -> frame %s func %08X resume %08X\n", g_longjmps, jb, val, esp,
                f ? "found" : "NONE", f ? f->func : 0, resume);
    if (!resume) {
        fprintf(stderr, "[longjmp] buffer 0x%08X (esp 0x%08X): no lifted setjmp caller owns it\n", jb, esp);
        hle_exit(3);
    }
    g_ebx = MEM32(jb), g_ecx = MEM32(jb + 4), g_edx = MEM32(jb + 8);
    g_esi = MEM32(jb + 0xC), g_edi = MEM32(jb + 0x10), g_ebp = MEM32(jb + 0x14);
    g_esp = esp;
    g_eax = val;
    recomp_eh_resume(f, resume);
}

/* Online play (the Windows host's net.c): not on this host. The patches
 * still call these once a frame and around player 2's mouse reads. */
void hotd_net_gate(void) {}
void hotd_p2_in(uint32_t record) { (void)record; }
void hotd_p2_out(void) {}

/* ---- faults and the watchdog ---------------------------------------------- */
static void report(const char *why) {
    fprintf(stderr, "\n[fault] %s in sub_%08X, esp %08X\n", why, g_cur_func, g_esp);
    fprintf(stderr, "[fault] stack:");
    for (int k = 0; k < 64; k++) {                  /* code addresses on the guest stack: the callers, roughly */
        uint32_t w = MEM32(g_esp + 4 * k);
        if (w >= 0x00401000u && w < 0x00600000u) fprintf(stderr, " %08X", w);
    }
    fprintf(stderr, "\n");
}
#ifndef __EMSCRIPTEN__
static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char buf[96];
    snprintf(buf, sizeof buf, "signal %d at host address %p", sig, si ? si->si_addr : NULL);
    report(buf);
    _exit(128 + sig);
}
#endif
static void *watchdog(void *arg) {
    (void)arg;
    sleep(g_watchdog_s);
    report("watchdog: the run is over");
    hle_exit(2);
    return NULL;
}
#ifdef __EMSCRIPTEN__
/* --sample: where the game is, every 5 s (a browser has no debugger to stop in) */
static void *sampler(void *arg) {
    (void)arg;
    for (;;) {
        sleep(5);
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        uint32_t tgt = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
        extern volatile unsigned hle_dbg_yields, hle_dbg_handed, hle_dbg_timeouts, hle_dbg_takes;
        int hle_debug_waiters(void);
        fprintf(stderr, "[sample] in sub_%08X, esp %08X; timeGetTime %u, game's last tick %u (now %u); lock waiters %d, yields %u handed %u timeouts %u takes %u\n",
                g_cur_func, g_esp, tgt, MEM32(0xD5A998), MEM32(0xD5A994), hle_debug_waiters(), hle_dbg_yields, hle_dbg_handed, hle_dbg_timeouts, hle_dbg_takes);
    }
    return NULL;
}
#endif

/* ---- scripted input: --at S:what[:args], for a headless run --------------- */
typedef struct { double at; char what[16]; char arg[260]; int done; } step_t;
static step_t g_steps[64];
static int g_nsteps;
static int step_arg(const char *spec) {
    if (g_nsteps >= 64) return 0;
    step_t *s = &g_steps[g_nsteps];
    char *end;
    s->at = strtod(spec, &end);
    if (*end != ':') return 0;
    const char *w = end + 1, *colon = strchr(w, ':');
    size_t wl = colon ? (size_t)(colon - w) : strlen(w);
    if (wl == 0 || wl >= sizeof s->what) return 0;
    memcpy(s->what, w, wl), s->what[wl] = 0;
    snprintf(s->arg, sizeof s->arg, "%s", colon ? colon + 1 : "");
    g_nsteps++;
    return 1;
}
/* The last frame shown, for --at S:shot:FILE (a PPM) */
static uint8_t *g_shot;
static int g_shot_w, g_shot_h;
static int g_frametrace;
static void on_frame(const uint8_t *px, int w, int h, int pitch, int bpp) {
    const uint8_t *fpx; const uint32_t *pal; int fw, fh, fpitch, fbpp;
    unsigned seq;
    if (!hle_dd_frame(&fpx, &fw, &fh, &fpitch, &fbpp, &pal, &seq)) return;
    if (g_frametrace) {                       /* --frametrace: what the primary holds, every 2 s */
        static uint32_t last;
        uint32_t now = hle_ticks_ms();
        if (now - last >= 2000) {
            last = now;
            unsigned hist[256] = { 0 }, npal = 0;
            if (fbpp == 8) for (int y = 0; y < fh; y += 4) for (int x = 0; x < fw; x += 4) hist[fpx[(size_t)y * fpitch + x]]++;
            if (pal) for (int i = 0; i < 256; i++) if (pal[i] & 0xFFFFFF) npal++;
            int top = 0; for (int i = 1; i < 256; i++) if (hist[i] > hist[top]) top = i;
            unsigned distinct = 0; for (int i = 0; i < 256; i++) if (hist[i]) distinct++;
            fprintf(stderr, "[frame] seq %u %dx%dx%d: %u distinct indices, most %u (%u), palette %s (%u non-black)",
                    seq, fw, fh, fbpp, distinct, top, hist[top], pal ? "set" : "none", npal);
            if (pal) { fprintf(stderr, " entries:"); for (int i = 0; i < 256; i += 32) fprintf(stderr, " %d=%06X", i, pal[i] & 0xFFFFFF); fprintf(stderr, " 255=%06X", pal[255] & 0xFFFFFF); }
            fprintf(stderr, "\n");
        }
    }
    (void)px; (void)pitch; (void)bpp;
    if (g_shot_w != w || g_shot_h != h) { free(g_shot); g_shot = (uint8_t *)malloc((size_t)w * h * 3); g_shot_w = w, g_shot_h = h; }
    if (!g_shot) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = g_shot + ((size_t)y * w + x) * 3;
            if (fbpp == 8 && pal) { uint32_t e = pal[fpx[(size_t)y * fpitch + x]]; o[0] = e & 0xFF, o[1] = e >> 8 & 0xFF, o[2] = e >> 16 & 0xFF; }
            else if (fbpp == 16) { uint16_t v = *(const uint16_t *)(fpx + (size_t)y * fpitch + x * 2); o[0] = (v >> 11) << 3, o[1] = (v >> 5 & 63) << 2, o[2] = (v & 31) << 3; }
            else { const uint8_t *s = fpx + (size_t)y * fpitch + x * 4; o[0] = s[2], o[1] = s[1], o[2] = s[0]; }
        }
}
static void shot(const char *file) {
    if (!g_shot) { fprintf(stderr, "[script] no frame to save yet\n"); return; }
    FILE *f = fopen(file, "wb");
    if (!f) { fprintf(stderr, "[script] cannot write %s\n", file); return; }
    fprintf(f, "P6\n%d %d\n255\n", g_shot_w, g_shot_h);
    fwrite(g_shot, 1, (size_t)g_shot_w * g_shot_h * 3, f);
    fclose(f);
    fprintf(stderr, "[script] %s: %dx%d\n", file, g_shot_w, g_shot_h);
}
static void steps_run(void) {
    static uint32_t t0;
    if (!t0) t0 = hle_ticks_ms();
    double now = (hle_ticks_ms() - t0) / 1000.0;
    for (int i = 0; i < g_nsteps; i++) {
        step_t *s = &g_steps[i];
        if (s->done || now < s->at) continue;
        s->done = 1;
        int x = 0, y = 0;
        if (!strcmp(s->what, "move") && sscanf(s->arg, "%d,%d", &x, &y) == 2) hle_input_mouse(0x0200u, x, y);
        else if (!strcmp(s->what, "click") && sscanf(s->arg, "%d,%d", &x, &y) == 2) {
            hle_input_mouse(0x0200u, x, y); hle_input_mouse(0x0201u, x, y);
        }
        else if (!strcmp(s->what, "release") && sscanf(s->arg, "%d,%d", &x, &y) == 2) hle_input_mouse(0x0202u, x, y);
        else if (!strcmp(s->what, "rclick") && sscanf(s->arg, "%d,%d", &x, &y) == 2) {
            hle_input_mouse(0x0200u, x, y); hle_input_mouse(0x0204u, x, y);
        }
        else if (!strcmp(s->what, "rrelease") && sscanf(s->arg, "%d,%d", &x, &y) == 2) hle_input_mouse(0x0205u, x, y);
        else if (!strcmp(s->what, "keydown")) hle_input_key(1, (uint32_t)strtoul(s->arg, NULL, 0), 0);
        else if (!strcmp(s->what, "keyup")) hle_input_key(0, (uint32_t)strtoul(s->arg, NULL, 0), 0);
        else if (!strcmp(s->what, "key")) { hle_input_key(1, (uint32_t)strtoul(s->arg, NULL, 0), 0); hle_input_key(0, (uint32_t)strtoul(s->arg, NULL, 0), 0); }
        else if (!strcmp(s->what, "shot")) shot(s->arg);
        else if (!strcmp(s->what, "quit")) { fprintf(stderr, "[script] quit\n"); hle_exit(0); }
        else fprintf(stderr, "[script] unknown step %s:%s\n", s->what, s->arg);
        fprintf(stderr, "[script] %.1fs %s %s\n", now, s->what, s->arg);
    }
}

/* ---- --msgtrace: every message the window procedure sees, and the game's run flags ---- */
static int g_msgtrace;
static void msg_hook(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (msg == 0x0113u || msg == 0x000Fu || msg == 0x0200u || msg == 0x0084u || msg == 0x0020u) return;   /* WM_TIMER, WM_PAINT, WM_MOUSEMOVE, WM_NCHITTEST, WM_SETCURSOR */
    fprintf(stderr, "[msg] hwnd %08X msg %04X wp %08X lp %08X\n", hwnd, msg, wp, lp);
}
static void flags_tick(void) {
    static uint32_t last;
    uint32_t now = hle_ticks_ms();
    if (now - last < 1000) return;
    last = now;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t tgt = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
    fprintf(stderr, "[flags] 0xC64B14=%02X game=%08X fps=%u frame=%u 0xC649F4=%08X mode=%08X\n", MEM8(0xC64B14), MEM32(0xC64B2C),
            MEM32(0xDA69AC), MEM32(0x5CD308), MEM32(0xC649F4), MEM32(0xC649E0));
    (void)tgt;
    {   /* the display object (the mode object at 0xC649E0): entries at +0x70, count at +0x14, viewport +0x20.. */
        uint32_t o = MEM32(0xC649E0);
        if (o) {
            uint32_t n = MEM32(o + 0x14), tab = MEM32(o + 0x70);
            fprintf(stderr, "[display] obj %08X count %u table %08X vp %d,%d-%d,%d ofs %d,%d flags34 %08X entries:", o, n, tab,
                    (int)MEM32(o + 0x20), (int)MEM32(o + 0x24), (int)MEM32(o + 0x28), (int)MEM32(o + 0x2C), (int)MEM32(o + 0x48), (int)MEM32(o + 0x4C), MEM32(o + 0x34));
            for (uint32_t i = 0; tab && i < 6 && i < n; i++) {
                uint32_t e = MEM32(tab + 4 * i);
                fprintf(stderr, " [%08X%s%08X]", e, e ? " code " : "", e ? MEM32(e + 8) : 0);
            }
            fprintf(stderr, "\n");
        }
    }
    {   /* the buffer the game renders into (its Lock's lpSurface and lPitch) against the primary the presenter shows */
        uint32_t lp = MEM32(0xC64CDC), pitch = MEM32(0xC65380), surf = MEM32(0xDA8980);
        const uint8_t *fpx; const uint32_t *pal; int fw, fh, fpitch, fbpp;
        int have = hle_dd_frame(&fpx, &fw, &fh, &fpitch, &fbpp, &pal, NULL);
        unsigned hist[256] = { 0 }, distinct = 0;
        if (lp && pitch && pitch < 4096) {
            const uint8_t *g = (const uint8_t *)hle_ptr(lp);
            for (int y = 0; y < 480; y += 4) for (int x = 0; x < 640; x += 4) hist[g[(size_t)y * pitch + x]]++;
            for (int i = 0; i < 256; i++) if (hist[i]) distinct++;
        }
        fprintf(stderr, "[buffers] game lpSurface %08X pitch %u surface obj %08X: %u distinct indices | primary px %08X pitch %d\n",
                lp, pitch, surf, distinct, have ? hle_va(fpx) : 0, have ? fpitch : 0);
    }
}

/* ---- RECOMP_TRACE: every lifted function entry, for a window of the run
 * (--calltrace-from S [--calltrace-max N]), interleaved with the layer's
 * traces on stderr, so one frame's render sequence can be read ---------- */
#ifdef RECOMP_TRACE
static double g_calltrace_from = -1;
static unsigned g_calltrace_max = 200000, g_calltrace_n;
static uint32_t g_trace_t0;
uint32_t g_enter_trace[RECOMP_ENTER_SIZE];
uint32_t g_enter_idx;
/* --listtrace: the display list's allocator (0x0057F3C0: eax the display object, [esp+4] the depth,
 * [esp+8] the entry code) and the present (0x0057CCE0: eax the object), as they are entered */
static int g_listtrace;
static unsigned g_alloc_seen, g_present_seen;
void recomp_trace_enter(uint32_t va) {
    if (va == 0x004182E4u) {                 /* the game's own debug printf (compiled out of the retail build): its format at [esp+4], arguments after */
        static unsigned n;
        if (n++ < 200) {
            char buf[512];
            uint32_t level = MEM32(g_esp + 4), fva = MEM32(g_esp + 8);   /* (level, format, ...) */
            const char *fmt = fva >= 0x401000u && fva < 0x01400000u ? (const char *)hle_ptr(fva) : NULL;
            if (fmt) { hle_guest_format(buf, sizeof buf, fmt, g_esp + 12); size_t l = strlen(buf); fprintf(stderr, "[game %u] %s%s", level, buf, l && buf[l - 1] == '\n' ? "" : "\n"); }
            else fprintf(stderr, "[game %u] (format at %08X)\n", level, fva);
        }
    }
    if (g_listtrace) {
        if (va == 0x0057F3C0u && g_alloc_seen < 40) {
            g_alloc_seen++;
            uint32_t zbits = MEM32(g_esp + 4);
            float z; memcpy(&z, &zbits, 4);
            fprintf(stderr, "[list] alloc obj %08X depth %g (0x%08X) code %08X buckets %u table %08X pool %08X\n", g_eax, (double)z, zbits, MEM32(g_esp + 8),
                    MEM32(g_eax + 0x14), MEM32(g_eax + 0x70), MEM32(0xDA6880));
        }
        if (va == 0x0057CCE0u && g_present_seen < 12) {
            g_present_seen++;
            uint32_t obj = g_eax, n = MEM32(obj + 0x14), tab = MEM32(obj + 0x70), used = 0, first = 0xFFFFFFFFu;
            for (uint32_t i = 0; tab && i < n && i < 4096; i++) if (MEM32(tab + 4 * i)) { used++; if (first == 0xFFFFFFFFu) first = i; }
            fprintf(stderr, "[list] present obj %08X buckets %u table %08X non-empty %u (first %d) flags34 %08X vp %d,%d-%d,%d\n", obj, n, tab, used, (int)first,
                    MEM32(obj + 0x34), (int)MEM32(obj + 0x20), (int)MEM32(obj + 0x24), (int)MEM32(obj + 0x28), (int)MEM32(obj + 0x2C));
            if (first != 0xFFFFFFFFu && g_present_seen <= 3) {   /* the entries of the first bucket: code, flags, vertex count, the vertices */
                uint32_t e = MEM32(tab + 4 * first);
                for (int k = 0; e && k < 6; k++, e = MEM32(e)) {
                    uint32_t nv = MEM32(e + 0x18);
                    fprintf(stderr, "[list]   entry %08X code %08X flags0C %08X nverts %u:", e, MEM32(e + 8), MEM32(e + 0xC), nv);
                    for (uint32_t v = 0; v < nv && v < 4; v++) {
                        float x, y, z; uint32_t bx = MEM32(e + 0x2C + 0x20 * v), by = MEM32(e + 0x30 + 0x20 * v), bz = MEM32(e + 0x34 + 0x20 * v);
                        memcpy(&x, &bx, 4); memcpy(&y, &by, 4); memcpy(&z, &bz, 4);
                        fprintf(stderr, " (%g, %g, %g)", (double)x, (double)y, (double)z);
                    }
                    fprintf(stderr, "\n");
                }
            }
        }
    }
    g_enter_trace[g_enter_idx++ & (RECOMP_ENTER_SIZE - 1)] = va;
    if (g_calltrace_from < 0 || g_calltrace_n >= g_calltrace_max) return;
    if (!g_trace_t0) g_trace_t0 = hle_ticks_ms();
    if ((hle_ticks_ms() - g_trace_t0) / 1000.0 < g_calltrace_from) return;
    g_calltrace_n++;
    fprintf(stderr, "> %08X\n", va);
    if (g_calltrace_n == g_calltrace_max) fprintf(stderr, "> (calltrace: %u entries, stopped)\n", g_calltrace_n);
}
void recomp_dump_trace(const char *why) { (void)why; }
#endif

/* ---- the pump ------------------------------------------------------------- */
static void frame_pump(void) {
    hle_screen_pump();
    if (g_msgtrace) flags_tick();
#ifdef __EMSCRIPTEN__
    web_pump();
#endif
    hotd_frame();
    if (g_nsteps) steps_run();
}

static uint32_t exe_stamp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t head[4096];
    size_t n = fread(head, 1, sizeof head, f);
    fclose(f);
    if (n < 0x40) return 0;
    uint32_t nt = head[0x3C] | head[0x3D] << 8 | head[0x3E] << 16 | (uint32_t)head[0x3F] << 24;
    if (nt + 12 > n) return 0;
    return head[nt + 8] | head[nt + 9] << 8 | head[nt + 10] << 16 | (uint32_t)head[nt + 11] << 24;
}

int main(int argc, char **argv) {
    const char *game = "game";
    int run = 0, mute = 0;
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--guest-args") && i + 1 < argc) g_guest_args = argv[++i];
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) g_fullscreen = 1;
        else if (!strcmp(argv[i], "--window")) g_fullscreen = 0;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_scale = argv[++i];
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--no-gun")) hotd_opt_gun = 0;
        else if (!strcmp(argv[i], "--controls") && i + 1 < argc) hotd_opt_controls = strcmp(argv[++i], "original") != 0;
        else if (!strcmp(argv[i], "--trace")) win32hle_trace = 1;
        else if (!strcmp(argv[i], "--filetrace")) hle_filetrace_on = 1;
        else if (!strcmp(argv[i], "--inputtrace")) g_input_trace = 1;
        else if (!strcmp(argv[i], "--dinput-trace")) g_dinput_trace = 1;
        else if (!strcmp(argv[i], "--mci-trace")) hotd_trace_mci = 1;
        else if (!strcmp(argv[i], "--msgtrace")) g_msgtrace = 1;
        else if (!strcmp(argv[i], "--frametrace")) g_frametrace = 1;
#ifdef RECOMP_TRACE
        else if (!strcmp(argv[i], "--calltrace-from") && i + 1 < argc) g_calltrace_from = strtod(argv[++i], NULL);
        else if (!strcmp(argv[i], "--listtrace")) g_listtrace = 1;
        else if (!strcmp(argv[i], "--calltrace-max") && i + 1 < argc) g_calltrace_max = (unsigned)strtoul(argv[++i], NULL, 0);
#endif
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) { if (!step_arg(argv[++i])) { fprintf(stderr, "bad --at %s\n", argv[i]); return 1; } }
#ifdef __EMSCRIPTEN__
        else if (!strcmp(argv[i], "--sample")) { pthread_t t; pthread_create(&t, NULL, sampler, NULL); pthread_detach(t); }
#endif
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) i++;   /* the Windows host's: here it is the game folder's thotd.exe */
        else {
            printf("usage: hotd [--run] [--headless | --fullscreen | --window] [--scale sharp|smooth|crt|nearest|integer]\n"
                   "            [--game DIR] [--mute] [--no-gun] [--controls original] [--watchdog S]\n"
                   "            [--trace] [--filetrace] [--inputtrace] [--dinput-trace] [--mci-trace]\n"
                   "            [--at S:move:x,y] [--at S:click:x,y] [--at S:rclick:x,y] [--at S:key:VK] [--at S:shot:FILE.ppm] [--at S:quit]\n");
            return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 1;
        }
    }
    char game_full[PATH_MAX], exe_host[PATH_MAX + 16];
#ifdef __EMSCRIPTEN__
    static char web_game[64], web_save[64];
    if (!web_mount(HOTD_EXE_NAME, web_game, sizeof web_game, web_save, sizeof web_save)) return 1;
    game = web_game;
#endif
    if (!realpath(game, game_full)) { fprintf(stderr, "no game folder at %s\n", game); return 1; }
    hle_set_drive('C', game_full);
    hle_set_drive(HOTD_CD_LETTER, game_full);
    {   /* the disc's root has no thotd.exe (hotd.c, the CD) */
        char hide[32];
        snprintf(hide, sizeof hide, "%c:\\" HOTD_EXE_NAME, HOTD_CD_LETTER);
        hle_path_hide(hide);
    }
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "[web] game folder %s, saves in %s\n", game_full, web_save);
    hle_set_overlay(game_full, web_save);
#endif
    if (!hle_host_path("C:\\" HOTD_EXE_NAME, exe_host, sizeof exe_host)) {
        fprintf(stderr, "no " HOTD_EXE_NAME " in %s\n", game_full);
        return 1;
    }
    uint32_t stamp = exe_stamp(exe_host);
    if (stamp != hotd_exe_stamp) {
        fprintf(stderr, "%s is not the build this host was lifted from (PE timestamp 0x%08X, wanted 0x%08X): lift it (run_lift.py) and build again.\n", exe_host, stamp, hotd_exe_stamp);
        return 1;
    }
#ifndef __EMSCRIPTEN__
    {   /* faults report on their own stack: a guest stack overflow is one */
        static uint8_t alt[1 << 16];
        stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt };
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
    }
#endif
    hotd_register_shims();             /* first: the game's own answers win */
    recomp_host_init();
#if defined(__EMSCRIPTEN__)
    printf(HOTD_TITLE " recomp host (WebAssembly)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
#elif defined(__APPLE__)
    printf(HOTD_TITLE " recomp host (native macOS)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
#else
    printf(HOTD_TITLE " recomp host (native Linux)\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);
#endif
    pe_image img;
    if (recomp_pe_map(exe_host, &img) != 0 || img.base != HOTD_IMAGE_BASE) {
        fprintf(stderr, "cannot map %s at 0x%08X\n", exe_host, HOTD_IMAGE_BASE);
        return 1;
    }
    hle_module_add("C:\\THOTD.EXE", img.base, img.resource_rva, img.resource_size, 1);
    int unresolved = recomp_pe_bind(&img, hle_resolve_or_stub);
    printf("  mapped %s: 0x%08X-0x%08X, %d imports not implemented yet\n", exe_host, img.base, img.base + img.span, unresolved);
    /* -cd_nocheck: the game's own switch past its CD check. The EN disc's exe
     * has no string 4004 (the file the check looks for on the disc), so the
     * check could never pass on it; the first CD-ROM drive is still recorded
     * for the music (hotd.c, the CD). */
    {
        /* -d3d / -ddraw pick the game's Direct3D or its 256-colour software
           renderer (the installer's shortcuts pass one or the other); the
           host's --guest-args adds any switches after -cd_nocheck. */
        static char cmd[512];
        snprintf(cmd, sizeof cmd, "\"C:\\THOTD.EXE\" -cd_nocheck%s%s", g_guest_args ? " " : "", g_guest_args ? g_guest_args : "");
        hle_set_command_line(cmd);
    }
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", hotd_entry_va);
        return 0;
    }
    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    if (mute) hle_dsound_set_master(0.0f);
    if (g_scale && !hle_screen_scale(g_scale)) fprintf(stderr, "[host] no scaling \"%s\": sharp, smooth, crt, nearest or integer\n", g_scale);
    hle_screen_bars(0);
    if (hle_screen_open(HOTD_TITLE, g_fullscreen > 0, g_headless) != 0) return 1;
    if (g_msgtrace) hle_proc_hook = msg_hook;
    hle_dinput_mouse_hook = hotd_mouse_motion;
    hle_dialog_hook = hotd_dialog;
    hle_winmm_aux_volume_hook = hotd_music_volume;
    hle_screen_frame_hook = on_frame;
    hle_set_pump_hook(frame_pump);
    if (g_watchdog_s) {
        pthread_t t;
        pthread_create(&t, NULL, watchdog, NULL);
        pthread_detach(t);
    }
    printf("  entering 0x%08X\n\n", hotd_entry_va);
    fflush(stdout);
    uint32_t rc = hle_call_guest(hotd_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", rc);
    hle_exit((int)rc);
    return 0;
}
