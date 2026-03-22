// 2026-03-10 00:00 v0.1.0
// vtcolor.c — Color string parsing and 256-color palette
// Ported from TeraTerm 5 vtterm.c (XsParseColor) + defaultcolortable.c
// Win32-free (no COLORREF), C99, pure functions — zero global state.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "vtcolor.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   // strncasecmp

// ---------------------------------------------------------------------------
// 256-color default palette — verbatim from TeraTerm defaultcolortable.c
// Indices 0-7:   Standard ANSI colors
// Indices 8-15:  High-intensity ANSI colors
// Indices 16-231: 6×6×6 RGB cube
// Indices 232-255: Grayscale ramp (8 to 238 in steps of 10)
// ---------------------------------------------------------------------------

static const VtRgb24 kDefaultPalette[256] = {
    // 0 - 7: Standard ANSI
    VT_RGB(  0,  0,  0), VT_RGB(255,  0,  0), VT_RGB(  0,255,  0), VT_RGB(255,255,  0),
    VT_RGB(  0,  0,255), VT_RGB(255,  0,255), VT_RGB(  0,255,255), VT_RGB(255,255,255),
    // 8 - 15: High-intensity ANSI
    VT_RGB(128,128,128), VT_RGB(128,  0,  0), VT_RGB(  0,128,  0), VT_RGB(128,128,  0),
    VT_RGB(  0,  0,128), VT_RGB(128,  0,128), VT_RGB(  0,128,128), VT_RGB(192,192,192),
    // 16 - 23
    VT_RGB(  0,  0,  0), VT_RGB(  0,  0, 95), VT_RGB(  0,  0,135), VT_RGB(  0,  0,175),
    VT_RGB(  0,  0,215), VT_RGB(  0,  0,255), VT_RGB(  0, 95,  0), VT_RGB(  0, 95, 95),
    // 24 - 31
    VT_RGB(  0, 95,135), VT_RGB(  0, 95,175), VT_RGB(  0, 95,215), VT_RGB(  0, 95,255),
    VT_RGB(  0,135,  0), VT_RGB(  0,135, 95), VT_RGB(  0,135,135), VT_RGB(  0,135,175),
    // 32 - 39
    VT_RGB(  0,135,215), VT_RGB(  0,135,255), VT_RGB(  0,175,  0), VT_RGB(  0,175, 95),
    VT_RGB(  0,175,135), VT_RGB(  0,175,175), VT_RGB(  0,175,215), VT_RGB(  0,175,255),
    // 40 - 47
    VT_RGB(  0,215,  0), VT_RGB(  0,215, 95), VT_RGB(  0,215,135), VT_RGB(  0,215,175),
    VT_RGB(  0,215,215), VT_RGB(  0,215,255), VT_RGB(  0,255,  0), VT_RGB(  0,255, 95),
    // 48 - 55
    VT_RGB(  0,255,135), VT_RGB(  0,255,175), VT_RGB(  0,255,215), VT_RGB(  0,255,255),
    VT_RGB( 95,  0,  0), VT_RGB( 95,  0, 95), VT_RGB( 95,  0,135), VT_RGB( 95,  0,175),
    // 56 - 63
    VT_RGB( 95,  0,215), VT_RGB( 95,  0,255), VT_RGB( 95, 95,  0), VT_RGB( 95, 95, 95),
    VT_RGB( 95, 95,135), VT_RGB( 95, 95,175), VT_RGB( 95, 95,215), VT_RGB( 95, 95,255),
    // 64 - 71
    VT_RGB( 95,135,  0), VT_RGB( 95,135, 95), VT_RGB( 95,135,135), VT_RGB( 95,135,175),
    VT_RGB( 95,135,215), VT_RGB( 95,135,255), VT_RGB( 95,175,  0), VT_RGB( 95,175, 95),
    // 72 - 79
    VT_RGB( 95,175,135), VT_RGB( 95,175,175), VT_RGB( 95,175,215), VT_RGB( 95,175,255),
    VT_RGB( 95,215,  0), VT_RGB( 95,215, 95), VT_RGB( 95,215,135), VT_RGB( 95,215,175),
    // 80 - 87
    VT_RGB( 95,215,215), VT_RGB( 95,215,255), VT_RGB( 95,255,  0), VT_RGB( 95,255, 95),
    VT_RGB( 95,255,135), VT_RGB( 95,255,175), VT_RGB( 95,255,215), VT_RGB( 95,255,255),
    // 88 - 95
    VT_RGB(135,  0,  0), VT_RGB(135,  0, 95), VT_RGB(135,  0,135), VT_RGB(135,  0,175),
    VT_RGB(135,  0,215), VT_RGB(135,  0,255), VT_RGB(135, 95,  0), VT_RGB(135, 95, 95),
    // 96 - 103
    VT_RGB(135, 95,135), VT_RGB(135, 95,175), VT_RGB(135, 95,215), VT_RGB(135, 95,255),
    VT_RGB(135,135,  0), VT_RGB(135,135, 95), VT_RGB(135,135,135), VT_RGB(135,135,175),
    // 104 - 111
    VT_RGB(135,135,215), VT_RGB(135,135,255), VT_RGB(135,175,  0), VT_RGB(135,175, 95),
    VT_RGB(135,175,135), VT_RGB(135,175,175), VT_RGB(135,175,215), VT_RGB(135,175,255),
    // 112 - 119
    VT_RGB(135,215,  0), VT_RGB(135,215, 95), VT_RGB(135,215,135), VT_RGB(135,215,175),
    VT_RGB(135,215,215), VT_RGB(135,215,255), VT_RGB(135,255,  0), VT_RGB(135,255, 95),
    // 120 - 127
    VT_RGB(135,255,135), VT_RGB(135,255,175), VT_RGB(135,255,215), VT_RGB(135,255,255),
    VT_RGB(175,  0,  0), VT_RGB(175,  0, 95), VT_RGB(175,  0,135), VT_RGB(175,  0,175),
    // 128 - 135
    VT_RGB(175,  0,215), VT_RGB(175,  0,255), VT_RGB(175, 95,  0), VT_RGB(175, 95, 95),
    VT_RGB(175, 95,135), VT_RGB(175, 95,175), VT_RGB(175, 95,215), VT_RGB(175, 95,255),
    // 136 - 143
    VT_RGB(175,135,  0), VT_RGB(175,135, 95), VT_RGB(175,135,135), VT_RGB(175,135,175),
    VT_RGB(175,135,215), VT_RGB(175,135,255), VT_RGB(175,175,  0), VT_RGB(175,175, 95),
    // 144 - 151
    VT_RGB(175,175,135), VT_RGB(175,175,175), VT_RGB(175,175,215), VT_RGB(175,175,255),
    VT_RGB(175,215,  0), VT_RGB(175,215, 95), VT_RGB(175,215,135), VT_RGB(175,215,175),
    // 152 - 159
    VT_RGB(175,215,215), VT_RGB(175,215,255), VT_RGB(175,255,  0), VT_RGB(175,255, 95),
    VT_RGB(175,255,135), VT_RGB(175,255,175), VT_RGB(175,255,215), VT_RGB(175,255,255),
    // 160 - 167
    VT_RGB(215,  0,  0), VT_RGB(215,  0, 95), VT_RGB(215,  0,135), VT_RGB(215,  0,175),
    VT_RGB(215,  0,215), VT_RGB(215,  0,255), VT_RGB(215, 95,  0), VT_RGB(215, 95, 95),
    // 168 - 175
    VT_RGB(215, 95,135), VT_RGB(215, 95,175), VT_RGB(215, 95,215), VT_RGB(215, 95,255),
    VT_RGB(215,135,  0), VT_RGB(215,135, 95), VT_RGB(215,135,135), VT_RGB(215,135,175),
    // 176 - 183
    VT_RGB(215,135,215), VT_RGB(215,135,255), VT_RGB(215,175,  0), VT_RGB(215,175, 95),
    VT_RGB(215,175,135), VT_RGB(215,175,175), VT_RGB(215,175,215), VT_RGB(215,175,255),
    // 184 - 191
    VT_RGB(215,215,  0), VT_RGB(215,215, 95), VT_RGB(215,215,135), VT_RGB(215,215,175),
    VT_RGB(215,215,215), VT_RGB(215,215,255), VT_RGB(215,255,  0), VT_RGB(215,255, 95),
    // 192 - 199
    VT_RGB(215,255,135), VT_RGB(215,255,175), VT_RGB(215,255,215), VT_RGB(215,255,255),
    VT_RGB(255,  0,  0), VT_RGB(255,  0, 95), VT_RGB(255,  0,135), VT_RGB(255,  0,175),
    // 200 - 207
    VT_RGB(255,  0,215), VT_RGB(255,  0,255), VT_RGB(255, 95,  0), VT_RGB(255, 95, 95),
    VT_RGB(255, 95,135), VT_RGB(255, 95,175), VT_RGB(255, 95,215), VT_RGB(255, 95,255),
    // 208 - 215
    VT_RGB(255,135,  0), VT_RGB(255,135, 95), VT_RGB(255,135,135), VT_RGB(255,135,175),
    VT_RGB(255,135,215), VT_RGB(255,135,255), VT_RGB(255,175,  0), VT_RGB(255,175, 95),
    // 216 - 223
    VT_RGB(255,175,135), VT_RGB(255,175,175), VT_RGB(255,175,215), VT_RGB(255,175,255),
    VT_RGB(255,215,  0), VT_RGB(255,215, 95), VT_RGB(255,215,135), VT_RGB(255,215,175),
    // 224 - 231
    VT_RGB(255,215,215), VT_RGB(255,215,255), VT_RGB(255,255,  0), VT_RGB(255,255, 95),
    VT_RGB(255,255,135), VT_RGB(255,255,175), VT_RGB(255,255,215), VT_RGB(255,255,255),
    // 232 - 239: Grayscale ramp
    VT_RGB(  8,  8,  8), VT_RGB( 18, 18, 18), VT_RGB( 28, 28, 28), VT_RGB( 38, 38, 38),
    VT_RGB( 48, 48, 48), VT_RGB( 58, 58, 58), VT_RGB( 68, 68, 68), VT_RGB( 78, 78, 78),
    // 240 - 247
    VT_RGB( 88, 88, 88), VT_RGB( 98, 98, 98), VT_RGB(108,108,108), VT_RGB(118,118,118),
    VT_RGB(128,128,128), VT_RGB(138,138,138), VT_RGB(148,148,148), VT_RGB(158,158,158),
    // 248 - 255
    VT_RGB(168,168,168), VT_RGB(178,178,178), VT_RGB(188,188,188), VT_RGB(198,198,198),
    VT_RGB(208,208,208), VT_RGB(218,218,218), VT_RGB(228,228,228), VT_RGB(238,238,238),
};

// ---------------------------------------------------------------------------
// VtColorParse — port of XsParseColor from vtterm.c
// Replaces COLORREF/RGB() with VtRgb24/VT_RGB(); _strnicmp → strncasecmp
// ---------------------------------------------------------------------------

bool VtColorParse(const char *spec, VtRgb24 *out_rgb) {
    unsigned int r, g, b;

    if (!spec || !out_rgb) return false;

    if (strncasecmp(spec, "rgb:", 4) == 0) {
        switch (strlen(spec)) {
        case 9:   // rgb:R/G/B
            if (sscanf(spec, "rgb:%1x/%1x/%1x", &r, &g, &b) != 3) return false;
            r *= 17; g *= 17; b *= 17;
            break;
        case 12:  // rgb:RR/GG/BB
            if (sscanf(spec, "rgb:%2x/%2x/%2x", &r, &g, &b) != 3) return false;
            break;
        case 15:  // rgb:RRR/GGG/BBB
            if (sscanf(spec, "rgb:%3x/%3x/%3x", &r, &g, &b) != 3) return false;
            r >>= 4; g >>= 4; b >>= 4;
            break;
        case 18:  // rgb:RRRR/GGGG/BBBB
            if (sscanf(spec, "rgb:%4x/%4x/%4x", &r, &g, &b) != 3) return false;
            r >>= 8; g >>= 8; b >>= 8;
            break;
        default:
            return false;
        }
    } else if (spec[0] == '#') {
        switch (strlen(spec)) {
        case 4:   // #RGB
            if (sscanf(spec, "#%1x%1x%1x", &r, &g, &b) != 3) return false;
            r <<= 4; g <<= 4; b <<= 4;
            break;
        case 7:   // #RRGGBB
            if (sscanf(spec, "#%2x%2x%2x", &r, &g, &b) != 3) return false;
            break;
        case 10:  // #RRRGGGBBB
            if (sscanf(spec, "#%3x%3x%3x", &r, &g, &b) != 3) return false;
            r >>= 4; g >>= 4; b >>= 4;
            break;
        case 13:  // #RRRRGGGGBBBB
            if (sscanf(spec, "#%4x%4x%4x", &r, &g, &b) != 3) return false;
            r >>= 8; g >>= 8; b >>= 8;
            break;
        default:
            return false;
        }
    } else {
        return false;
    }

    if (r > 255 || g > 255 || b > 255) return false;

    *out_rgb = VT_RGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return true;
}

// ---------------------------------------------------------------------------
// VtColorDefaultPalette — return pointer to static 256-entry palette
// ---------------------------------------------------------------------------

const VtRgb24 *VtColorDefaultPalette(void) {
    return kDefaultPalette;
}

// ---------------------------------------------------------------------------
// VtColorIndex256ToRGB — bounds-checked palette lookup
// ---------------------------------------------------------------------------

VtRgb24 VtColorIndex256ToRGB(int index, const VtRgb24 *palette) {
    if (index < 0 || index > 255) return VT_COLOR_DEFAULT;
    if (!palette) palette = kDefaultPalette;
    return palette[index];
}

// ---------------------------------------------------------------------------
// VtColorFindClosest — Euclidean nearest-color search
// Port of DispFindClosestColor from vtdisp.c
// ---------------------------------------------------------------------------

int VtColorFindClosest(VtRgb24 rgb, const VtRgb24 *palette) {
    if (!palette) palette = kDefaultPalette;

    int    best_idx  = 0;
    long   best_dist = 0x7FFFFFFF;

    int tr = (int)VT_R(rgb);
    int tg = (int)VT_G(rgb);
    int tb = (int)VT_B(rgb);

    for (int i = 0; i < 256; i++) {
        int dr = tr - (int)VT_R(palette[i]);
        int dg = tg - (int)VT_G(palette[i]);
        int db = tb - (int)VT_B(palette[i]);
        long dist = (long)(dr*dr) + (long)(dg*dg) + (long)(db*db);
        if (dist < best_dist) {
            best_dist = dist;
            best_idx  = i;
            if (dist == 0) break;  // exact match, no need to continue
        }
    }

    return best_idx;
}
