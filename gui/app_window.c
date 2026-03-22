// 2026-03-20 v0.5.0
// app_window.c — GtkApplicationWindow: root layout and orchestrator.
// Phase G6: config load/save, window geometry persistence, serial config dialog.
// Sole file that includes all sibling module headers.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "app_window.h"
#include "config.h"
#include "menu.h"
#include "toolbar.h"
#include "toolbar_toggle.h"
#include "terminal_view.h"
#include "status_bar.h"
#include "view_settings.h"
#include "serial_conn.h"

// Module state for cross-module notifications
static GtkWidget *g_revealer = NULL;
static GtkWidget *g_window = NULL;

static void save_window_geometry(void) {
    if (!g_window) return;
    int w, h;
    gtk_window_get_size(GTK_WINDOW(g_window), &w, &h);
    config_set_int("Window", "width", w);
    config_set_int("Window", "height", h);

    int x, y;
    gtk_window_get_position(GTK_WINDOW(g_window), &x, &y);
    config_set_int("Window", "x", x);
    config_set_int("Window", "y", y);

    gboolean maximized = gtk_window_is_maximized(GTK_WINDOW(g_window));
    config_set_bool("Window", "maximized", maximized);
}

static void save_connection_settings(void) {
    const char *port = toolbar_get_port();
    if (port && port[0]) {
        config_set_string("Connection", "port", port);
    }
    config_set_int("Connection", "baud", (int)toolbar_get_baud());
}

static void on_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    serial_conn_close();
    save_window_geometry();
    save_connection_settings();
    config_save();
    terminal_view_destroy();
}

void app_window_create(GtkApplication *app) {
    // Load persistent configuration
    config_load();

    // Window
    GtkWidget *window = gtk_application_window_new(app);
    g_window = window;
    gtk_window_set_title(GTK_WINDOW(window), "ttcore v0.5.0");

    // Restore window geometry from config (clamp to sane range)
    int w = config_get_int("Window", "width", 800);
    int h = config_get_int("Window", "height", 600);
    if (w < 640 || w > 4096) w = 800;
    if (h < 384 || h > 4096) h = 600;
    gtk_window_set_default_size(GTK_WINDOW(window), w, h);

    int x = config_get_int("Window", "x", -1);
    int y = config_get_int("Window", "y", -1);
    if (x >= 0 && x < 8192 && y >= 0 && y < 8192) {
        gtk_window_move(GTK_WINDOW(window), x, y);
    }
    if (config_get_bool("Window", "maximized", false)) {
        gtk_window_maximize(GTK_WINDOW(window));
    }

    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    // Accelerator group — shared by all menu items
    GtkAccelGroup *accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(window), accel_group);

    // Root layout: vertical box
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Toolbar + toggle strip (must be created before menu for revealer ptr)
    GtkWidget *toolbar = toolbar_create();
    GtkWidget *revealer = NULL;
    GtkWidget *toggle_strip = toolbar_toggle_create(toolbar, &revealer);
    g_revealer = revealer;

    // Restore last-used port/baud from config into toolbar
    const char *saved_port = config_get_string("Connection", "port", NULL);
    if (saved_port) toolbar_set_port(saved_port);
    int saved_baud = config_get_int("Connection", "baud", 0);
    if (saved_baud > 0) toolbar_set_baud((uint32_t)saved_baud);

    // Menu bar (needs window + revealer)
    GtkWidget *menubar = menu_create(app, accel_group, window, revealer);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    // Toggle strip + revealer (toolbar inside)
    gtk_box_pack_start(GTK_BOX(vbox), toggle_strip, FALSE, FALSE, 0);

    // Horizontal GtkPaned: terminal (left) + side panel (right, hidden)
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    // Terminal view (left pane, expands)
    GtkWidget *terminal = terminal_view_create();
    gtk_paned_pack1(GTK_PANED(paned), terminal, TRUE, FALSE);

    // Side panel placeholder (right pane, hidden by default)
    GtkWidget *side_panel = gtk_label_new("Side Panel");
    gtk_paned_pack2(GTK_PANED(paned), side_panel, FALSE, FALSE);
    gtk_widget_set_no_show_all(side_panel, TRUE);

    // Status bar
    GtkWidget *status = status_bar_create();
    gtk_box_pack_start(GTK_BOX(vbox), status, FALSE, FALSE, 0);

    // accel_group ref is now held by the window
    g_object_unref(accel_group);

    gtk_widget_show_all(window);

    // Apply backspace key setting from config
    int bskey = config_get_int("Connection", "backspace_key", 0);
    terminal_view_set_backspace_del(bskey == 1);

    // Grab focus to terminal drawing area for key input
    terminal_view_grab_focus();
}

void app_window_notify_connected(bool connected) {
    // Update toolbar state (button label, combo sensitivity)
    toolbar_notify_connected(connected);

    // Update status bar
    if (connected) {
        const char *port = toolbar_get_port();
        uint32_t baud = toolbar_get_baud();
        status_bar_set_connection(true, port, baud);
    } else {
        status_bar_set_connection(false, NULL, 0);
    }

    // Toolbar auto-hide on connect / re-show on disconnect
    if (g_revealer) {
        toolbar_toggle_notify_connected(g_revealer, connected);
    }
}
