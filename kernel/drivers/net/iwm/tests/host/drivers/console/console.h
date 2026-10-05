/* Host stub */
typedef enum { CONSOLE_STYLE_INFO, CONSOLE_STYLE_OK, CONSOLE_STYLE_MUTED,
    CONSOLE_STYLE_ACCENT, CONSOLE_STYLE_WARN, CONSOLE_STYLE_ERROR } console_style_t;
void console_write(const char *str, console_style_t style);
