#ifndef USERSPACE_IC_DIALOG_H
#define USERSPACE_IC_DIALOG_H

/* Dialogs the window manager shows for any program (/dev/dialog): an
 * error, a warning, a question with its answers.  On a program's window the
 * dialog sits over it and the window takes no input until it is answered;
 * the program itself keeps running, and drawing, meanwhile.  A program with
 * no window (a command-line tool) gets one in the middle of the screen.
 *
 * Programs built on ic_app use ic_app_dialog (ic_app.h), which calls back
 * with the answer; others ask with ic_dialog_open and look with
 * ic_dialog_result, or wait with ic_dialog_run. */

typedef enum {
    IC_DIALOG_INFO = 0,
    IC_DIALOG_WARNING,
    IC_DIALOG_ERROR,
    IC_DIALOG_QUESTION
} ic_dialog_kind_t;

typedef struct {
    ic_dialog_kind_t   kind;
    const char        *title;
    const char        *message;            /* may hold newlines; 0: none */
    const char *const *buttons;            /* up to 4, in order; 0: "OK" */
    int                count;
    int                default_button;     /* Enter's: 0 the first */
    int                cancel_button;      /* Escape's, and a closed window's: -1 the last */
    int                destructive_button; /* drawn as dangerous: -1 none */
} ic_dialog_t;

#define IC_DIALOG_OPEN    (-2)   /* not answered yet */
#define IC_DIALOG_NO_HOST (-3)   /* no window manager to show it (text mode) */

/* Asks for it: 0, or -1 when it cannot be asked (this program has one open) */
int  ic_dialog_open(const ic_dialog_t *d);

/* The answer: the button's index, -1 when it went without one, or
 * IC_DIALOG_OPEN / IC_DIALOG_NO_HOST.  An answer is given once. */
int  ic_dialog_result(void);

/* takes an open request back */
void ic_dialog_cancel(void);

/* asks and waits for the answer (IC_DIALOG_NO_HOST at once without a
 * window manager) */
int  ic_dialog_run(const ic_dialog_t *d);

/* the usual one: a message and OK */
int  ic_dialog_message(ic_dialog_kind_t kind, const char *title, const char *message);

#endif
