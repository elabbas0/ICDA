#include <assert.h>
#include <stdio.h>
#include <string.h>
#define main terminal_app_main
#include "../userspace/terminal.c"
#undef main

static char clipboard[TERM_CLIP_CAP + 1];
static int writes;
uint64_t ic_strlen(const char *s) { return strlen(s); }
char *ic_strcpy(char *d, const char *s, uint64_t cap) { if (cap) snprintf(d, cap, "%s", s); return d; }
uint64_t ic_strlcat(char *d, const char *s, uint64_t cap) { size_t n = strlen(d); if (n < cap) snprintf(d+n, cap-n, "%s", s); return n+strlen(s); }
void ic_uint_to_str(uint64_t v, char *s, uint64_t cap) { snprintf(s, cap, "%lu", (unsigned long)v); }
int ic_notify(const char *app, const char *title, const char *body) { (void)app; (void)title; (void)body; return 0; }
int ic_clipboard_set(const char *s, uint64_t n) {
    assert(n <= TERM_CLIP_CAP);
    memcpy(clipboard, s, n); clipboard[n] = 0; writes++;
    return 0;
}
static void reset(int cols) {
    memset(&term, 0, sizeof(term));
    term.cols = cols; term.rows = 3;
    term_reset_screen();
}
int main(void) {
    reset(20);
    feed_str("\033]52;c;aHR0cHM6Ly9leGFtcGxlLmNvbS8=\a");
    assert(writes == 1 && !strcmp(clipboard, "https://example.com/"));
    feed_str("\033]52;;Y2xpcA==\033\\");
    assert(writes == 2 && !strcmp(clipboard, "clip"));
    const char *bad[] = {"\033]52;c;?\a", "\033]52;c;YQ=\a", "\033]52;c;Y===\a",
                         "\033]52;c;YQ==YQ==\a", "\033]52;c;YQ$=\a", "\033]52;c;AA==\a",
                         "\033]52;p;YQ==\a", "\033]0;title\a"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) feed_str(bad[i]);
    assert(writes == 2 && !strcmp(clipboard, "clip"));
    feed_str("\033]52;c;");
    for (int i = 0; i < TERM_OSC_CAP + 20; i++) feed('A');
    feed('\a');
    assert(writes == 2 && term.state == PS_NORMAL);
    feed_str("\033]52;c;\a");
    assert(writes == 3 && !clipboard[0]);
    reset(10);
    feed_str("hello\r\nworld");
    term.selection_anchor = 0; term.selection_end = 15;
    assert(copy_selection() == 1 && !strcmp(clipboard, "hello\nworld"));
    term.selection_anchor = 15; term.selection_end = 0;
    assert(copy_selection() == 1 && !strcmp(clipboard, "hello\nworld"));
    reset(10);
    feed_str("https://example.com/");
    term.selection_anchor = 0; term.selection_end = 20;
    assert(copy_selection() == 1 && !strcmp(clipboard, "https://example.com/"));
    reset(20);
    feed_str("caf\303\251");
    term.selection_anchor = 0; term.selection_end = 4;
    assert(copy_selection() == 1 && !strcmp(clipboard, "caf\303\251"));
    term.selection_end = term.selection_anchor;
    int before = writes;
    assert(copy_selection() == 0 && writes == before);
    puts("Terminal clipboard: OSC 52 BEL/ST, malformed/oversized/query rejection, text selection, reverse selection, wrapped URL and UTF-8 passed");
}
