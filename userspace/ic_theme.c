


#include "ic_theme.h"
#include "settings_store.h"

typedef struct {
    uint32_t dark;
    uint32_t light;
} ic_accent_pair_t;

static const ic_accent_pair_t ic_accents[IC_ACCENT_COUNT] = {
    { 0x3A8BFF, 0x2F7BF0 },   
    { 0xA46BF5, 0x8E52E3 },   
    { 0xF0609E, 0xDC437C },   
    { 0xF2555A, 0xE0434A },   
    { 0xF5923A, 0xE27A1F },   
    { 0x3CC46A, 0x2DA25A },   
    { 0x8E8E93, 0x77777D },   
};

static const ic_shadow_spec_t ic_shadow_dark[IC_ELEV_COUNT][2] = {
    
    { { 1, 1, 0x50 },       { 3, 1, 0x28 } },   
    { { 2, 1, 0x60 },       { 16, 8, 0x68 } },  
    { { 3, 1, 0x70 },       { 28, 16, 0x78 } }, 
    { { 3, 1, 0x50 },       { 18, 10, 0x4C } }, 
};

static const ic_shadow_spec_t ic_shadow_light[IC_ELEV_COUNT][2] = {
    { { 1, 1, 0x30 },       { 3, 1, 0x14 } },
    { { 2, 1, 0x30 },       { 16, 8, 0x3A } },
    { { 3, 1, 0x3C },       { 28, 16, 0x4A } },
    { { 3, 1, 0x28 },       { 18, 10, 0x28 } },
};

static ic_palette_t ic_current;
static int ic_current_ready;
static int ic_current_dark = -1;
static int ic_current_accent = -1;

ic_color_t ic_accent_swatch(ic_accent_t accent) {
    if ((int)accent < 0 || accent >= IC_ACCENT_COUNT) accent = IC_ACCENT_BLUE;
    return IC_RGB(ic_current_dark == 0 ? ic_accents[accent].light : ic_accents[accent].dark);
}

void ic_palette_build(ic_palette_t *p, int dark, ic_accent_t accent) {
    ic_color_t a;
    if (!p) return;
    if ((int)accent < 0 || accent >= IC_ACCENT_COUNT) accent = IC_ACCENT_BLUE;
    a = IC_RGB(dark ? ic_accents[accent].dark : ic_accents[accent].light);
    p->dark = dark;
    if (dark) {
        p->desktop            = IC_RGB(0x1C1C1E);
        p->window             = IC_RGB(0x29292C);
        p->content            = IC_RGB(0x1F1F21);
        p->sidebar            = IC_RGB(0x242427);
        p->group              = IC_RGB(0x323235);
        p->titlebar           = IC_RGB(0x323235);
        p->material_menu      = IC_RGBA(0x2B2B2E, 0xD6);
        p->material_bar       = IC_RGBA(0x1E1E21, 0xC4);

        p->control            = IC_RGB(0x444448);
        p->control_hover      = IC_RGB(0x4E4E52);
        p->control_pressed    = IC_RGB(0x5A5A5F);
        p->control_stroke     = IC_RGBA(0xFFFFFF, 0x16);
        p->field              = IC_RGB(0x1F1F21);
        p->toggle_off         = IC_RGB(0x48484C);
        p->knob               = IC_RGB(0xEDEDF0);
        p->fill_hover         = IC_RGBA(0xFFFFFF, 0x14);
        p->fill_pressed       = IC_RGBA(0xFFFFFF, 0x22);
        p->fill_selected_idle = IC_RGBA(0xFFFFFF, 0x1C);
        p->segment_track      = IC_RGBA(0xFFFFFF, 0x12);
        p->segment_selected   = IC_RGB(0x68686D);
        p->scroller           = IC_RGBA(0xFFFFFF, 0x66);

        p->label              = IC_RGB(0xF2F2F5);
        p->label_secondary    = IC_RGBA(0xEBEBF5, 0x99);
        p->label_tertiary     = IC_RGBA(0xEBEBF5, 0x5E);
        p->label_disabled     = IC_RGBA(0xEBEBF5, 0x40);
        p->label_on_accent    = IC_RGB(0xFFFFFF);

        p->separator          = IC_RGBA(0xFFFFFF, 0x1A);
        p->frame              = IC_RGBA(0x000000, 0xA0);
        p->highlight          = IC_RGBA(0xFFFFFF, 0x1C);
        p->group_stroke       = IC_RGBA(0xFFFFFF, 0x0C);
        p->bar_edge           = IC_RGBA(0x000000, 0x66);

        p->accent             = a;
        p->accent_hover       = ic_color_mix(a, IC_RGB(0xFFFFFF), 0.10f);
        p->accent_pressed     = ic_color_mix(a, IC_RGB(0x000000), 0.18f);
        p->accent_soft        = ic_color_with_alpha(a, 0x4A);
        p->focus_ring         = ic_color_with_alpha(a, 0x8C);
        p->success            = IC_RGB(0x34C85A);
        p->warning            = IC_RGB(0xF5B324);
        p->danger             = IC_RGB(0xFF5A4F);
        p->close_hover        = IC_RGB(0xE5484D);
    } else {
        p->desktop            = IC_RGB(0xE6E6EA);
        p->window             = IC_RGB(0xF3F3F5);
        p->content            = IC_RGB(0xFFFFFF);
        p->sidebar            = IC_RGB(0xEAEAED);
        p->group              = IC_RGB(0xFFFFFF);
        p->titlebar           = IC_RGB(0xE9E9EC);
        p->material_menu      = IC_RGBA(0xF7F7F9, 0xDC);
        p->material_bar       = IC_RGBA(0xF2F2F5, 0xC8);

        p->control            = IC_RGB(0xFFFFFF);
        p->control_hover      = IC_RGB(0xF5F5F7);
        p->control_pressed    = IC_RGB(0xE4E4E8);
        p->control_stroke     = IC_RGBA(0x000000, 0x22);
        p->field              = IC_RGB(0xFFFFFF);
        p->toggle_off         = IC_RGB(0xD9D9DE);
        p->knob               = IC_RGB(0xFFFFFF);
        p->fill_hover         = IC_RGBA(0x000000, 0x0F);
        p->fill_pressed       = IC_RGBA(0x000000, 0x1A);
        p->fill_selected_idle = IC_RGBA(0x000000, 0x14);
        p->segment_track      = IC_RGBA(0x000000, 0x0E);
        p->segment_selected   = IC_RGB(0xFFFFFF);
        p->scroller           = IC_RGBA(0x000000, 0x55);

        p->label              = IC_RGB(0x1D1D1F);
        p->label_secondary    = IC_RGBA(0x3C3C43, 0x9E);
        p->label_tertiary     = IC_RGBA(0x3C3C43, 0x60);
        p->label_disabled     = IC_RGBA(0x3C3C43, 0x42);
        p->label_on_accent    = IC_RGB(0xFFFFFF);

        p->separator          = IC_RGBA(0x000000, 0x17);
        p->frame              = IC_RGBA(0x000000, 0x38);
        p->highlight          = IC_RGBA(0xFFFFFF, 0x90);
        p->group_stroke       = IC_RGBA(0x000000, 0x10);
        p->bar_edge           = IC_RGBA(0x000000, 0x1A);

        p->accent             = a;
        p->accent_hover       = ic_color_mix(a, IC_RGB(0x000000), 0.06f);
        p->accent_pressed     = ic_color_mix(a, IC_RGB(0x000000), 0.18f);
        p->accent_soft        = ic_color_with_alpha(a, 0x33);
        p->focus_ring         = ic_color_with_alpha(a, 0x80);
        p->success            = IC_RGB(0x28A745);
        p->warning            = IC_RGB(0xE0A100);
        p->danger             = IC_RGB(0xE5433A);
        p->close_hover        = IC_RGB(0xE5484D);
    }
}

int ic_palette_reload(void) {
    icda_settings_t s;
    int dark, accent;
    icda_settings_load(&s);
    dark = s.appearance == ICDA_APPEARANCE_LIGHT ? 0 : 1;
    accent = s.accent;
    if (accent < 0 || accent >= IC_ACCENT_COUNT) accent = IC_ACCENT_BLUE;
    if (ic_current_ready && dark == ic_current_dark && accent == ic_current_accent) return 0;
    ic_palette_build(&ic_current, dark, (ic_accent_t)accent);
    ic_current_dark = dark;
    ic_current_accent = accent;
    ic_current_ready = 1;
    return 1;
}

const ic_palette_t *ic_palette(void) {
    if (!ic_current_ready) ic_palette_reload();
    return &ic_current;
}

void ic_theme_shadow_faded(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                           ic_elevation_t level, float opacity) {
    const ic_shadow_spec_t *spec;
    if ((int)level < 0 || level >= IC_ELEV_COUNT) level = IC_ELEV_WINDOW;
    if (opacity <= 0.0f) return;
    if (opacity > 1.0f) opacity = 1.0f;
    spec = ic_palette()->dark ? ic_shadow_dark[level] : ic_shadow_light[level];
    ic_gfx_shadow(c, x, y, w, h, radius, spec[1].blur, spec[1].dy,
                  (uint32_t)((float)spec[1].alpha * opacity + 0.5f));
    ic_gfx_shadow(c, x, y, w, h, radius, spec[0].blur, spec[0].dy,
                  (uint32_t)((float)spec[0].alpha * opacity + 0.5f));
}

void ic_theme_shadow(ic_canvas_t *c, int x, int y, int w, int h, float radius,
                     ic_elevation_t level) {
    ic_theme_shadow_faded(c, x, y, w, h, radius, level, 1.0f);
}

ic_color_t ic_syntax_color(ic_syntax_t kind) {
    static const uint32_t dark[IC_SYN_COUNT] = {
        0xD4D4D4, 0xC586C0, 0x4EC9B0, 0xCE9178, 0xB5CEA8, 0x6A9955, 0x569CD6, 0xDCDCAA
    };
    static const uint32_t light[IC_SYN_COUNT] = {
        0x1F1F1F, 0xAF00DB, 0x267F99, 0xA31515, 0x098658, 0x008000, 0x0000FF, 0x795E26
    };
    const ic_palette_t *p = ic_palette();
    if ((int)kind <= 0 || kind >= IC_SYN_COUNT) return p->label;
    return IC_RGB(p->dark ? dark[kind] : light[kind]);
}
