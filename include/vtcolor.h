// 2026-03-10 00:00 v0.1.0
// vtcolor.h — Color string parsing and 256-color palette
// Ported from TeraTerm 5 vtterm.c (XsParseColor) + defaultcolortable.c
// Win32-free (no COLORREF), C99, pure functions — zero global state.
//
// Copyright (C) 1994-1998 T. Teranishi / (C) 2007- TeraTerm Project
// Port (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef VTCOLOR_H_
#define VTCOLOR_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// RGB24 color type: 0x00RRGGBB  (same byte order as CSS / X11)
// ---------------------------------------------------------------------------
typedef uint32_t VtRgb24;

#define VT_RGB(r, g, b)  \
    ((VtRgb24)(((uint32_t)(uint8_t)(r) << 16) | \
               ((uint32_t)(uint8_t)(g) << 8)  | \
                (uint32_t)(uint8_t)(b)))

#define VT_R(rgb)  ((uint8_t)(((rgb) >> 16) & 0xFFu))
#define VT_G(rgb)  ((uint8_t)(((rgb) >>  8) & 0xFFu))
#define VT_B(rgb)  ((uint8_t)( (rgb)        & 0xFFu))

// Sentinel: "not specified / use default"
#define VT_COLOR_DEFAULT  0xFF000000u

// ---------------------------------------------------------------------------
// Color string parsing  (replaces XsParseColor from vtterm.c)
// ---------------------------------------------------------------------------

// Parse an X11/xterm color specification into an RGB24 value.
//
// Accepted formats (case-insensitive prefix):
//   rgb:R/G/B          — 1 hex digit per channel  (× 17 → 0-255)
//   rgb:RR/GG/BB       — 2 hex digits per channel (direct 0-255)
//   rgb:RRR/GGG/BBB    — 3 hex digits per channel (>> 4 → 0-255)
//   rgb:RRRR/GGGG/BBBB — 4 hex digits per channel (>> 8 → 0-255)
//   #RGB               — 1 hex digit  (<< 4 → 0-255)
//   #RRGGBB            — 2 hex digits (direct 0-255)
//   #RRRGGGBBB         — 3 hex digits (>> 4)
//   #RRRRGGGGBBBB      — 4 hex digits (>> 8)
//
// Returns true on success, false on parse error.
bool VtColorParse(const char *spec, VtRgb24 *out_rgb);

// ---------------------------------------------------------------------------
// 256-color palette  (replaces DefaultColorTable from defaultcolortable.c)
// ---------------------------------------------------------------------------

// Return pointer to the static 256-entry default palette.
// Each entry is a VtRgb24 value (0x00RRGGBB).
const VtRgb24 *VtColorDefaultPalette(void);

// Convert a 256-color index (0-255) to its RGB24 value using the palette.
// Uses the default palette if palette is NULL.
// Returns VT_COLOR_DEFAULT if index is out of range.
VtRgb24 VtColorIndex256ToRGB(int index, const VtRgb24 *palette);

// ---------------------------------------------------------------------------
// Closest-color search  (replaces DispFindClosestColor from vtdisp.c)
// ---------------------------------------------------------------------------

// Find the 256-color index closest to the given RGB (Euclidean distance).
// Uses the default palette if palette is NULL.
// Returns -1 on error (e.g. empty palette).
int VtColorFindClosest(VtRgb24 rgb, const VtRgb24 *palette);

#ifdef __cplusplus
}
#endif

#endif  // VTCOLOR_H_
