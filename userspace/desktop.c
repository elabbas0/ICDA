#include "gui.h"
#include "icda_sys.h"
#include "libicda.h"
#include "font.h"

#include <stdint.h>

#define DESKTOP_W 760
#define DESKTOP_H 520
#define PATH_CAP 192
#define NAME_CAP 64
#define STATUS_CAP 96
#define LIST_CAP 4096
#define MAX_ITEMS 96
#define HISTORY_CAP 16
#define EDIT_CAP 4096

#define TOP_H 88
#define SIDE_W 132
#define STATUS_H 36
#define ICON_CELL_W 96
#define ICON_CELL_H 82

enum {
    MODE_BROWSER = 0,
    MODE_EDITOR = 1
};

enum {
    DIALOG_NONE = 0,
    DIALOG_NEW_FILE,
    DIALOG_NEW_FOLDER,
    DIALOG_GOTO
};

enum {
    SPECIAL_UP = 1,
    SPECIAL_DOWN,
    SPECIAL_LEFT,
    SPECIAL_RIGHT,
    SPECIAL_DELETE
};

typedef struct {
    char name[NAME_CAP];
    char path[PATH_CAP];
    int is_dir;
    int is_app;
    int is_wav;
    uint64_t size;
    uint8_t readonly;
    int x;
    int y;
    int w;
    int h;
} desktop_item_t;

static desktop_item_t items[MAX_ITEMS];
static int item_count = 0;
static int selected_item = -1;
static int hover_item = -1;
static int page_offset = 0;
static int layout_cols = 1;
static int layout_visible = 1;

static char current_path[PATH_CAP] = "/";
static char status_text[STATUS_CAP] = "Ready";
static char list_buf[LIST_CAP];
static char history[HISTORY_CAP][PATH_CAP];
static int history_count = 0;

static int mode = MODE_BROWSER;
static int dialog = DIALOG_NONE;
static char dialog_input[PATH_CAP];
static char dialog_title[48];
static int dialog_cursor = 0;

static char editor_path[PATH_CAP];
static char editor_buf[EDIT_CAP];
static uint64_t editor_len = 0;
static uint64_t editor_cursor = 0;
static int editor_modified = 0;
static int editor_top_row = 0;

static uint64_t last_click_tick = 0;
static int last_click_item = -1;
static int key_seq_state = 0;

static uint64_t d_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int d_streq(const char *a, const char *b) {
    uint64_t i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static char d_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static void d_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void d_append(char *dst, const char *src, uint64_t cap) {
    uint64_t at = d_strlen(dst);
    uint64_t i = 0;
    if (!dst || cap == 0 || at >= cap) return;
    while (src && src[i] && at + 1 < cap) {
        dst[at++] = src[i++];
    }
    dst[at] = 0;
}

static void uint_to_text(uint64_t v, char *out, uint64_t cap) {
    char tmp[32];
    uint64_t len = 0;
    uint64_t i = 0;

    if (!out || cap == 0) return;
    if (v == 0) {
        d_copy(out, "0", cap);
        return;
    }
    while (v && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (len && i + 1 < cap) {
        out[i++] = tmp[--len];
    }
    out[i] = 0;
}

static int has_suffix(const char *text, const char *suffix) {
    uint64_t tl = d_strlen(text);
    uint64_t sl = d_strlen(suffix);
    if (sl > tl) return 0;
    for (uint64_t i = 0; i < sl; i++) {
        if (d_lower(text[tl - sl + i]) != d_lower(suffix[i])) return 0;
    }
    return 1;
}

static void set_status(const char *text) {
    d_copy(status_text, text, sizeof(status_text));
}

static int hit_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static uint32_t blend(uint32_t a, uint32_t b, int n, int d) {
    int ar = (int)((a >> 16) & 0xFF);
    int ag = (int)((a >> 8) & 0xFF);
    int ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF);
    int bg = (int)((b >> 8) & 0xFF);
    int bb = (int)(b & 0xFF);
    int r = ar + ((br - ar) * n) / d;
    int g = ag + ((bg - ag) * n) / d;
    int c = ab + ((bb - ab) * n) / d;
    return (uint32_t)((r << 16) | (g << 8) | c);
}

static void draw_gradient(int x, int y, int w, int h, uint32_t a, uint32_t b) {
    if (h <= 1) {
        gui_fill_rect(x, y, w, h, a);
        return;
    }
    for (int row = 0; row < h; row++) {
        gui_fill_rect(x, y + row, w, 1, blend(a, b, row, h - 1));
    }
}

static void draw_text_clip(int x, int y, const char *text, uint32_t fg, uint32_t bg, int max_px) {
    int cx = x;
    if (max_px <= 0) return;
    while (text && *text && cx + FONT_CELL_WIDTH <= x + max_px) {
        gui_draw_char(cx, y, *text, fg, bg);
        cx += FONT_CELL_WIDTH;
        text++;
    }
}

static void draw_panel(int x, int y, int w, int h, uint32_t fill, uint32_t edge) {
    gui_fill_rect(x, y, w, h, fill);
    gui_draw_rect_outline(x, y, w, h, edge);
}

static void draw_button(int x, int y, int w, int h, const char *label, int active) {
    uint32_t top = active ? 0x003D8BFF : 0x00FFFFFF;
    uint32_t bottom = active ? 0x001F5EBE : 0x00DDEBFF;
    uint32_t edge = active ? 0x000C3C88 : 0x006EA6E8;
    draw_gradient(x, y, w, h, top, bottom);
    gui_draw_rect_outline(x, y, w, h, edge);
    draw_text_clip(x + 8, y + 5, label, active ? 0x00FFFFFF : 0x001D3F66, bottom, w - 14);
}

static void path_join(char *out, uint64_t cap, const char *dir, const char *name) {
    d_copy(out, dir && *dir ? dir : "/", cap);
    if (!d_streq(out, "/")) d_append(out, "/", cap);
    d_append(out, name, cap);
}

static void path_parent(char *out, uint64_t cap, const char *path) {
    uint64_t len;
    d_copy(out, path && *path ? path : "/", cap);
    len = d_strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
    if (len <= 1) {
        d_copy(out, "/", cap);
        return;
    }
    while (len > 1 && out[len - 1] != '/') len--;
    if (len <= 1) {
        d_copy(out, "/", cap);
    } else {
        out[len - 1] = 0;
    }
}

static int valid_new_name(const char *name) {
    uint64_t len = d_strlen(name);
    if (len == 0 || len >= NAME_CAP) return 0;
    for (uint64_t i = 0; i < len; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c < 32) return 0;
    }
    return 1;
}

static void history_push(const char *path) {
    if (history_count >= HISTORY_CAP) {
        for (int i = 1; i < HISTORY_CAP; i++) {
            d_copy(history[i - 1], history[i], sizeof(history[0]));
        }
        history_count = HISTORY_CAP - 1;
    }
    d_copy(history[history_count++], path, sizeof(history[0]));
}

static void layout_items(int win_w, int win_h) {
    int grid_x = SIDE_W + 16;
    int grid_y = TOP_H + 14;
    int grid_w = win_w - grid_x - 14;
    int grid_h = win_h - grid_y - STATUS_H - 10;

    layout_cols = grid_w / ICON_CELL_W;
    if (layout_cols < 1) layout_cols = 1;
    layout_visible = layout_cols * (grid_h / ICON_CELL_H);
    if (layout_visible < 1) layout_visible = layout_cols;

    if (page_offset >= item_count) page_offset = 0;
    if (selected_item >= item_count) selected_item = -1;

    for (int i = 0; i < item_count; i++) {
        items[i].x = items[i].y = items[i].w = items[i].h = 0;
    }
    for (int slot = 0; slot < layout_visible; slot++) {
        int idx = page_offset + slot;
        if (idx >= item_count) break;
        items[idx].x = grid_x + (slot % layout_cols) * ICON_CELL_W;
        items[idx].y = grid_y + (slot / layout_cols) * ICON_CELL_H;
        items[idx].w = ICON_CELL_W - 8;
        items[idx].h = ICON_CELL_H - 6;
    }
}

static void browser_refresh(void) {
    uint64_t rc;
    uint64_t pos = 0;
    item_count = 0;
    hover_item = -1;

    rc = icda_list_dir(current_path, list_buf, sizeof(list_buf));
    if ((long)rc < 0) {
        set_status("Could not read folder");
        current_path[0] = '/';
        current_path[1] = 0;
        rc = icda_list_dir(current_path, list_buf, sizeof(list_buf));
        if ((long)rc < 0) return;
    }

    while (pos < rc && item_count < MAX_ITEMS) {
        char entry[NAME_CAP];
        uint64_t ei = 0;
        int is_dir = 0;

        while (pos < rc && list_buf[pos] != '\n' && ei + 1 < sizeof(entry)) {
            entry[ei++] = list_buf[pos++];
        }
        while (pos < rc && list_buf[pos] != '\n') pos++;
        if (pos < rc && list_buf[pos] == '\n') pos++;
        entry[ei] = 0;
        if (ei == 0) continue;
        if (entry[ei - 1] == '/') {
            entry[ei - 1] = 0;
            is_dir = 1;
        }

        d_copy(items[item_count].name, entry, sizeof(items[item_count].name));
        path_join(items[item_count].path, sizeof(items[item_count].path), current_path, entry);
        items[item_count].is_dir = is_dir;
        items[item_count].is_app = has_suffix(entry, ".app") || has_suffix(entry, ".elf");
        items[item_count].is_wav = has_suffix(entry, ".wav");
        items[item_count].size = 0;
        items[item_count].readonly = 0;
        {
            icda_stat_t st;
            if ((long)icda_stat(items[item_count].path, &st) >= 0) {
                items[item_count].size = st.size;
                items[item_count].readonly = st.readonly;
                items[item_count].is_dir = st.type == 2;
            }
        }
        item_count++;
    }
    set_status("Folder loaded");
}

static void navigate_to(const char *path, int record_history) {
    if (!path || !*path) return;
    if (record_history) history_push(current_path);
    d_copy(current_path, path, sizeof(current_path));
    selected_item = -1;
    page_offset = 0;
    browser_refresh();
}

static void navigate_up(void) {
    char parent[PATH_CAP];
    if (d_streq(current_path, "/")) {
        set_status("Already at root");
        return;
    }
    path_parent(parent, sizeof(parent), current_path);
    navigate_to(parent, 1);
}

static void navigate_back(void) {
    if (history_count <= 0) {
        set_status("No previous folder");
        return;
    }
    history_count--;
    d_copy(current_path, history[history_count], sizeof(current_path));
    selected_item = -1;
    page_offset = 0;
    browser_refresh();
}

static void draw_item(desktop_item_t *item, int idx, uint64_t tick) {
    int selected = idx == selected_item;
    int hover = idx == hover_item;
    int lift = hover ? 2 : 0;
    uint32_t bg = selected ? 0x00CFE6FF : (hover ? 0x00EEF7FF : 0x00FFFFFF);
    uint32_t edge = selected ? 0x003D8BFF : (hover ? 0x0092C5F8 : 0x00D7E2F1);
    const char *icon_name;
    const ic_icon_t *icon;
    ic_canvas_t c;

    if (selected && ((tick / 8) & 1)) bg = 0x00DDF0FF;
    draw_panel(item->x, item->y - lift, item->w, item->h, bg, edge);

    /* Real icons from the engine's builtin set instead of hand-drawn pixels */
    icon_name = item->is_dir ? "folder" : (item->is_wav ? "wav" : (item->is_app ? "app" : "file"));
    icon = ic_icon_builtin(icon_name);
    if (icon) {
        c.px = gui_pixel_buffer();
        c.w = gui_window_width();
        c.h = gui_window_height();
        ic_icon_draw(&c, item->x + 10, item->y + 4 - lift, 54, 48, icon);
    }
    draw_text_clip(item->x + 6, item->y + 60 - lift, item->name, 0x001F2937, bg, item->w - 12);
}

static void draw_sidebar_button(int y, const char *label, const char *path) {
    int active = d_streq(current_path, path);
    draw_button(12, y, SIDE_W - 24, 26, label, active);
}

static void draw_browser(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    uint64_t tick = icda_ticks();
    icda_audio_info_t audio;
    char count_buf[32];

    layout_items(w, h);
    draw_gradient(0, 0, w, h, 0x00E7F1FF, 0x00F9FCFF);
    draw_gradient(0, 0, w, TOP_H, 0x003D8BFF, 0x001F5EBE);
    gui_fill_rect(0, TOP_H, SIDE_W, h - TOP_H - STATUS_H, 0x00DDEBFF);
    gui_draw_vline(SIDE_W, TOP_H, h - TOP_H - STATUS_H, 0x0092B7E8);

    gui_fill_rect(12, 12, w - 24, 26, 0x00FFFFFF);
    gui_draw_rect_outline(12, 12, w - 24, 26, 0x006EA6E8);
    draw_text_clip(20, 17, current_path, 0x001F2937, 0x00FFFFFF, w - 40);

    draw_button(12, 50, 50, 25, "Back", history_count > 0);
    draw_button(68, 50, 42, 25, "Up", !d_streq(current_path, "/"));
    draw_button(116, 50, 62, 25, "Reload", 0);
    draw_button(190, 50, 72, 25, "New File", 0);
    draw_button(268, 50, 86, 25, "New Folder", 0);
    draw_button(366, 50, 50, 25, "Edit", selected_item >= 0 && !items[selected_item].is_dir);
    draw_button(422, 50, 54, 25, "Play", selected_item >= 0 && items[selected_item].is_wav);
    draw_button(482, 50, 54, 25, "Stop", 0);
    draw_button(w - 174, 50, 78, 25, "Terminal", 0);
    draw_button(w - 88, 50, 76, 25, "Diskman", 0);

    draw_text_clip(18, TOP_H + 14, "Places", 0x001D3F66, 0x00DDEBFF, SIDE_W - 28);
    draw_sidebar_button(TOP_H + 42, "Root", "/");
    draw_sidebar_button(TOP_H + 74, "Home", "/home");
    draw_sidebar_button(TOP_H + 106, "Apps", "/apps");
    draw_sidebar_button(TOP_H + 138, "Audio", "/usr/share/audio");
    draw_sidebar_button(TOP_H + 170, "Volumes", "/volumes");
    draw_button(12, h - STATUS_H - 42, SIDE_W - 24, 26, "Open Path", 0);

    gui_fill_rect(SIDE_W + 1, TOP_H, w - SIDE_W - 1, h - TOP_H - STATUS_H, 0x00FFFFFF);
    gui_draw_hline(SIDE_W + 1, TOP_H, w - SIDE_W - 1, 0x00C9DAF2);

    for (int i = page_offset; i < item_count && i < page_offset + layout_visible; i++) {
        draw_item(&items[i], i, tick);
    }

    if (item_count == 0) {
        draw_text_clip(SIDE_W + 28, TOP_H + 34, "This folder is empty.", 0x0064758B, 0x00FFFFFF, w - SIDE_W - 56);
    }

    if (page_offset > 0) draw_button(w - 176, h - STATUS_H - 30, 72, 22, "Previous", 0);
    if (page_offset + layout_visible < item_count) draw_button(w - 94, h - STATUS_H - 30, 72, 22, "Next", 0);

    gui_fill_rect(0, h - STATUS_H, w, STATUS_H, 0x00EAF2FF);
    gui_draw_hline(0, h - STATUS_H, w, 0x0092B7E8);
    uint_to_text((uint64_t)item_count, count_buf, sizeof(count_buf));
    draw_text_clip(12, h - 25, count_buf, 0x001D3F66, 0x00EAF2FF, 56);
    draw_text_clip(36, h - 25, "items", 0x001D3F66, 0x00EAF2FF, 56);
    draw_text_clip(96, h - 25, status_text, 0x00334455, 0x00EAF2FF, 340);

    if ((long)icda_audio_info(&audio) >= 0 && audio.active) {
        char secs[32];
        uint_to_text(audio.seconds_left, secs, sizeof(secs));
        draw_text_clip(w - 284, h - 25, "Playing:", 0x007C3AED, 0x00EAF2FF, 72);
        draw_text_clip(w - 212, h - 25, audio.name, 0x001F2937, 0x00EAF2FF, 130);
        draw_text_clip(w - 74, h - 25, secs, 0x001F2937, 0x00EAF2FF, 32);
        draw_text_clip(w - 42, h - 25, "s", 0x001F2937, 0x00EAF2FF, 16);
    } else {
        draw_text_clip(w - 120, h - 25, "Audio idle", 0x0064758B, 0x00EAF2FF, 100);
    }
}

static void perform_create(int make_dir) {
    char full[PATH_CAP];
    if (!valid_new_name(dialog_input)) {
        set_status("Name is not valid");
        return;
    }
    path_join(full, sizeof(full), current_path, dialog_input);
    if (make_dir) {
        if ((long)icda_mkdir(full) < 0) {
            set_status("Could not create folder");
            return;
        }
        set_status("Folder created");
    } else {
        if ((long)icda_create(full) < 0) {
            set_status("Could not create file");
            return;
        }
        set_status("File created");
    }
    dialog = DIALOG_NONE;
    browser_refresh();
}

static void perform_goto(void) {
    icda_stat_t st;
    if (!dialog_input[0]) return;
    if ((long)icda_stat(dialog_input, &st) < 0 || st.type != 2) {
        set_status("Path is not a folder");
        return;
    }
    dialog = DIALOG_NONE;
    navigate_to(dialog_input, 1);
}

static void draw_dialog(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int x = (w - 390) / 2;
    int y = (h - 150) / 2;
    if (dialog == DIALOG_NONE) return;
    draw_panel(x + 4, y + 5, 390, 150, 0x0064758B, 0x0064758B);
    draw_panel(x, y, 390, 150, 0x00F8FBFF, 0x003D8BFF);
    draw_gradient(x + 1, y + 1, 388, 30, 0x003D8BFF, 0x001F5EBE);
    draw_text_clip(x + 12, y + 8, dialog_title, 0x00FFFFFF, 0x002C73D2, 260);
    gui_fill_rect(x + 20, y + 58, 350, 28, 0x00FFFFFF);
    gui_draw_rect_outline(x + 20, y + 58, 350, 28, 0x006EA6E8);
    draw_text_clip(x + 28, y + 64, dialog_input, 0x001F2937, 0x00FFFFFF, 320);
    gui_fill_rect(x + 28 + dialog_cursor * 8, y + 63, 2, 18, 0x003D8BFF);
    draw_button(x + 210, y + 108, 72, 25, "OK", 1);
    draw_button(x + 292, y + 108, 78, 25, "Cancel", 0);
}

static uint64_t editor_line_start(uint64_t pos) {
    if (pos > editor_len) pos = editor_len;
    while (pos > 0 && editor_buf[pos - 1] != '\n') pos--;
    return pos;
}

static uint64_t editor_line_end(uint64_t pos) {
    if (pos > editor_len) pos = editor_len;
    while (pos < editor_len && editor_buf[pos] != '\n') pos++;
    return pos;
}

static uint64_t editor_cursor_row(void) {
    uint64_t row = 0;
    for (uint64_t i = 0; i < editor_cursor && i < editor_len; i++) {
        if (editor_buf[i] == '\n') row++;
    }
    return row;
}

static uint64_t editor_find_row_start(uint64_t row) {
    uint64_t pos = 0;
    uint64_t r = 0;
    while (pos < editor_len && r < row) {
        if (editor_buf[pos++] == '\n') r++;
    }
    return pos;
}

static uint64_t editor_column(void) {
    return editor_cursor - editor_line_start(editor_cursor);
}

static void editor_ensure_visible(void) {
    int h = gui_window_height();
    int rows = (h - 130) / FONT_CELL_HEIGHT;
    int row = (int)editor_cursor_row();
    if (rows < 1) rows = 1;
    if (row < editor_top_row) editor_top_row = row;
    if (row >= editor_top_row + rows) editor_top_row = row - rows + 1;
    if (editor_top_row < 0) editor_top_row = 0;
}

static void editor_insert(char ch) {
    if (editor_len + 1 >= EDIT_CAP) {
        set_status("Editor buffer full");
        return;
    }
    for (uint64_t i = editor_len; i > editor_cursor; i--) {
        editor_buf[i] = editor_buf[i - 1];
    }
    editor_buf[editor_cursor++] = ch;
    editor_len++;
    editor_buf[editor_len] = 0;
    editor_modified = 1;
    editor_ensure_visible();
}

static void editor_backspace(void) {
    if (editor_cursor == 0) return;
    for (uint64_t i = editor_cursor - 1; i < editor_len; i++) {
        editor_buf[i] = editor_buf[i + 1];
    }
    editor_cursor--;
    editor_len--;
    editor_modified = 1;
    editor_ensure_visible();
}

static void editor_delete(void) {
    if (editor_cursor >= editor_len) return;
    for (uint64_t i = editor_cursor; i < editor_len; i++) {
        editor_buf[i] = editor_buf[i + 1];
    }
    editor_len--;
    editor_modified = 1;
    editor_ensure_visible();
}

static void editor_move_vertical(int down) {
    uint64_t start = editor_line_start(editor_cursor);
    uint64_t col = editor_cursor - start;
    uint64_t target_start;
    uint64_t target_end;

    if (down) {
        uint64_t end = editor_line_end(editor_cursor);
        if (end >= editor_len) return;
        target_start = end + 1;
        target_end = editor_line_end(target_start);
    } else {
        if (start == 0) return;
        target_end = start - 1;
        target_start = editor_line_start(target_end);
    }
    editor_cursor = target_start + col;
    if (editor_cursor > target_end) editor_cursor = target_end;
    editor_ensure_visible();
}

static void editor_save(void) {
    if ((long)icda_write_file(editor_path, editor_buf, editor_len) < 0) {
        set_status("Save failed");
        return;
    }
    editor_modified = 0;
    set_status("Saved");
    browser_refresh();
}

static void open_editor_path(const char *path) {
    long rc;
    if (!path || !*path) return;
    rc = (long)icda_read_file(path, editor_buf, sizeof(editor_buf) - 1);
    if (rc < 0) {
        set_status("Could not open file");
        return;
    }
    editor_len = (uint64_t)rc;
    editor_buf[editor_len] = 0;
    editor_cursor = editor_len;
    editor_top_row = 0;
    editor_modified = 0;
    d_copy(editor_path, path, sizeof(editor_path));
    mode = MODE_EDITOR;
    set_status("Editing file");
    editor_ensure_visible();
}

static void open_selected(void) {
    if (selected_item < 0 || selected_item >= item_count) {
        set_status("Select an item first");
        return;
    }
    if (items[selected_item].is_dir) {
        navigate_to(items[selected_item].path, 1);
        return;
    }
    if (items[selected_item].is_wav) {
        if ((long)icda_play_audio_file(items[selected_item].path) < 0) {
            set_status("Could not play WAV");
        } else {
            set_status("Playing WAV");
        }
        return;
    }
    if (items[selected_item].is_app) {
        if ((long)icda_spawn(items[selected_item].path) < 0) set_status("Could not launch app");
        else set_status("App launched");
        return;
    }
    open_editor_path(items[selected_item].path);
}

static void play_selected(void) {
    if (selected_item < 0 || selected_item >= item_count || !items[selected_item].is_wav) {
        set_status("Select a WAV file");
        return;
    }
    if ((long)icda_play_audio_file(items[selected_item].path) < 0) set_status("Could not play WAV");
    else set_status("Playing WAV");
}

static void draw_editor(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int area_x = 14;
    int area_y = 84;
    int area_w = w - 28;
    int area_h = h - area_y - STATUS_H - 12;
    int rows = area_h / FONT_CELL_HEIGHT;
    int cols = (area_w - 58) / FONT_CELL_WIDTH;
    uint64_t pos = editor_find_row_start((uint64_t)editor_top_row);
    uint64_t cursor_row = editor_cursor_row();
    uint64_t cursor_col = editor_column();

    if (rows < 1) rows = 1;
    if (cols < 8) cols = 8;

    draw_gradient(0, 0, w, h, 0x00F9FCFF, 0x00EEF6FF);
    draw_gradient(0, 0, w, 70, 0x002B6FD4, 0x001D4FA8);
    draw_text_clip(14, 12, "ICDA Notepad", 0x00FFFFFF, 0x002762C4, 160);
    draw_text_clip(14, 36, editor_path, 0x00EAF2FF, 0x002762C4, w - 220);
    draw_button(w - 176, 24, 72, 26, "Save", editor_modified);
    draw_button(w - 94, 24, 76, 26, "Back", 0);

    draw_panel(area_x, area_y, area_w, area_h, 0x00FFFFFF, 0x0092B7E8);
    for (int r = 0; r < rows; r++) {
        char nbuf[16];
        uint64_t line_end;
        uint64_t line_no = (uint64_t)(editor_top_row + r + 1);
        int y = area_y + 4 + r * FONT_CELL_HEIGHT;
        int x = area_x + 8;
        uint_to_text(line_no, nbuf, sizeof(nbuf));
        draw_text_clip(x, y, nbuf, 0x0091A5C4, 0x00FFFFFF, 40);
        x += 50;
        line_end = editor_line_end(pos);
        for (int c = 0; c < cols; c++) {
            uint64_t at = pos + (uint64_t)c;
            char ch = ' ';
            int is_cursor = ((uint64_t)(editor_top_row + r) == cursor_row && (uint64_t)c == cursor_col);
            if (at < line_end) {
                ch = editor_buf[at];
                if (ch < 32 || ch > 126) ch = '.';
            }
            if (is_cursor) {
                gui_fill_rect(x + c * FONT_CELL_WIDTH, y, FONT_CELL_WIDTH, FONT_CELL_HEIGHT, 0x003D8BFF);
                gui_draw_char(x + c * FONT_CELL_WIDTH, y, ch, 0x00FFFFFF, 0x003D8BFF);
            } else {
                gui_draw_char(x + c * FONT_CELL_WIDTH, y, ch, 0x001F2937, 0x00FFFFFF);
            }
        }
        pos = line_end;
        if (pos < editor_len && editor_buf[pos] == '\n') pos++;
    }

    gui_fill_rect(0, h - STATUS_H, w, STATUS_H, 0x00EAF2FF);
    gui_draw_hline(0, h - STATUS_H, w, 0x0092B7E8);
    draw_text_clip(12, h - 25, editor_modified ? "Modified" : "Saved", editor_modified ? 0x00B45309 : 0x001D6F42, 0x00EAF2FF, 92);
    draw_text_clip(112, h - 25, status_text, 0x00334455, 0x00EAF2FF, w - 130);
}

static void open_dialog(int kind, const char *title, const char *initial) {
    dialog = kind;
    d_copy(dialog_title, title, sizeof(dialog_title));
    d_copy(dialog_input, initial ? initial : "", sizeof(dialog_input));
    dialog_cursor = (int)d_strlen(dialog_input);
}

static void browser_select_move(int dx, int dy) {
    int next;
    if (item_count <= 0) return;
    if (selected_item < 0) selected_item = 0;
    next = selected_item + dx + dy * layout_cols;
    if (next < 0) next = 0;
    if (next >= item_count) next = item_count - 1;
    selected_item = next;
    if (selected_item < page_offset) page_offset = (selected_item / layout_visible) * layout_visible;
    if (selected_item >= page_offset + layout_visible) page_offset = (selected_item / layout_visible) * layout_visible;
}

static void browser_special_key(int special) {
    if (special == SPECIAL_LEFT) browser_select_move(-1, 0);
    else if (special == SPECIAL_RIGHT) browser_select_move(1, 0);
    else if (special == SPECIAL_UP) browser_select_move(0, -1);
    else if (special == SPECIAL_DOWN) browser_select_move(0, 1);
}

static void editor_special_key(int special) {
    if (special == SPECIAL_LEFT) {
        if (editor_cursor > 0) editor_cursor--;
        editor_ensure_visible();
    } else if (special == SPECIAL_RIGHT) {
        if (editor_cursor < editor_len) editor_cursor++;
        editor_ensure_visible();
    } else if (special == SPECIAL_UP) {
        editor_move_vertical(0);
    } else if (special == SPECIAL_DOWN) {
        editor_move_vertical(1);
    } else if (special == SPECIAL_DELETE) {
        editor_delete();
    }
}

static void handle_dialog_key(uint32_t key);

static void handle_special_key(int special) {
    if (dialog != DIALOG_NONE) {
        /* The dialog's text field understands the same special keys
         * (arrows, Delete); Up/Down are ignored there. */
        handle_dialog_key((uint32_t)special);
        return;
    }
    if (mode == MODE_EDITOR) editor_special_key(special);
    else browser_special_key(special);
}

static int feed_escape_sequence(uint32_t key) {
    if (key_seq_state == 0) {
        if (key == 27) {
            key_seq_state = 1;
            return 1;
        }
        return 0;
    }
    if (key_seq_state == 1) {
        key_seq_state = key == '[' ? 2 : 0;
        return 1;
    }
    if (key_seq_state == 2) {
        key_seq_state = 0;
        if (key == 'A') handle_special_key(SPECIAL_UP);
        else if (key == 'B') handle_special_key(SPECIAL_DOWN);
        else if (key == 'C') handle_special_key(SPECIAL_RIGHT);
        else if (key == 'D') handle_special_key(SPECIAL_LEFT);
        else if (key == '3') key_seq_state = 3;
        return 1;
    }
    if (key_seq_state == 3) {
        key_seq_state = 0;
        if (key == '~') handle_special_key(SPECIAL_DELETE);
        return 1;
    }
    key_seq_state = 0;
    return 0;
}

static void dialog_remove_at(int at) {
    uint64_t len = d_strlen(dialog_input);
    for (uint64_t i = (uint64_t)at; i + 1 < sizeof(dialog_input) && i < len; i++) {
        dialog_input[i] = dialog_input[i + 1];
    }
}

static void handle_dialog_key(uint32_t key) {
    uint64_t len;
    if (key == 27) {
        dialog = DIALOG_NONE;
        return;
    }
    if (key == '\r' || key == '\n') {
        if (dialog == DIALOG_NEW_FILE) perform_create(0);
        else if (dialog == DIALOG_NEW_FOLDER) perform_create(1);
        else if (dialog == DIALOG_GOTO) perform_goto();
        return;
    }
    if (key == '\b') {
        if (dialog_cursor > 0) {
            dialog_cursor--;
            dialog_remove_at(dialog_cursor);
        }
        return;
    }
    if (key == SPECIAL_LEFT) {
        if (dialog_cursor > 0) dialog_cursor--;
        return;
    }
    if (key == SPECIAL_RIGHT) {
        len = d_strlen(dialog_input);
        if ((uint64_t)dialog_cursor < len) dialog_cursor++;
        return;
    }
    if (key == SPECIAL_DELETE) {
        len = d_strlen(dialog_input);
        if ((uint64_t)dialog_cursor < len) dialog_remove_at(dialog_cursor);
        return;
    }
    if (key >= 32 && key <= 126 && dialog_cursor + 1 < (int)sizeof(dialog_input)) {
        len = d_strlen(dialog_input);
        if ((uint64_t)dialog_cursor > len) dialog_cursor = (int)len;
        for (uint64_t i = len; i > (uint64_t)dialog_cursor; i--) {
            if (i + 1 < sizeof(dialog_input)) dialog_input[i] = dialog_input[i - 1];
        }
        dialog_input[dialog_cursor++] = (char)key;
        dialog_input[(uint64_t)dialog_cursor] = 0;
    }
}

static void handle_editor_key(uint32_t key) {
    if (key == 19) {
        editor_save();
        return;
    }
    if (key == 24) {
        mode = MODE_BROWSER;
        browser_refresh();
        return;
    }
    if (key == '\b') {
        editor_backspace();
        return;
    }
    if (key == '\r' || key == '\n') {
        editor_insert('\n');
        return;
    }
    if (key >= 32 && key <= 126) {
        editor_insert((char)key);
    }
}

static void handle_browser_key(uint32_t key) {
    if (key == '\r' || key == '\n') open_selected();
    else if (key == '\b') navigate_up();
    else if (key == 'r' || key == 'R') browser_refresh();
    else if (key == 'n' || key == 'N') open_dialog(DIALOG_NEW_FILE, "Create new file", "");
    else if (key == 'f' || key == 'F') open_dialog(DIALOG_NEW_FOLDER, "Create new folder", "");
    else if (key == 'e' || key == 'E') {
        if (selected_item >= 0 && !items[selected_item].is_dir) open_editor_path(items[selected_item].path);
        else set_status("Select a file to edit");
    } else if (key == 'p' || key == 'P') play_selected();
    else if (key == 't' || key == 'T') icda_spawn("/apps/terminal.app");
}

static void handle_key(uint32_t key) {
    if (dialog != DIALOG_NONE) {
        /* Parse arrow/delete escape sequences first so the dialog can
         * edit like a real text field; without this the ESC of an arrow
         * key closes the dialog and '[' 'A' get typed into the name. */
        if (feed_escape_sequence(key)) return;
        handle_dialog_key(key);
        return;
    }
    if (feed_escape_sequence(key)) return;
    if (mode == MODE_EDITOR) handle_editor_key(key);
    else handle_browser_key(key);
}

static void handle_dialog_click(int mx, int my) {
    int w = gui_window_width();
    int h = gui_window_height();
    int x = (w - 390) / 2;
    int y = (h - 150) / 2;
    if (hit_rect(mx, my, x + 210, y + 108, 72, 25)) {
        if (dialog == DIALOG_NEW_FILE) perform_create(0);
        else if (dialog == DIALOG_NEW_FOLDER) perform_create(1);
        else if (dialog == DIALOG_GOTO) perform_goto();
    } else if (hit_rect(mx, my, x + 292, y + 108, 78, 25)) {
        dialog = DIALOG_NONE;
    }
}

static void handle_editor_click(int mx, int my) {
    int w = gui_window_width();
    int area_x = 14;
    int area_y = 84;
    int area_w = w - 28;
    if (hit_rect(mx, my, w - 176, 24, 72, 26)) {
        editor_save();
        return;
    }
    if (hit_rect(mx, my, w - 94, 24, 76, 26)) {
        mode = MODE_BROWSER;
        browser_refresh();
        return;
    }
    if (hit_rect(mx, my, area_x, area_y, area_w, gui_window_height() - area_y - STATUS_H - 12)) {
        int col = (mx - area_x - 58) / FONT_CELL_WIDTH;
        int row = (my - area_y - 4) / FONT_CELL_HEIGHT;
        uint64_t line_start;
        uint64_t line_end;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        line_start = editor_find_row_start((uint64_t)(editor_top_row + row));
        line_end = editor_line_end(line_start);
        editor_cursor = line_start + (uint64_t)col;
        if (editor_cursor > line_end) editor_cursor = line_end;
        editor_ensure_visible();
    }
}

static void handle_browser_click(int mx, int my) {
    int w = gui_window_width();
    int h = gui_window_height();
    uint64_t now = icda_ticks();

    if (hit_rect(mx, my, 12, 12, w - 24, 26)) {
        open_dialog(DIALOG_GOTO, "Open path", current_path);
        return;
    }
    if (hit_rect(mx, my, 12, 50, 50, 25)) { navigate_back(); return; }
    if (hit_rect(mx, my, 68, 50, 42, 25)) { navigate_up(); return; }
    if (hit_rect(mx, my, 116, 50, 62, 25)) { browser_refresh(); return; }
    if (hit_rect(mx, my, 190, 50, 72, 25)) { open_dialog(DIALOG_NEW_FILE, "Create new file", ""); return; }
    if (hit_rect(mx, my, 268, 50, 86, 25)) { open_dialog(DIALOG_NEW_FOLDER, "Create new folder", ""); return; }
    if (hit_rect(mx, my, 366, 50, 50, 25)) {
        if (selected_item >= 0 && !items[selected_item].is_dir) open_editor_path(items[selected_item].path);
        else set_status("Select a file to edit");
        return;
    }
    if (hit_rect(mx, my, 422, 50, 54, 25)) { play_selected(); return; }
    if (hit_rect(mx, my, 482, 50, 54, 25)) { icda_stop_audio(); set_status("Audio stopped"); return; }
    if (hit_rect(mx, my, w - 174, 50, 78, 25)) { icda_spawn("/apps/terminal.app"); return; }
    if (hit_rect(mx, my, w - 88, 50, 76, 25)) { icda_spawn("/apps/diskman.app"); return; }

    if (hit_rect(mx, my, 12, TOP_H + 42, SIDE_W - 24, 26)) { navigate_to("/", 1); return; }
    if (hit_rect(mx, my, 12, TOP_H + 74, SIDE_W - 24, 26)) { navigate_to("/home", 1); return; }
    if (hit_rect(mx, my, 12, TOP_H + 106, SIDE_W - 24, 26)) { navigate_to("/apps", 1); return; }
    if (hit_rect(mx, my, 12, TOP_H + 138, SIDE_W - 24, 26)) { navigate_to("/usr/share/audio", 1); return; }
    if (hit_rect(mx, my, 12, TOP_H + 170, SIDE_W - 24, 26)) { navigate_to("/volumes", 1); return; }
    if (hit_rect(mx, my, 12, h - STATUS_H - 42, SIDE_W - 24, 26)) { open_dialog(DIALOG_GOTO, "Open path", current_path); return; }

    if (page_offset > 0 && hit_rect(mx, my, w - 176, h - STATUS_H - 30, 72, 22)) {
        page_offset -= layout_visible;
        if (page_offset < 0) page_offset = 0;
        return;
    }
    if (page_offset + layout_visible < item_count && hit_rect(mx, my, w - 94, h - STATUS_H - 30, 72, 22)) {
        page_offset += layout_visible;
        return;
    }

    for (int i = page_offset; i < item_count && i < page_offset + layout_visible; i++) {
        if (hit_rect(mx, my, items[i].x, items[i].y, items[i].w, items[i].h)) {
            int double_click = (last_click_item == i && now - last_click_tick < 35);
            selected_item = i;
            set_status(items[i].is_dir ? "Folder selected" : "File selected");
            if (double_click) open_selected();
            last_click_item = i;
            last_click_tick = now;
            return;
        }
    }
    selected_item = -1;
}

static void handle_mouse(gui_msg_t *msg) {
    int mx = msg->mouse.x;
    int my = msg->mouse.y;
    int w = gui_window_width();
    int h = gui_window_height();

    if (mode == MODE_BROWSER) {
        hover_item = -1;
        layout_items(w, h);
        for (int i = page_offset; i < item_count && i < page_offset + layout_visible; i++) {
            if (hit_rect(mx, my, items[i].x, items[i].y, items[i].w, items[i].h)) {
                hover_item = i;
                break;
            }
        }
    }

    if (msg->mouse.buttons & GUI_BTN_LEFT) {
        if (dialog != DIALOG_NONE) {
            handle_dialog_click(mx, my);
        } else if (mode == MODE_EDITOR) {
            handle_editor_click(mx, my);
        } else {
            handle_browser_click(mx, my);
        }
    }
}

static void draw_all(void) {
    if (mode == MODE_EDITOR) draw_editor();
    else draw_browser();
    draw_dialog();
    gui_flush();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (gui_open_window("ICDA Explorer", DESKTOP_W, DESKTOP_H) != 0) {
        return -1;
    }

    /* File/folder icons come from /usr/share/icons when present. */
    ic_icon_load_folder("/usr/share/icons");

    browser_refresh();
    draw_all();

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_MOUSE_EVENT) {
                handle_mouse(&msg);
            } else if (msg.type == GUI_MSG_KEY_EVENT && msg.key.pressed) {
                handle_key(msg.key.keycode);
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
        }
        /* Redraw immediately on input; otherwise keep ~20fps for the
         * selection blink / status animations instead of repainting the
         * whole window (and flushing to the WM) 100 times a second. */
        if (changed || (icda_ticks() % 5) == 0) {
            draw_all();
        }
        icda_sleep(1);
    }
}
