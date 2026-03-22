// 2026-03-20 v0.1.0
// serial_config_dialog.h — Dialog for detailed serial port configuration.
// Phase G6: full parameter control beyond toolbar combos.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_SERIAL_CONFIG_DIALOG_H_
#define GUI_SERIAL_CONFIG_DIALOG_H_

#include <gtk/gtk.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Show the Serial Config dialog.
// Pre-populates fields from config.ini; on OK, saves to config and updates toolbar.
// parent: transient-for window.
// Returns true if user clicked OK (settings were applied).
bool serial_config_dialog_show(GtkWindow *parent);

#ifdef __cplusplus
}
#endif

#endif  // GUI_SERIAL_CONFIG_DIALOG_H_
