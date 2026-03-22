// 2026-03-20 v0.3.0
// view_settings.c — Default fg/bg colors, font, presets, and appearance dialog.
// Phase G6: persist colors, font, auto-hide via config.h.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "view_settings.h"
#include "config.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static ViewColors g_colors;
static char g_font[128] = "Monospace 14";
static bool g_auto_hide = true;
static bool g_initialized = false;

static void ensure_init(void) {
    if (g_initialized) return;
    g_initialized = true;

    // Default: Tomorrow Night
    view_settings_preset_tomorrow_night(&g_colors);

    // Override from config.ini if keys exist
    double r = config_get_double("Appearance", "fg_r", -1.0);
    if (r >= 0.0) {
        g_colors.fg.red   = config_get_double("Appearance", "fg_r", g_colors.fg.red);
        g_colors.fg.green = config_get_double("Appearance", "fg_g", g_colors.fg.green);
        g_colors.fg.blue  = config_get_double("Appearance", "fg_b", g_colors.fg.blue);
        g_colors.fg.alpha = 1.0;
    }
    r = config_get_double("Appearance", "bg_r", -1.0);
    if (r >= 0.0) {
        g_colors.bg.red   = config_get_double("Appearance", "bg_r", g_colors.bg.red);
        g_colors.bg.green = config_get_double("Appearance", "bg_g", g_colors.bg.green);
        g_colors.bg.blue  = config_get_double("Appearance", "bg_b", g_colors.bg.blue);
        g_colors.bg.alpha = 1.0;
    }

    const char *saved_font = config_get_string("Appearance", "font", NULL);
    if (saved_font) {
        snprintf(g_font, sizeof(g_font), "%s", saved_font);
    }

    // Auto-hide: default true, override from config
    g_auto_hide = config_get_bool("Appearance", "auto_hide_toolbar", true);
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

void view_settings_preset_classic(ViewColors *out) {
    gdk_rgba_parse(&out->bg, "#000000");
    gdk_rgba_parse(&out->fg, "#00ff00");
}

void view_settings_preset_solarized_dark(ViewColors *out) {
    gdk_rgba_parse(&out->bg, "#002b36");
    gdk_rgba_parse(&out->fg, "#839496");
}

void view_settings_preset_tomorrow_night(ViewColors *out) {
    gdk_rgba_parse(&out->bg, "#1d1f21");
    gdk_rgba_parse(&out->fg, "#c5c8c6");
}

// ---------------------------------------------------------------------------
// Getters / setters
// ---------------------------------------------------------------------------

const ViewColors *view_settings_get_colors(void) {
    ensure_init();
    return &g_colors;
}

void view_settings_set_colors(const ViewColors *c) {
    ensure_init();
    g_colors = *c;
}

const char *view_settings_get_font(void) {
    ensure_init();
    return g_font;
}

void view_settings_set_font(const char *font_desc) {
    ensure_init();
    if (!font_desc) return;
    snprintf(g_font, sizeof(g_font), "%s", font_desc);
}

bool view_settings_get_auto_hide(void) {
    ensure_init();
    return g_auto_hide;
}

void view_settings_set_auto_hide(bool enabled) {
    ensure_init();
    g_auto_hide = enabled;
    config_set_bool("Appearance", "auto_hide_toolbar", enabled);
    config_save();
}

// ---------------------------------------------------------------------------
// Monospace font filter for GtkFontChooser
// ---------------------------------------------------------------------------

static gboolean mono_filter(const PangoFontFamily *family,
                             const PangoFontFace *face,
                             gpointer user_data) {
    (void)face;
    (void)user_data;
    return pango_font_family_is_monospace((PangoFontFamily *)family);
}

// ---------------------------------------------------------------------------
// Appearance dialog
// ---------------------------------------------------------------------------

typedef struct {
    GtkWidget *btn_bg;
    GtkWidget *btn_fg;
    GtkWidget *font_chooser;
    ViewColors result;
    bool changed;
} DialogCtx;

static void on_preset_classic(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DialogCtx *ctx = (DialogCtx *)user_data;
    ViewColors vc;
    view_settings_preset_classic(&vc);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ctx->btn_bg), &vc.bg);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ctx->btn_fg), &vc.fg);
}

static void on_preset_solarized(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DialogCtx *ctx = (DialogCtx *)user_data;
    ViewColors vc;
    view_settings_preset_solarized_dark(&vc);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ctx->btn_bg), &vc.bg);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ctx->btn_fg), &vc.fg);
}

static void on_preset_tomorrow(GtkButton *btn, gpointer user_data) {
    (void)btn;
    DialogCtx *ctx = (DialogCtx *)user_data;
    ViewColors vc;
    view_settings_preset_tomorrow_night(&vc);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ctx->btn_bg), &vc.bg);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(ctx->btn_fg), &vc.fg);
}

bool view_settings_show_dialog(GtkWindow *parent) {
    ensure_init();

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Appearance", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Apply", GTK_RESPONSE_APPLY,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 500);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    DialogCtx ctx;
    ctx.changed = false;

    // Preset buttons
    GtkWidget *preset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *btn_classic = gtk_button_new_with_label("Classic");
    GtkWidget *btn_solar = gtk_button_new_with_label("Solarized Dark");
    GtkWidget *btn_tomorrow = gtk_button_new_with_label("Tomorrow Night");
    gtk_box_pack_start(GTK_BOX(preset_box), btn_classic, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(preset_box), btn_solar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(preset_box), btn_tomorrow, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), preset_box, FALSE, FALSE, 0);

    // Color buttons
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);

    gtk_grid_attach(GTK_GRID(grid),
                    gtk_label_new("Background:"), 0, 0, 1, 1);
    ctx.btn_bg = gtk_color_button_new_with_rgba(&g_colors.bg);
    gtk_grid_attach(GTK_GRID(grid), ctx.btn_bg, 1, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid),
                    gtk_label_new("Foreground:"), 0, 1, 1, 1);
    ctx.btn_fg = gtk_color_button_new_with_rgba(&g_colors.fg);
    gtk_grid_attach(GTK_GRID(grid), ctx.btn_fg, 1, 1, 1, 1);

    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

    g_signal_connect(btn_classic, "clicked",
                     G_CALLBACK(on_preset_classic), &ctx);
    g_signal_connect(btn_solar, "clicked",
                     G_CALLBACK(on_preset_solarized), &ctx);
    g_signal_connect(btn_tomorrow, "clicked",
                     G_CALLBACK(on_preset_tomorrow), &ctx);

    // Font chooser section
    gtk_box_pack_start(GTK_BOX(content),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(content),
        gtk_label_new("Font:"), FALSE, FALSE, 0);

    ctx.font_chooser = gtk_font_chooser_widget_new();
    gtk_font_chooser_set_filter_func(GTK_FONT_CHOOSER(ctx.font_chooser),
                                      mono_filter, NULL, NULL);
    // Set current font
    PangoFontDescription *cur_fd = pango_font_description_from_string(g_font);
    gtk_font_chooser_set_font_desc(GTK_FONT_CHOOSER(ctx.font_chooser), cur_fd);
    pango_font_description_free(cur_fd);

    gtk_box_pack_start(GTK_BOX(content), ctx.font_chooser, TRUE, TRUE, 0);

    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));

    if (response == GTK_RESPONSE_APPLY) {
        gtk_color_chooser_get_rgba(
            GTK_COLOR_CHOOSER(ctx.btn_bg), &g_colors.bg);
        gtk_color_chooser_get_rgba(
            GTK_COLOR_CHOOSER(ctx.btn_fg), &g_colors.fg);

        // Read selected font
        PangoFontDescription *fd = gtk_font_chooser_get_font_desc(
            GTK_FONT_CHOOSER(ctx.font_chooser));
        if (fd) {
            char *desc_str = pango_font_description_to_string(fd);
            if (desc_str) {
                view_settings_set_font(desc_str);
                g_free(desc_str);
            }
            pango_font_description_free(fd);
        }

        // Persist to config.ini
        config_set_double("Appearance", "fg_r", g_colors.fg.red);
        config_set_double("Appearance", "fg_g", g_colors.fg.green);
        config_set_double("Appearance", "fg_b", g_colors.fg.blue);
        config_set_double("Appearance", "bg_r", g_colors.bg.red);
        config_set_double("Appearance", "bg_g", g_colors.bg.green);
        config_set_double("Appearance", "bg_b", g_colors.bg.blue);
        config_set_string("Appearance", "font", g_font);
        config_save();

        ctx.changed = true;
    }

    gtk_widget_destroy(dialog);
    return ctx.changed;
}
