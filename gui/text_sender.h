// 2026-03-20 v0.1.0
// text_sender.h — Line-by-line text send engine with GLib timer.
// Phase G7: Send Text / Send Text File.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_TEXT_SENDER_H_
#define GUI_TEXT_SENDER_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Line-ending modes.
typedef enum {
    TEXT_SENDER_EOL_CR   = 0,  // \r
    TEXT_SENDER_EOL_LF   = 1,  // \n
    TEXT_SENDER_EOL_CRLF = 2,  // \r\n
} TextSenderEol;

// Start sending text line-by-line via GLib timer.
// text: full text to send (copied internally).
// eol: line-ending style appended after each line.
// delay_ms: inter-line delay (0 = send all at once, no timer).
// Returns true if started, false if already sending or serial closed.
bool text_sender_start(const char *text, TextSenderEol eol, int delay_ms);

// Cancel an in-progress send.  Stops the GLib timer.
// Safe to call when not sending (no-op).
void text_sender_cancel(void);

// Returns true if a send is currently in progress.
bool text_sender_is_active(void);

#ifdef __cplusplus
}
#endif

#endif  // GUI_TEXT_SENDER_H_
