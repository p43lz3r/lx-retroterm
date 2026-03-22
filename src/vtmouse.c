// 2026-03-10 00:00 v0.1.0
// vtmouse.c — Mouse report encoder: X10, VT200, SGR, UTF-8, urxvt, NetTerm
// Ported from TeraTerm 5 vtterm.c (MakeMouseReportStr, MouseReport)
// Win32-free, C99, callback-based, zero global state.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "vtmouse.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal constants
// ---------------------------------------------------------------------------

#define MOUSE_POS_LIMIT  223   // X10 classic: max col/row (223+32=255)

// ---------------------------------------------------------------------------
// Internal context
// ---------------------------------------------------------------------------

struct VtMouseCtxTag {
    VtMouseOps     ops;
    void          *client_data;
    VtMouseMode    mode;
    VtMouseExtMode ext;
    int            last_x;
    int            last_y;
    int            last_button;
};

// ---------------------------------------------------------------------------
// encode_utf8_coord — encode a mouse coordinate value as UTF-8 bytes
// Returns bytes written (1 or 2); 0 on buffer overflow.
// ---------------------------------------------------------------------------

static int encode_utf8_coord(unsigned int v, char *buf, size_t avail) {
    if (v < 0x80u) {
        if (avail < 1) return 0;
        buf[0] = (char)v;
        return 1;
    }
    // Values up to 2047 fit in 2-byte UTF-8 (mouse coordinates stay well within)
    if (avail < 2) return 0;
    buf[0] = (char)(((v >> 6) & 0x1Fu) | 0xC0u);
    buf[1] = (char)((v & 0x3Fu) | 0x80u);
    return 2;
}

// ---------------------------------------------------------------------------
// modifier_to_mb — map VTMOUSE_MOD_* bits to X10 protocol modifier bits
// X10 protocol: bit2=shift, bit3=alt/meta, bit4=ctrl
// ---------------------------------------------------------------------------

static int modifier_to_mb(uint8_t modifiers) {
    int mb_mod = 0;
    if (modifiers & VTMOUSE_MOD_SHIFT) mb_mod |= 0x04;
    if (modifiers & VTMOUSE_MOD_ALT)   mb_mod |= 0x08;
    if (modifiers & VTMOUSE_MOD_CTRL)  mb_mod |= 0x10;
    return mb_mod;
}

// ---------------------------------------------------------------------------
// VtMouseEncode — pure CSI-body encoder, no context, no state
// ---------------------------------------------------------------------------

int VtMouseEncode(VtMouseExtMode ext, int mb, int x, int y,
                  char *buf, size_t bufsize) {
    if (!buf || bufsize < 1) return 0;

    switch (ext) {

    case VtMouseExt_None: {
        // Classic X10: M + 3 raw bytes; coordinates clamped to MOUSE_POS_LIMIT
        if (bufsize < 4) return 0;
        if (x > MOUSE_POS_LIMIT) x = MOUSE_POS_LIMIT;
        if (y > MOUSE_POS_LIMIT) y = MOUSE_POS_LIMIT;
        buf[0] = 'M';
        buf[1] = (char)((mb & 0xFF) + 32);
        buf[2] = (char)(x + 32);
        buf[3] = (char)(y + 32);
        return 4;
    }

    case VtMouseExt_UTF8: {
        // UTF-8 extended: M + utf8(mb+32) + utf8(x+32) + utf8(y+32)
        // Fixes TeraTerm vtterm.c line 5576: used x>>6 for y coordinate
        buf[0] = 'M';
        int pos = 1;
        int n;
        n = encode_utf8_coord((unsigned)(mb + 32), buf + pos,
                              bufsize - (size_t)pos);
        if (n == 0) return 0;
        pos += n;
        n = encode_utf8_coord((unsigned)(x + 32), buf + pos,
                              bufsize - (size_t)pos);
        if (n == 0) return 0;
        pos += n;
        n = encode_utf8_coord((unsigned)(y + 32), buf + pos,
                              bufsize - (size_t)pos);
        if (n == 0) return 0;
        pos += n;
        return pos;
    }

    case VtMouseExt_SGR:
    case VtMouseExt_SGRP: {
        // SGR: <mb_clean;x;y M  or  <mb_clean;x;y m (release)
        int mb_clean = mb & 0x7F;
        char trailer = (mb & 0x80) ? 'm' : 'M';
        int n = snprintf(buf, bufsize, "<%d;%d;%d%c", mb_clean, x, y, trailer);
        if (n < 0 || (size_t)n >= bufsize) return 0;
        return n;
    }

    case VtMouseExt_URXVT: {
        // urxvt: mb+32;x;y M
        int n = snprintf(buf, bufsize, "%d;%d;%dM", mb + 32, x, y);
        if (n < 0 || (size_t)n >= bufsize) return 0;
        return n;
    }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// VtMouseInit / Finish / SetMode / GetMode / GetExtMode
// ---------------------------------------------------------------------------

VtMouseCtx *VtMouseInit(const VtMouseOps *ops, void *client_data) {
    VtMouseCtx *ctx = (VtMouseCtx *)calloc(1, sizeof(VtMouseCtx));
    if (!ctx) return NULL;
    if (ops) ctx->ops = *ops;
    ctx->client_data = client_data;
    ctx->mode        = VtMouseMode_None;
    ctx->ext         = VtMouseExt_None;
    ctx->last_x      = -1;
    ctx->last_y      = -1;
    ctx->last_button = 0;
    return ctx;
}

void VtMouseFinish(VtMouseCtx *ctx) {
    free(ctx);  // free(NULL) is safe in C99
}

void VtMouseSetMode(VtMouseCtx *ctx, VtMouseMode mode, VtMouseExtMode ext) {
    if (!ctx) return;
    ctx->mode = mode;
    ctx->ext  = ext;
}

VtMouseMode VtMouseGetMode(const VtMouseCtx *ctx) {
    if (!ctx) return VtMouseMode_None;
    return ctx->mode;
}

VtMouseExtMode VtMouseGetExtMode(const VtMouseCtx *ctx) {
    if (!ctx) return VtMouseExt_None;
    return ctx->ext;
}

// ---------------------------------------------------------------------------
// send_csi_report — prepend ESC[ and deliver via write_report callback
// ---------------------------------------------------------------------------

static void send_csi_report(VtMouseCtx *ctx, const char *body, int body_len) {
    if (!ctx->ops.write_report || body_len <= 0) return;
    char tmp[512];
    if (body_len + 2 > (int)sizeof(tmp)) return;
    tmp[0] = '\033';
    tmp[1] = '[';
    memcpy(tmp + 2, body, (size_t)body_len);
    ctx->ops.write_report(tmp, body_len + 2, ctx->client_data);
}

// ---------------------------------------------------------------------------
// VtMouseReport — state machine; routes events to encoders
// ---------------------------------------------------------------------------

bool VtMouseReport(VtMouseCtx *ctx, VtMouseEvent event, int button,
                   int x, int y, uint8_t modifiers) {
    if (!ctx || ctx->mode == VtMouseMode_None) return false;

    // NetTerm: proprietary format, not CSI-based
    if (ctx->mode == VtMouseMode_NetTerm) {
        if (event == VtMouseEv_BtnDown && ctx->ops.write_report) {
            char tmp[64];
            // Format: ESC } y,x CR  (note: y before x)
            int n = snprintf(tmp, sizeof(tmp), "\033}%d,%d\r", y, x);
            if (n > 0) ctx->ops.write_report(tmp, n, ctx->client_data);
            return true;
        }
        return false;
    }

    char  body[64];
    int   body_len;
    int   mb;

    switch (event) {

    case VtMouseEv_BtnDown:
        mb = button | modifier_to_mb(modifiers);
        ctx->last_button = button;
        ctx->last_x = x;
        ctx->last_y = y;
        body_len = VtMouseEncode(ctx->ext, mb, x, y, body, sizeof(body));
        if (body_len <= 0) return false;
        send_csi_report(ctx, body, body_len);
        return true;

    case VtMouseEv_BtnUp:
        // X10 mode: button releases are not reported
        if (ctx->mode == VtMouseMode_X10) return false;
        if (ctx->ext == VtMouseExt_SGR || ctx->ext == VtMouseExt_SGRP) {
            // SGR/SGRP: signal release via high bit in mb
            mb = button | modifier_to_mb(modifiers) | 0x80;
        } else {
            // Classic: button 3 means "release"
            mb = VTMOUSE_BTN_RELEASE | modifier_to_mb(modifiers);
        }
        ctx->last_x = x;
        ctx->last_y = y;
        body_len = VtMouseEncode(ctx->ext, mb, x, y, body, sizeof(body));
        if (body_len <= 0) return false;
        send_csi_report(ctx, body, body_len);
        return true;

    case VtMouseEv_Move:
        if (ctx->mode != VtMouseMode_AllEvent &&
            ctx->mode != VtMouseMode_BtnEvent) return false;
        // Suppress duplicate position reports
        if (x == ctx->last_x && y == ctx->last_y) return false;
        ctx->last_x = x;
        ctx->last_y = y;
        // Motion flag: last_button | 32
        mb = ctx->last_button | 32 | modifier_to_mb(modifiers);
        body_len = VtMouseEncode(ctx->ext, mb, x, y, body, sizeof(body));
        if (body_len <= 0) return false;
        send_csi_report(ctx, body, body_len);
        return true;

    case VtMouseEv_Wheel:
        // Wheel: button offset by 64 per X10 wheel protocol
        mb = button | 64 | modifier_to_mb(modifiers);
        body_len = VtMouseEncode(ctx->ext, mb, x, y, body, sizeof(body));
        if (body_len <= 0) return false;
        send_csi_report(ctx, body, body_len);
        return true;

    case VtMouseEv_CurStat:
        return false;
    }

    return false;
}
