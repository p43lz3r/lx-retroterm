// 2026-03-18 v0.2.0
// status_bar.c — Status bar with connection info labels.
// Phase G3: dynamic connection indicator, throughput, error display.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "status_bar.h"

#include <stdio.h>

// ---------------------------------------------------------------------------
// Module state: label pointers for dynamic updates
// ---------------------------------------------------------------------------

static GtkWidget *g_conn_label = NULL;
static GtkWidget *g_port_label = NULL;
static GtkWidget *g_encoding_label = NULL;
static GtkWidget *g_geometry_label = NULL;
static GtkWidget *g_cursor_label = NULL;
static GtkWidget *g_throughput_label = NULL;
static GtkWidget *g_progress_label = NULL;

static GtkWidget *make_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_end(label, 6);
    return label;
}

GtkWidget *status_bar_create(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 2);
    gtk_widget_set_margin_bottom(box, 2);

    // CSS: dark background + light text for all labels inside the status bar
    gtk_widget_set_name(box, "ttcore-statusbar");
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "#ttcore-statusbar { background-color: #2d2d2d; }\n"
        "#ttcore-statusbar label { color: #d4d4d4; }",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // Connection indicator (red dot = disconnected)
    g_conn_label = gtk_label_new(NULL);
    gtk_label_set_use_markup(GTK_LABEL(g_conn_label), TRUE);
    gtk_label_set_markup(GTK_LABEL(g_conn_label),
        "<span foreground='#c94e4e'>\342\227\217</span> Disconnected");
    gtk_widget_set_margin_start(g_conn_label, 6);
    gtk_widget_set_margin_end(g_conn_label, 6);
    gtk_box_pack_start(GTK_BOX(box), g_conn_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    // Port info
    g_port_label = make_label("\342\200\224");
    gtk_box_pack_start(GTK_BOX(box), g_port_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    // Encoding
    g_encoding_label = make_label("UTF-8");
    gtk_box_pack_start(GTK_BOX(box), g_encoding_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    // Geometry
    g_geometry_label = make_label("80\303\22724");
    gtk_box_pack_start(GTK_BOX(box), g_geometry_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    // Cursor
    g_cursor_label = make_label("Cursor: 0,0");
    gtk_box_pack_start(GTK_BOX(box), g_cursor_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    // Progress (hidden by default)
    g_progress_label = make_label("");
    gtk_widget_set_no_show_all(g_progress_label, TRUE);
    gtk_box_pack_start(GTK_BOX(box), g_progress_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);

    // Throughput
    g_throughput_label = make_label("0 B/s");
    gtk_box_pack_start(GTK_BOX(box), g_throughput_label, FALSE, FALSE, 0);

    return box;
}

void status_bar_update_cursor(int x, int y) {
    if (!g_cursor_label) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Cursor: %d,%d", x, y);
    gtk_label_set_text(GTK_LABEL(g_cursor_label), buf);
}

void status_bar_update_geometry(int cols, int lines) {
    if (!g_geometry_label) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d\303\227%d", cols, lines);
    gtk_label_set_text(GTK_LABEL(g_geometry_label), buf);
}

void status_bar_set_connection(bool connected, const char *port,
                               uint32_t baud) {
    if (g_conn_label) {
        gtk_label_set_use_markup(GTK_LABEL(g_conn_label), TRUE);
        if (connected) {
            gtk_label_set_markup(GTK_LABEL(g_conn_label),
                "<span foreground='#4ec94e'>\342\227\217</span> Connected");
        } else {
            gtk_label_set_markup(GTK_LABEL(g_conn_label),
                "<span foreground='#c94e4e'>\342\227\217</span> Disconnected");
        }
    }
    if (g_port_label) {
        if (connected && port) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s @ %u", port, (unsigned)baud);
            gtk_label_set_text(GTK_LABEL(g_port_label), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(g_port_label), "\342\200\224");
        }
    }
}

void status_bar_set_error(const char *msg) {
    if (!g_conn_label) return;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "<span foreground='#c94e4e'>\342\227\217</span> Error: %s",
             msg ? msg : "");
    gtk_label_set_use_markup(GTK_LABEL(g_conn_label), TRUE);
    gtk_label_set_markup(GTK_LABEL(g_conn_label), buf);
}

void status_bar_update_throughput(uint32_t bytes_per_sec) {
    if (!g_throughput_label) return;
    char buf[32];
    if (bytes_per_sec >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB/s",
                 (double)bytes_per_sec / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%u B/s", (unsigned)bytes_per_sec);
    }
    gtk_label_set_text(GTK_LABEL(g_throughput_label), buf);
}

void status_bar_set_progress(const char *msg) {
    if (!g_progress_label) return;
    if (msg && msg[0]) {
        gtk_label_set_text(GTK_LABEL(g_progress_label), msg);
        gtk_widget_show(g_progress_label);
    } else {
        gtk_label_set_text(GTK_LABEL(g_progress_label), "");
        gtk_widget_hide(g_progress_label);
    }
}
