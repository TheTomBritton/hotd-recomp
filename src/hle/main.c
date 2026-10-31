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
extern const uint32_t hotd_setjmp_resume[][2];    /* recomp_dispatch.c: (function VA, resume VA) */
extern const uint32_t recomp_dispatch_count;
extern int win32hle_trace;
extern int hle_filetrace_on;
extern int g_input_trace;

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
void hotd_longjmp(void) {
    uint32_t jb = g_eax, val = g_edx ? g_edx : 1;
    uint32_t esp = MEM32(jb + 0x1C);
    recomp_ehframe_t *f = recomp_eh_owner(esp);
    uint32_t resume = 0;
    for (int i = 0; f && hotd_setjmp_resume[i][0]; i++)
        if (hotd_setjmp_resume[i][0] == f->func) resume = hotd_setjmp_resume[i][1];
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
        fprintf(stderr, "[sample] in sub_%08X, esp %08X\n", g_cur_func, g_esp);
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
static void on_frame(const uint8_t *px, int w, int h, int pitch, int bpp) {
    const uint8_t *fpx; const uint32_t *pal; int fw, fh, fpitch, fbpp;
    if (!hle_dd_frame(&fpx, &fw, &fh, &fpitch, &fbpp, &pal, NULL)) return;
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

/* ---- the pump ------------------------------------------------------------- */
static void frame_pump(void) {
    hle_screen_pump();
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
    if (stamp != HOTD_EXE_STAMP) {
        fprintf(stderr, "%s is not the build this host was lifted from (PE timestamp 0x%08X, wanted 0x%08X).\n", exe_host, stamp, HOTD_EXE_STAMP);
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
    hle_set_command_line("\"C:\\THOTD.EXE\"");
    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", hotd_entry_va);
        return 0;
    }
    if (chdir(game_full) != 0) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    if (mute) hle_dsound_set_master(0.0f);
    if (g_scale && !hle_screen_scale(g_scale)) fprintf(stderr, "[host] no scaling \"%s\": sharp, smooth, crt, nearest or integer\n", g_scale);
    hle_screen_bars(0);
    if (hle_screen_open(HOTD_TITLE, g_fullscreen > 0, g_headless) != 0) return 1;
    hle_dinput_mouse_hook = hotd_mouse_motion;
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
