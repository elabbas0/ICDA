












#include "libicda.h"
#include "settings_store.h"

#define WIN_W 780
#define WIN_H 520

#define PATH_CAP     512
#define NAME_CAP     256
#define STATUS_CAP   128
#define LIST_CAP     65536
#define MAX_ITEMS    1024
#define HISTORY_CAP  16
#define DIALOG_CAP   PATH_CAP
#define STATUS_H     24
#define GRID_CELL_W  104
#define GRID_CELL_H  88
#define ROW_H        IC_H_ROW


enum { VIEW_GRID = 0, VIEW_LIST };


enum { DLG_NONE = 0, DLG_NEW_FILE, DLG_NEW_FOLDER, DLG_GOTO, DLG_RENAME, DLG_DELETE };




enum {
    CA_OPEN = 1,
    CA_EDIT,
    CA_RENAME,
    CA_INFO,
    CA_NEW_FILE,
    CA_NEW_FOLDER,
    CA_OPEN_TERMINAL,
    CA_REFRESH
};

typedef struct {
    char     name[NAME_CAP];
    char     path[PATH_CAP];
    int      is_dir;
    int      is_app;
    int      is_wav;
    uint64_t size;
    uint8_t  readonly;
} ex_item_t;

static struct {
    ex_item_t items[MAX_ITEMS];
    int       count;
    int       selected;          
    int       hover;              

    char      path[PATH_CAP];
    char      history[HISTORY_CAP][PATH_CAP];
    int       history_count;
    int       history_pos;       

    int       view;
    int       scroll;             
    int       rows;               
    int       cols;               
    int       first_item;         
    int       followed;
    int       last_item;

    
    int hover_back;
    int hover_up;
    int hover_view;
    int hover_new_folder;
    int hover_new_file;
    int hover_sidebar;
    int list_focused;
    int last_click_item;
    uint32_t last_click_ms;

    
    int  dialog;
    char dialog_title[48];
    char dialog_buf[DIALOG_CAP];
    int  dialog_cursor;
    int  dialog_scroll;

    
    ic_menu_model_t menu;
    int       menu_actions[IC_MENU_ITEMS_MAX];
    int  menu_x, menu_y;
    int  menu_open;
    int  menu_item;               
    int  menu_hover;

    
    int  info_open;

    char list_buf[LIST_CAP];
    char status[STATUS_CAP];
} ex;



static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t sidebar_rect(ic_app_t *app) {
    return ic_rect_make(0, IC_H_TOOLBAR, IC_W_SIDEBAR, app->height - IC_H_TOOLBAR - STATUS_H);
}

static ic_rect_t content_rect(ic_app_t *app) {
    ic_rect_t s = sidebar_rect(app);
    return ic_rect_make(s.x + s.w, s.y, app->width - s.x - s.w, s.h);
}

static ic_rect_t status_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - STATUS_H, app->width, STATUS_H);
}

static ic_rect_t back_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    return ic_rect_make(b.x + IC_SP_2, (b.h - IC_H_CONTROL) / 2, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t up_rect(ic_app_t *app) {
    ic_rect_t r = back_rect(app);
    return ic_rect_make(r.x + r.w + IC_SP_1, r.y, IC_H_CONTROL, IC_H_CONTROL);
}







static const char *toolbar_label(int wide, const char *long_label, const char *short_label) {
    return wide ? long_label : short_label;
}

static int toolbar_wide(ic_app_t *app) {
    return app->width >= 880;
}

static ic_rect_t new_file_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    const char *label = toolbar_label(toolbar_wide(app), "New File", "File");
    int w = ic_ui_button_width(label, IC_SYM_PLUS);
    return ic_rect_make(b.w - IC_SP_3 - w, (b.h - IC_H_CONTROL) / 2, w, IC_H_CONTROL);
}

static ic_rect_t new_folder_rect(ic_app_t *app) {
    ic_rect_t n = new_file_rect(app);
    const char *label = toolbar_label(toolbar_wide(app), "New Folder", "Folder");
    int w = ic_ui_button_width(label, IC_SYM_FOLDER);
    return ic_rect_make(n.x - IC_SP_2 - w, n.y, w, IC_H_CONTROL);
}

static ic_rect_t view_rect(ic_app_t *app) {
    ic_rect_t n = new_folder_rect(app);
    return ic_rect_make(n.x - IC_SP_2 - IC_H_CONTROL, n.y, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t path_rect(ic_app_t *app) {
    ic_rect_t r = up_rect(app);
    ic_rect_t v = view_rect(app);
    int x = r.x + r.w + IC_SP_2;
    int w = v.x - IC_SP_2 - x;
    if (w < 60) w = 60;
    return ic_rect_make(x, (toolbar_rect(app).h - IC_H_CONTROL) / 2, w, IC_H_CONTROL);
}


static const char *const PLACES[] = {
    "/", "/home", "/apps", "/usr/share/audio", "/usr/share/apps", "/etc", "/cfg"
};
#define PLACE_COUNT ((int)(sizeof(PLACES) / sizeof(PLACES[0])))

static const ic_symbol_t PLACE_SYMBOLS[PLACE_COUNT] = {
    IC_SYM_DISK, IC_SYM_FOLDER, IC_SYM_GRID, IC_SYM_MUSIC, IC_SYM_FOLDER, IC_SYM_GEAR,
    IC_SYM_GEAR
};



#define PLACE_HEADER_H 18

static ic_rect_t place_rect(ic_app_t *app, int i) {
    ic_rect_t s = sidebar_rect(app);
    return ic_rect_make(s.x, s.y + IC_SP_3 + PLACE_HEADER_H + i * (IC_H_ROW + 2),
                        s.w, IC_H_ROW);
}

static ic_rect_t item_rect(ic_app_t *app, int index) {
    ic_rect_t c = content_rect(app);
    if (ex.view == VIEW_LIST) {
        int first = ex.first_item;
        return ic_rect_make(c.x + IC_SP_2, c.y + IC_SP_2 + (index - first) * ROW_H,
                            c.w - 2 * IC_SP_2 - IC_SP_2, ROW_H);
    }
    {
        int slot = index - ex.scroll;
        int col = slot % ex.cols;
        int row = slot / ex.cols;
        return ic_rect_make(c.x + IC_SP_3 + col * GRID_CELL_W, c.y + IC_SP_3 + row * GRID_CELL_H,
                            GRID_CELL_W - IC_SP_2, GRID_CELL_H - IC_SP_2);
    }
}

static int can_go_back(void) { return ex.history_pos > 0; }

static int can_go_up(void) { return !ic_streq(ex.path, "/"); }

static void layout(ic_app_t *app) {
    ic_rect_t c = content_rect(app);
    if (ex.view == VIEW_LIST) {
        int rows = (c.h - 2 * IC_SP_2) / ROW_H;
        if (rows < 1) rows = 1;
        ex.rows = rows;
        ex.cols = 1;
        if (ex.selected != ex.followed) {
            if (ex.selected < 0) ex.first_item = 0;
            else if (ex.selected < ex.first_item) ex.first_item = ex.selected;
            else if (ex.selected >= ex.first_item + rows) ex.first_item = ex.selected - rows + 1;
            ex.followed = ex.selected;
        }
        if (ex.first_item > ex.count - rows) ex.first_item = ex.count - rows;
        if (ex.first_item < 0) ex.first_item = 0;
        ex.last_item = ex.first_item + rows;
        if (ex.last_item > ex.count) ex.last_item = ex.count;
    } else {
        int cols = (c.w - 2 * IC_SP_3) / GRID_CELL_W;
        int rows;
        if (cols < 1) cols = 1;
        rows = (c.h - 2 * IC_SP_3) / GRID_CELL_H;
        if (rows < 1) rows = 1;
        ex.cols = cols;
        ex.rows = rows;
        if (ex.selected != ex.followed) {
            if (ex.selected < 0) {
                ex.scroll = 0;
            } else {
                int row = ex.selected / cols;
                int top = ex.scroll / cols;
                if (row < top) ex.scroll = row * cols;
                else if (row >= top + rows) ex.scroll = (row - rows + 1) * cols;
            }
            ex.followed = ex.selected;
        }
        {
            int total_rows = (ex.count + cols - 1) / cols;
            int max_top = total_rows - rows;
            int top = ex.scroll / cols;
            if (max_top < 0) max_top = 0;
            if (top > max_top) top = max_top;
            if (top < 0) top = 0;
            ex.scroll = top * cols;
        }
    }
}



static uint64_t d_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void d_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int d_streq(const char *a, const char *b) {
    uint64_t i = 0;
    if (!a || !b) return a == b;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static char d_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int has_suffix(const char *text, const char *suffix) {
    uint64_t tl = d_strlen(text);
    uint64_t sl = d_strlen(suffix);
    if (tl < sl) return 0;
    for (uint64_t i = 0; i < sl; i++) {
        if (d_lower(text[tl - sl + i]) != suffix[i]) return 0;
    }
    return 1;
}

static void ex_status(const char *text) {
    d_copy(ex.status, text, STATUS_CAP);
}

static void size_text(uint64_t bytes, char *out, uint64_t cap) {
    static const char *const units[4] = { "bytes", "KB", "MB", "GB" };
    uint64_t whole = bytes;
    int unit = 0;
    if (cap == 0) return;
    while (whole >= 1000 && unit < 3) { whole /= 1000; unit++; }
    out[0] = 0;
    ic_snprintf_u64(out, cap, whole);
    ic_strlcat(out, " ", cap);
    ic_strlcat(out, units[unit], cap);
}

static void path_join(char *out, uint64_t cap, const char *dir, const char *name) {
    d_copy(out, (dir && *dir) ? dir : "/", cap);
    if (!d_streq(out, "/")) ic_strlcat(out, "/", cap);
    ic_strlcat(out, name, cap);
}

static void path_parent(char *out, uint64_t cap, const char *path) {
    uint64_t len;
    d_copy(out, (path && *path) ? path : "/", cap);
    len = d_strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = 0;
    if (len <= 1) { d_copy(out, "/", cap); return; }
    while (len > 1 && out[len - 1] != '/') len--;
    if (len <= 1) d_copy(out, "/", cap);
    else out[len - 1] = 0;
}

static int valid_name(const char *name) {
    uint64_t len = d_strlen(name);
    if (len == 0 || len >= NAME_CAP) return 0;
    for (uint64_t i = 0; i < len; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || c < 32) return 0;
    }
    return 1;
}

static int valid_path(const char *path) {
    uint64_t len = d_strlen(path);
    if (len == 0 || len >= PATH_CAP) return 0;
    for (uint64_t i = 0; i < len; i++) {
        char c = path[i];
        if (c == '\\' || c < 32) return 0;
    }
    return path[0] == '/';
}



static void refresh(void) {
    uint64_t rc;
    uint64_t pos = 0;
    ex.count = 0;
    ex.hover = -1;
    ex.scroll = 0;
    ex.first_item = 0;

    rc = icda_list_dir(ex.path, ex.list_buf, sizeof(ex.list_buf) - 1);
    if ((long)rc < 0) {
        ex_status("That folder could not be read");
        ex.count = 0;
        ex.selected = -1;
        return;
    }
    ex.list_buf[rc] = 0;

    while (pos < rc && ex.count < MAX_ITEMS) {
        char entry[NAME_CAP];
        uint64_t ei = 0;
        int is_dir = 0;
        ex_item_t *it = &ex.items[ex.count];

        while (pos < rc && ex.list_buf[pos] != '\n' && ei + 1 < sizeof(entry)) {
            entry[ei++] = ex.list_buf[pos++];
        }
        while (pos < rc && ex.list_buf[pos] != '\n') pos++;
        if (pos < rc) pos++;
        entry[ei] = 0;
        if (ei == 0) continue;
        if (entry[ei - 1] == '/') { entry[ei - 1] = 0; is_dir = 1; }
        if (entry[0] == 0) continue;

        d_copy(it->name, entry, sizeof(it->name));
        path_join(it->path, sizeof(it->path), ex.path, entry);
        it->is_dir = is_dir;
        it->is_app = has_suffix(entry, ".app") || has_suffix(entry, ".elf");
        it->is_wav = has_suffix(entry, ".wav");
        it->size = 0;
        it->readonly = 0;
        {
            icda_stat_t st;
            if ((long)icda_stat(it->path, &st) >= 0) {
                it->size = st.size;
                it->readonly = st.readonly;
                it->is_dir = st.type == 2;
            }
        }
        ex.count++;
    }
    if (ex.selected >= ex.count) ex.selected = -1;
    if (ex.count == 0) ex_status("This folder is empty");
    else ex_status("Ready");
}

static void navigate_to(const char *path, int record_history) {
    if (!valid_path(path)) {
        ex_status("That is not a valid path");
        return;
    }
    if (record_history && !d_streq(path, ex.path)) {
        if (ex.history_pos + 1 < HISTORY_CAP) {
            ex.history_pos++;
            d_copy(ex.history[ex.history_pos], ex.path, PATH_CAP);
            ex.history_count = ex.history_pos + 1;
        }
    }
    d_copy(ex.path, path, PATH_CAP);
    ex.selected = -1;
    ex.scroll = 0;
    ex.first_item = 0;
    refresh();
}

static void go_back(void) {
    if (!can_go_back()) return;
    ex.history_pos--;
    d_copy(ex.path, ex.history[ex.history_pos], PATH_CAP);
    ex.selected = -1;
    ex.scroll = 0;
    ex.first_item = 0;
    refresh();
}

static void go_up(void) {
    char parent[PATH_CAP];
    if (!can_go_up()) return;
    path_parent(parent, sizeof(parent), ex.path);
    navigate_to(parent, 1);
}



static void open_item(int index) {
    ex_item_t *it;
    if (index < 0 || index >= ex.count) {
        ex_status("Select something first");
        return;
    }
    it = &ex.items[index];
    if (it->is_dir) {
        navigate_to(it->path, 1);
        return;
    }
    if (it->is_wav) {
        icda_settings_t opt;
        icda_settings_load(&opt);
        if (!opt.audio) {
            ex_status("Sound is off. Turn it on in Settings.");
            return;
        }
        if ((long)icda_play_audio_file(it->path) < 0) ex_status("That track could not be played");
        else ex_status("Playing");
        return;
    }
    if (it->is_app) {
        if ((long)icda_spawn(it->path) < 0) ex_status("That app could not be launched");
        else ex_status("App launched");
        return;
    }
    if ((long)icda_spawn_args("/apps/editor.app", it->path) < 0) {
        ex_status("The editor could not be launched");
        return;
    }
    ex_status("Opened in the editor");
}

static void open_in_editor(int index) {
    ex_item_t *it;
    if (index < 0 || index >= ex.count || ex.items[index].is_dir) {
        ex_status("Select a file to edit");
        return;
    }
    it = &ex.items[index];
    if ((long)icda_spawn_args("/apps/editor.app", it->path) < 0) {
        ex_status("The editor could not be launched");
        return;
    }
    ex_status("Opened in the editor");
}

static void create_entry(int make_dir) {
    char path[PATH_CAP];
    uint64_t n;
    if (!valid_name(ex.dialog_buf)) {
        ex_status("That name cannot be used");
        return;
    }
    path_join(path, sizeof(path), ex.path, ex.dialog_buf);
    if (make_dir) n = icda_mkdir(path);
    else n = icda_write_file(path, "", 0);
    if (n == (uint64_t)-1) {
        ex_status("That name is already taken");
        return;
    }
    refresh();
    for (int i = 0; i < ex.count; i++) {
        if (d_streq(ex.items[i].name, ex.dialog_buf)) ex.selected = i;
    }
    ex_status(make_dir ? "Folder created" : "File created");
}




static void open_dialog(int kind, const char *title, const char *initial);

static void perform_rename(void) {
    char *data;
    char from[PATH_CAP];
    char to[PATH_CAP];
    icda_stat_t st;
    long n;
    if (ex.menu_item < 0 || ex.menu_item >= ex.count) {
        ex_status("Select something to rename");
        return;
    }
    if (!valid_name(ex.dialog_buf)) {
        ex_status("That name cannot be used");
        return;
    }
    d_copy(from, ex.items[ex.menu_item].path, sizeof(from));
    path_join(to, sizeof(to), ex.path, ex.dialog_buf);
    if (d_streq(from, to)) return;
    if ((long)icda_stat(to, &st) >= 0) {
        ex_status("The new name is already taken");
        return;
    }
    if (icda_rename(from, to) == 0) {
        refresh();
        for (int i = 0; i < ex.count; i++) {
            if (d_streq(ex.items[i].name, ex.dialog_buf)) ex.selected = i;
        }
        ex_status("Renamed");
        return;
    }
    if (ex.items[ex.menu_item].is_dir) {
        ex_status("That folder could not be renamed");
        return;
    }
    if ((long)icda_stat(from, &st) < 0 || !(data = (char *)ic_malloc(st.size + 1))) {
        ex_status("That item could not be read");
        return;
    }
    n = (long)icda_read_file(from, data, st.size + 1);
    if (n < 0 || icda_write_file(to, data, (uint64_t)n) == (uint64_t)-1) {
        ic_free(data);
        ex_status(n < 0 ? "That item could not be read" : "That name could not be written");
        return;
    }
    ic_free(data);
    icda_remove(from);
    refresh();
    for (int i = 0; i < ex.count; i++) {
        if (d_streq(ex.items[i].name, ex.dialog_buf)) ex.selected = i;
    }
    ex_status("Renamed");
}

static void perform_delete(void) {
    int was_dir;
    if (ex.menu_item < 0 || ex.menu_item >= ex.count) return;
    was_dir = ex.items[ex.menu_item].is_dir;
    if (icda_remove(ex.items[ex.menu_item].path) < 0) {
        ex_status("That item is protected and cannot be deleted");
        return;
    }
    refresh();
    if (ex.selected >= ex.count) ex.selected = ex.count - 1;
    ex_status(was_dir ? "Folder deleted" : "File deleted");
}

static void ask_delete(int item) {
    char title[DIALOG_CAP + 16];
    if (item < 0 || item >= ex.count) return;
    ex.menu_item = item;
    d_copy(title, "Delete \"", sizeof(title));
    d_copy(title + d_strlen(title), ex.items[item].name, sizeof(title) - d_strlen(title));
    d_copy(title + d_strlen(title), "\"?", sizeof(title) - d_strlen(title));
    open_dialog(DLG_DELETE, title, ex.items[item].is_dir ? "The folder and everything in it will be removed."
                                                         : "This file will be removed.");
}



static void open_dialog(int kind, const char *title, const char *initial) {
    ex.dialog = kind;
    d_copy(ex.dialog_title, title, sizeof(ex.dialog_title));
    d_copy(ex.dialog_buf, initial ? initial : "", DIALOG_CAP);
    ex.dialog_cursor = (int)d_strlen(ex.dialog_buf);
    ex.dialog_scroll = 0;
}

static void close_dialog(void) {
    ex.dialog = DLG_NONE;
    ex.dialog_buf[0] = 0;
    ex.dialog_cursor = 0;
}

static void commit_dialog(void) {
    int kind = ex.dialog;
    ex.dialog = DLG_NONE;
    switch (kind) {
    case DLG_NEW_FILE:    create_entry(0); break;
    case DLG_NEW_FOLDER:  create_entry(1); break;
    case DLG_GOTO:        navigate_to(ex.dialog_buf, 1); break;
    case DLG_RENAME:      perform_rename(); break;
    case DLG_DELETE:      perform_delete(); break;
    default: break;
    }
    close_dialog();
}



static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    const ic_palette_t *p = ic_palette();

    ic_ui_toolbar(c, b);
    ic_ui_icon_button(c, back_rect(app), IC_SYM_CHEVRON_LEFT,
                      !can_go_back() ? IC_STATE_DISABLED
                                     : (ex.hover_back ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_icon_button(c, up_rect(app), IC_SYM_CHEVRON_UP,
                      !can_go_up() ? IC_STATE_DISABLED
                                  : (ex.hover_up ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), path_rect(app), ex.path, p->label,
                    IC_ALIGN_LEFT);
    ic_ui_icon_button(c, view_rect(app), ex.view == VIEW_GRID ? IC_SYM_GRID : IC_SYM_DOCUMENT,
                      ex.hover_view ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, new_folder_rect(app), toolbar_label(toolbar_wide(app), "New Folder", "Folder"),
                 IC_SYM_FOLDER, IC_BUTTON_DEFAULT,
                 ex.hover_new_folder ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, new_file_rect(app), toolbar_label(toolbar_wide(app), "New File", "File"),
                 IC_SYM_PLUS, IC_BUTTON_DEFAULT,
                 ex.hover_new_file ? IC_STATE_HOVER : IC_STATE_NORMAL);
}

static void draw_sidebar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = sidebar_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_ui_sidebar_bg(c, s);
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH),
                    ic_rect_make(s.x + IC_SP_3, s.y + IC_SP_3, s.w - IC_SP_4,
                                 PLACE_HEADER_H - IC_SP_2),
                    "PLACES", p->label_tertiary, IC_ALIGN_LEFT);
    for (int i = 0; i < PLACE_COUNT; i++) {
        const char *label = PLACES[i];
        
        char shown[NAME_CAP];
        if (d_streq(PLACES[i], "/")) d_copy(shown, "Disk", sizeof(shown));
        else d_copy(shown, PLACES[i] + 1, sizeof(shown));
        ic_ui_sidebar_item(c, place_rect(app, i), PLACE_SYMBOLS[i], shown,
                           d_streq(ex.path, label),
                           i == ex.hover_sidebar ? 1.0f : 0.0f);
    }
}


static ic_symbol_t item_symbol(const ex_item_t *it) {
    if (it->is_dir) return IC_SYM_FOLDER;
    if (it->is_wav) return IC_SYM_MUSIC;
    if (it->is_app) return IC_SYM_GRID;
    return IC_SYM_DOCUMENT;
}

static void draw_grid(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t area = content_rect(app);
    const ic_palette_t *p = ic_palette();
    int first = ex.scroll;
    int last = ex.scroll + ex.cols * ex.rows;
    const ic_icon_t *icon;

    if (last > ex.count) last = ex.count;
    ic_gfx_fill(c, area.x, area.y, area.w, area.h, p->content);

    for (int i = first; i < last; i++) {
        ic_rect_t r = item_rect(app, i);
        ex_item_t *it = &ex.items[i];
        int sel = i == ex.selected;
        ic_rect_t plate = ic_rect_make(r.x, r.y, r.w, r.h - 14);
        float hover = i == ex.hover ? 1.0f : 0.0f;

        if (sel) ic_gfx_rrect(c, plate.x, plate.y, plate.w, plate.h, IC_R_TILE, p->accent_soft);
        else if (hover > 0.0f) ic_gfx_rrect(c, plate.x, plate.y, plate.w, plate.h, IC_R_TILE,
                                            p->fill_hover);
        icon = ic_icon_builtin(it->is_dir ? "folder" : (it->is_app ? "desktop" : "document"));
        if (icon && ic_icon_valid(icon)) {
            ic_gfx_image_rgba(c, plate.x + (plate.w - 48) / 2, plate.y + 8, 48, 48,
                             icon->rgba, icon->w, icon->h, 255);
        } else {
            ic_symbol_draw(c, item_symbol(it), (float)(plate.x + plate.w / 2),
                           (float)(plate.y + plate.h / 2), 32.0f, p->label_secondary);
        }
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION),
                        ic_rect_make(r.x, plate.y + plate.h + 1, r.w, 13), it->name,
                        sel ? p->label : (hover > 0.0f ? p->label : p->label_secondary),
                        IC_ALIGN_CENTER);
    }

    if (ex.count == 0) {
        ic_ui_empty_state(c, area, IC_SYM_FOLDER, "This folder is empty",
                          "Use New Folder or New File to add something here.");
    } else if (ex.count > ex.cols * ex.rows) {
        ic_ui_scrollbar(c, area, ex.scroll / ex.cols * GRID_CELL_H,
                        (ex.count + ex.cols - 1) / ex.cols * GRID_CELL_H + 2 * IC_SP_3, 1.0f);
    }
}

static void draw_list(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t area = content_rect(app);
    const ic_palette_t *p = ic_palette();
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    const ic_face_t *meta = ic_font(IC_FONT_CAPTION);
    const ic_face_t *mono = ic_font(IC_FONT_MONO_SMALL);
    char sz[32];
    int name_w;

    ic_gfx_fill(c, area.x, area.y, area.w, area.h, p->content);
    name_w = area.w - 2 * IC_SP_3 - 32 - 90 - 110;

    for (int i = ex.first_item; i < ex.last_item; i++) {
        ic_rect_t r = item_rect(app, i);
        ex_item_t *it = &ex.items[i];
        ic_color_t text = ic_ui_list_row(c, r, i == ex.selected, ex.list_focused,
                                         i == ex.hover ? 1.0f : 0.0f);
        ic_symbol_draw(c, item_symbol(it), (float)(r.x + IC_SP_3 + 8), (float)(r.y + r.h / 2),
                       16.0f, p->label_secondary);
        ic_text_draw_in(c, body, ic_rect_make(r.x + IC_SP_3 + 24, r.y, name_w, r.h), it->name,
                        text, IC_ALIGN_LEFT);
        ic_text_draw_in(c, meta, ic_rect_make(r.x + IC_SP_3 + 24 + name_w, r.y, 80, r.h),
                        it->is_dir ? "Folder" : (it->is_app ? "App"
                                                : (it->is_wav ? "Audio" : "Text")),
                        p->label_tertiary, IC_ALIGN_LEFT);
        if (it->is_dir) d_copy(sz, "-", sizeof(sz));
        else size_text(it->size, sz, sizeof(sz));
        ic_text_draw_in(c, mono, ic_rect_make(r.x + r.w - 110, r.y, 100, r.h), sz, text,
                        IC_ALIGN_RIGHT);
        if (it->readonly) {
            ic_symbol_draw(c, IC_SYM_INFO, (float)(r.x + r.w - 20), (float)(r.y + r.h / 2),
                           14.0f, p->warning);
        }
    }

    if (ex.count == 0) {
        ic_ui_empty_state(c, area, IC_SYM_FOLDER, "This folder is empty",
                          "Use New Folder or New File to add something here.");
    } else if (ex.count > ex.rows) {
        ic_ui_scrollbar(c, area, ex.first_item * ROW_H, ex.count * ROW_H + 2 * IC_SP_2, 1.0f);
    }
}

static void draw_content(ic_app_t *app, ic_canvas_t *c) {
    if (ex.view == VIEW_LIST) draw_list(app, c);
    else draw_grid(app, c);
}

static void draw_status(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = status_rect(app);
    const ic_palette_t *p = ic_palette();
    char left[STATUS_CAP];

    ic_ui_statusbar(c, s, ex.status);
    left[0] = 0;
    ic_strlcat(left, ex.path, sizeof(left));
    ic_strlcat(left, "   ", sizeof(left));
    if (ex.count == 1) ic_strlcat(left, "1 item", sizeof(left));
    else {
        char n[24];
        ic_snprintf_u64(n, sizeof(n), (uint64_t)ex.count);
        ic_strlcat(left, n, sizeof(left));
        ic_strlcat(left, " items", sizeof(left));
    }
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                    ic_rect_make(s.x + s.w - 320, s.y, 320 - IC_SP_3, s.h), left,
                    p->label_tertiary, IC_ALIGN_RIGHT);
}

static void draw_dialog(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    int w = 420, h = 150;
    ic_rect_t r = ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
    ic_textfield_t tf;
    char buf[DIALOG_CAP];

    ic_ui_panel(c, r, IC_R_PANEL, IC_ELEV_MENU, 0, 0);
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(r.x + IC_SP_5, r.y + IC_SP_4,
                                                             r.w - 2 * IC_SP_5, 20),
                    ex.dialog_title, p->label, IC_ALIGN_LEFT);
    if (ex.dialog == DLG_DELETE) {
        int bw = ic_ui_button_width("Create", IC_SYM_NONE);
        int cw = ic_ui_button_width("Cancel", IC_SYM_NONE);
        int by = r.y + r.h - IC_H_CONTROL - IC_SP_4;
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + IC_SP_5, r.y + IC_SP_4 + 30,
                                                               r.w - 2 * IC_SP_5, 20),
                        ex.dialog_buf, p->label_secondary, IC_ALIGN_LEFT);
        ic_ui_button(c, ic_rect_make(r.x + r.w - bw - IC_SP_5, by, bw, IC_H_CONTROL),
                     "Delete", IC_SYM_NONE, IC_BUTTON_DESTRUCTIVE, IC_STATE_NORMAL);
        ic_ui_button(c, ic_rect_make(r.x + r.w - bw - cw - IC_SP_5 - IC_SP_3, by, cw,
                                     IC_H_CONTROL),
                     "Cancel", IC_SYM_NONE, IC_BUTTON_DEFAULT, IC_STATE_NORMAL);
        return;
    }
    d_copy(buf, ex.dialog_buf, sizeof(buf));
    tf.text = buf;
    tf.cursor = ex.dialog_cursor;
    tf.sel_start = tf.sel_end = ex.dialog_cursor;
    tf.focused = 1;
    tf.caret_on = ic_app_caret_visible(app);
    tf.placeholder = "Name";
    tf.leading = IC_SYM_NONE;
    tf.scroll_px = ex.dialog_scroll;
    ic_ui_textfield(c, ic_rect_make(r.x + IC_SP_5, r.y + IC_SP_4 + 24, r.w - 2 * IC_SP_5,
                                    IC_H_CONTROL), &tf);
    {
        int bw = ic_ui_button_width("Create", IC_SYM_NONE);
        int cw = ic_ui_button_width("Cancel", IC_SYM_NONE);
        int by = r.y + r.h - IC_H_CONTROL - IC_SP_4;
        ic_ui_button(c, ic_rect_make(r.x + r.w - bw - IC_SP_5, by, bw, IC_H_CONTROL),
                     ex.dialog == DLG_GOTO ? "Go" : "Create", IC_SYM_NONE, IC_BUTTON_PRIMARY,
                     IC_STATE_NORMAL);
        ic_ui_button(c, ic_rect_make(r.x + r.w - bw - cw - IC_SP_5 - IC_SP_3, by, cw,
                                     IC_H_CONTROL),
                     "Cancel", IC_SYM_NONE, IC_BUTTON_DEFAULT, IC_STATE_NORMAL);
    }
}

static void draw_info(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    int w = 380, h = 190;
    ic_rect_t r = ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
    char line[STATUS_CAP];
    char sz[32];
    int y = r.y + IC_SP_4;
    int iw = r.w - 2 * IC_SP_5;
    const ex_item_t *it = (ex.menu_item >= 0 && ex.menu_item < ex.count)
                              ? &ex.items[ex.menu_item] : 0;

    ic_ui_panel(c, r, IC_R_PANEL, IC_ELEV_MENU, 0, 0);
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(r.x + IC_SP_5, y, iw, 20),
                    it ? it->name : ex.path, p->label, IC_ALIGN_LEFT);
    y += 26;

    if (it) {
        d_copy(line, it->path, sizeof(line));
        ic_text_draw_in(c, ic_font(IC_FONT_MONO_SMALL), ic_rect_make(r.x + IC_SP_5, y, iw, 16),
                        line, p->label_secondary, IC_ALIGN_LEFT);
        y += 22;
        if (it->is_dir) d_copy(sz, "Folder", sizeof(sz));
        else size_text(it->size, sz, sizeof(sz));
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + IC_SP_5, y, iw, 16),
                        sz, p->label_secondary, IC_ALIGN_LEFT);
        y += 20;
        d_copy(line, it->readonly ? "Read-only" : "Read and write", sizeof(line));
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + IC_SP_5, y, iw, 16),
                        line, p->label_secondary, IC_ALIGN_LEFT);
    } else {
        d_copy(line, ex.path, sizeof(line));
        ic_text_draw_in(c, ic_font(IC_FONT_MONO_SMALL), ic_rect_make(r.x + IC_SP_5, y, iw, 16),
                        line, p->label_secondary, IC_ALIGN_LEFT);
        y += 22;
        if (ex.count == 1) d_copy(line, "1 item", sizeof(line));
        else {
            char n[24];
            ic_snprintf_u64(n, sizeof(n), (uint64_t)ex.count);
            d_copy(line, n, sizeof(line));
            ic_strlcat(line, " items", sizeof(line));
        }
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + IC_SP_5, y, iw, 16),
                        line, p->label_secondary, IC_ALIGN_LEFT);
    }
    {
        int bw = ic_ui_button_width("OK", IC_SYM_NONE);
        ic_ui_button(c, ic_rect_make(r.x + r.w - bw - IC_SP_5, r.y + r.h - IC_H_CONTROL - IC_SP_4,
                                     bw, IC_H_CONTROL),
                     "OK", IC_SYM_NONE, IC_BUTTON_PRIMARY, IC_STATE_NORMAL);
    }
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    layout(app);
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_sidebar(app, c);
    draw_content(app, c);
    draw_status(app, c);

    if (ex.info_open) draw_info(app, c);
    if (ex.dialog != DLG_NONE) draw_dialog(app, c);
    if (ex.menu_open) {
        ex.menu.hover = ic_ui_menu_hit(&ex.menu, ex.menu_x, ex.menu_y,
                                       app->mouse_x, app->mouse_y);
        ic_ui_menu(c, &ex.menu, ex.menu_x, ex.menu_y, 0, 0);
    }
    if (app->focused) ic_app_animate(app);
}



static int item_at(ic_app_t *app, int x, int y) {
    if (ex.view == VIEW_LIST) {
        for (int i = ex.first_item; i < ex.last_item; i++) {
            if (ic_ui_hit(item_rect(app, i), x, y)) return i;
        }
        return -1;
    }
    {
        ic_rect_t c = content_rect(app);
        int col, row, idx;
        




        if (!ic_ui_hit(c, x, y)) return -1;
        col = (x - c.x - IC_SP_3) / GRID_CELL_W;
        row = (y - c.y - IC_SP_3) / GRID_CELL_H;
        if (col < 0 || col >= ex.cols || row < 0 || row >= ex.rows) return -1;
        idx = ex.scroll + row * ex.cols + col;
        return (idx >= 0 && idx < ex.count) ? idx : -1;
    }
}

static int place_at(ic_app_t *app, int x, int y) {
    for (int i = 0; i < PLACE_COUNT; i++) {
        if (ic_ui_hit(place_rect(app, i), x, y)) return i;
    }
    return -1;
}

enum {
    MA_NONE = 0, MA_OPEN, MA_EDIT, MA_RENAME, MA_DELETE, MA_INFO, MA_NEW_FOLDER, MA_NEW_FILE,
    MA_TERMINAL, MA_REFRESH, MA_VIEW_GRID, MA_VIEW_LIST
};

static int menu_add(int n, const char *label, int action, int disabled) {
    if (n >= IC_MENU_ITEMS_MAX) return n;
    ex.menu.labels[n] = label;
    ex.menu.shortcuts[n] = 0;
    ex.menu.disabled[n] = (uint8_t)disabled;
    ex.menu.checked[n] = 0;
    ex.menu.submenu[n] = 0;
    ex.menu_actions[n] = action;
    return n + 1;
}

static void build_menu(void) {
    int n = 0;
    ex.menu.count = 0;
    ex.menu.hover = -1;
    if (ex.menu_item >= 0 && ex.menu_item < ex.count) {
        ex_item_t *it = &ex.items[ex.menu_item];
        n = menu_add(n, "Open", MA_OPEN, 0);
        if (!it->is_dir) n = menu_add(n, "Open in Editor", MA_EDIT, 0);
        n = menu_add(n, "Rename", MA_RENAME, 0);
        n = menu_add(n, "Delete", MA_DELETE, 0);
        n = menu_add(n, "Get Info", MA_INFO, 0);
        n = menu_add(n, IC_MENU_SEPARATOR, MA_NONE, 0);
    }
    n = menu_add(n, "New Folder", MA_NEW_FOLDER, 0);
    n = menu_add(n, "New File", MA_NEW_FILE, 0);
    n = menu_add(n, "Open in Terminal", MA_TERMINAL, 0);
    n = menu_add(n, IC_MENU_SEPARATOR, MA_NONE, 0);
    n = menu_add(n, "Icons", MA_VIEW_GRID, 0);
    ex.menu.checked[n - 1] = ex.view == VIEW_GRID ? IC_MENU_CHECK_ON : IC_MENU_CHECK_OFF;
    n = menu_add(n, "List", MA_VIEW_LIST, 0);
    ex.menu.checked[n - 1] = ex.view == VIEW_LIST ? IC_MENU_CHECK_ON : IC_MENU_CHECK_OFF;
    n = menu_add(n, IC_MENU_SEPARATOR, MA_NONE, 0);
    n = menu_add(n, "Refresh", MA_REFRESH, 0);
    ex.menu.count = n;
}

static void open_menu_at(ic_app_t *app, int x, int y) {
    int mw, mh;
    build_menu();
    mw = ic_ui_menu_width(&ex.menu);
    mh = ic_ui_menu_height(&ex.menu);
    if (x + mw > app->width - IC_SP_2) x = app->width - IC_SP_2 - mw;
    if (y + mh > app->height - IC_SP_2) y = app->height - IC_SP_2 - mh;
    if (x < IC_SP_2) x = IC_SP_2;
    if (y < IC_SP_2) y = IC_SP_2;
    ex.menu_x = x;
    ex.menu_y = y;
    ex.menu_open = 1;
}

static void menu_activate(ic_app_t *app, int index) {
    int item = ex.menu_item;
    int action = (index >= 0 && index < ex.menu.count) ? ex.menu_actions[index] : MA_NONE;
    int has_item = item >= 0 && item < ex.count;
    ex.menu_open = 0;
    switch (action) {
    case MA_OPEN:       if (has_item) open_item(item); break;
    case MA_EDIT:       if (has_item) open_in_editor(item); break;
    case MA_RENAME:
        if (has_item) open_dialog(DLG_RENAME, "Rename", ex.items[item].name);
        break;
    case MA_DELETE:     if (has_item) ask_delete(item); break;
    case MA_INFO:       if (has_item) ex.info_open = 1; break;
    case MA_NEW_FOLDER: open_dialog(DLG_NEW_FOLDER, "New Folder", ""); break;
    case MA_NEW_FILE:   open_dialog(DLG_NEW_FILE, "New File", ""); break;
    case MA_TERMINAL:   icda_spawn("/apps/terminal.app"); break;
    case MA_REFRESH:    refresh(); break;
    case MA_VIEW_GRID:  ex.view = VIEW_GRID; break;
    case MA_VIEW_LIST:  ex.view = VIEW_LIST; break;
    default: break;
    }
    (void)app;
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        if (ex.dialog != DLG_NONE || ex.info_open) break;
        if (ex.menu_open) {
            ex.menu.hover = ic_ui_menu_hit(&ex.menu, ex.menu_x, ex.menu_y, ev->x, ev->y);
            break;
        }
        ex.hover_back = ic_ui_hit(back_rect(app), ev->x, ev->y);
        ex.hover_up = ic_ui_hit(up_rect(app), ev->x, ev->y);
        ex.hover_view = ic_ui_hit(view_rect(app), ev->x, ev->y);
        ex.hover_new_folder = ic_ui_hit(new_folder_rect(app), ev->x, ev->y);
        ex.hover_new_file = ic_ui_hit(new_file_rect(app), ev->x, ev->y);
        ex.hover_sidebar = place_at(app, ev->x, ev->y);
        ex.hover = item_at(app, ev->x, ev->y);
        break;
    case IC_EV_MOUSE_DOWN: {
        int i;
        if (ev->button == GUI_BTN_RIGHT) {
            if (ex.dialog != DLG_NONE || ex.info_open) break;
            ex.menu_item = item_at(app, ev->x, ev->y);
            if (ex.menu_item >= 0) ex.selected = ex.menu_item;
            open_menu_at(app, ev->x, ev->y);
            break;
        }
        if (ev->button != GUI_BTN_LEFT) break;
        if (ex.dialog != DLG_NONE) {
            int w = 420, h = 150;
            ic_rect_t r = ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
            int bw = ic_ui_button_width("Create", IC_SYM_NONE);
            int cw = ic_ui_button_width("Cancel", IC_SYM_NONE);
            int by = r.y + r.h - IC_H_CONTROL - IC_SP_4;
            if (ic_ui_hit(ic_rect_make(r.x + r.w - bw - IC_SP_5, by, bw, IC_H_CONTROL),
                          ev->x, ev->y)) {
                commit_dialog();
            } else if (ic_ui_hit(ic_rect_make(r.x + r.w - bw - cw - IC_SP_5 - IC_SP_3, by, cw,
                                             IC_H_CONTROL), ev->x, ev->y)) {
                close_dialog();
            } else {
                ic_rect_t f = ic_rect_make(r.x + IC_SP_5, r.y + IC_SP_4 + 24,
                                           r.w - 2 * IC_SP_5, IC_H_CONTROL);
                if (ic_ui_hit(f, ev->x, ev->y)) {
                    ex.dialog_cursor = ic_ui_textfield_index_at(f, &(ic_textfield_t){
                        .text = ex.dialog_buf, .cursor = ex.dialog_cursor,
                        .sel_start = ex.dialog_cursor, .sel_end = ex.dialog_cursor
                    }, ev->x);
                }
            }
            break;
        }
        if (ex.info_open) {
            int w = 380, h = 190;
            ic_rect_t r = ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
            int bw = ic_ui_button_width("OK", IC_SYM_NONE);
            if (ic_ui_hit(ic_rect_make(r.x + r.w - bw - IC_SP_5,
                                       r.y + r.h - IC_H_CONTROL - IC_SP_4, bw, IC_H_CONTROL),
                          ev->x, ev->y) ||
                !ic_ui_hit(r, ev->x, ev->y)) {
                ex.info_open = 0;
            }
            break;
        }
        if (ex.menu_open) {
            int hit = ic_ui_menu_hit(&ex.menu, ex.menu_x, ex.menu_y, ev->x, ev->y);
            if (hit >= 0) menu_activate(app, hit);
            else ex.menu_open = 0;
            break;
        }
        if (ex.hover_back) { go_back(); break; }
        if (ex.hover_up) { go_up(); break; }
        if (ex.hover_view) { ex.view = ex.view == VIEW_GRID ? VIEW_LIST : VIEW_GRID; break; }
        if (ex.hover_new_folder) { open_dialog(DLG_NEW_FOLDER, "New Folder", ""); break; }
        if (ex.hover_new_file) { open_dialog(DLG_NEW_FILE, "New File", ""); break; }
        i = place_at(app, ev->x, ev->y);
        if (i >= 0) { navigate_to(PLACES[i], 1); break; }
        i = item_at(app, ev->x, ev->y);
        if (i >= 0) {
            uint32_t now = (uint32_t)ic_time_ms();
            int dbl = i == ex.last_click_item && now - ex.last_click_ms < 400;
            ex.selected = i;
            ex.list_focused = 1;
            ex.last_click_item = dbl ? -1 : i;
            ex.last_click_ms = now;
            if (dbl) open_item(i);
        } else {
            ex.last_click_item = -1;
            ex.selected = -1;
            ex.list_focused = 0;
        }
        break;
    }
    case IC_EV_MOUSE_UP:
        break;
    case IC_EV_MOUSE_LEAVE:
        ex.hover = -1;
        ex.hover_sidebar = -1;
        ex.hover_back = ex.hover_up = ex.hover_view = 0;
        ex.hover_new_folder = ex.hover_new_file = 0;
        break;
    case IC_EV_SCROLL:
        if (ex.view == VIEW_LIST) ex.first_item += ev->wheel * 3;
        else ex.scroll += ev->wheel * ex.cols;
        break;
    case IC_EV_KEY:
        if (ex.info_open) {
            if (ev->key == IC_KEY_ESCAPE || ev->key == IC_KEY_ENTER) ex.info_open = 0;
            break;
        }
        if (ex.dialog == DLG_DELETE) {
            if (ev->key == IC_KEY_ENTER) commit_dialog();
            else if (ev->key == IC_KEY_ESCAPE) close_dialog();
            break;
        }
        if (ex.dialog != DLG_NONE) {
            int len = (int)d_strlen(ex.dialog_buf);
            ic_rect_t f = ic_rect_make((app->width - 420) / 2 + IC_SP_5,
                                       (app->height - 150) / 2 + IC_SP_4 + 24,
                                       420 - 2 * IC_SP_5, IC_H_CONTROL);
            switch (ev->key) {
            case IC_KEY_ENTER:  commit_dialog(); break;
            case IC_KEY_ESCAPE: close_dialog(); break;
            case IC_KEY_LEFT:   if (ex.dialog_cursor > 0) ex.dialog_cursor--; break;
            case IC_KEY_RIGHT:  if (ex.dialog_cursor < len) ex.dialog_cursor++; break;
            case IC_KEY_HOME:   ex.dialog_cursor = 0; break;
            case IC_KEY_END:    ex.dialog_cursor = len; break;
            case IC_KEY_BACKSPACE:
                if (ex.dialog_cursor > 0) {
                    for (int k = ex.dialog_cursor; k < len; k++) {
                        ex.dialog_buf[k - 1] = ex.dialog_buf[k];
                    }
                    ex.dialog_buf[len - 1] = 0;
                    ex.dialog_cursor--;
                }
                break;
            case IC_KEY_DELETE:
                if (ex.dialog_cursor < len) {
                    for (int k = ex.dialog_cursor; k < len - 1; k++) {
                        ex.dialog_buf[k] = ex.dialog_buf[k + 1];
                    }
                    ex.dialog_buf[len - 1] = 0;
                }
                break;
            default:
                if (ev->key >= 32 && ev->key < 127 && !(ev->mods & (IC_MOD_CTRL | IC_MOD_ALT)) && len + 1 < DIALOG_CAP) {
                    for (int k = len; k > ex.dialog_cursor; k--) {
                        ex.dialog_buf[k] = ex.dialog_buf[k - 1];
                    }
                    ex.dialog_buf[ex.dialog_cursor++] = (char)ev->key;
                    ex.dialog_buf[len + 1] = 0;
                }
                break;
            }
            (void)f;
            break;
        }
        if (ex.menu_open) { ex.menu_open = 0; break; }
        switch (ev->key) {
        case IC_KEY_ENTER:
            if (ex.selected >= 0) open_item(ex.selected);
            break;
        case IC_KEY_UP:
            if (ex.selected > 0) ex.selected--;
            else if (ex.view == VIEW_GRID && ex.scroll > 0) ex.scroll -= ex.cols;
            break;
        case IC_KEY_DOWN:
            if (ex.selected + 1 < ex.count) ex.selected++;
            break;
        case IC_KEY_LEFT:
            if (ex.view == VIEW_GRID && ex.selected % ex.cols > 0) ex.selected--;
            break;
        case IC_KEY_RIGHT:
            if (ex.view == VIEW_GRID && ex.selected % ex.cols + 1 < ex.cols &&
                ex.selected + 1 < ex.count) {
                ex.selected++;
            }
            break;
        case IC_KEY_PAGE_UP:
            if (ex.view == VIEW_LIST) ex.first_item -= ex.rows;
            else ex.scroll -= ex.cols * ex.rows;
            break;
        case IC_KEY_PAGE_DOWN:
            if (ex.view == VIEW_LIST) ex.first_item += ex.rows;
            else ex.scroll += ex.cols * ex.rows;
            break;
        case IC_KEY_ESCAPE:
            ex.menu_item = ex.selected;
            open_menu_at(app, app->mouse_x, app->mouse_y);
            break;
        case IC_KEY_DELETE:
            ask_delete(ex.selected);
            break;
        case IC_KEY_HOME: ex.selected = 0; break;
        case IC_KEY_END:  if (ex.count) ex.selected = ex.count - 1; break;
        case 'v': case 'V': ex.view = ex.view == VIEW_GRID ? VIEW_LIST : VIEW_GRID; break;
        case 'r': case 'R': refresh(); break;
        case 'n': case 'N': open_dialog(DLG_NEW_FOLDER, "New Folder", ""); break;
        case 'g': case 'G': open_dialog(DLG_GOTO, "Go to Folder", ex.path); break;
        default: break;
        }
        break;
    case IC_EV_RESIZE:
        layout(app);
        break;
    case IC_EV_BLUR:
        ex.list_focused = 0;
        break;
    case IC_EV_FOCUS:
    case IC_EV_APPEARANCE:
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    (void)app;
    ex.count = 0;
    ex.selected = -1;
    ex.hover = -1;
    ex.view = VIEW_GRID;
    ex.scroll = 0;
    ex.first_item = 0;
    ex.history_count = 0;
    ex.history_pos = -1;
    ex.dialog = DLG_NONE;
    ex.menu_open = 0;
    ex.menu_item = -1;
    ex.menu_hover = -1;
    ex.info_open = 0;
    ex.list_focused = 1;
    ex.hover_back = ex.hover_up = ex.hover_view = 0;
    ex.hover_new_folder = ex.hover_new_file = 0;
    ex.hover_sidebar = -1;
    d_copy(ex.path, "/", PATH_CAP);
    ex.status[0] = 0;
    refresh();
    if (app->user) {
        const char *arg = (const char *)app->user;
        if (arg[0]) navigate_to(arg, 0);
    }
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Explorer", WIN_W, WIN_H, init, draw, event, 0 };
    const char *arg = (argc > 1 && argv) ? argv[1] : 0;
    if (ic_app_run(&desc, (void *)arg) != 0) {
        icda_write("explorer requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
