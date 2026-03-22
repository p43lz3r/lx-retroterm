// 2026-03-21 v0.2.0
// xmodem_gui.h — XMODEM file transfer GUI integration (GLib timer + RX ring).
// Phase G10: XMODEM Send + Receive via ttcore_transfer + ttcore_io mode switch.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_XMODEM_GUI_H_
#define GUI_XMODEM_GUI_H_

#include <stdbool.h>

#include "ttcore_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start an XMODEM send transfer.
// filepath: path to the file to send.
// opt: TTCORE_XMODEM_CRC (default) or TTCORE_XMODEM_1K_CRC.
// Returns true if started, false on error (not connected, already active, etc.).
bool xmodem_gui_send_start(const char *filepath, TtcoreXmodemOpt opt);

// Start an XMODEM receive transfer.
// filepath: path to save the received file.
// opt: TTCORE_XMODEM_CRC (default) or TTCORE_XMODEM_1K_CRC.
// Returns true if started, false on error (not connected, already active, etc.).
bool xmodem_gui_recv_start(const char *filepath, TtcoreXmodemOpt opt);

// Cancel an in-progress XMODEM transfer.
// Safe to call when not active (no-op).
void xmodem_gui_cancel(void);

// Returns true if an XMODEM transfer is currently in progress.
bool xmodem_gui_is_active(void);

#ifdef __cplusplus
}
#endif

#endif  // GUI_XMODEM_GUI_H_
