/*
 * eh_setjmp.c - the guest's setjmp/longjmp on a win32hle host.
 *
 * pcrecomp's eh32.c (runtime/native32) carries this with MSVC's C++
 * exception handling, which a Windows host needs and this one does not: it
 * includes <windows.h> and is compiled into native32.c. The four functions
 * the lift's landing pads use (RECOMP_EH_ENTER in recomp_types.h) are plain
 * C, so they live here for the Linux, macOS and browser hosts.
 *
 * The guest's setjmp saves its caller's registers and esp, and longjmp puts
 * them back and returns from setjmp a second time: in a lifted program that
 * is a jump into the middle of a body further up the host stack. The caller
 * of setjmp gets a landing pad (a label after the call), and the host's
 * longjmp (hotd_longjmp, main.c) finds it here: the newest frame entered
 * above the saved esp is the one that called setjmp. The game's task
 * scheduler (sub_0043E354) runs every task this way.
 *
 * From pcrecomp runtime/native32/eh32.c (MIT, sp00nznet).
 */
#include <stddef.h>
#include <setjmp.h>
#include "recomp_types.h"

static __thread recomp_ehframe_t *t_frames;   /* newest first */

void recomp_eh_push(recomp_ehframe_t *f) { f->prev = t_frames; t_frames = f; }

void recomp_eh_pop(recomp_ehframe_t *f) {
    /* A longjmp past this frame already dropped it. */
    for (recomp_ehframe_t *p = t_frames; p; p = p->prev)
        if (p == f) { t_frames = f->prev; return; }
}

recomp_ehframe_t *recomp_eh_owner(uint32_t esp) {
    for (recomp_ehframe_t *f = t_frames; f; f = f->prev)
        if (f->entry_esp > esp) return f;
    return NULL;
}

void recomp_eh_resume(recomp_ehframe_t *f, uint32_t resume) {
    t_frames = f;
    f->resume = resume;
    g_cur_func = f->func;
    longjmp(f->jb, 1);
}
