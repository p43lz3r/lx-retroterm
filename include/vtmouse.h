// 2026-03-10 00:00 v0.1.0
// vtmouse.h — Headless mouse report encoder: X10, VT200, SGR, UTF-8, urxvt
// Ported from TeraTerm 5 vtterm.c (MakeMouseReportStr, MouseReport)
// Win32-free, C99, callback-based, zero global state.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef VTMOUSE_H_
#define VTMOUSE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Mouse tracking mode  (maps to IdMouseTrack* from tttypes.h)
// ---------------------------------------------------------------------------
typedef enum {
    VtMouseMode_None     = 0,  // No tracking
    VtMouseMode_DECELR   = 1,  // VT320 DECELR locator
    VtMouseMode_X10      = 2,  // X10 — button press only
    VtMouseMode_VT200    = 3,  // VT200 — button + release
    VtMouseMode_VT200Hl  = 4,  // VT200 highlight (not supported)
    VtMouseMode_BtnEvent = 5,  // Button-event tracking
    VtMouseMode_AllEvent = 6,  // All-motion tracking
    VtMouseMode_NetTerm  = 7,  // NetTerm format
} VtMouseMode;

// ---------------------------------------------------------------------------
// Mouse extension mode  (maps to IdMouseTrackExt* from tttypes.h)
// ---------------------------------------------------------------------------
typedef enum {
    VtMouseExt_None  = 0,  // Classic X10/VT200 (byte-encoded, limit 223)
    VtMouseExt_UTF8  = 1,  // UTF-8 extended (CSI 1005, limit 2015)
    VtMouseExt_SGR   = 2,  // SGR decimal (CSI 1006)
    VtMouseExt_URXVT = 3,  // rxvt-unicode decimal (CSI 1015)
    VtMouseExt_SGRP  = 4,  // SGR-Pixels (CSI 1016)
} VtMouseExtMode;

// ---------------------------------------------------------------------------
// Mouse event type  (maps to IdMouseEvent* from tttypes.h)
// ---------------------------------------------------------------------------
typedef enum {
    VtMouseEv_CurStat = 0,  // Cursor status query
    VtMouseEv_BtnDown = 1,  // Button pressed
    VtMouseEv_BtnUp   = 2,  // Button released
    VtMouseEv_Move    = 3,  // Motion
    VtMouseEv_Wheel   = 4,  // Scroll wheel
} VtMouseEvent;

// ---------------------------------------------------------------------------
// Button numbers  (matches TeraTerm: 0=left, 1=middle, 2=right, 3=release)
// ---------------------------------------------------------------------------
#define VTMOUSE_BTN_LEFT    0
#define VTMOUSE_BTN_MIDDLE  1
#define VTMOUSE_BTN_RIGHT   2
#define VTMOUSE_BTN_RELEASE 3
#define VTMOUSE_BTN_WHEEL_UP   64
#define VTMOUSE_BTN_WHEEL_DOWN 65

// ---------------------------------------------------------------------------
// Modifier key bitmask (passed to VtMouseReport)
// ---------------------------------------------------------------------------
#define VTMOUSE_MOD_SHIFT  0x01u
#define VTMOUSE_MOD_ALT    0x02u
#define VTMOUSE_MOD_CTRL   0x04u

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------
typedef struct {
    // Deliver a complete escape sequence to the remote host.
    // For CSI modes: "\033[" + encoded string.
    // For NetTerm:   "\033}" + encoded string + "\r".
    void (*write_report)(const char *buf, int len, void *client_data);
} VtMouseOps;

// ---------------------------------------------------------------------------
// Opaque context
// ---------------------------------------------------------------------------
typedef struct VtMouseCtxTag VtMouseCtx;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Create context.  ops is copied internally.
// Returns NULL on allocation failure.
VtMouseCtx *VtMouseInit(const VtMouseOps *ops, void *client_data);

// Free all resources (safe to call with NULL).
void VtMouseFinish(VtMouseCtx *ctx);

// Set the tracking and extension mode at runtime.
void VtMouseSetMode(VtMouseCtx *ctx, VtMouseMode mode, VtMouseExtMode ext);

// Retrieve current modes.
VtMouseMode    VtMouseGetMode(const VtMouseCtx *ctx);
VtMouseExtMode VtMouseGetExtMode(const VtMouseCtx *ctx);

// Report a mouse event.
//   event     — VtMouseEv_*
//   button    — VTMOUSE_BTN_* or VTMOUSE_BTN_WHEEL_*
//   x, y      — 1-based screen column / row (already in terminal coordinates)
//   modifiers — VTMOUSE_MOD_* bitmask
//
// Returns true if a report was generated and sent via write_report.
bool VtMouseReport(VtMouseCtx *ctx, VtMouseEvent event, int button,
                   int x, int y, uint8_t modifiers);

// Pure encoder: build the CSI-body of a mouse report into buf.
// No state is touched; no callbacks are fired.
// Returns number of bytes written (0 on error / out-of-bounds).
int VtMouseEncode(VtMouseExtMode ext, int mb, int x, int y,
                  char *buf, size_t bufsize);

#ifdef __cplusplus
}
#endif

#endif  // VTMOUSE_H_
