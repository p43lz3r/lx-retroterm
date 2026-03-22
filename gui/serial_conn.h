// 2026-03-21 v0.3.0
// serial_conn.h — Serial connection module (single-threaded, poll-driven).
// Phase G9: transfer mode callback + mode switch for XMODEM.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_SERIAL_CONN_H_
#define GUI_SERIAL_CONN_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open a serial connection.  Starts 20ms poll timer.
// Returns 0 on success, <0 on error.
int serial_conn_open(const char *device, uint32_t baud,
                     uint8_t data_bits, uint8_t stop_bits,
                     uint8_t parity, uint8_t flow);

// Close the serial connection.  Stops poll timer.
void serial_conn_close(void);

// Returns true if the connection is open.
bool serial_conn_is_open(void);

// Write data to the serial port (buffered).
// Returns 0 on success, <0 on error.
int serial_conn_write(const uint8_t *data, int len);

// Set DTR line state.
int serial_conn_set_dtr(bool active);

// Set RTS line state.
int serial_conn_set_rts(bool active);

// Returns the current throughput in bytes/sec (updated every 1s).
uint32_t serial_conn_get_throughput(void);

// Local echo: when enabled, sent bytes are also fed to terminal_view.
void serial_conn_set_local_echo(bool enable);
bool serial_conn_get_local_echo(void);

// Transfer mode: callback for RX bytes in TRANSFER mode.
// The callback signature matches TtcoreIoCallbacks.on_xfer_data.
// Pass NULL to clear.  Works even when the port is already open.
typedef void (*serial_conn_xfer_fn)(const uint8_t *data, size_t len,
                                     void *ud);
void serial_conn_set_xfer_callback(serial_conn_xfer_fn fn, void *ud);

// Switch between TERMINAL and TRANSFER I/O mode.
// In TRANSFER mode, RX bytes are routed to the xfer callback.
// In TERMINAL mode, RX bytes are routed to terminal_view_feed_data.
void serial_conn_set_transfer_mode(bool transfer);

#ifdef __cplusplus
}
#endif

#endif  // GUI_SERIAL_CONN_H_
