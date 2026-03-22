// 2026-03-10 00:00 v0.1.0
// vtcharset.c — Headless charset decoder: UTF-8, ISO 2022, SJIS, EUC, SBCS
// Ported from TeraTerm 5 charset.cpp + codeconv_mb.cpp (unicode.cpp tables)
// Win32-free, C99, callback-based, zero global state.
//
// Copyright (C) 2023- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <iconv.h>
#include <errno.h>

#include "vtcharset.h"

// ---------------------------------------------------------------------------
// Internal constants
// ---------------------------------------------------------------------------

#define REPLACEMENT_CHAR  0xFFFDU   // Unicode replacement character
#define US  0x1FU                   // Unit Separator (last C0 control)
#define DEL 0x7FU                   // Delete

#define DEBUG_NONE  0
#define DEBUG_NORM  1
#define DEBUG_HEXD  2
#define DEBUG_NOUT  3
#define DEBUG_MAX   4

// ---------------------------------------------------------------------------
// SBCS lookup table type  (matches SBCSTable_t from TeraTerm)
// ---------------------------------------------------------------------------

typedef struct {
    unsigned char  code;     // 0x00..0xFF SBCS byte
    unsigned short unicode;  // Unicode (≤ U+FFFF)
} SbcsEntry;

// ---------------------------------------------------------------------------
// SBCS lookup tables — one per encoding
// Generated from TeraTerm mapping/*.map (BSD 3-Clause)
// ---------------------------------------------------------------------------

static const SbcsEntry kISO8859_2[] = {
#include "mapping/iso8859-2.map"
};
static const SbcsEntry kISO8859_3[] = {
#include "mapping/iso8859-3.map"
};
static const SbcsEntry kISO8859_4[] = {
#include "mapping/iso8859-4.map"
};
static const SbcsEntry kISO8859_5[] = {
#include "mapping/iso8859-5.map"
};
static const SbcsEntry kISO8859_6[] = {
#include "mapping/iso8859-6.map"
};
static const SbcsEntry kISO8859_7[] = {
#include "mapping/iso8859-7.map"
};
static const SbcsEntry kISO8859_8[] = {
#include "mapping/iso8859-8.map"
};
static const SbcsEntry kISO8859_9[] = {
#include "mapping/iso8859-9.map"
};
static const SbcsEntry kISO8859_10[] = {
#include "mapping/iso8859-10.map"
};
static const SbcsEntry kISO8859_11[] = {
#include "mapping/iso8859-11.map"
};
static const SbcsEntry kISO8859_13[] = {
#include "mapping/iso8859-13.map"
};
static const SbcsEntry kISO8859_14[] = {
#include "mapping/iso8859-14.map"
};
static const SbcsEntry kISO8859_15[] = {
#include "mapping/iso8859-15.map"
};
static const SbcsEntry kISO8859_16[] = {
#include "mapping/iso8859-16.map"
};
static const SbcsEntry kCP437[] = {
#include "mapping/cp437.map"
};
static const SbcsEntry kCP737[] = {
#include "mapping/cp737.map"
};
static const SbcsEntry kCP775[] = {
#include "mapping/cp775.map"
};
static const SbcsEntry kCP850[] = {
#include "mapping/cp850.map"
};
static const SbcsEntry kCP852[] = {
#include "mapping/cp852.map"
};
static const SbcsEntry kCP855[] = {
#include "mapping/cp855.map"
};
static const SbcsEntry kCP857[] = {
#include "mapping/cp857.map"
};
static const SbcsEntry kCP860[] = {
#include "mapping/cp860.map"
};
static const SbcsEntry kCP861[] = {
#include "mapping/cp861.map"
};
static const SbcsEntry kCP862[] = {
#include "mapping/cp862.map"
};
static const SbcsEntry kCP863[] = {
#include "mapping/cp863.map"
};
static const SbcsEntry kCP864[] = {
#include "mapping/cp864.map"
};
static const SbcsEntry kCP865[] = {
#include "mapping/cp865.map"
};
static const SbcsEntry kCP866[] = {
#include "mapping/cp866.map"
};
static const SbcsEntry kCP869[] = {
#include "mapping/cp869.map"
};
static const SbcsEntry kCP874[] = {
#include "mapping/cp874.map"
};
static const SbcsEntry kCP1250[] = {
#include "mapping/cp1250.map"
};
static const SbcsEntry kCP1251[] = {
#include "mapping/cp1251.map"
};
static const SbcsEntry kCP1252[] = {
#include "mapping/cp1252.map"
};
static const SbcsEntry kCP1253[] = {
#include "mapping/cp1253.map"
};
static const SbcsEntry kCP1254[] = {
#include "mapping/cp1254.map"
};
static const SbcsEntry kCP1255[] = {
#include "mapping/cp1255.map"
};
static const SbcsEntry kCP1256[] = {
#include "mapping/cp1256.map"
};
static const SbcsEntry kCP1257[] = {
#include "mapping/cp1257.map"
};
static const SbcsEntry kCP1258[] = {
#include "mapping/cp1258.map"
};
static const SbcsEntry kKOI8R[] = {
#include "mapping/koi8-r.map"
};

// ---------------------------------------------------------------------------
// SBCS table lookup
// ---------------------------------------------------------------------------

typedef struct {
    VtCharsetEncoding  enc;
    const SbcsEntry   *table;
    int                size;
} SbcsTableInfo;

static const SbcsTableInfo kSbcsTables[] = {
    { VtCharsetEnc_ISO8859_1,  NULL,        0                                    },
    { VtCharsetEnc_ISO8859_2,  kISO8859_2,  (int)(sizeof(kISO8859_2)  / sizeof(kISO8859_2[0]))  },
    { VtCharsetEnc_ISO8859_3,  kISO8859_3,  (int)(sizeof(kISO8859_3)  / sizeof(kISO8859_3[0]))  },
    { VtCharsetEnc_ISO8859_4,  kISO8859_4,  (int)(sizeof(kISO8859_4)  / sizeof(kISO8859_4[0]))  },
    { VtCharsetEnc_ISO8859_5,  kISO8859_5,  (int)(sizeof(kISO8859_5)  / sizeof(kISO8859_5[0]))  },
    { VtCharsetEnc_ISO8859_6,  kISO8859_6,  (int)(sizeof(kISO8859_6)  / sizeof(kISO8859_6[0]))  },
    { VtCharsetEnc_ISO8859_7,  kISO8859_7,  (int)(sizeof(kISO8859_7)  / sizeof(kISO8859_7[0]))  },
    { VtCharsetEnc_ISO8859_8,  kISO8859_8,  (int)(sizeof(kISO8859_8)  / sizeof(kISO8859_8[0]))  },
    { VtCharsetEnc_ISO8859_9,  kISO8859_9,  (int)(sizeof(kISO8859_9)  / sizeof(kISO8859_9[0]))  },
    { VtCharsetEnc_ISO8859_10, kISO8859_10, (int)(sizeof(kISO8859_10) / sizeof(kISO8859_10[0])) },
    { VtCharsetEnc_ISO8859_11, kISO8859_11, (int)(sizeof(kISO8859_11) / sizeof(kISO8859_11[0])) },
    { VtCharsetEnc_ISO8859_13, kISO8859_13, (int)(sizeof(kISO8859_13) / sizeof(kISO8859_13[0])) },
    { VtCharsetEnc_ISO8859_14, kISO8859_14, (int)(sizeof(kISO8859_14) / sizeof(kISO8859_14[0])) },
    { VtCharsetEnc_ISO8859_15, kISO8859_15, (int)(sizeof(kISO8859_15) / sizeof(kISO8859_15[0])) },
    { VtCharsetEnc_ISO8859_16, kISO8859_16, (int)(sizeof(kISO8859_16) / sizeof(kISO8859_16[0])) },
    { VtCharsetEnc_CP437,  kCP437,  (int)(sizeof(kCP437)  / sizeof(kCP437[0]))  },
    { VtCharsetEnc_CP737,  kCP737,  (int)(sizeof(kCP737)  / sizeof(kCP737[0]))  },
    { VtCharsetEnc_CP775,  kCP775,  (int)(sizeof(kCP775)  / sizeof(kCP775[0]))  },
    { VtCharsetEnc_CP850,  kCP850,  (int)(sizeof(kCP850)  / sizeof(kCP850[0]))  },
    { VtCharsetEnc_CP852,  kCP852,  (int)(sizeof(kCP852)  / sizeof(kCP852[0]))  },
    { VtCharsetEnc_CP855,  kCP855,  (int)(sizeof(kCP855)  / sizeof(kCP855[0]))  },
    { VtCharsetEnc_CP857,  kCP857,  (int)(sizeof(kCP857)  / sizeof(kCP857[0]))  },
    { VtCharsetEnc_CP860,  kCP860,  (int)(sizeof(kCP860)  / sizeof(kCP860[0]))  },
    { VtCharsetEnc_CP861,  kCP861,  (int)(sizeof(kCP861)  / sizeof(kCP861[0]))  },
    { VtCharsetEnc_CP862,  kCP862,  (int)(sizeof(kCP862)  / sizeof(kCP862[0]))  },
    { VtCharsetEnc_CP863,  kCP863,  (int)(sizeof(kCP863)  / sizeof(kCP863[0]))  },
    { VtCharsetEnc_CP864,  kCP864,  (int)(sizeof(kCP864)  / sizeof(kCP864[0]))  },
    { VtCharsetEnc_CP865,  kCP865,  (int)(sizeof(kCP865)  / sizeof(kCP865[0]))  },
    { VtCharsetEnc_CP866,  kCP866,  (int)(sizeof(kCP866)  / sizeof(kCP866[0]))  },
    { VtCharsetEnc_CP869,  kCP869,  (int)(sizeof(kCP869)  / sizeof(kCP869[0]))  },
    { VtCharsetEnc_CP874,  kCP874,  (int)(sizeof(kCP874)  / sizeof(kCP874[0]))  },
    { VtCharsetEnc_CP1250, kCP1250, (int)(sizeof(kCP1250) / sizeof(kCP1250[0])) },
    { VtCharsetEnc_CP1251, kCP1251, (int)(sizeof(kCP1251) / sizeof(kCP1251[0])) },
    { VtCharsetEnc_CP1252, kCP1252, (int)(sizeof(kCP1252) / sizeof(kCP1252[0])) },
    { VtCharsetEnc_CP1253, kCP1253, (int)(sizeof(kCP1253) / sizeof(kCP1253[0])) },
    { VtCharsetEnc_CP1254, kCP1254, (int)(sizeof(kCP1254) / sizeof(kCP1254[0])) },
    { VtCharsetEnc_CP1255, kCP1255, (int)(sizeof(kCP1255) / sizeof(kCP1255[0])) },
    { VtCharsetEnc_CP1256, kCP1256, (int)(sizeof(kCP1256) / sizeof(kCP1256[0])) },
    { VtCharsetEnc_CP1257, kCP1257, (int)(sizeof(kCP1257) / sizeof(kCP1257[0])) },
    { VtCharsetEnc_CP1258, kCP1258, (int)(sizeof(kCP1258) / sizeof(kCP1258[0])) },
    { VtCharsetEnc_KOI8,   kKOI8R,  (int)(sizeof(kKOI8R)  / sizeof(kKOI8R[0]))  },
};

// Lookup SBCS byte → Unicode.  Returns 0 if not found.
static uint32_t SbcsToU32(VtCharsetEncoding enc, uint8_t b) {
    if (enc == VtCharsetEnc_ISO8859_1) {
        return (uint32_t)b;  // ISO 8859-1 == Unicode for U+0000..U+00FF
    }
    for (size_t i = 0; i < sizeof(kSbcsTables)/sizeof(kSbcsTables[0]); i++) {
        if (kSbcsTables[i].enc != enc) continue;
        const SbcsEntry *tbl = kSbcsTables[i].table;
        int n = kSbcsTables[i].size;
        if (!tbl || n == 0) return (uint32_t)b;
        // Tables are 0x00..0xFF indexed; if size == 256, direct lookup
        if (n == 0x100) return (uint32_t)tbl[b].unicode;
        for (int j = 0; j < n; j++) {
            if (tbl[j].code == b) return (uint32_t)tbl[j].unicode;
        }
        return 0;
    }
    return (uint32_t)b;
}

// ---------------------------------------------------------------------------
// JIS ↔ SJIS math  (ported from codeconv_mb.cpp — pure arithmetic)
// ---------------------------------------------------------------------------

static uint16_t JisToSjis(uint16_t kcode) {
    uint16_t n1 = (uint16_t)((kcode - 0x2121U) / 0x200U);
    uint16_t n2 = (uint16_t)((kcode - 0x2121U) % 0x200U);
    uint16_t sjis;

    if (n1 <= 0x1eU) {
        sjis = (uint16_t)(0x8100U + (uint16_t)(n1 * 256U));
    } else {
        sjis = (uint16_t)(0xC100U + (uint16_t)(n1 * 256U));
    }

    if (n2 <= 0x3eU) {
        return (uint16_t)(sjis + n2 + 0x40U);
    } else if (n2 <= 0x5dU) {
        return (uint16_t)(sjis + n2 + 0x41U);
    } else {
        return (uint16_t)(sjis + n2 - 0x61U);
    }
}

// ---------------------------------------------------------------------------
// MBCS → UTF-32 via iconv
// Uses iconv to convert 1 or 2 bytes from the given charset into a UTF-8
// buffer, then decodes the first UTF-8 code point as uint32_t.
// Returns 0 on conversion failure.
// ---------------------------------------------------------------------------

static uint32_t MbcsToU32(iconv_t cd, uint8_t b1, uint8_t b2, bool is_double) {
    char   in_buf[2];
    char   out_buf[8];
    char  *in  = in_buf;
    char  *out = out_buf;
    size_t in_left  = is_double ? 2U : 1U;
    size_t out_left = sizeof(out_buf);
    size_t bytes_written;
    const uint8_t *p;
    uint32_t u32;

    in_buf[0] = (char)b1;
    in_buf[1] = (char)b2;
    memset(out_buf, 0, sizeof(out_buf));

    if (cd == (iconv_t)-1) return 0;

    if (iconv(cd, &in, &in_left, &out, &out_left) == (size_t)-1) {
        iconv(cd, NULL, NULL, NULL, NULL);  // reset iconv state
        return 0;
    }

    bytes_written = sizeof(out_buf) - out_left;
    if (bytes_written == 0) return 0;

    // Decode first UTF-8 code point
    p = (const uint8_t *)out_buf;
    u32 = 0;
    if (p[0] < 0x80U) {
        u32 = p[0];
    } else if (p[0] < 0xE0U && bytes_written >= 2U) {
        u32 = ((uint32_t)(p[0] & 0x1FU) << 6U) | (uint32_t)(p[1] & 0x3FU);
    } else if (p[0] < 0xF0U && bytes_written >= 3U) {
        u32 = ((uint32_t)(p[0] & 0x0FU) << 12U)
            | ((uint32_t)(p[1] & 0x3FU) << 6U)
            |  (uint32_t)(p[2] & 0x3FU);
    } else if (bytes_written >= 4U) {
        u32 = ((uint32_t)(p[0] & 0x07U) << 18U)
            | ((uint32_t)(p[1] & 0x3FU) << 12U)
            | ((uint32_t)(p[2] & 0x3FU) << 6U)
            |  (uint32_t)(p[3] & 0x3FU);
    }
    return u32;
}

// ---------------------------------------------------------------------------
// iconv handle helpers
// ---------------------------------------------------------------------------

// Returns the iconv charset name for an encoding, or NULL if not MBCS.
static const char *IconvNameFor(VtCharsetEncoding enc) {
    switch (enc) {
    // All Japanese modes convert input to SJIS byte pairs via JIS math first,
    // then decode those SJIS bytes via CP932 → UTF-8.
    case VtCharsetEnc_SJIS:   return "CP932";
    case VtCharsetEnc_EUC:    return "CP932";
    case VtCharsetEnc_JIS:    return "CP932";
    // Korean / Chinese: direct MBCS decode
    case VtCharsetEnc_CP949:  return "CP949";
    case VtCharsetEnc_GB2312: return "CP936";
    case VtCharsetEnc_Big5:   return "CP950";
    default:                  return NULL;
    }
}

// ---------------------------------------------------------------------------
// Opaque context definition
// ---------------------------------------------------------------------------

struct VtCharsetCtxTag {
    // --- Config (replaces ts.*) ---
    VtCharsetConfig cfg;

    // --- Callbacks ---
    VtCharsetOps ops;
    void        *client_data;

    // --- ISO 2022 state ---
    int         glr[2];      // GL index (0..3), GR index (0..3)
    VtCharsetCS gn[4];       // G0, G1, G2, G3 charset slots
    int         gl_tmp;      // Temporary GL for SS2/SS3
    bool        ss_flag;     // Single Shift active

    // --- UTF-8 accumulator ---
    uint8_t utf8_buf[4];
    int     utf8_count;
    bool    fallbacked;      // UTF-8 → SJIS fallback mid-sequence

    // --- MBCS (SJIS/EUC/JIS/KR/CN) state ---
    bool     kanji_in;       // Awaiting 2nd byte
    uint16_t kanji;          // 1st byte stored << 8
    bool     conv_jis;       // Need JIS→SJIS conversion on 2nd byte

    // --- EUC supplementary ---
    bool euc_kana_in;        // After EUC SS2 (0x8E)
    bool euc_sup_in;         // After EUC SS3 (0x8F)
    int  euc_count;          // Remaining supplementary bytes

    // --- Debug ---
    uint8_t debug_flag;

    // --- iconv for MBCS → UTF-32 (via UTF-8) ---
    iconv_t iconv_cd;        // (iconv_t)-1 if unused
};

// ---------------------------------------------------------------------------
// ISO 2022 default state initialisation
// ---------------------------------------------------------------------------

static void Iso2022Init(VtCharsetCtx *ctx) {
    VtCharsetEncoding enc = ctx->cfg.encoding;
    bool is_japanese = (enc == VtCharsetEnc_SJIS ||
                        enc == VtCharsetEnc_EUC  ||
                        enc == VtCharsetEnc_JIS);

    if (is_japanese) {
        ctx->gn[0] = VtCharsetCS_ASCII;
        ctx->gn[1] = VtCharsetCS_Katakana;
        ctx->gn[2] = VtCharsetCS_Katakana;
        ctx->gn[3] = VtCharsetCS_Kanji;
        ctx->glr[0] = 0;
        if (enc == VtCharsetEnc_JIS && !ctx->cfg.jis7_katakana) {
            ctx->glr[1] = 2;  // 8-bit katakana in GR
        } else {
            ctx->glr[1] = 3;
        }
    } else {
        ctx->gn[0] = VtCharsetCS_ASCII;
        ctx->gn[1] = VtCharsetCS_Special;
        ctx->gn[2] = VtCharsetCS_ASCII;
        ctx->gn[3] = VtCharsetCS_ASCII;
        ctx->glr[0] = 0;
        ctx->glr[1] = 0;
    }
}

// ---------------------------------------------------------------------------
// iconv lifecycle
// ---------------------------------------------------------------------------

static void IconvClose(VtCharsetCtx *ctx) {
    if (ctx->iconv_cd != (iconv_t)-1) {
        iconv_close(ctx->iconv_cd);
        ctx->iconv_cd = (iconv_t)-1;
    }
}

static void IconvOpen(VtCharsetCtx *ctx) {
    IconvClose(ctx);
    const char *cs = IconvNameFor(ctx->cfg.encoding);
    if (cs) {
        ctx->iconv_cd = iconv_open("UTF-8", cs);
        // If iconv_open fails, iconv_cd == (iconv_t)-1; MbcsToU32 returns 0
    }
}

// ---------------------------------------------------------------------------
// Public: VtCharsetInit
// ---------------------------------------------------------------------------

VtCharsetCtx *VtCharsetInit(const VtCharsetOps *ops, void *client_data,
                             const VtCharsetConfig *cfg) {
    VtCharsetCtx *ctx = (VtCharsetCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->ops         = *ops;
    ctx->client_data = client_data;
    ctx->cfg         = *cfg;
    ctx->iconv_cd    = (iconv_t)-1;

    Iso2022Init(ctx);
    ctx->gl_tmp       = 0;
    ctx->ss_flag      = false;
    ctx->debug_flag   = DEBUG_NONE;
    ctx->kanji_in     = false;
    ctx->euc_kana_in  = false;
    ctx->euc_sup_in   = false;
    ctx->conv_jis     = false;
    ctx->fallbacked   = false;
    ctx->utf8_count   = 0;

    IconvOpen(ctx);
    return ctx;
}

// ---------------------------------------------------------------------------
// Public: VtCharsetFinish
// ---------------------------------------------------------------------------

void VtCharsetFinish(VtCharsetCtx *ctx) {
    if (!ctx) return;
    IconvClose(ctx);
    free(ctx);
}

// ---------------------------------------------------------------------------
// Public: VtCharsetSetConfig
// ---------------------------------------------------------------------------

void VtCharsetSetConfig(VtCharsetCtx *ctx, const VtCharsetConfig *cfg) {
    assert(ctx);
    bool enc_changed = (cfg->encoding != ctx->cfg.encoding);
    ctx->cfg = *cfg;
    if (enc_changed) {
        Iso2022Init(ctx);
        // Reset MBCS state on encoding change
        ctx->kanji_in    = false;
        ctx->euc_kana_in = false;
        ctx->euc_sup_in  = false;
        ctx->conv_jis    = false;
        ctx->utf8_count  = 0;
        ctx->fallbacked  = false;
        IconvOpen(ctx);
    }
}

// ---------------------------------------------------------------------------
// Helper: emit callbacks
// ---------------------------------------------------------------------------

static void PutChar(VtCharsetCtx *ctx, uint32_t u32) {
    ctx->ops.put_char(u32, ctx->client_data);
}
static void PutCtrl(VtCharsetCtx *ctx, uint8_t b) {
    ctx->ops.put_ctrl(b, ctx->client_data);
}

static inline bool IsC0(uint8_t b) { return b <= US; }
static inline bool IsC1(uint32_t c) { return c >= 0x80U && c <= 0x9FU; }

// ---------------------------------------------------------------------------
// Helper: CP932 single-byte via iconv (or direct for < 0x80)
// Used for half-width katakana and other single-byte SJIS values.
// ---------------------------------------------------------------------------

static uint32_t Cp932SingleToU32(VtCharsetCtx *ctx, uint8_t b) {
    if (b < 0x80U) return (uint32_t)b;
    return MbcsToU32(ctx->iconv_cd, b, 0, false);
}

static uint32_t Cp932DoubleToU32(VtCharsetCtx *ctx, uint8_t b1, uint8_t b2) {
    return MbcsToU32(ctx->iconv_cd, b1, b2, true);
}

// ---------------------------------------------------------------------------
// Helper: SJIS lead-byte detection (ismbbleadSJIS from TeraTerm)
// ---------------------------------------------------------------------------

static bool SjisIsLeadByte(uint8_t b) {
    return (b > 0x80U && b < 0xA0U) || (b > 0xDFU && b < 0xFDU);
}

// ---------------------------------------------------------------------------
// Helper: MBCS lead-byte detection for KR/CN encodings
// Replaces __ismbblead(b, code_page) from ttcstd.h
// ---------------------------------------------------------------------------

static bool MbcsIsLeadByte(uint8_t b, VtCharsetEncoding enc) {
    switch (enc) {
    case VtCharsetEnc_CP949:   // Korean CP949: lead 0x81-0xFE
        return b >= 0x81U && b <= 0xFEU;
    case VtCharsetEnc_GB2312:  // Simplified Chinese CP936: lead 0x81-0xFE
        return b >= 0x81U && b <= 0xFEU;
    case VtCharsetEnc_Big5:    // Traditional Chinese CP950: lead 0x81-0xFE
        return b >= 0x81U && b <= 0xFEU;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Helper: replacement character output for invalid sequences
// Ported from PutReplacementChr in charset.cpp
// ---------------------------------------------------------------------------

static void PutReplacement(VtCharsetCtx *ctx, const uint8_t *ptr, int len,
                            bool fallback) {
    for (int i = 0; i < len; i++) {
        uint8_t c = ptr[i];
        assert(!IsC0(c));
        if (fallback) {
            PutChar(ctx, (uint32_t)c);
        } else {
            if (c < 0x80U) {
                PutChar(ctx, (uint32_t)c);
            } else {
                PutChar(ctx, REPLACEMENT_CHAR);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// CheckKanji: decide if byte b starts a MBCS kanji sequence (JP modes).
// Returns true when kanji_in should be set.
// Sets ctx->conv_jis to true when JIS→SJIS conversion is needed.
// Ported from CheckKanji() in charset.cpp.
// ---------------------------------------------------------------------------

static bool CheckKanji(VtCharsetCtx *ctx, uint8_t b) {
    VtCharsetEncoding enc = ctx->cfg.encoding;
    bool is_japanese = (enc == VtCharsetEnc_SJIS ||
                        enc == VtCharsetEnc_EUC  ||
                        enc == VtCharsetEnc_JIS);

    if (!is_japanese && enc != VtCharsetEnc_UTF8) return false;

    ctx->conv_jis = false;

    if (enc == VtCharsetEnc_SJIS ||
        (ctx->cfg.fallback_to_cp932 && enc == VtCharsetEnc_UTF8)) {
        if (SjisIsLeadByte(b)) {
            ctx->fallbacked = true;
            return true;  // SJIS double-byte lead
        }
        if (b >= 0xA1U && b <= 0xDFU) return false;  // half-width katakana
    }

    if (b >= 0x21U && b <= 0x7EU) {
        bool check = (ctx->gn[ctx->glr[0]] == VtCharsetCS_Kanji);
        ctx->conv_jis = check;
        return check;
    }
    if (b >= 0xA1U && b <= 0xFEU) {
        bool check;
        if (enc == VtCharsetEnc_EUC) {
            check = true;
        } else if (enc == VtCharsetEnc_JIS && ctx->cfg.fixed_jis && !ctx->cfg.jis7_katakana) {
            check = false;  // 8-bit katakana
        } else {
            check = (ctx->gn[ctx->glr[1]] == VtCharsetCS_Kanji);
        }
        ctx->conv_jis = check;
        return check;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ParseFirstJP: Japanese byte dispatch
// Ported from ParseFirstJP() in charset.cpp
// ---------------------------------------------------------------------------

static void ParseFirstJP(VtCharsetCtx *ctx, uint8_t b) {
    VtCharsetEncoding enc = ctx->cfg.encoding;

    if (ctx->kanji_in) {
        bool valid_trail;
        if (ctx->conv_jis) {
            valid_trail = ((b > 0x20U && b < 0x7FU) || (b > 0xA0U && b < 0xFFU));
        } else {
            valid_trail = (b > 0x3FU && b < 0xFDU);
        }

        if (valid_trail) {
            uint8_t b1 = (uint8_t)(ctx->kanji >> 8);
            uint8_t b2 = b;
            uint32_t u32;

            if (ctx->conv_jis) {
                // JIS → SJIS → UTF-32
                uint16_t jis = (uint16_t)(((uint16_t)(ctx->kanji & 0x7F00U) >> 0U) |
                                           (uint16_t)(b & 0x7FU));
                jis = (uint16_t)((b1 & 0x7FU) << 8U | (b2 & 0x7FU));
                uint16_t sjis = JisToSjis(jis);
                u32 = Cp932DoubleToU32(ctx, (uint8_t)(sjis >> 8), (uint8_t)(sjis & 0xFF));
            } else {
                u32 = Cp932DoubleToU32(ctx, b1, b2);
            }
            PutChar(ctx, u32 ? u32 : REPLACEMENT_CHAR);
            ctx->kanji_in = false;
            return;
        }
        if (!ctx->cfg.ctrl_in_kanji) {
            ctx->kanji_in = false;
        }
    }

    if (ctx->ss_flag) {
        if (ctx->gn[ctx->gl_tmp] == VtCharsetCS_Kanji) {
            ctx->kanji    = (uint16_t)((uint16_t)b << 8);
            ctx->kanji_in = true;
            ctx->ss_flag  = false;
            return;
        }
        if (ctx->gn[ctx->gl_tmp] == VtCharsetCS_Katakana) {
            b = (uint8_t)(b | 0x80U);
        }
        PutChar(ctx, Cp932SingleToU32(ctx, b));
        ctx->ss_flag = false;
        return;
    }

    if (!ctx->euc_sup_in && !ctx->euc_kana_in && !ctx->kanji_in &&
        CheckKanji(ctx, b)) {
        ctx->kanji    = (uint16_t)((uint16_t)b << 8);
        ctx->kanji_in = true;
        return;
    }

    if (b <= US) {
        PutCtrl(ctx, b);
    } else if (b == 0x20U) {
        PutChar(ctx, 0x20U);
    } else if (b >= 0x21U && b <= 0x7EU) {
        if (ctx->euc_sup_in) {
            if (--ctx->euc_count == 0) ctx->euc_sup_in = false;
            return;
        }
        if (ctx->gn[ctx->glr[0]] == VtCharsetCS_Katakana || ctx->euc_kana_in) {
            b = (uint8_t)(b | 0x80U);
            ctx->euc_kana_in = false;
            PutChar(ctx, Cp932SingleToU32(ctx, b));
            return;
        }
        PutChar(ctx, (uint32_t)b);
    } else if (b == DEL) {
        /* skip */
    } else if (b >= 0x80U && b <= 0x8DU) {
        PutCtrl(ctx, b);
    } else if (b == 0x8EU) {  // SS2
        switch (enc) {
        case VtCharsetEnc_EUC:
            if (ctx->cfg.iso2022_ss2) ctx->euc_kana_in = true;
            break;
        case VtCharsetEnc_UTF8:
            PutChar(ctx, REPLACEMENT_CHAR);
            break;
        default:
            PutCtrl(ctx, b);
        }
    } else if (b == 0x8FU) {  // SS3
        switch (enc) {
        case VtCharsetEnc_EUC:
            if (ctx->cfg.iso2022_ss3) {
                ctx->euc_count  = 2;
                ctx->euc_sup_in = true;
            }
            break;
        case VtCharsetEnc_UTF8:
            PutChar(ctx, REPLACEMENT_CHAR);
            break;
        default:
            PutCtrl(ctx, b);
        }
    } else if (b >= 0x90U && b <= 0x9FU) {
        PutCtrl(ctx, b);
    } else if (b == 0xA0U) {
        PutChar(ctx, 0x20U);
    } else if (b >= 0xA1U && b <= 0xFEU) {
        if (ctx->euc_sup_in) {
            if (--ctx->euc_count == 0) ctx->euc_sup_in = false;
            return;
        }
        if (ctx->gn[ctx->glr[1]] != VtCharsetCS_ASCII ||
            (enc == VtCharsetEnc_EUC && ctx->euc_kana_in) ||
            enc == VtCharsetEnc_SJIS ||
            (enc == VtCharsetEnc_JIS && !ctx->cfg.jis7_katakana && ctx->cfg.fixed_jis)) {
            PutChar(ctx, Cp932SingleToU32(ctx, b));
        } else {
            if (ctx->gn[ctx->glr[1]] == VtCharsetCS_ASCII) {
                b = (uint8_t)(b & 0x7FU);
            }
            PutChar(ctx, (uint32_t)b);
        }
        ctx->euc_kana_in = false;
    } else {
        PutChar(ctx, (uint32_t)b);
    }
}

// ---------------------------------------------------------------------------
// ParseFirstKR: Korean CP949
// ---------------------------------------------------------------------------

static void ParseFirstKR(VtCharsetCtx *ctx, uint8_t b) {
    if (ctx->kanji_in) {
        bool valid_trail = ((b >= 0x41U && b <= 0x5AU) ||
                            (b >= 0x61U && b <= 0x7AU) ||
                            (b >= 0x81U && b <= 0xFEU));
        if (valid_trail) {
            uint8_t b1 = (uint8_t)(ctx->kanji >> 8);
            uint32_t u32 = MbcsToU32(ctx->iconv_cd, b1, b, true);
            PutChar(ctx, u32 ? u32 : REPLACEMENT_CHAR);
            ctx->kanji_in = false;
            return;
        }
        if (!ctx->cfg.ctrl_in_kanji) ctx->kanji_in = false;
    }

    if (!ctx->kanji_in && MbcsIsLeadByte(b, ctx->cfg.encoding)) {
        ctx->kanji    = (uint16_t)((uint16_t)b << 8);
        ctx->kanji_in = true;
        return;
    }

    if (b <= US)                        PutCtrl(ctx, b);
    else if (b == 0x20U)                PutChar(ctx, 0x20U);
    else if (b >= 0x21U && b <= 0x7EU)  PutChar(ctx, (uint32_t)b);
    else if (b == DEL)                  { /* skip */ }
    else if (b >= 0x80U && b <= 0x9FU)  PutCtrl(ctx, b);
    else if (b == 0xA0U)                PutChar(ctx, 0x20U);
    else if (b >= 0xA1U && b <= 0xFEU) {
        if (ctx->gn[ctx->glr[1]] == VtCharsetCS_ASCII) b = (uint8_t)(b & 0x7FU);
        PutChar(ctx, (uint32_t)b);
    } else {
        PutChar(ctx, (uint32_t)b);
    }
}

// ---------------------------------------------------------------------------
// ParseFirstCn: Chinese GB2312 / Big5
// ---------------------------------------------------------------------------

static void ParseFirstCn(VtCharsetCtx *ctx, uint8_t b) {
    if (ctx->kanji_in) {
        bool valid_trail = ((b >= 0x40U && b <= 0x7EU) ||
                            (b >= 0xA1U && b <= 0xFEU));
        if (valid_trail) {
            uint8_t b1 = (uint8_t)(ctx->kanji >> 8);
            uint32_t u32 = MbcsToU32(ctx->iconv_cd, b1, b, true);
            PutChar(ctx, u32 ? u32 : REPLACEMENT_CHAR);
            ctx->kanji_in = false;
            return;
        }
        if (!ctx->cfg.ctrl_in_kanji) ctx->kanji_in = false;
    }

    if (!ctx->kanji_in && MbcsIsLeadByte(b, ctx->cfg.encoding)) {
        ctx->kanji    = (uint16_t)((uint16_t)b << 8);
        ctx->kanji_in = true;
        return;
    }

    if (b <= US)                        PutCtrl(ctx, b);
    else if (b == 0x20U)                PutChar(ctx, 0x20U);
    else if (b >= 0x21U && b <= 0x7EU)  PutChar(ctx, (uint32_t)b);
    else if (b == DEL)                  { /* skip */ }
    else if (b >= 0x80U && b <= 0x9FU)  PutCtrl(ctx, b);
    else if (b == 0xA0U)                PutChar(ctx, 0x20U);
    else if (b >= 0xA1U && b <= 0xFEU) {
        if (ctx->gn[ctx->glr[1]] == VtCharsetCS_ASCII) b = (uint8_t)(b & 0x7FU);
        PutChar(ctx, (uint32_t)b);
    } else {
        PutChar(ctx, (uint32_t)b);
    }
}

// ---------------------------------------------------------------------------
// ParseASCII: plain 7-bit ASCII fallback
// ---------------------------------------------------------------------------

static void ParseASCII(VtCharsetCtx *ctx, uint8_t b) {
    if (ctx->ss_flag) {
        PutChar(ctx, (uint32_t)b);
        ctx->ss_flag = false;
        return;
    }
    if (b <= US)                        PutCtrl(ctx, b);
    else if (b >= 0x20U && b <= 0x7EU)  PutChar(ctx, (uint32_t)b);
    else if (b == 0x8EU || b == 0x8FU)  PutChar(ctx, REPLACEMENT_CHAR);
    else if (b >= 0x80U && b <= 0x9FU)  PutCtrl(ctx, b);
    else if (b >= 0xA0U)                PutChar(ctx, (uint32_t)b);
}

// ---------------------------------------------------------------------------
// ParseFirstUTF8: UTF-8 byte stream decoder
// Ported from ParseFirstUTF8() in charset.cpp
// ---------------------------------------------------------------------------

static void ParseFirstUTF8(VtCharsetCtx *ctx, uint8_t b) {
    uint32_t code;
    bool fallback = ctx->cfg.fallback_to_cp932;

    if (ctx->fallbacked) {
        ParseFirstJP(ctx, b);
        ctx->fallbacked = false;
        return;
    }

recheck:
    if (ctx->utf8_count == 0) {
        if (IsC0(b)) {
            PutCtrl(ctx, b);
            return;
        }
        if (b <= 0x7FU) {
            PutChar(ctx, (uint32_t)b);
            return;
        }
        if (b >= 0xC2U && b <= 0xF4U) {
            ctx->utf8_buf[ctx->utf8_count++] = b;
            return;
        }
        // Invalid start byte (0x80-0xC1, 0xF5-0xFF)
        if (fallback && SjisIsLeadByte(b)) {
            ctx->fallbacked = true;
            ctx->conv_jis   = false;
            ctx->kanji      = (uint16_t)((uint16_t)b << 8);
            ctx->kanji_in   = true;
            return;
        }
        ctx->utf8_buf[0] = b;
        PutReplacement(ctx, ctx->utf8_buf, 1, false);
        return;
    }

    // Continuation byte check
    if ((b & 0xC0U) != 0x80U) {
        PutReplacement(ctx, ctx->utf8_buf, ctx->utf8_count, fallback);
        ctx->utf8_count = 0;
        goto recheck;
    }

    ctx->utf8_buf[ctx->utf8_count++] = b;

    // 2-byte sequence
    if (ctx->utf8_count == 2) {
        if ((ctx->utf8_buf[0] & 0xE0U) == 0xC0U) {
            code = ((uint32_t)(ctx->utf8_buf[0] & 0x1FU) << 6U)
                 |  (uint32_t)(b & 0x3FU);
            ctx->utf8_count = 0;
            if (IsC1(code)) {
                PutCtrl(ctx, (uint8_t)code);
            } else {
                PutChar(ctx, code);
            }
        }
        return;
    }

    // 3-byte sequence
    if (ctx->utf8_count == 3) {
        if ((ctx->utf8_buf[0] & 0xF0U) == 0xE0U) {
            // Validate 2nd byte range for E0 and ED
            if ((ctx->utf8_buf[0] == 0xE0U && (ctx->utf8_buf[1] < 0xA0U || ctx->utf8_buf[1] > 0xBFU)) ||
                (ctx->utf8_buf[0] == 0xEDU && ctx->utf8_buf[1] > 0x9FU)) {
                PutReplacement(ctx, ctx->utf8_buf, 2, fallback);
                ctx->utf8_count = 0;
                goto recheck;
            }
            code = ((uint32_t)(ctx->utf8_buf[0] & 0x0FU) << 12U)
                 | ((uint32_t)(ctx->utf8_buf[1] & 0x3FU) << 6U)
                 |  (uint32_t)(ctx->utf8_buf[2] & 0x3FU);
            ctx->utf8_count = 0;
            PutChar(ctx, code);
        }
        return;
    }

    // 4-byte sequence
    assert(ctx->utf8_count == 4);
    if ((ctx->utf8_buf[0] & 0xF8U) == 0xF0U) {
        if ((ctx->utf8_buf[0] == 0xF0U && (ctx->utf8_buf[1] < 0x90U || ctx->utf8_buf[1] > 0xBFU)) ||
            (ctx->utf8_buf[0] == 0xF4U && (ctx->utf8_buf[1] < 0x80U || ctx->utf8_buf[1] > 0x8FU))) {
            PutReplacement(ctx, ctx->utf8_buf, 3, fallback);
            ctx->utf8_count = 0;
            goto recheck;
        }
        code = ((uint32_t)(ctx->utf8_buf[0] & 0x07U) << 18U)
             | ((uint32_t)(ctx->utf8_buf[1] & 0x3FU) << 12U)
             | ((uint32_t)(ctx->utf8_buf[2] & 0x3FU) << 6U)
             |  (uint32_t)(ctx->utf8_buf[3] & 0x3FU);
        ctx->utf8_count = 0;
        PutChar(ctx, code);
    }
}

// ---------------------------------------------------------------------------
// ParseSBCS: Single-byte character set dispatch
// Ported from ParseEnglish() / ParseCodePage() in charset.cpp
// ---------------------------------------------------------------------------

static void ParseSBCS(VtCharsetCtx *ctx, uint8_t b) {
    uint32_t u32 = SbcsToU32(ctx->cfg.encoding, b);
    if (u32 < 0x100U) {
        ParseASCII(ctx, (uint8_t)u32);
    } else {
        PutChar(ctx, u32);
    }
}

// ---------------------------------------------------------------------------
// Debug mode output
// Simplified: no UI dependencies; hex/normal output via put_char only.
// ---------------------------------------------------------------------------

static void PutDebugChar(VtCharsetCtx *ctx, uint8_t b) {
    if (ctx->debug_flag == DEBUG_HEXD) {
        char buf[3];
        int n = snprintf(buf, sizeof(buf), "%02X", (unsigned)b);
        for (int i = 0; i < n; i++) PutChar(ctx, (uint32_t)(uint8_t)buf[i]);
        PutChar(ctx, ' ');
    } else if (ctx->debug_flag == DEBUG_NORM) {
        if (b <= US) {
            PutChar(ctx, '^');
            PutChar(ctx, (uint32_t)((uint8_t)(b + 0x40U)));
        } else if (b == DEL) {
            PutChar(ctx, '<'); PutChar(ctx, 'D');
            PutChar(ctx, 'E'); PutChar(ctx, 'L'); PutChar(ctx, '>');
        } else {
            PutChar(ctx, (uint32_t)b);
        }
    }
    // DEBUG_NOUT: no output
}

// ---------------------------------------------------------------------------
// Public: VtCharsetFeedByte  (replaces ParseFirst)
// ---------------------------------------------------------------------------

void VtCharsetFeedByte(VtCharsetCtx *ctx, uint8_t b) {
    assert(ctx);
    VtCharsetEncoding enc = ctx->cfg.encoding;

    if (ctx->debug_flag != DEBUG_NONE) {
        PutDebugChar(ctx, b);
        return;
    }

    switch (enc) {
    case VtCharsetEnc_SJIS:
    case VtCharsetEnc_EUC:
    case VtCharsetEnc_JIS:
        ParseFirstJP(ctx, b);
        return;

    case VtCharsetEnc_UTF8:
        ParseFirstUTF8(ctx, b);
        return;

    case VtCharsetEnc_CP949:
        ParseFirstKR(ctx, b);
        return;

    case VtCharsetEnc_GB2312:
    case VtCharsetEnc_Big5:
        ParseFirstCn(ctx, b);
        return;

    case VtCharsetEnc_ISO8859_1:
    case VtCharsetEnc_ISO8859_2:
    case VtCharsetEnc_ISO8859_3:
    case VtCharsetEnc_ISO8859_4:
    case VtCharsetEnc_ISO8859_5:
    case VtCharsetEnc_ISO8859_6:
    case VtCharsetEnc_ISO8859_7:
    case VtCharsetEnc_ISO8859_8:
    case VtCharsetEnc_ISO8859_9:
    case VtCharsetEnc_ISO8859_10:
    case VtCharsetEnc_ISO8859_11:
    case VtCharsetEnc_ISO8859_13:
    case VtCharsetEnc_ISO8859_14:
    case VtCharsetEnc_ISO8859_15:
    case VtCharsetEnc_ISO8859_16:
    case VtCharsetEnc_CP437:
    case VtCharsetEnc_CP737:
    case VtCharsetEnc_CP775:
    case VtCharsetEnc_CP850:
    case VtCharsetEnc_CP852:
    case VtCharsetEnc_CP855:
    case VtCharsetEnc_CP857:
    case VtCharsetEnc_CP860:
    case VtCharsetEnc_CP861:
    case VtCharsetEnc_CP862:
    case VtCharsetEnc_CP863:
    case VtCharsetEnc_CP864:
    case VtCharsetEnc_CP865:
    case VtCharsetEnc_CP866:
    case VtCharsetEnc_CP869:
    case VtCharsetEnc_CP874:
    case VtCharsetEnc_CP1250:
    case VtCharsetEnc_CP1251:
    case VtCharsetEnc_CP1252:
    case VtCharsetEnc_CP1253:
    case VtCharsetEnc_CP1254:
    case VtCharsetEnc_CP1255:
    case VtCharsetEnc_CP1256:
    case VtCharsetEnc_CP1257:
    case VtCharsetEnc_CP1258:
    case VtCharsetEnc_KOI8:
        ParseSBCS(ctx, b);
        return;

    case VtCharsetEnc_Debug:
        PutDebugChar(ctx, b);
        return;

    default:
        break;
    }

    // Fallback: 7-bit ASCII
    if (ctx->ss_flag) {
        PutChar(ctx, (uint32_t)b);
        ctx->ss_flag = false;
        return;
    }
    if (b <= US)                        PutCtrl(ctx, b);
    else if (b >= 0x20U && b <= 0x7EU)  PutChar(ctx, (uint32_t)b);
    else if (b >= 0x80U && b <= 0x9FU)  PutCtrl(ctx, b);
    else if (b >= 0xA0U)                PutChar(ctx, (uint32_t)b);
}

// ---------------------------------------------------------------------------
// Public: VtCharset2022Designate
// ---------------------------------------------------------------------------

void VtCharset2022Designate(VtCharsetCtx *ctx, int gn, VtCharsetCS cs) {
    assert(ctx);
    assert(gn >= 0 && gn <= 3);
    ctx->gn[gn] = cs;
}

// ---------------------------------------------------------------------------
// Public: VtCharset2022Invoke
// ---------------------------------------------------------------------------

void VtCharset2022Invoke(VtCharsetCtx *ctx, VtCharset2022Shift shift) {
    assert(ctx);
    switch (shift) {
    case VtCharsetShift_LS0:  ctx->glr[0] = 0; break;
    case VtCharsetShift_LS1:  ctx->glr[0] = 1; break;
    case VtCharsetShift_LS2:  ctx->glr[0] = 2; break;
    case VtCharsetShift_LS3:  ctx->glr[0] = 3; break;
    case VtCharsetShift_LS1R: ctx->glr[1] = 1; break;
    case VtCharsetShift_LS2R: ctx->glr[1] = 2; break;
    case VtCharsetShift_LS3R: ctx->glr[1] = 3; break;
    case VtCharsetShift_SS2:  ctx->gl_tmp = 2; ctx->ss_flag = true; break;
    case VtCharsetShift_SS3:  ctx->gl_tmp = 3; ctx->ss_flag = true; break;
    default: assert(0); break;
    }
}

// ---------------------------------------------------------------------------
// Public: VtCharsetIsSpecial
// ---------------------------------------------------------------------------

bool VtCharsetIsSpecial(VtCharsetCtx *ctx, uint8_t b) {
    assert(ctx);
    VtCharsetCS cs;

    if (b >= 0x5FU && b < 0x7FU) {
        cs = ctx->ss_flag ? ctx->gn[ctx->gl_tmp] : ctx->gn[ctx->glr[0]];
    } else if (b >= 0xDFU && b < 0xFFU) {
        cs = ctx->ss_flag ? ctx->gn[ctx->gl_tmp] : ctx->gn[ctx->glr[1]];
    } else {
        return false;
    }
    return (cs == VtCharsetCS_Special);
}

// ---------------------------------------------------------------------------
// Public: VtCharsetSaveState / VtCharsetLoadState
// ---------------------------------------------------------------------------

void VtCharsetSaveState(VtCharsetCtx *ctx, VtCharsetState *state) {
    assert(ctx && state);
    state->v[0] = ctx->glr[0];
    state->v[1] = ctx->glr[1];
    for (int i = 0; i < 4; i++) state->v[2 + i] = (int)ctx->gn[i];
}

void VtCharsetLoadState(VtCharsetCtx *ctx, const VtCharsetState *state) {
    assert(ctx && state);
    ctx->glr[0] = state->v[0];
    ctx->glr[1] = state->v[1];
    for (int i = 0; i < 4; i++) ctx->gn[i] = (VtCharsetCS)state->v[2 + i];
}

// ---------------------------------------------------------------------------
// Public: VtCharsetFallbackFinish
// ---------------------------------------------------------------------------

void VtCharsetFallbackFinish(VtCharsetCtx *ctx) {
    assert(ctx);
    ctx->fallbacked = false;
}

// ---------------------------------------------------------------------------
// Public: VtCharsetSetDebugMode / VtCharsetGetDebugMode
// ---------------------------------------------------------------------------

void VtCharsetSetDebugMode(VtCharsetCtx *ctx, uint8_t mode) {
    assert(ctx);
    ctx->debug_flag = mode % DEBUG_MAX;
}

uint8_t VtCharsetGetDebugMode(const VtCharsetCtx *ctx) {
    assert(ctx);
    return ctx->debug_flag;
}
