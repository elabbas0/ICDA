#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

/* Boot splash: replaces the raw "[boot] probing ..." console dump with a
 * clean loading screen (wordmark + progress bar).  While active, console
 * text is hidden from the framebuffer but still mirrored to the serial
 * log, so the full boot log stays available for debugging.
 *
 * Call splash_init() once the framebuffer console exists (still on the
 * physical mapping - the splash is drawn before vmm_init like the rest
 * of the early boot console), splash_progress() from bootstage_set, and
 * splash_finish() right before the shell/WM starts so the text console
 * (and VT switching) works normally afterwards. */

void splash_init(void);
void splash_progress(uint32_t stage, const char *label);
void splash_finish(void);
int  splash_active(void);

#endif
