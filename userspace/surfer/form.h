#ifndef SURFER_FORM_H
#define SURFER_FORM_H

#include <stddef.h>
#include <stdint.h>
#include "dom.h"

/* Live state of form controls.  The DOM keeps the markup's initial values;
 * what the user (or a script) changes lives here, created on first use. */
typedef struct form_ctl {
    char    *value;          /* UTF-8, NUL terminated */
    size_t   len, cap;
    size_t   cursor;         /* byte offset of the caret */
    size_t   anchor;         /* selection anchor; == cursor when nothing is selected */
    uint8_t  checked;
    int      selected;       /* <select>: index of the chosen <option>, -1 if none */
    float    scroll_x;       /* horizontal scroll of a single-line field, CSS px */
} form_ctl_t;

enum {
    FK_NONE = 0, FK_TEXT, FK_PASSWORD, FK_TEXTAREA, FK_CHECKBOX, FK_RADIO, FK_SELECT,
    FK_SUBMIT, FK_IMAGE, FK_RESET, FK_BUTTON, FK_HIDDEN, FK_FILE
};

int          form_kind(const dom_node_t *n);
int          form_is_text(const dom_node_t *n);      /* typed into (text, password, textarea) */
int          form_is_focusable(const dom_node_t *n);
int          form_disabled(const dom_node_t *n);
form_ctl_t  *form_ctl(dom_node_t *n);                /* creates the state on first use */
const char  *form_value(dom_node_t *n);

/* Editing (text kinds).  Return 1 if the value changed. */
int  form_insert(dom_node_t *n, const char *text, size_t len);
int  form_backspace(dom_node_t *n);
int  form_delete(dom_node_t *n);
void form_move(dom_node_t *n, int delta, int extend);  /* by characters */
void form_home_end(dom_node_t *n, int end, int extend);
void form_select_all(dom_node_t *n);
int  form_set_value(dom_node_t *n, const char *value);
/* Copies the selected text (or everything) into out; returns its length. */
size_t form_selection(dom_node_t *n, char *out, size_t cap);

/* Checkboxes, radios and selects. */
void form_toggle(dom_node_t *doc_root, dom_node_t *n);
int  form_option_count(dom_node_t *select);
dom_node_t *form_option(dom_node_t *select, int index);
void form_choose(dom_node_t *select, int index);
size_t form_option_label(dom_node_t *option, char *out, size_t cap);

/* Submission.  The form that owns a control (form= attribute or ancestor). */
dom_node_t *form_owner(dom_node_t *doc_root, dom_node_t *n);
/* Builds the request for submitting form via submitter (may be 0).
 * url gets the target (with the query for GET); *body is malloc'd for POST
 * (application/x-www-form-urlencoded) and 0 for GET.  Returns 0 on success. */
int  form_submission(dom_doc_t *doc, dom_node_t *form, dom_node_t *submitter,
                     char *url, size_t url_cap, int *is_post, char **body, size_t *body_len);
void form_reset(dom_node_t *doc_root, dom_node_t *form);

/* Frees all control state under root (call before dom_free). */
void form_release(dom_node_t *root);

#endif
