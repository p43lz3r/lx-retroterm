// 2026-03-21 v0.1.0
// hex_upload.h — Intel HEX file upload via text_sender.
// Phase G8: thin wrapper — file validation + delegation to text_sender.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_HEX_UPLOAD_H_
#define GUI_HEX_UPLOAD_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start Intel HEX upload.
// Reads the file, strips non-HEX lines, stops at EOF record (:00000001FF).
// Delegates line-by-line send to text_sender with EOL=CR.
// delay_ms: inter-line delay (0 = send all at once).
// Returns true if started, false if file invalid or send already active.
bool hex_upload_start(const char *filepath, int delay_ms);

// Cancel in-progress upload.  Delegates to text_sender_cancel().
// Safe to call when not uploading (no-op).
void hex_upload_cancel(void);

#ifdef __cplusplus
}
#endif

#endif  // GUI_HEX_UPLOAD_H_
