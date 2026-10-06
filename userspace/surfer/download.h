#ifndef SURFER_DOWNLOAD_H
#define SURFER_DOWNLOAD_H

#include "libicda.h"
#include "http.h"

/* Downloads: a response Surfer does not show (or a link with download=) is
 * saved to a folder the user picks.  The transfer keeps running while the
 * save sheet is open; bytes go to the file as they arrive. */

/* 1 if this response should be saved rather than displayed */
int  dl_wanted(const http_req_t *r);

/* Takes over r (finished or still receiving) for url. */
void dl_begin(http_req_t *r, const char *url);
/* Starts a fresh request (link with a download attribute). */
void dl_begin_url(const char *url, const char *name);

int  dl_tick(void);                    /* 1 if something visible changed */
int  dl_busy(void);                    /* transfers running: keep ticking */
int  dl_sheet_open(void);              /* the save sheet takes input */
void dl_draw(ic_app_t *app, ic_canvas_t *c);
int  dl_event(ic_app_t *app, const ic_event_t *ev);   /* 1 if used */
/* status bar text for downloads, or 0 */
const char *dl_status(void);

#endif
