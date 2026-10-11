#ifndef USERSPACE_WM_ASSISTANT_H
#define USERSPACE_WM_ASSISTANT_H

#include "libicda.h"

/* The assistant overlay (wm_assistant.c): a glowing wave along the bottom
 * of the screen while it listens, thinks and answers, and a card above it
 * with the question and the streamed answer.  The answers come from
 * /sbin/assistantd (local or cloud models); the two talk through
 * /tmp/ai/request and /tmp/ai/reply. */

#define WM_AI_KEEP        0
#define WM_AI_REDRAW      1   /* only the wave moved: repaint the wave band */
#define WM_AI_REDRAW_ALL  2   /* the card or the fade moved: repaint all of it */
#define WM_AI_CLOSE       3

void      wm_ai_init(void);                 /* starts the daemon */
int       wm_ai_is_open(void);
void      wm_ai_open(void);
void      wm_ai_close(void);
ic_rect_t wm_ai_rect(int sw, int sh);       /* what the overlay covers */
ic_rect_t wm_ai_wave_rect(int sw, int sh);  /* just the band the wave moves in */
ic_rect_t wm_ai_card_rect(int sw, int sh);  /* the card, which clears the band */
int       wm_ai_card_visible(void);         /* it waits for the wave before it shows */
float     wm_ai_card_opacity(void);
float     wm_ai_opacity(void);              /* 0..1 while it fades in or out */
int       wm_ai_tick(void);                 /* KEEP / REDRAW / REDRAW_ALL / CLOSE */
void      wm_ai_draw_wave(ic_canvas_t *c, int sw, int sh);
void      wm_ai_draw_card(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len);
int       wm_ai_key(int key);
int       wm_ai_click(int sw, int sh, int mx, int my);

#endif
