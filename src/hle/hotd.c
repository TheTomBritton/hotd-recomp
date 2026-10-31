/*
 * hotd.c - The House of the Dead's own answers on the portable host.
 *
 * The CD. The game looks for its disc by drive type (GetDriveTypeA) and
 * takes the first CD-ROM drive whose root has no thotd.exe (the retail
 * disc's layout; the drive that has one is the hard disk install). The
 * virtual disc is the game folder on HOTD_CD_LETTER, and the path layer
 * hides thotd.exe on it (hle_path_hide, main.c). Real drives are not shown.
 *
 * The light gun. The PC port imports four cdecl calls from Sega's
 * gamebios.dll, the arcade gun's driver: gbInitDeviceDriver answers 1 (a
 * gun is there; -1 is the driver's "none found"), gblSetScreenMode 1, and
 * the two polls give the pointer: gblGetCursorPosition in picture pixels
 * (640x480), 0x3FF for both when the gun points off the screen, which is how
 * the cabinet reloads, and gblGetButtonStatus bit 0 for the trigger. The
 * right button is a reload: off screen with the trigger pulled. Player 2's
 * gun points off screen with no trigger until a second pointer arrives.
 *
 * The control settings. The game keeps each player's device (0 mouse, 1-2
 * keyboard, 3-4 joystick, 5-6 light gun 1-2) at DEV_P1 and DEV_P2, and
 * whether the driver found guns at GUNS_PRESENT; with the gun on, the pump
 * sets light gun 1 and 2 every frame, so the game aims where the pointer
 * is, as on the cabinet, and no Options screen has to be visited first.
 * --controls original leaves the game's own choice.
 *
 * The mouse. The game turns DirectInput motion into two cursors of its own,
 * the input layer's sum at MOUSE_SUM and the menus' hand at MENU_HAND; with
 * the gun on, both are put under the pointer and no motion is reported, so
 * the hand follows the pointer exactly. (Addresses: the 1998 build, PE
 * stamp 0x354411D6, the only one run_lift.py lifts.)
 *
 * CD audio. The music is the disc's audio tracks, played through MCI
 * command strings ("play cdaudio from ... to ..."), which winmm.c answers
 * "no device". Here they are parsed and handed to hotd_music_*: on the web
 * the page plays the tracks (converted from the disc), natively nothing
 * plays yet; the status answers keep the game's loop happy either way.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "win32hle.h"
#include "hotd.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

int hotd_opt_gun = 1, hotd_opt_controls = 1, hotd_trace_mci;

#define GUNS_PRESENT 0x005CD354u
#define DEV_P1       0x00D5B200u
#define DEV_P2       0x00D5B204u
#define MOUSE_SUM_X  0x012FDB90u
#define MOUSE_SUM_Y  0x012FDB94u
#define MENU_HAND_X  0x00D5A97Cu
#define MENU_HAND_Y  0x00D5A980u
#define GUN_OFFSCREEN 0x3FFu
#define PIC_W 640
#define PIC_H 480

/* ---- the CD drive ---------------------------------------------------------- */
static void h_GetDriveTypeA(void) {
    const char *r = ASTR(0);
    if (!r || !*r) RET(3u, 1);                                 /* no root given: the current drive, fixed */
    if ((r[0] & ~0x20) == HOTD_CD_LETTER) RET(5u, 1);          /* DRIVE_CDROM */
    char path[16], host[1024];
    snprintf(path, sizeof path, "%c:\\", r[0]);
    RET(hle_host_path(path, host, sizeof host) && host[0] ? 3u : 1u, 1);   /* DRIVE_FIXED or DRIVE_NO_ROOT_DIR */
}

/* ---- gamebios.dll: the light gun (cdecl: the caller pops the arguments) ---- */
static int g_guns_seen;
static void gun_position(int player, uint32_t *x, uint32_t *y, int *trigger) {
    *x = *y = GUN_OFFSCREEN;
    *trigger = 0;
    if (player != 0) return;                                   /* player 2: no gun yet */
    int cx, cy;
    hle_cursor_get(&cx, &cy);
    int left = hle_key_down(0x01), right = hle_key_down(0x02);
    *trigger = left || right;
    if (right) return;                                         /* reload: fired off the screen */
    if (cx < 0 || cy < 0 || cx >= PIC_W || cy >= PIC_H) return;
    *x = (uint32_t)cx, *y = (uint32_t)cy;
}
static void gb_InitDeviceDriver(void) {
    fprintf(stderr, "[gun] gbInitDeviceDriver -> %s\n", hotd_opt_gun ? "a light gun (the pointer)" : "no gun");
    g_guns_seen = 1;
    g_eax = hotd_opt_gun ? 1u : 0xFFFFFFFFu;
    g_esp += 4;
}
static void gb_SetScreenMode(void) {
    fprintf(stderr, "[gun] gblSetScreenMode(%u)\n", A32(0));
    g_eax = 1;
    g_esp += 4;
}
static void gb_GetButtonStatus(void) {
    uint32_t x, y;
    int trig;
    gun_position((int)A32(0), &x, &y, &trig);
    g_eax = trig ? 1u : 0u;
    g_esp += 4;
}
static void gb_GetCursorPosition(void) {
    uint32_t x, y, px = A32(1), py = A32(2);
    int trig;
    gun_position((int)A32(0), &x, &y, &trig);
    if (px) MEM32(px) = x;
    if (py) MEM32(py) = y;
    g_eax = 1;
    g_esp += 4;
}

/* ---- the control settings and the mouse ------------------------------------ */
void hotd_frame(void) {
    if (hotd_opt_gun && hotd_opt_controls && MEM32(GUNS_PRESENT)) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[gun] control settings were %u / %u; light gun 1 / light gun 2 from here\n", MEM32(DEV_P1), MEM32(DEV_P2));
        }
        if (MEM32(DEV_P1) != 5) MEM32(DEV_P1) = 5;
        if (MEM32(DEV_P2) != 6) MEM32(DEV_P2) = 6;
    }
}
void hotd_mouse_motion(int x, int y, int *dx, int *dy) {
    if (!(hotd_opt_gun && hotd_opt_controls)) return;          /* the mouse as the game knows it */
    MEM32(MOUSE_SUM_X) = (uint32_t)x, MEM32(MOUSE_SUM_Y) = (uint32_t)y;
    MEM32(MENU_HAND_X) = (uint32_t)x, MEM32(MENU_HAND_Y) = (uint32_t)y;
    *dx = *dy = 0;
}

/* ---- CD audio over MCI ------------------------------------------------------ */
static int g_music_first, g_music_last, g_music_on, g_music_cur = 2;
static int g_tracks = 12;                                      /* the disc: a data track and the music */
static uint32_t g_music_t0;

#ifdef __EMSCRIPTEN__
/* The page plays the tracks (Module['hotdMusic'](op, first, last)): 1 play,
 * 0 stop. It runs on the page's thread, which owns the <audio> elements. */
static void page_music(int op, int first, int last) {
    MAIN_THREAD_ASYNC_EM_ASM({ if (Module['hotdMusic']) Module['hotdMusic']($0, $1, $2); }, op, first, last);
}
#endif
void hotd_music_play(int first, int last) {
    if (first < 1) first = g_music_cur;
    if (last < first) last = first;
    g_music_first = first, g_music_last = last, g_music_cur = first, g_music_on = 1, g_music_t0 = hle_ticks_ms();
    fprintf(stderr, "[cd] play tracks %d..%d\n", first, last);
#ifdef __EMSCRIPTEN__
    page_music(1, first, last);
#endif
}
void hotd_music_stop(void) {
    if (g_music_on) fprintf(stderr, "[cd] stop\n");
    g_music_on = 0;
#ifdef __EMSCRIPTEN__
    page_music(0, 0, 0);
#endif
}
int hotd_music_playing(void) { return g_music_on; }
int hotd_music_current(void) { return g_music_cur; }
int hotd_music_tracks(void) { return g_tracks; }
void hotd_music_set_tracks(int n) { if (n > 1) g_tracks = n; }

/* The answers MCI gives in TMSF: "tt:mm:ss:ff", bytes little-endian as the
 * command strings print them. A track's length is 3 minutes here: the game
 * only needs them to be non-zero and the position to move. */
static int word(const char **p, char *out, size_t n) {
    while (**p == ' ' || **p == '\t') (*p)++;
    size_t i = 0;
    if (**p == '"') { (*p)++; while (**p && **p != '"') { if (i + 1 < n) out[i++] = **p; (*p)++; } if (**p) (*p)++; }
    else while (**p && **p != ' ' && **p != '\t') { if (i + 1 < n) out[i++] = **p; (*p)++; }
    out[i] = 0;
    return i > 0;
}
static int track_of(const char *s) {                           /* "tt" or "tt:mm:ss:ff" or a plain number */
    return atoi(s);
}
static uint32_t mci(const char *cmd, char *ret, uint32_t n) {
    char verb[32], dev[64], w[64];
    const char *p = cmd;
    if (ret && n) ret[0] = 0;
    if (!word(&p, verb, sizeof verb)) return 0;
    word(&p, dev, sizeof dev);                                 /* "cdaudio" or the alias */
    for (char *c = verb; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!strcmp(verb, "open") || !strcmp(verb, "close") || !strcmp(verb, "set") || !strcmp(verb, "seek")
        || !strcmp(verb, "pause") || !strcmp(verb, "resume") || !strcmp(verb, "capability") || !strcmp(verb, "info") || !strcmp(verb, "sysinfo")) {
        if (!strcmp(verb, "capability") && ret && n) snprintf(ret, n, "true");
        if (!strcmp(verb, "sysinfo") && ret && n) snprintf(ret, n, "1");
        if (!strcmp(verb, "info") && ret && n) snprintf(ret, n, "CD Audio");
        return 0;
    }
    if (!strcmp(verb, "play")) {
        int first = 0, last = 0;
        while (word(&p, w, sizeof w)) {
            if (!strcasecmp(w, "from") && word(&p, w, sizeof w)) first = track_of(w);
            else if (!strcasecmp(w, "to") && word(&p, w, sizeof w)) last = track_of(w);
        }
        hotd_music_play(first, last ? last - (last > first) : 0);   /* "to" is the position the play stops at: the next track's start */
        return 0;
    }
    if (!strcmp(verb, "stop")) { hotd_music_stop(); return 0; }
    if (!strcmp(verb, "status")) {
        char what[128] = "";
        while (word(&p, w, sizeof w)) {
            if (!strcasecmp(w, "wait") || !strcasecmp(w, "notify")) continue;
            if (*what) strncat(what, " ", sizeof what - strlen(what) - 1);
            strncat(what, w, sizeof what - strlen(what) - 1);
        }
        if (!ret || !n) return 0;
        if (!strcasecmp(what, "mode")) snprintf(ret, n, g_music_on ? "playing" : "stopped");
        else if (!strcasecmp(what, "number of tracks")) snprintf(ret, n, "%d", g_tracks);
        else if (!strcasecmp(what, "current track")) snprintf(ret, n, "%d", g_music_cur);
        else if (!strcasecmp(what, "media present")) snprintf(ret, n, "true");
        else if (!strcasecmp(what, "ready")) snprintf(ret, n, "true");
        else if (!strcasecmp(what, "position")) {
            uint32_t ms = g_music_on ? hle_ticks_ms() - g_music_t0 : 0, s = ms / 1000;
            snprintf(ret, n, "%02d:%02u:%02u:%02u", g_music_cur, (s / 60) % 100, s % 60, (ms % 1000) * 75 / 1000);
        }
        else if (!strncasecmp(what, "length", 6)) snprintf(ret, n, "03:00:00");
        else if (!strncasecmp(what, "position track", 14)) snprintf(ret, n, "%02d:00:00:00", atoi(what + 14));
        else if (!strncasecmp(what, "type track", 10)) snprintf(ret, n, atoi(what + 10) == 1 ? "other" : "audio");
        else snprintf(ret, n, "0");
        return 0;
    }
    return 0;                                                  /* anything else: done, nothing to say */
}
static void h_mciSendStringA(void) {                           /* (lpszCommand, lpszReturn, cchReturn, hwndCallback) */
    const char *cmd = ASTR(0);
    char *ret = ASTR(1);
    uint32_t n = A32(2);
    static int shown;
    uint32_t r = cmd ? mci(cmd, ret, n) : 0x101u;              /* MCIERR_INVALID_DEVICE_ID */
    if (hotd_trace_mci || shown < 40) {
        shown++;
        fprintf(stderr, "[mci] \"%s\" -> %u%s%s\n", cmd ? cmd : "(null)", r, ret && n && ret[0] ? ": " : "", ret && n ? ret : "");
    }
    RET(r, 4);
}
static void h_mciGetErrorStringA(void) { RET(0, 3); }

static const win32hle_shim g_hotd_shims[] = {
    { "GetDriveTypeA",         h_GetDriveTypeA },
    { "gbInitDeviceDriver",    gb_InitDeviceDriver },
    { "gblSetScreenMode",      gb_SetScreenMode },
    { "gblGetButtonStatus",    gb_GetButtonStatus },
    { "gblGetCursorPosition",  gb_GetCursorPosition },
    { "mciSendStringA",        h_mciSendStringA },
    { "mciGetErrorStringA",    h_mciGetErrorStringA },
    { 0, 0 } };

void hotd_register_shims(void) { win32hle_register(g_hotd_shims); }
