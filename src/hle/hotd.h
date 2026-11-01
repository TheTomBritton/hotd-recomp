/*
 * hotd.h - what is The House of the Dead's own on the portable host
 * (src/hle): the virtual CD, Sega's light-gun DLL, CD audio over MCI, and
 * the game's control settings. main.c runs the host; hotd.c answers these.
 */
#ifndef HOTD_HLE_H
#define HOTD_HLE_H
#include <stdint.h>

#define HOTD_IMAGE_BASE 0x00400000u
/* The build the lift is of is hotd_exe_stamp in recomp_dispatch.c (run_lift.py BUILDS). */
#define HOTD_EXE_NAME   "thotd.exe"
#define HOTD_CD_LETTER  'E'                  /* the virtual disc: the game folder, without thotd.exe at its root */
#define HOTD_TITLE      "The House of the Dead"

/* Before recomp_host_init: the game's own answers, which win over the layer's. */
void hotd_register_shims(void);
/* Options from the command line (main.c). */
extern int hotd_opt_gun;          /* 1: gamebios.dll finds a light gun (the pointer aims it); 0: no gun, the mouse */
extern int hotd_opt_controls;     /* 1: the control settings are made light gun 1 / light gun 2 every frame */
extern int hotd_trace_mci;        /* print every MCI command string */
/* Once a frame, from the pump: the control settings, the music. */
void hotd_frame(void);
/* The DirectInput mouse: with the guns on, the game's own cursors go under
 * the pointer and no motion is reported (hle_dinput_mouse_hook). */
void hotd_mouse_motion(int x, int y, int *dx, int *dy);
/* CD audio: the host side the MCI shim calls. Tracks are the disc's numbers
 * (2 is the first music track). A web page or an SDL mixer answers these. */
void hotd_music_play(int first, int last);
void hotd_music_stop(void);
int  hotd_music_playing(void);                /* 1 while a track plays */
int  hotd_music_current(void);                /* the track playing, or the last asked */
int  hotd_music_tracks(void);                 /* number of tracks on the disc (data track included) */
void hotd_music_volume(uint32_t aux);         /* auxSetVolume: left low word, right high (0xFFFF full) */

void hotd_dialog(uint32_t hwnd, uint32_t resource_id);   /* answers the Direct3D driver-select dialog */

#endif
