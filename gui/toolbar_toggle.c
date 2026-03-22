// 2026-03-18 23:30 v0.2.2
// toolbar_toggle.c — Thin toggle strip + GtkRevealer for toolbar.
// Phase G2: [<]/[>] button toggles slide-down animation.
// Fix: suppress terminal resize during animation (notify::child-revealed).
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "toolbar_toggle.h"
#include "view_settings.h"
#include "terminal_view.h"

typedef struct {
    GtkWidget *revealer;
    GtkWidget *button;
} ToggleCtx;

// Called when GtkRevealer animation completes (child-revealed changes).
static void on_child_revealed(GObject *obj, GParamSpec *pspec,
                               gpointer user_data) {
    (void)obj;
    (void)pspec;
    (void)user_data;
    terminal_view_suppress_resize(false);
    terminal_view_apply_pending_resize();
}

static void on_toggle_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ToggleCtx *ctx = (ToggleCtx *)user_data;
    toolbar_toggle_toggle(ctx->revealer);
    bool visible = gtk_revealer_get_reveal_child(
        GTK_REVEALER(ctx->revealer));
    gtk_button_set_label(GTK_BUTTON(ctx->button),
                         visible ? "\342\200\271" : "\342\200\272");
}

GtkWidget *toolbar_toggle_create(GtkWidget *toolbar,
                                  GtkWidget **out_revealer) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    // Toggle strip: thin bar with button right-aligned
    GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(strip, -1, 20);

    // CSS for strip background
    GtkWidget *strip_name = strip;
    gtk_widget_set_name(strip_name, "ttcore-toggle-strip");
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "#ttcore-toggle-strip {"
        "  background-color: #3c3c3c;"
        "  border-bottom: 1px solid #555555;"
        "}\n"
        "#ttcore-toggle-strip button {"
        "  padding: 0 6px;"
        "  min-height: 16px;"
        "  min-width: 20px;"
        "}",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // Spacer pushes button to the right
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(strip), spacer, TRUE, TRUE, 0);

    // Toggle button — starts as chevron-left (toolbar visible)
    ToggleCtx *ctx = g_new0(ToggleCtx, 1);
    ctx->button = gtk_button_new_with_label("\342\200\271");
    gtk_box_pack_end(GTK_BOX(strip), ctx->button, FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(outer), strip, FALSE, FALSE, 0);

    // Revealer wrapping the toolbar
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 200);
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    gtk_container_add(GTK_CONTAINER(revealer), toolbar);
    gtk_box_pack_start(GTK_BOX(outer), revealer, FALSE, FALSE, 0);

    // Suppress terminal resize during animation
    g_signal_connect(revealer, "notify::child-revealed",
                     G_CALLBACK(on_child_revealed), NULL);

    ctx->revealer = revealer;
    g_signal_connect(ctx->button, "clicked",
                     G_CALLBACK(on_toggle_clicked), ctx);
    // Free ctx when strip is destroyed
    g_object_set_data_full(G_OBJECT(strip), "toggle-ctx", ctx, g_free);

    if (out_revealer) *out_revealer = revealer;
    return outer;
}

void toolbar_toggle_toggle(GtkWidget *revealer) {
    terminal_view_suppress_resize(true);
    gboolean visible = gtk_revealer_get_reveal_child(
        GTK_REVEALER(revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), !visible);
}

void toolbar_toggle_notify_connected(GtkWidget *revealer, bool connected) {
    if (connected && view_settings_get_auto_hide()) {
        terminal_view_suppress_resize(true);
        gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    } else if (!connected) {
        terminal_view_suppress_resize(true);
        gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    }
}
