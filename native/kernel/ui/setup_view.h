/* setup_view.h -- puts the setup wizard's view (svc/setup_wizard.h) on
 * Allen's window (ui/allen_ui.h): his words when they change (he speaks them
 * word by word), the panel of lines and choices, and the line being typed in
 * the bar. Shared by the kernel (x86/kmain_x86.c) and the host test so both
 * show exactly the same thing. */
#ifndef AIENOS_SETUP_VIEW_H
#define AIENOS_SETUP_VIEW_H
#include "../svc/setup_wizard.h"
#include "allen_ui.h"

/* *said: the view's said_gen last spoken (start it at ~0u). done: the panel goes. */
static inline void ck_setup_show(const sw_view *v, unsigned *said, int done, uint64_t now)
{
    if (done) {
        ck_allen_ui_panel(0, now);
        ck_allen_ui_input("", 0, now);
        return;
    }
    if (v->said_gen != *said) {
        *said = v->said_gen;
        ck_allen_ui_said(0, v->say, now);
    }
    ck_allen_ui_panel_view p = {{0}, 0, {0}, 0, v->lit, v->hint, v->aside, v->prompt};
    for (unsigned i = 0; i < v->nline && i < CK_ALLEN_UI_PANEL_LINES; i++)
        p.line[p.nline++] = v->line[i];
    for (unsigned i = 0; i < v->nchoice && i < CK_ALLEN_UI_CHOICES; i++)
        p.choice[p.nchoice++] = v->choice[i];
    ck_allen_ui_panel(&p, now);
    ck_allen_ui_input(v->typed, v->ntyped, now);
}
#endif
