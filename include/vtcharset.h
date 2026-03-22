// 2026-03-10 00:00 v0.1.0
// vtcharset.h — Headless charset decoder: UTF-8, ISO 2022, SJIS, EUC, SBCS
// Ported from TeraTerm 5 charset.cpp — Win32-free, C99, callback-based.
//
// Copyright (C) 2023- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef VTCHARSET_H_
#define VTCHARSET_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Encoding enum  (maps to IdKanjiCode from tttypes_charset.h)
// ---------------------------------------------------------------------------
typedef enum {
    VtCharsetEnc_UTF8 = 0,
    // ISO 8859
    VtCharsetEnc_ISO8859_1,
    VtCharsetEnc_ISO8859_2,
    VtCharsetEnc_ISO8859_3,
    VtCharsetEnc_ISO8859_4,
    VtCharsetEnc_ISO8859_5,
    VtCharsetEnc_ISO8859_6,
    VtCharsetEnc_ISO8859_7,
    VtCharsetEnc_ISO8859_8,
    VtCharsetEnc_ISO8859_9,
    VtCharsetEnc_ISO8859_10,
    VtCharsetEnc_ISO8859_11,
    VtCharsetEnc_ISO8859_13,
    VtCharsetEnc_ISO8859_14,
    VtCharsetEnc_ISO8859_15,
    VtCharsetEnc_ISO8859_16,
    // Japanese
    VtCharsetEnc_SJIS,
    VtCharsetEnc_EUC,
    VtCharsetEnc_JIS,
    // Korean
    VtCharsetEnc_CP949,
    // Chinese
    VtCharsetEnc_GB2312,
    VtCharsetEnc_Big5,
    // Code Pages (Single Byte)
    VtCharsetEnc_CP437,
    VtCharsetEnc_CP737,
    VtCharsetEnc_CP775,
    VtCharsetEnc_CP850,
    VtCharsetEnc_CP852,
    VtCharsetEnc_CP855,
    VtCharsetEnc_CP857,
    VtCharsetEnc_CP860,
    VtCharsetEnc_CP861,
    VtCharsetEnc_CP862,
    VtCharsetEnc_CP863,
    VtCharsetEnc_CP864,
    VtCharsetEnc_CP865,
    VtCharsetEnc_CP866,
    VtCharsetEnc_CP869,
    VtCharsetEnc_CP874,
    VtCharsetEnc_CP1250,
    VtCharsetEnc_CP1251,
    VtCharsetEnc_CP1252,
    VtCharsetEnc_CP1253,
    VtCharsetEnc_CP1254,
    VtCharsetEnc_CP1255,
    VtCharsetEnc_CP1256,
    VtCharsetEnc_CP1257,
    VtCharsetEnc_CP1258,
    VtCharsetEnc_KOI8,
    // Internal
    VtCharsetEnc_Debug,
    VtCharsetEnc_COUNT_
} VtCharsetEncoding;

// ---------------------------------------------------------------------------
// ISO 2022 character set slot contents
// ---------------------------------------------------------------------------
typedef enum {
    VtCharsetCS_ASCII    = 0,
    VtCharsetCS_Katakana = 1,
    VtCharsetCS_Kanji    = 2,
    VtCharsetCS_Special  = 3,
} VtCharsetCS;

// ---------------------------------------------------------------------------
// ISO 2022 locking/single shift
// ---------------------------------------------------------------------------
typedef enum {
    VtCharsetShift_LS0,   // Locking Shift 0: G0 → GL  (SI, 0x0F)
    VtCharsetShift_LS1,   // Locking Shift 1: G1 → GL  (SO, 0x0E)
    VtCharsetShift_LS2,   // Locking Shift 2: G2 → GL  (ESC n)
    VtCharsetShift_LS3,   // Locking Shift 3: G3 → GL  (ESC o)
    VtCharsetShift_LS1R,  // Locking Shift 1: G1 → GR  (ESC ~)
    VtCharsetShift_LS2R,  // Locking Shift 2: G2 → GR  (ESC })
    VtCharsetShift_LS3R,  // Locking Shift 3: G3 → GR  (ESC |)
    VtCharsetShift_SS2,   // Single Shift 2: next char from G2
    VtCharsetShift_SS3,   // Single Shift 3: next char from G3
} VtCharset2022Shift;

// ---------------------------------------------------------------------------
// Configuration  (replaces relevant ts.* globals)
// ---------------------------------------------------------------------------
typedef struct {
    VtCharsetEncoding encoding;   // Active encoding
    bool fallback_to_cp932;       // UTF-8: on bad seq, try SJIS
    bool jis7_katakana;           // JIS: use 7-bit katakana (not 8-bit)
    bool ctrl_in_kanji;           // Allow ctrl bytes inside MBCS sequence
    bool fixed_jis;               // JIS: fixed 8-bit katakana mode
    bool iso2022_ss2;             // Honour EUC SS2 (0x8E)
    bool iso2022_ss3;             // Honour EUC SS3 (0x8F)
} VtCharsetConfig;

// ---------------------------------------------------------------------------
// ISO 2022 state snapshot (for DECSC/DECRC)
// ---------------------------------------------------------------------------
typedef struct {
    int v[6];  // Glr[0], Glr[1], Gn[0..3]
} VtCharsetState;

// ---------------------------------------------------------------------------
// Callbacks  (every external effect goes through here)
// ---------------------------------------------------------------------------
typedef struct {
    // Unicode code point output
    void (*put_char)(uint32_t u32, void *client_data);
    // C0/C1 control byte (0x00-0x1F, 0x80-0x9F)
    void (*put_ctrl)(uint8_t b, void *client_data);
} VtCharsetOps;

// ---------------------------------------------------------------------------
// Opaque context
// ---------------------------------------------------------------------------
typedef struct VtCharsetCtxTag VtCharsetCtx;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Create context.  ops and cfg are copied internally.
// Returns NULL on allocation failure.
VtCharsetCtx *VtCharsetInit(const VtCharsetOps *ops, void *client_data,
                             const VtCharsetConfig *cfg);

// Free all resources (safe to call with NULL).
void VtCharsetFinish(VtCharsetCtx *ctx);

// Update configuration at runtime (e.g., encoding change).
void VtCharsetSetConfig(VtCharsetCtx *ctx, const VtCharsetConfig *cfg);

// Feed one raw byte from the transport layer.
// Triggers put_char / put_ctrl callbacks as needed.
void VtCharsetFeedByte(VtCharsetCtx *ctx, uint8_t b);

// ISO 2022 designation: set Gn[gn] = cs.  gn ∈ {0,1,2,3}.
void VtCharset2022Designate(VtCharsetCtx *ctx, int gn, VtCharsetCS cs);

// ISO 2022 invocation: shift GL/GR, or set Single Shift.
void VtCharset2022Invoke(VtCharsetCtx *ctx, VtCharset2022Shift shift);

// Returns true if byte b maps to DEC Special Graphics character.
bool VtCharsetIsSpecial(VtCharsetCtx *ctx, uint8_t b);

// Save / restore the ISO 2022 GL/GR/G0-G3 state.
void VtCharsetSaveState(VtCharsetCtx *ctx, VtCharsetState *state);
void VtCharsetLoadState(VtCharsetCtx *ctx, const VtCharsetState *state);

// Terminate UTF-8 → CP932 fallback sequence.
void VtCharsetFallbackFinish(VtCharsetCtx *ctx);

// Debug mode: 0=off 1=normal 2=hex 3=no-output
void VtCharsetSetDebugMode(VtCharsetCtx *ctx, uint8_t mode);
uint8_t VtCharsetGetDebugMode(const VtCharsetCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif  // VTCHARSET_H_
