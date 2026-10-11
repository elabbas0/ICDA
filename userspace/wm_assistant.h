#ifndef USERSPACE_WM_ASSISTANT_H
#define USERSPACE_WM_ASSISTANT_H

#include "libicda.h"

/* The assistant overlay (wm_assistant.c): a glowing wave along the bottom
 * of the screen while it listens, thinks and answers, and a card above it
 * with the question and the streamed answer.  The answers come from
 * /sbin/assistantd (local or cloud models); the two talk through
 * /tmp/ai/request and /tmp/ai/reply. */

#define WM_AI_KEEP    0
#define WM_AI_REDRAW  1
#define WM_AI_CLOSE   2

void      wm_ai_init(void);                 /* starts the daemon */
int       wm_ai_is_open(void);
void      wm_ai_open(void);
void      wm_ai_close(void);
ic_rect_t wm_ai_rect(int sw, int sh);       /* what the overlay covers */
int       wm_ai_tick(void);                 /* REDRAW while it animates or the answer grows */
void      wm_ai_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len);
int       wm_ai_key(int key);
int       wm_ai_click(int sw, int sh, int mx, int my);

#endif
