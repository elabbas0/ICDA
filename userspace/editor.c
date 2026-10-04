#include "libicda.h"

#define WIN_W 860
#define WIN_H 560

#define EDIT_BUF_CAP    65536
#define EDIT_PATH_CAP   256
#define EDIT_STATUS_CAP 128
#define EDIT_GUTTER_W   46
#define EDIT_TAB        4
#define EDIT_SIDEBAR_W  210
#define EDIT_STATUS_H   24
#define TREE_ROW_H      22
#define TREE_MAX        400
#define LINE_CLS_CAP    1024

enum { LANG_TEXT = 0, LANG_C, LANG_JS, LANG_PY, LANG_SH, LANG_MAKE, LANG_ASM, LANG_JSON, LANG_MD };
enum { PROMPT_NONE = 0, PROMPT_FOLDER, PROMPT_FILE };
enum { TB_SIDEBAR = 0, TB_OPEN_FOLDER, TB_OPEN_FILE, TB_NEW, TB_SAVE, TB_COUNT };

typedef struct {
    char    name[64];
    char    path[EDIT_PATH_CAP];
    uint8_t depth;
    uint8_t is_dir;
    uint8_t expanded;
} tree_node_t;

static struct {
    char     path[EDIT_PATH_CAP];
    char     buf[EDIT_BUF_CAP];
    uint64_t len;
    uint64_t cursor;
    int      modified;
    int      scroll_row;
    int      scroll_col;
    uint64_t followed;
    uint64_t anchor;
    int      has_sel;
    int      lang;

    int rows;
    int cols;
    int text_x, text_y, text_w, text_h;
    int ch, cw;

    int  sidebar;
    char root[EDIT_PATH_CAP];
    tree_node_t tree[TREE_MAX];
    int  tree_count;
    int  tree_scroll;
    int  tree_hover;

    int  hover_tb;
    char pending_open[EDIT_PATH_CAP];
    int  confirm_new;

    int  prompt;
    char prompt_buf[EDIT_PATH_CAP];

    char status[EDIT_STATUS_CAP];
} ed;

static const char *const TB_LABELS[TB_COUNT] = { 0, "Open Folder", "Open File", "New", "Save" };

static void ed_status(const char *text) {
    ic_strcpy(ed.status, text, EDIT_STATUS_CAP);
}

static const ic_face_t *mono(void) { return ic_font(IC_FONT_MONO); }

static int sidebar_w(void) { return ed.sidebar ? EDIT_SIDEBAR_W : 0; }

static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t sidebar_rect(ic_app_t *app) {
    return ic_rect_make(0, IC_H_TOOLBAR, sidebar_w(), app->height - IC_H_TOOLBAR - EDIT_STATUS_H);
}

static ic_rect_t text_rect(ic_app_t *app) {
    int x = sidebar_w();
    return ic_rect_make(x, IC_H_TOOLBAR, app->width - x, app->height - IC_H_TOOLBAR - EDIT_STATUS_H);
}

static ic_rect_t tb_rect(ic_app_t *app, int i) {
    ic_rect_t b = toolbar_rect(app);
    int x = b.x + IC_SP_4;
    for (int k = 0; k <= i; k++) {
        int w = TB_LABELS[k] ? ic_ui_button_width(TB_LABELS[k], IC_SYM_NONE) : IC_H_CONTROL_SM + 6;
        if (k == i) return ic_rect_make(x, (b.h - IC_H_CONTROL_SM) / 2, w, IC_H_CONTROL_SM);
        x += w + (k == TB_SIDEBAR || k == TB_OPEN_FILE ? IC_SP_4 : IC_SP_2);
    }
    return ic_rect_make(0, 0, 0, 0);
}

static void layout(ic_app_t *app) {
    ic_rect_t t = text_rect(app);
    const ic_face_t *f = mono();
    ed.ch = f->line_h > 0 ? f->line_h : 1;
    ed.cw = ic_text_measure(f, "0");
    if (ed.cw <= 0) ed.cw = 8;
    ed.text_x = t.x + EDIT_GUTTER_W + IC_SP_2;
    ed.text_y = t.y + IC_SP_2;
    ed.text_w = t.x + t.w - ed.text_x - IC_SP_3;
    ed.text_h = t.h - 2 * IC_SP_2;
    ed.rows = ed.text_h / ed.ch;
    ed.cols = ed.text_w / ed.cw;
    if (ed.rows < 1) ed.rows = 1;
    if (ed.cols < 8) ed.cols = 8;
}

static uint64_t line_start(uint64_t pos) {
    if (pos > ed.len) pos = ed.len;
    while (pos > 0 && ed.buf[pos - 1] != '\n') pos--;
    return pos;
}

static uint64_t line_end(uint64_t pos) {
    if (pos > ed.len) pos = ed.len;
    while (pos < ed.len && ed.buf[pos] != '\n') pos++;
    return pos;
}

static uint64_t row_of(uint64_t pos) {
    uint64_t row = 0;
    for (uint64_t i = 0; i < pos && i < ed.len; i++) {
        if (ed.buf[i] == '\n') row++;
    }
    return row;
}

static int visual_col(uint64_t pos) {
    int c = 0;
    for (uint64_t i = line_start(pos); i < pos; i++) {
        if (ed.buf[i] == '\t') c += EDIT_TAB - (c % EDIT_TAB);
        else c++;
    }
    return c;
}

static uint64_t offset_of_row(uint64_t row) {
    uint64_t r = 0;
    uint64_t pos = 0;
    while (pos < ed.len && r < row) {
        if (ed.buf[pos++] == '\n') r++;
    }
    return pos;
}

static void scroll_to_cursor(void) {
    uint64_t row;
    int col;
    if (ed.cursor == ed.followed) return;
    ed.followed = ed.cursor;
    row = row_of(ed.cursor);
    col = visual_col(ed.cursor);
    if ((int)row < ed.scroll_row) ed.scroll_row = (int)row;
    if ((int)row >= ed.scroll_row + ed.rows) ed.scroll_row = (int)row - ed.rows + 1;
    if (col < ed.scroll_col) ed.scroll_col = col;
    if (col >= ed.scroll_col + ed.cols) ed.scroll_col = col - ed.cols + 1;
    if (ed.scroll_row < 0) ed.scroll_row = 0;
    if (ed.scroll_col < 0) ed.scroll_col = 0;
}

static void insert_char(char ch) {
    if (ed.len + 1 >= EDIT_BUF_CAP) {
        ed_status("Document is full (64 KB)");
        return;
    }
    for (uint64_t i = ed.len; i > ed.cursor; i--) ed.buf[i] = ed.buf[i - 1];
    ed.buf[ed.cursor] = ch;
    ed.len++;
    ed.cursor++;
    ed.buf[ed.len] = 0;
    ed.modified = 1;
}

static void backspace(void) {
    if (ed.cursor == 0) return;
    for (uint64_t i = ed.cursor - 1; i < ed.len; i++) ed.buf[i] = ed.buf[i + 1];
    ed.cursor--;
    ed.len--;
    ed.buf[ed.len] = 0;
    ed.modified = 1;
}

static void delete_forward(void) {
    if (ed.cursor >= ed.len) return;
    for (uint64_t i = ed.cursor; i < ed.len; i++) ed.buf[i] = ed.buf[i + 1];
    ed.len--;
    ed.buf[ed.len] = 0;
    ed.modified = 1;
}

static void move_vertical(int direction) {
    uint64_t start = line_start(ed.cursor);
    uint64_t col = ed.cursor - start;
    uint64_t target;
    uint64_t end;
    if (direction < 0) {
        if (start == 0) return;
        target = line_start(start - 1);
        end = line_end(target);
    } else {
        target = line_end(start);
        if (target >= ed.len) return;
        target++;
        end = line_end(target);
    }
    ed.cursor = target + col;
    if (ed.cursor > end) ed.cursor = end;
}

static void move_to(uint64_t pos) {
    if (pos > ed.len) pos = ed.len;
    ed.cursor = pos;
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static uint64_t word_left(uint64_t pos) {
    while (pos > 0 && !is_word_char(ed.buf[pos - 1])) pos--;
    while (pos > 0 && is_word_char(ed.buf[pos - 1])) pos--;
    return pos;
}

static uint64_t word_right(uint64_t pos) {
    while (pos < ed.len && !is_word_char(ed.buf[pos])) pos++;
    while (pos < ed.len && is_word_char(ed.buf[pos])) pos++;
    return pos;
}

static void sel_range(uint64_t *a, uint64_t *b) {
    *a = ed.anchor < ed.cursor ? ed.anchor : ed.cursor;
    *b = ed.anchor < ed.cursor ? ed.cursor : ed.anchor;
}

static int delete_selection(void) {
    uint64_t a, b;
    if (!ed.has_sel) return 0;
    sel_range(&a, &b);
    ed.has_sel = 0;
    if (a == b) return 0;
    for (uint64_t i = b; i <= ed.len; i++) ed.buf[a + i - b] = ed.buf[i];
    ed.len -= b - a;
    ed.cursor = a;
    ed.modified = 1;
    return 1;
}

static int handle_move_key(const ic_event_t *ev) {
    int ctrl = (ev->mods & IC_MOD_CTRL) != 0;
    uint64_t before = ed.cursor;
    switch (ev->key) {
    case IC_KEY_LEFT:  move_to(ctrl ? word_left(ed.cursor) : (ed.cursor > 0 ? ed.cursor - 1 : 0)); break;
    case IC_KEY_RIGHT: move_to(ctrl ? word_right(ed.cursor) : ed.cursor + 1); break;
    case IC_KEY_UP:    move_vertical(-1); break;
    case IC_KEY_DOWN:  move_vertical(1); break;
    case IC_KEY_HOME:  move_to(ctrl ? 0 : line_start(ed.cursor)); break;
    case IC_KEY_END:   move_to(ctrl ? ed.len : line_end(ed.cursor)); break;
    case IC_KEY_PAGE_UP:
        for (int i = 0; i < ed.rows; i++) move_vertical(-1);
        break;
    case IC_KEY_PAGE_DOWN:
        for (int i = 0; i < ed.rows; i++) move_vertical(1);
        break;
    default:
        return 0;
    }
    if (ev->mods & IC_MOD_SHIFT) {
        if (!ed.has_sel) {
            ed.anchor = before;
            ed.has_sel = 1;
        }
        if (ed.anchor == ed.cursor) ed.has_sel = 0;
    } else {
        ed.has_sel = 0;
    }
    return 1;
}

static int ends_with(const char *s, const char *suffix) {
    uint64_t a = ic_strlen(s), b = ic_strlen(suffix);
    return a >= b && ic_streq(s + a - b, suffix);
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1]) b = p + 1;
    }
    return b;
}

static int detect_lang(const char *path) {
    const char *b = base_name(path);
    if (ends_with(b, ".c") || ends_with(b, ".h") || ends_with(b, ".cpp") || ends_with(b, ".hpp") ||
        ends_with(b, ".cc")) return LANG_C;
    if (ends_with(b, ".js") || ends_with(b, ".ts") || ends_with(b, ".mjs") || ends_with(b, ".jsx") ||
        ends_with(b, ".tsx")) return LANG_JS;
    if (ends_with(b, ".py")) return LANG_PY;
    if (ends_with(b, ".sh") || ends_with(b, ".bash")) return LANG_SH;
    if (ic_streq(b, "Makefile") || ends_with(b, ".mk")) return LANG_MAKE;
    if (ends_with(b, ".asm") || ends_with(b, ".s") || ends_with(b, ".S")) return LANG_ASM;
    if (ends_with(b, ".json")) return LANG_JSON;
    if (ends_with(b, ".md")) return LANG_MD;
    return LANG_TEXT;
}

static const char *lang_name(int lang) {
    switch (lang) {
    case LANG_C:    return "C";
    case LANG_JS:   return "JavaScript";
    case LANG_PY:   return "Python";
    case LANG_SH:   return "Shell";
    case LANG_MAKE: return "Makefile";
    case LANG_ASM:  return "Assembly";
    case LANG_JSON: return "JSON";
    case LANG_MD:   return "Markdown";
    default:        return "Plain Text";
    }
}

static const char *const KW_C[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
    "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "const", "volatile", "extern",
    "inline", "register", "class", "public", "private", "namespace", "template", "new", "delete",
    "true", "false", "NULL", "nullptr", 0
};
static const char *const TY_C[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "size_t", "uintptr_t", "intptr_t", 0
};
static const char *const KW_JS[] = {
    "function", "return", "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "var", "let", "const", "new", "class", "extends", "import", "export", "from", "async",
    "await", "try", "catch", "finally", "throw", "typeof", "instanceof", "this", "null", "undefined",
    "true", "false", "of", "in", "interface", "type", 0
};
static const char *const TY_JS[] = { "string", "number", "boolean", "any", "void", "Array", "Promise", 0 };
static const char *const KW_PY[] = {
    "def", "class", "return", "if", "elif", "else", "for", "while", "in", "not", "and", "or", "import",
    "from", "as", "with", "try", "except", "finally", "raise", "pass", "break", "continue", "lambda",
    "yield", "None", "True", "False", "global", "nonlocal", "is", "async", "await", "self", 0
};
static const char *const TY_PY[] = { "int", "str", "float", "bool", "list", "dict", "tuple", "set", "bytes", 0 };
static const char *const KW_SH[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac", "function",
    "return", "in", "export", "local", "set", "unset", "echo", "exit", "cd", "shift", 0
};
static const char *const KW_MAKE[] = { "ifeq", "ifneq", "ifdef", "ifndef", "else", "endif", "include", "define", "endef", 0 };
static const char *const KW_ASM[] = {
    "section", "global", "extern", "bits", "db", "dw", "dd", "dq", "resb", "resw", "resd", "resq",
    "align", "alignb", "equ", "times", 0
};
static const char *const KW_JSON[] = { "true", "false", "null", 0 };

static int word_in(const char *s, int n, const char *const *list) {
    for (int i = 0; list && list[i]; i++) {
        int k = 0;
        while (k < n && list[i][k] && list[i][k] == s[k]) k++;
        if (k == n && !list[i][k]) return 1;
    }
    return 0;
}

static void lang_lists(int lang, const char *const **kw, const char *const **ty) {
    *kw = 0;
    *ty = 0;
    switch (lang) {
    case LANG_C:    *kw = KW_C; *ty = TY_C; break;
    case LANG_JS:   *kw = KW_JS; *ty = TY_JS; break;
    case LANG_PY:   *kw = KW_PY; *ty = TY_PY; break;
    case LANG_SH:   *kw = KW_SH; break;
    case LANG_MAKE: *kw = KW_MAKE; break;
    case LANG_ASM:  *kw = KW_ASM; break;
    case LANG_JSON: *kw = KW_JSON; break;
    default: break;
    }
}

static int lang_has_block(int lang) { return lang == LANG_C || lang == LANG_JS; }

static int line_comment_at(int lang, const char *s, int i, int n) {
    switch (lang) {
    case LANG_C:
    case LANG_JS:   return i + 1 < n && s[i] == '/' && s[i + 1] == '/';
    case LANG_PY:
    case LANG_SH:
    case LANG_MAKE: return s[i] == '#';
    case LANG_ASM:  return s[i] == ';' || s[i] == '#';
    default:        return 0;
    }
}

static void highlight_line(const char *s, int n, uint8_t *cls, int *in_block) {
    const char *const *kw, *const *ty;
    int lang = ed.lang;
    int i = 0;
    int first_nonspace = 0;
    lang_lists(lang, &kw, &ty);
    for (int k = 0; k < n; k++) cls[k] = IC_SYN_PLAIN;
    if (lang == LANG_TEXT) return;
    while (first_nonspace < n && (s[first_nonspace] == ' ' || s[first_nonspace] == '\t')) first_nonspace++;
    if (lang == LANG_MD) {
        if (first_nonspace < n && s[first_nonspace] == '#') {
            for (int k = 0; k < n; k++) cls[k] = IC_SYN_KEYWORD;
            return;
        }
        for (int k = 0; k < n; k++) {
            if (s[k] == '`') {
                int e = k + 1;
                while (e < n && s[e] != '`') e++;
                for (int j = k; j <= e && j < n; j++) cls[j] = IC_SYN_STRING;
                k = e;
            }
        }
        return;
    }
    if (lang == LANG_C && !*in_block && first_nonspace < n && s[first_nonspace] == '#') {
        for (int k = first_nonspace; k < n; k++) {
            if (line_comment_at(lang, s, k, n)) break;
            cls[k] = IC_SYN_PREPROC;
        }
        i = n;
    }
    while (i < n) {
        char c = s[i];
        if (*in_block) {
            cls[i] = IC_SYN_COMMENT;
            if (c == '*' && i + 1 < n && s[i + 1] == '/') {
                cls[i + 1] = IC_SYN_COMMENT;
                i += 2;
                *in_block = 0;
                continue;
            }
            i++;
            continue;
        }
        if (lang_has_block(lang) && c == '/' && i + 1 < n && s[i + 1] == '*') {
            *in_block = 1;
            cls[i] = cls[i + 1] = IC_SYN_COMMENT;
            i += 2;
            continue;
        }
        if (line_comment_at(lang, s, i, n)) {
            for (; i < n; i++) cls[i] = IC_SYN_COMMENT;
            break;
        }
        if (c == '"' || c == '\'' || (c == '`' && lang == LANG_JS)) {
            int j = i + 1;
            cls[i] = IC_SYN_STRING;
            while (j < n && s[j] != c) {
                cls[j] = IC_SYN_STRING;
                if (s[j] == '\\' && j + 1 < n) {
                    cls[j + 1] = IC_SYN_STRING;
                    j++;
                }
                j++;
            }
            if (j < n) cls[j] = IC_SYN_STRING;
            i = j + 1;
            continue;
        }
        if (c >= '0' && c <= '9' && (i == 0 || !is_word_char(s[i - 1]))) {
            int j = i;
            while (j < n && (is_word_char(s[j]) || s[j] == '.')) cls[j++] = IC_SYN_NUMBER;
            i = j;
            continue;
        }
        if (is_word_char(c)) {
            int j = i;
            int k;
            while (j < n && is_word_char(s[j])) j++;
            k = j;
            while (k < n && s[k] == ' ') k++;
            if (word_in(s + i, j - i, kw)) {
                for (int m = i; m < j; m++) cls[m] = IC_SYN_KEYWORD;
            } else if (word_in(s + i, j - i, ty)) {
                for (int m = i; m < j; m++) cls[m] = IC_SYN_TYPE;
            } else if (k < n && s[k] == '(' && lang != LANG_JSON) {
                for (int m = i; m < j; m++) cls[m] = IC_SYN_FUNCTION;
            } else if (lang == LANG_SH && i > 0 && s[i - 1] == '$') {
                for (int m = i - 1; m < j; m++) cls[m] = IC_SYN_TYPE;
            }
            i = j;
            continue;
        }
        i++;
    }
}

static int block_state_before(uint64_t offset) {
    int in_block = 0;
    if (!lang_has_block(ed.lang)) return 0;
    for (uint64_t i = 0; i + 1 < offset && i + 1 < ed.len; i++) {
        char c = ed.buf[i];
        if (!in_block && c == '/' && ed.buf[i + 1] == '/') {
            while (i < offset && i < ed.len && ed.buf[i] != '\n') i++;
            continue;
        }
        if (!in_block && (c == '"' || c == '\'')) {
            uint64_t j = i + 1;
            while (j < ed.len && ed.buf[j] != c && ed.buf[j] != '\n') {
                if (ed.buf[j] == '\\') j++;
                j++;
            }
            i = j;
            continue;
        }
        if (!in_block && c == '/' && ed.buf[i + 1] == '*') { in_block = 1; i++; continue; }
        if (in_block && c == '*' && ed.buf[i + 1] == '/') { in_block = 0; i++; continue; }
    }
    return in_block;
}

static int tree_cmp(const tree_node_t *a, const tree_node_t *b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return ic_strcmp(a->name, b->name);
}

static void join_path(char *out, const char *dir, const char *name) {
    ic_strcpy(out, dir, EDIT_PATH_CAP);
    if (!ends_with(out, "/")) ic_strlcat(out, "/", EDIT_PATH_CAP);
    ic_strlcat(out, name, EDIT_PATH_CAP);
}

static int tree_insert_children(int at, const char *dir, int depth) {
    static char list[8192];
    static tree_node_t kids[128];
    int nk = 0;
    long n = (long)icda_list_dir(dir, list, sizeof(list) - 1);
    uint64_t pos = 0;
    if (n < 0) {
        ed_status("That folder could not be read");
        return 0;
    }
    list[n < (long)sizeof(list) ? n : (long)sizeof(list) - 1] = 0;
    while (list[pos] && nk < 128) {
        tree_node_t *k = &kids[nk];
        uint64_t len = 0;
        while (list[pos] && list[pos] != '\n' && len + 1 < sizeof(k->name)) k->name[len++] = list[pos++];
        while (list[pos] && list[pos] != '\n') pos++;
        if (list[pos] == '\n') pos++;
        k->name[len] = 0;
        if (len == 0 || ic_streq(k->name, "./") || ic_streq(k->name, "../")) continue;
        k->is_dir = k->name[len - 1] == '/';
        if (k->is_dir) k->name[len - 1] = 0;
        if (k->name[0] == '.') continue;
        join_path(k->path, dir, k->name);
        k->depth = (uint8_t)depth;
        k->expanded = 0;
        nk++;
    }
    for (int a = 1; a < nk; a++) {
        tree_node_t t = kids[a];
        int b = a - 1;
        while (b >= 0 && tree_cmp(&kids[b], &t) > 0) {
            kids[b + 1] = kids[b];
            b--;
        }
        kids[b + 1] = t;
    }
    if (ed.tree_count + nk > TREE_MAX) nk = TREE_MAX - ed.tree_count;
    if (nk <= 0) return 0;
    for (int i = ed.tree_count - 1; i >= at; i--) ed.tree[i + nk] = ed.tree[i];
    for (int i = 0; i < nk; i++) ed.tree[at + i] = kids[i];
    ed.tree_count += nk;
    return nk;
}

static void tree_collapse(int i) {
    int end = i + 1;
    while (end < ed.tree_count && ed.tree[end].depth > ed.tree[i].depth) end++;
    for (int k = end; k < ed.tree_count; k++) ed.tree[k - (end - i - 1)] = ed.tree[k];
    ed.tree_count -= end - i - 1;
    ed.tree[i].expanded = 0;
}

static void open_folder(const char *dir) {
    static char tmp[8192];
    long n = (long)icda_list_dir(dir, tmp, sizeof(tmp) - 1);
    if (n < 0) {
        ed_status("That folder could not be opened");
        return;
    }
    ic_strcpy(ed.root, dir, EDIT_PATH_CAP);
    ed.tree_count = 0;
    ed.tree_scroll = 0;
    tree_insert_children(0, ed.root, 0);
    ed.sidebar = 1;
    ed_status("Folder opened");
}

static void open_file(const char *path) {
    long n;
    ed.has_sel = 0;
    ic_strcpy(ed.path, path, EDIT_PATH_CAP);
    n = (long)icda_read_file(ed.path, ed.buf, sizeof(ed.buf) - 1);
    if (n < 0) {
        ed.buf[0] = 0;
        ed.len = 0;
        ed_status("New file");
    } else {
        ed.len = (uint64_t)n;
        ed.buf[ed.len] = 0;
        ed_status(ed.len == sizeof(ed.buf) - 1 ? "Opened (truncated at 64 KB)" : "Opened");
    }
    ed.lang = detect_lang(ed.path);
    ed.cursor = 0;
    ed.followed = (uint64_t)-1;
    ed.scroll_row = 0;
    ed.scroll_col = 0;
    ed.modified = 0;
    ed.pending_open[0] = 0;
}

static void new_file(void) {
    ic_strcpy(ed.path, ed.root[0] ? ed.root : "/home", EDIT_PATH_CAP);
    if (!ends_with(ed.path, "/")) ic_strlcat(ed.path, "/", EDIT_PATH_CAP);
    ic_strlcat(ed.path, "untitled.txt", EDIT_PATH_CAP);
    ed.has_sel = 0;
    ed.buf[0] = 0;
    ed.len = 0;
    ed.cursor = 0;
    ed.modified = 0;
    ed.scroll_row = 0;
    ed.scroll_col = 0;
    ed.lang = LANG_TEXT;
    ed_status("New document");
}

static int tree_contains(const char *path) {
    for (int i = 0; i < ed.tree_count; i++) {
        if (ic_streq(ed.tree[i].path, path)) return 1;
    }
    return 0;
}

static void open_folder(const char *dir);

static void save(void) {
    if (icda_write_file(ed.path, ed.buf, ed.len) != (uint64_t)-1) {
        ed.modified = 0;
        if (ed.root[0] && !tree_contains(ed.path) && ic_strprefix(ed.path, ed.root)) {
            char root[EDIT_PATH_CAP];
            ic_strcpy(root, ed.root, EDIT_PATH_CAP);
            open_folder(root);
        }
        ed_status("Saved");
    } else {
        ed_status("Could not save this file");
    }
}

static int guard_unsaved(const char *next_path) {
    if (!ed.modified) return 1;
    if (ic_streq(ed.pending_open, next_path)) return 1;
    ic_strcpy(ed.pending_open, next_path, EDIT_PATH_CAP);
    ed_status("Unsaved changes. Do it again to discard them, or press Ctrl+S.");
    return 0;
}

static int is_dir(const char *path) {
    static char tmp[8192];
    return (long)icda_list_dir(path, tmp, sizeof(tmp) - 1) >= 0;
}

static void open_prompt(int kind) {
    ed.prompt = kind;
    if (kind == PROMPT_FOLDER) ic_strcpy(ed.prompt_buf, ed.root[0] ? ed.root : "/home", EDIT_PATH_CAP);
    else ic_strcpy(ed.prompt_buf, ed.path, EDIT_PATH_CAP);
}

static void prompt_accept(void) {
    int kind = ed.prompt;
    ed.prompt = PROMPT_NONE;
    if (!ed.prompt_buf[0]) return;
    if (kind == PROMPT_FOLDER || is_dir(ed.prompt_buf)) {
        open_folder(ed.prompt_buf);
    } else if (guard_unsaved(ed.prompt_buf)) {
        open_file(ed.prompt_buf);
    }
}

static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_rect_t last;
    char title[EDIT_PATH_CAP + 4];
    ic_ui_toolbar(c, b);
    for (int i = 0; i < TB_COUNT; i++) {
        ic_rect_t r = tb_rect(app, i);
        ic_state_t st = ed.hover_tb == i ? IC_STATE_HOVER : IC_STATE_NORMAL;
        if (i == TB_SIDEBAR) {
            ic_ui_icon_button(c, r, IC_SYM_GRID, st);
            continue;
        }
        ic_ui_button(c, r, TB_LABELS[i], IC_SYM_NONE,
                     i == TB_SAVE && ed.modified ? IC_BUTTON_PRIMARY : IC_BUTTON_DEFAULT, st);
    }
    last = tb_rect(app, TB_SAVE);
    title[0] = 0;
    ic_strlcat(title, ed.path, sizeof(title));
    if (ed.modified) ic_strlcat(title, "  *", sizeof(title));
    ic_text_draw_in(c, ic_font(IC_FONT_BODY),
                    ic_rect_make(last.x + last.w + IC_SP_4, 0, b.w - (last.x + last.w) - IC_SP_6, b.h),
                    title, p->label_secondary, IC_ALIGN_LEFT);
}

static ic_rect_t tree_row_rect(ic_app_t *app, int vis) {
    ic_rect_t s = sidebar_rect(app);
    return ic_rect_make(s.x, s.y + 30 + vis * TREE_ROW_H, s.w, TREE_ROW_H);
}

static int tree_rows_visible(ic_app_t *app) {
    ic_rect_t s = sidebar_rect(app);
    return (s.h - 30) / TREE_ROW_H;
}

static void draw_sidebar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = sidebar_rect(app);
    const ic_palette_t *p = ic_palette();
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    int vis = tree_rows_visible(app);
    ic_rect_t saved;
    if (!ed.sidebar) return;
    ic_ui_sidebar_bg(c, s);
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(s.x + IC_SP_3, s.y, s.w - IC_SP_4, 28),
                    ed.root[0] ? base_name(ed.root) : "EXPLORER", p->label_tertiary, IC_ALIGN_LEFT);
    if (!ed.root[0]) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(s.x + IC_SP_3, s.y + 34, s.w - IC_SP_4, 18),
                        "No folder open", p->label_secondary, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(s.x + IC_SP_3, s.y + 52, s.w - IC_SP_4, 18),
                        "Use Open Folder above", p->label_tertiary, IC_ALIGN_LEFT);
        return;
    }
    ic_canvas_push_clip(c, s.x, s.y + 30, s.w, s.h - 30, &saved);
    for (int v = 0; v < vis && ed.tree_scroll + v < ed.tree_count; v++) {
        int i = ed.tree_scroll + v;
        tree_node_t *n = &ed.tree[i];
        ic_rect_t r = tree_row_rect(app, v);
        int x = r.x + IC_SP_2 + n->depth * 12;
        int current = !n->is_dir && ic_streq(n->path, ed.path);
        if (current) ic_gfx_rrect(c, r.x + 4, r.y, r.w - 8, r.h, IC_R_ROW, p->fill_selected_idle);
        else if (ed.tree_hover == i) ic_gfx_rrect(c, r.x + 4, r.y, r.w - 8, r.h, IC_R_ROW, p->fill_hover);
        if (n->is_dir) {
            ic_symbol_draw(c, n->expanded ? IC_SYM_CHEVRON_DOWN : IC_SYM_CHEVRON_RIGHT, (float)x + 5.0f,
                           (float)r.y + (float)r.h * 0.5f, 9.0f, p->label_secondary);
        }
        ic_symbol_draw(c, n->is_dir ? IC_SYM_FOLDER : IC_SYM_DOCUMENT, (float)x + 20.0f,
                       (float)r.y + (float)r.h * 0.5f, 13.0f, n->is_dir ? p->accent : p->label_secondary);
        ic_text_draw_in(c, body, ic_rect_make(x + 30, r.y, r.x + r.w - x - 34, r.h), n->name,
                        current ? p->label : p->label_secondary, IC_ALIGN_LEFT);
    }
    ic_canvas_pop_clip(c, &saved);
    ic_gfx_vline(c, s.x + s.w - 1, s.y, s.h, p->separator);
}

static void draw_text(ic_app_t *app, ic_canvas_t *c) {
    static uint8_t cls[LINE_CLS_CAP];
    ic_rect_t t = text_rect(app);
    const ic_palette_t *p = ic_palette();
    const ic_face_t *f = mono();
    const ic_face_t *num = ic_font(IC_FONT_MONO_SMALL);
    ic_rect_t saved;
    uint64_t cursor_row = row_of(ed.cursor);
    uint64_t last_row = row_of(ed.len);
    uint64_t start = offset_of_row((uint64_t)ed.scroll_row);
    int in_block = block_state_before(start);
    int gx = t.x;
    char digits[24];

    ic_gfx_fill(c, t.x, t.y, t.w, t.h, p->content);
    ic_canvas_push_clip(c, t.x, t.y, t.w, t.h, &saved);
    ic_gfx_fill(c, gx, t.y, EDIT_GUTTER_W, t.h, p->sidebar);
    ic_gfx_vline(c, gx + EDIT_GUTTER_W, t.y, t.h, p->separator);

    for (int r = 0; r < ed.rows; r++) {
        uint64_t row = (uint64_t)(ed.scroll_row + r);
        uint64_t end;
        int y = ed.text_y + r * ed.ch;
        int base = y + ic_text_center_baseline(f, 0, ed.ch);
        int n, col = 0, len;
        if (row > last_row) break;
        end = line_end(start);
        if (row == cursor_row) ic_gfx_fill(c, gx, y, EDIT_GUTTER_W, ed.ch, p->accent_soft);
        n = (int)ic_snprintf_u64(digits, sizeof(digits), row + 1);
        ic_text_draw_n(c, num, gx + EDIT_GUTTER_W - IC_SP_2 - ic_text_measure_n(num, digits, n), base, digits, n,
                       row == cursor_row ? p->accent : p->label_tertiary);

        if (ed.has_sel) {
            uint64_t a, b;
            sel_range(&a, &b);
            if (b > start && a <= end) {
                uint64_t s0 = a > start ? a : start, s1 = b < end ? b : end;
                int c0 = visual_col(s0), c1 = visual_col(s1) + (b > end ? 1 : 0);
                if (c1 > c0) {
                    ic_gfx_fill(c, ed.text_x + (c0 - ed.scroll_col) * ed.cw, y, (c1 - c0) * ed.cw, ed.ch,
                                p->accent_soft);
                }
            }
        }

        len = (int)(end - start);
        if (len > LINE_CLS_CAP) len = LINE_CLS_CAP;
        highlight_line(ed.buf + start, len, cls, &in_block);
        for (int i = 0; i < len && col < ed.scroll_col + ed.cols;) {
            unsigned char ch = (unsigned char)ed.buf[start + i];
            int j, run_col;
            if (ch == '\t') {
                col += EDIT_TAB - (col % EDIT_TAB);
                i++;
                continue;
            }
            if (ch < 32 || ch > 126) {
                if (col >= ed.scroll_col) {
                    ic_text_draw_n(c, f, ed.text_x + (col - ed.scroll_col) * ed.cw, base, "?", 1, p->label_tertiary);
                }
                col++;
                i++;
                continue;
            }
            j = i;
            while (j < len && cls[j] == cls[i] && ed.buf[start + j] != '\t' &&
                   (unsigned char)ed.buf[start + j] >= 32 && (unsigned char)ed.buf[start + j] <= 126) {
                j++;
            }
            run_col = col;
            {
                int end_col = run_col + (j - i);
                int vis_from = run_col > ed.scroll_col ? run_col : ed.scroll_col;
                int vis_to = end_col < ed.scroll_col + ed.cols ? end_col : ed.scroll_col + ed.cols;
                if (vis_from < vis_to) {
                    ic_text_draw_n(c, f, ed.text_x + (vis_from - ed.scroll_col) * ed.cw, base,
                                   ed.buf + start + i + (vis_from - run_col), vis_to - vis_from,
                                   ic_syntax_color((ic_syntax_t)cls[i]));
                }
                col = end_col;
            }
            i = j;
        }
        if (len == LINE_CLS_CAP && end - start > LINE_CLS_CAP) in_block = 0;
        start = end < ed.len ? end + 1 : end;
    }
    ic_canvas_pop_clip(c, &saved);
}

static void draw_caret(ic_app_t *app, ic_canvas_t *c) {
    uint64_t row = row_of(ed.cursor);
    int col = visual_col(ed.cursor);
    int r = (int)row - ed.scroll_row;
    int cc = col - ed.scroll_col;
    if (ed.prompt || r < 0 || r >= ed.rows || cc < 0 || cc >= ed.cols) return;
    if (!ic_app_caret_visible(app)) return;
    ic_gfx_fill(c, ed.text_x + cc * ed.cw, ed.text_y + r * ed.ch, 2, ed.ch, ic_palette()->label);
}

static void draw_status(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = ic_rect_make(0, app->height - EDIT_STATUS_H, app->width, EDIT_STATUS_H);
    const ic_palette_t *p = ic_palette();
    char pos[48];
    char n[24];
    pos[0] = 0;
    ic_strlcat(pos, "Ln ", sizeof(pos));
    ic_snprintf_u64(n, sizeof(n), row_of(ed.cursor) + 1);
    ic_strlcat(pos, n, sizeof(pos));
    ic_strlcat(pos, ", Col ", sizeof(pos));
    ic_snprintf_u64(n, sizeof(n), (uint64_t)visual_col(ed.cursor) + 1);
    ic_strlcat(pos, n, sizeof(pos));
    ic_ui_statusbar(c, s, ed.status);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(s.x + s.w - 300, s.y, 130, s.h), pos,
                    p->label_secondary, IC_ALIGN_RIGHT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(s.x + s.w - 160, s.y, 160 - IC_SP_3, s.h),
                    lang_name(ed.lang), p->label_secondary, IC_ALIGN_RIGHT);
}

static ic_rect_t prompt_rect(ic_app_t *app) {
    int w = 460, h = 112;
    return ic_rect_make((app->width - w) / 2, IC_H_TOOLBAR + 40, w, h);
}

static void draw_prompt(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t r = prompt_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_textfield_t tf;
    int len = (int)ic_strlen(ed.prompt_buf);
    if (!ed.prompt) return;
    ic_theme_shadow(c, r.x, r.y, r.w, r.h, IC_R_PANEL, IC_ELEV_MENU);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_PANEL, ic_color_over(p->window, p->material_menu));
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_PANEL, 1.0f, p->frame);
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(r.x + IC_SP_4, r.y + IC_SP_3, r.w - 2 * IC_SP_4, 20),
                    ed.prompt == PROMPT_FOLDER ? "Open Folder" : "Open File", p->label, IC_ALIGN_LEFT);
    tf.text = ed.prompt_buf;
    tf.cursor = len;
    tf.sel_start = tf.sel_end = len;
    tf.focused = 1;
    tf.caret_on = ic_app_caret_visible(app);
    tf.placeholder = "Path";
    tf.leading = ed.prompt == PROMPT_FOLDER ? IC_SYM_FOLDER : IC_SYM_DOCUMENT;
    tf.scroll_px = 0;
    ic_ui_textfield(c, ic_rect_make(r.x + IC_SP_4, r.y + 40, r.w - 2 * IC_SP_4, IC_H_CONTROL), &tf);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + IC_SP_4, r.y + 78, r.w - 2 * IC_SP_4, 18),
                    "Enter to open, Escape to cancel", p->label_tertiary, IC_ALIGN_LEFT);
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    layout(app);
    scroll_to_cursor();
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_sidebar(app, c);
    draw_text(app, c);
    draw_caret(app, c);
    draw_status(app, c);
    draw_prompt(app, c);
    if (app->focused) ic_app_animate(app);
}

static void click_to_cursor(int mx, int my) {
    int r = (my - ed.text_y) / ed.ch;
    int col = (mx - ed.text_x) / ed.cw + ed.scroll_col;
    uint64_t row = (uint64_t)(ed.scroll_row + (r < 0 ? 0 : r));
    uint64_t start, end, pos;
    int c = 0;
    if (r < 0 || r >= ed.rows) return;
    start = offset_of_row(row);
    end = line_end(start);
    pos = start;
    while (pos < end && c < col) {
        if (ed.buf[pos] == '\t') c += EDIT_TAB - (c % EDIT_TAB);
        else c++;
        pos++;
    }
    move_to(pos);
}

static int tree_hit(ic_app_t *app, int x, int y) {
    ic_rect_t s = sidebar_rect(app);
    int v;
    if (!ed.sidebar || !ed.root[0] || !ic_ui_hit(s, x, y) || y < s.y + 30) return -1;
    v = (y - s.y - 30) / TREE_ROW_H;
    if (ed.tree_scroll + v >= ed.tree_count) return -1;
    return ed.tree_scroll + v;
}

static void tree_activate(int i) {
    tree_node_t *n = &ed.tree[i];
    if (n->is_dir) {
        if (n->expanded) {
            tree_collapse(i);
        } else {
            n->expanded = 1;
            if (tree_insert_children(i + 1, n->path, n->depth + 1) == 0) ed_status("Folder is empty");
        }
        return;
    }
    if (guard_unsaved(n->path)) open_file(n->path);
}

static void prompt_key(const ic_event_t *ev) {
    uint64_t len = ic_strlen(ed.prompt_buf);
    if (ev->key == IC_KEY_ESCAPE) {
        ed.prompt = PROMPT_NONE;
    } else if (ev->key == IC_KEY_ENTER) {
        prompt_accept();
    } else if (ev->key == IC_KEY_BACKSPACE) {
        if (len) ed.prompt_buf[len - 1] = 0;
    } else if ((ev->mods & IC_MOD_CTRL) && ev->key == 'u') {
        ed.prompt_buf[0] = 0;
    } else if (ev->key >= 32 && ev->key < 127 && !(ev->mods & (IC_MOD_CTRL | IC_MOD_ALT)) &&
               len + 1 < EDIT_PATH_CAP) {
        ed.prompt_buf[len] = (char)ev->key;
        ed.prompt_buf[len + 1] = 0;
    }
}

static void toolbar_action(int i) {
    switch (i) {
    case TB_SIDEBAR:     ed.sidebar = !ed.sidebar; break;
    case TB_OPEN_FOLDER: open_prompt(PROMPT_FOLDER); break;
    case TB_OPEN_FILE:   open_prompt(PROMPT_FILE); break;
    case TB_NEW:         if (guard_unsaved("")) new_file(); break;
    case TB_SAVE:        save(); break;
    default: break;
    }
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        ed.hover_tb = -1;
        for (int i = 0; i < TB_COUNT; i++) {
            if (ic_ui_hit(tb_rect(app, i), ev->x, ev->y)) ed.hover_tb = i;
        }
        ed.tree_hover = tree_hit(app, ev->x, ev->y);
        ic_app_set_cursor(app, !ed.prompt && ic_ui_hit(text_rect(app), ev->x, ev->y) &&
                                       ev->x >= text_rect(app).x + EDIT_GUTTER_W
                                   ? IC_CURSOR_TEXT : IC_CURSOR_ARROW);
        break;
    case IC_EV_MOUSE_DOWN: {
        int t;
        if (ev->button != GUI_BTN_LEFT) break;
        if (ed.prompt) {
            if (!ic_ui_hit(prompt_rect(app), ev->x, ev->y)) ed.prompt = PROMPT_NONE;
            break;
        }
        if (ed.hover_tb >= 0) {
            toolbar_action(ed.hover_tb);
            break;
        }
        t = tree_hit(app, ev->x, ev->y);
        if (t >= 0) {
            tree_activate(t);
            break;
        }
        if (ic_ui_hit(text_rect(app), ev->x, ev->y)) {
            ed.has_sel = 0;
            click_to_cursor(ev->x, ev->y);
        }
        break;
    }
    case IC_EV_MOUSE_LEAVE:
        ed.hover_tb = -1;
        ed.tree_hover = -1;
        break;
    case IC_EV_SCROLL:
        if (ed.sidebar && ev->x < sidebar_w()) {
            int max = ed.tree_count - tree_rows_visible(app);
            ed.tree_scroll += ev->wheel * 3;
            if (ed.tree_scroll > max) ed.tree_scroll = max;
            if (ed.tree_scroll < 0) ed.tree_scroll = 0;
        } else {
            int max_row = (int)row_of(ed.len);
            ed.scroll_row += ev->wheel * 3;
            if (ed.scroll_row > max_row) ed.scroll_row = max_row;
            if (ed.scroll_row < 0) ed.scroll_row = 0;
        }
        break;
    case IC_EV_KEY:
        if (ed.prompt) {
            prompt_key(ev);
            break;
        }
        if (handle_move_key(ev)) break;
        if (ev->mods & IC_MOD_CTRL) {
            switch (ev->key) {
            case 's': save(); break;
            case 'o': open_prompt((ev->mods & IC_MOD_SHIFT) ? PROMPT_FOLDER : PROMPT_FILE); break;
            case 'b': ed.sidebar = !ed.sidebar; break;
            case 'n': if (guard_unsaved("")) new_file(); break;
            case 'a':
                ed.anchor = 0;
                ed.cursor = ed.len;
                ed.has_sel = ed.len > 0;
                break;
            default: break;
            }
            break;
        }
        switch (ev->key) {
        case IC_KEY_BACKSPACE: if (!delete_selection()) backspace(); break;
        case IC_KEY_DELETE:    if (!delete_selection()) delete_forward(); break;
        case IC_KEY_ENTER: {
            uint64_t ls = line_start(ed.cursor);
            uint64_t indent = ls;
            delete_selection();
            while (indent < ed.cursor && (ed.buf[indent] == ' ' || ed.buf[indent] == '\t')) indent++;
            insert_char('\n');
            for (uint64_t k = ls; k < indent; k++) insert_char(ed.buf[k]);
            break;
        }
        case IC_KEY_TAB:       delete_selection(); insert_char('\t'); break;
        case IC_KEY_ESCAPE:    ed.has_sel = 0; break;
        default:
            if (ev->key >= 32 && ev->key < 127 && !(ev->mods & IC_MOD_ALT)) {
                delete_selection();
                insert_char((char)ev->key);
            }
            break;
        }
        if (ed.modified) ed.pending_open[0] = 0;
        break;
    case IC_EV_RESIZE:
        layout(app);
        break;
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void dir_of(char *out, const char *path) {
    uint64_t n;
    ic_strcpy(out, path, EDIT_PATH_CAP);
    n = ic_strlen(out);
    while (n > 1 && out[n - 1] != '/') n--;
    if (n > 1) n--;
    out[n] = 0;
}

static void init(ic_app_t *app) {
    char req[EDIT_PATH_CAP];
    long n = (long)icda_read_file("/home/.edit.request", req, sizeof(req) - 1);
    const char *arg = (const char *)app->user;
    ed.hover_tb = -1;
    ed.tree_hover = -1;
    ed.root[0] = 0;
    if (arg && arg[0]) {
        open_file(arg);
    } else if (n > 0) {
        req[n] = 0;
        while (n > 0 && (req[n - 1] == '\n' || req[n - 1] == '\r')) req[--n] = 0;
        open_file(req);
    } else {
        new_file();
    }
    {
        char dir[EDIT_PATH_CAP];
        dir_of(dir, ed.path);
        open_folder(dir[0] ? dir : "/home");
    }
    layout(app);
    ed_status("Ready");
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Editor", WIN_W, WIN_H, init, draw, event, 0 };
    const char *arg = (argc > 1 && argv) ? argv[1] : 0;
    if (ic_app_run(&desc, (void *)arg) != 0) {
        icda_write("editor requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
