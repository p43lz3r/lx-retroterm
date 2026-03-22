// 2026-03-22 v0.4.0
// toolbar.h — Toolbar widget with serial port configuration.
// Phase G12: toolbar_next_port() for Ctrl+Shift+N cycling.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_TOOLBAR_H_
#define GUI_TOOLBAR_H_

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create the toolbar widget.
// The returned GtkBox is owned by the caller (GTK ref-counting).
GtkWidget *toolbar_create(void);

// Get the currently selected port path (e.g. "/dev/ttyACM0").
// Returns pointer to internal string — valid until next combo change.
const char *toolbar_get_port(void);

// Get the currently selected baud rate.
uint32_t toolbar_get_baud(void);

// Get data/parity/stop config.
void toolbar_get_dps(uint8_t *data_bits, uint8_t *stop_bits, uint8_t *parity);

// Get flow control setting.
uint8_t toolbar_get_flow(void);

// Set the port combo entry text (e.g. from config restore).
void toolbar_set_port(const char *port);

// Set the baud combo to match the given rate (selects closest match).
void toolbar_set_baud(uint32_t baud);

// Notify toolbar of connection state change.
// Updates connect button label and sensitivity of combos.
void toolbar_notify_connected(bool connected);

// Scan /dev/tty* and repopulate the port combo.
// Sorted by priority: ttyACM > ttyUSB > rest, then alphabetical.
void toolbar_scan_ports(void);

// Cycle to the next port in the combo (wraps around).
void toolbar_next_port(void);

#ifdef __cplusplus
}
#endif

#endif  // GUI_TOOLBAR_H_
