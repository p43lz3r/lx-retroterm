// 2026-03-18 23:30 v0.2.2
// toolbar_toggle.h — Thin toggle strip above the toolbar.
// Phase G2: always-visible strip with collapse/expand button.
// Fix: suppress terminal resize during animation.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_TOOLBAR_TOGGLE_H_
#define GUI_TOOLBAR_TOGGLE_H_

#include <gtk/gtk.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the toggle strip widget.  toolbar is wrapped in a GtkRevealer.
// Returns the outer container (toggle strip + revealer) to pack into vbox.
// *out_revealer receives the GtkRevealer for external control.
GtkWidget *toolbar_toggle_create(GtkWidget *toolbar,
                                  GtkWidget **out_revealer);

// Toggle toolbar visibility.
void toolbar_toggle_toggle(GtkWidget *revealer);

// Notify connected/disconnected state for auto-hide logic.
void toolbar_toggle_notify_connected(GtkWidget *revealer, bool connected);

#ifdef __cplusplus
}
#endif

#endif  // GUI_TOOLBAR_TOGGLE_H_
