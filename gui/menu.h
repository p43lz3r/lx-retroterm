// 2026-03-18 22:00 v0.2.0
// menu.h — GtkMenuBar with all application menu entries.
// Phase G2: added View menu (Appearance, Toggle Toolbar, Auto-hide).
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_MENU_H_
#define GUI_MENU_H_

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the menu bar.
// app: GtkApplication for the quit action.
// accel_group: accelerator group attached to the window.
// window: the main window (for transient dialog).
// revealer: the toolbar GtkRevealer (for toggle).
// The returned GtkMenuBar is owned by the caller (GTK ref-counting).
GtkWidget *menu_create(GtkApplication *app, GtkAccelGroup *accel_group,
                        GtkWidget *window, GtkWidget *revealer);

#ifdef __cplusplus
}
#endif

#endif  // GUI_MENU_H_
