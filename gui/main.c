// 2026-03-18 14:00 v0.1.0
// main.c — GtkApplication entry point for the ttcore GUI.
// Phase G1: skeleton only — no terminal content, no serial connection.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include "app_window.h"

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    app_window_create(app);
}

// Timer callback for --quit-after: clean shutdown for Valgrind testing.
static gboolean quit_timer_cb(gpointer user_data) {
    GApplication *app = G_APPLICATION(user_data);
    g_application_quit(app);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    // Parse --quit-after <ms> before passing to GTK.
    int quit_after_ms = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quit-after") == 0 && i + 1 < argc) {
            quit_after_ms = atoi(argv[i + 1]);
            // Remove these two args so GTK doesn't see them.
            for (int j = i; j + 2 < argc; j++) {
                argv[j] = argv[j + 2];
            }
            argc -= 2;
            i--;
        }
    }

    GtkApplication *app = gtk_application_new(
        "de.ttcore.gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    if (quit_after_ms > 0) {
        g_timeout_add((guint)quit_after_ms, quit_timer_cb, app);
    }

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
