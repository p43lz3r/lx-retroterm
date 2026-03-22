// 2026-03-18 23:00 v0.2.1
// view_settings.h — Default fg/bg colors, font, and appearance presets.
// Phase G2: classic/solarized/tomorrow-night presets, font chooser, auto-hide.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_VIEW_SETTINGS_H_
#define GUI_VIEW_SETTINGS_H_

#include <gtk/gtk.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    GdkRGBA fg;
    GdkRGBA bg;
} ViewColors;

// Get current default colors (never NULL).
const ViewColors *view_settings_get_colors(void);

// Set new default colors (copied internally).
void view_settings_set_colors(const ViewColors *c);

// Get current font as Pango description string (e.g. "Monospace 14").
const char *view_settings_get_font(void);

// Set font from Pango description string (copied internally).
void view_settings_set_font(const char *font_desc);

// Auto-hide toolbar state.
bool view_settings_get_auto_hide(void);
void view_settings_set_auto_hide(bool enabled);

// Color presets.
void view_settings_preset_classic(ViewColors *out);
void view_settings_preset_solarized_dark(ViewColors *out);
void view_settings_preset_tomorrow_night(ViewColors *out);

// Show the appearance dialog.  parent: transient-for window.
// Returns true if colors or font were changed.
bool view_settings_show_dialog(GtkWindow *parent);

#ifdef __cplusplus
}
#endif

#endif  // GUI_VIEW_SETTINGS_H_
