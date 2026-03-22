// 2026-03-18 v0.2.0
// app_window.h — GtkApplicationWindow: root layout and orchestrator.
// Phase G3: serial connection notification, toolbar auto-hide on connect.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_APP_WINDOW_H_
#define GUI_APP_WINDOW_H_

#include <gtk/gtk.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create and show the main application window.
void app_window_create(GtkApplication *app);

// Notify the window of a serial connection state change.
// Updates status bar, toolbar, and menu state.
void app_window_notify_connected(bool connected);

#ifdef __cplusplus
}
#endif

#endif  // GUI_APP_WINDOW_H_
