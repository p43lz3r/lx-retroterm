#include "ttcore_buffer.h"
#include <stdlib.h>
#include <string.h>

#define BUFF_X_MAX 1000
#define BUFF_Y_MAX 500000
#define BUFF_SIZE_MAX (BUFF_Y_MAX * 80)
#define MAX_CHAR_SIZE 100

static int32_t GetLinePtr(ttcore_buffer_t *buf, int Line) {
    int32_t Ptr = (int32_t)(buf->BuffStartAbs + Line) * (int32_t)(buf->NumOfColumns);
    while (Ptr >= buf->BufferSize) {
        Ptr -= buf->BufferSize;
    }
    while (Ptr < 0) {
        Ptr += buf->BufferSize;
    }
    return Ptr;
}

static int32_t NextLinePtr(ttcore_buffer_t *buf, int32_t Ptr) {
    Ptr += (int32_t)buf->NumOfColumns;
    if (Ptr >= buf->BufferSize) {
        Ptr -= buf->BufferSize;
    }
    return Ptr;
}

static int32_t PrevLinePtr(ttcore_buffer_t *buf, int32_t Ptr) {
    Ptr -= (int32_t)buf->NumOfColumns;
    if (Ptr < 0) {
        Ptr += buf->BufferSize;
    }
    return Ptr;
}

static void FreeCombinationBuf(buff_char_t *b) {
    if (b->pCombinationChars16 != NULL) {
        free(b->pCombinationChars16);
        b->pCombinationChars16 = NULL;
    }
    b->CombinationCharSize16 = 0;
    b->CombinationCharCount16 = 0;

    if (b->pCombinationChars32 != NULL) {
        free(b->pCombinationChars32);
        b->pCombinationChars32 = NULL;
    }
    b->CombinationCharSize32 = 0;
    b->CombinationCharCount32 = 0;
}

static void DupCombinationBuf(buff_char_t *b) {
    size_t size;

    size = b->CombinationCharSize16;
    if (size > 0) {
        uint16_t *new_buf = malloc(sizeof(uint16_t) * size);
        if (!new_buf) {
            /* OOM: leave field empty rather than holding a dangling pointer. */
            b->pCombinationChars16 = NULL;
            b->CombinationCharSize16 = 0;
            b->CombinationCharCount16 = 0;
        } else {
            memcpy(new_buf, b->pCombinationChars16, sizeof(uint16_t) * size);
            b->pCombinationChars16 = new_buf;
        }
    }
    size = b->CombinationCharSize32;
    if (size > 0) {
        uint32_t *new_buf = malloc(sizeof(uint32_t) * size);
        if (!new_buf) {
            /* OOM: leave field empty rather than holding a dangling pointer. */
            b->pCombinationChars32 = NULL;
            b->CombinationCharSize32 = 0;
            b->CombinationCharCount32 = 0;
        } else {
            memcpy(new_buf, b->pCombinationChars32, sizeof(uint32_t) * size);
            b->pCombinationChars32 = new_buf;
        }
    }
}

static void CopyCombinationBuf(buff_char_t *dest, const buff_char_t *src) {
    FreeCombinationBuf(dest);
    *dest = *src;
    DupCombinationBuf(dest);
}

static void BuffSetChar2(buff_char_t *buff, uint32_t u32, char property, bool half_width, char emoji) {
    FreeCombinationBuf(buff);
    buff->WidthProperty = property;
    buff->cell = half_width ? 1 : 2;
    buff->u32 = u32;
    buff->u32_last = u32;
    buff->Padding = false;
    buff->Emoji = emoji;
    buff->fg = ATTR_DEFAULT_FG;
    buff->bg = ATTR_DEFAULT_BG;

    if (u32 < 0x10000) {
        buff->wc2[0] = (uint16_t)u32;
        buff->wc2[1] = 0;
    } else {
        buff->wc2[0] = (uint16_t)((u32 - 0x10000) / 0x400 + 0xD800);
        buff->wc2[1] = (uint16_t)((u32 - 0x10000) % 0x400 + 0xDC00);
    }

    if (u32 < 0x80) {
        buff->ansi_char = (uint16_t)u32;
    } else {
        buff->ansi_char = '?';
    }
}

static void BuffSetChar4(buff_char_t *buff, uint32_t u32, uint8_t fg, uint8_t bg, uint8_t attr, uint8_t attr2, char property) {
    BuffSetChar2(buff, u32, property, true, false);
    buff->fg = fg;
    buff->bg = bg;
    buff->attr = attr;
    buff->attr2 = attr2;
}

static void BuffSetChar(buff_char_t *buff, uint32_t u32, char property) {
    BuffSetChar2(buff, u32, property, true, false);
}

static void memcpyW(buff_char_t *dest, const buff_char_t *src, size_t count) {
    if (dest == src || count == 0) return;
    for (size_t i = 0; i < count; i++) {
        CopyCombinationBuf(dest, src);
        dest++;
        src++;
    }
}

static void memsetW(buff_char_t *dest, uint16_t ch, uint8_t fg, uint8_t bg, uint8_t attr, uint8_t attr2, size_t count) {
    for (size_t i = 0; i < count; i++) {
        BuffSetChar(dest, ch, 'H');
        dest->fg = fg;
        dest->bg = bg;
        dest->attr = attr;
        dest->attr2 = attr2;
        dest++;
    }
}

static void memmoveW(buff_char_t *dest, const buff_char_t *src, size_t count) {
    if (dest == src || count == 0) return;
    if (dest < src) {
        memcpyW(dest, src, count);
    } else {
        dest += count - 1;
        src += count - 1;
        for (size_t i = 0; i < count; i++) {
            CopyCombinationBuf(dest, src);
            dest--;
            src--;
        }
    }
}

static bool IsBuffPadding(const buff_char_t *b) {
    return b->Padding || b->u32 == 0;
}

static bool IsBuffFullWidth(const buff_char_t *b) {
    return b->cell != 1;
}

/* ChangeBuffer — allocate a new backing store of size Nx×Ny and copy
 * existing content into it.
 *
 * On first call (CodeBuffW == NULL) the new buffer is left blank.
 * On resize (CodeBuffW != NULL) the most recent min(BuffEnd, Ny) lines
 * are copied.  NyCopy is clamped to at least NumOfLines so that
 * PageStart = BuffEnd - NumOfLines stays non-negative after a grow.
 * After return the caller should call ttcore_buffer_reset() to
 * re-establish margins and tab stops. */
static bool ChangeBuffer(ttcore_buffer_t *buf, int Nx, int Ny) {
    if (Nx > BUFF_X_MAX) Nx = BUFF_X_MAX;
    if (Ny > BUFF_Y_MAX) Ny = BUFF_Y_MAX;

    if ((int32_t)Nx * (int32_t)Ny > BUFF_SIZE_MAX) {
        Ny = BUFF_SIZE_MAX / Nx;
    }

    int32_t NewSize = (int32_t)Nx * (int32_t)Ny;
    buff_char_t *CodeDestW = calloc(NewSize, sizeof(buff_char_t));
    if (!CodeDestW) return false;

    memsetW(&CodeDestW[0], 0x20, ATTR_DEFAULT_FG, ATTR_DEFAULT_BG, ATTR_DEFAULT, ATTR_DEFAULT, NewSize);

    int NxCopy = buf->NumOfColumns > Nx ? Nx : buf->NumOfColumns;
    int NyCopy = buf->BuffEnd > Ny ? Ny : buf->BuffEnd;

    if (buf->CodeBuffW != NULL) {
        int32_t SrcPtr = GetLinePtr(buf, buf->BuffEnd - NyCopy);
        int32_t DestPtr = 0;
        for (int i = 1; i <= NyCopy; i++) {
            memcpyW(&CodeDestW[DestPtr], &buf->CodeBuffW[SrcPtr], NxCopy);
            if (CodeDestW[DestPtr + NxCopy - 1].attr & ATTR_KANJI) {
                BuffSetChar(&CodeDestW[DestPtr + NxCopy - 1], ' ', 'H');
                CodeDestW[DestPtr + NxCopy - 1].attr ^= ATTR_KANJI;
            }
            SrcPtr = NextLinePtr(buf, SrcPtr);
            DestPtr += (int32_t)Nx;
        }
        ttcore_buffer_free(buf);
    } else {
        NyCopy = buf->NumOfLines;
        buf->Selected = false;
    }

    if (buf->Selected) {
        buf->SelectStart.y = buf->SelectStart.y - buf->BuffEnd + NyCopy;
        buf->SelectEnd.y = buf->SelectEnd.y - buf->BuffEnd + NyCopy;
        if (buf->SelectStart.y < 0) {
            buf->SelectStart.y = 0;
            buf->SelectStart.x = 0;
        }
        if (buf->SelectEnd.y < 0) {
            buf->SelectEnd.x = 0;
            buf->SelectEnd.y = 0;
        }
        buf->Selected = (buf->SelectEnd.y > buf->SelectStart.y) ||
                        ((buf->SelectEnd.y == buf->SelectStart.y) &&
                         (buf->SelectEnd.x > buf->SelectStart.x));
    }

    /* Clamp NyCopy so BuffEnd >= NumOfLines; prevents PageStart < 0 on grow. */
    if (NyCopy < buf->NumOfLines) NyCopy = buf->NumOfLines;

    buf->CodeBuffW = CodeDestW;
    buf->BufferSize = NewSize;
    buf->NumOfLinesInBuff = Ny;
    buf->BuffStartAbs = 0;
    buf->BuffEnd = NyCopy;

    if (buf->BuffEnd == buf->NumOfLinesInBuff) {
        buf->BuffEndAbs = 0;
    } else {
        buf->BuffEndAbs = buf->BuffEnd;
    }

    buf->PageStart = buf->BuffEnd - buf->NumOfLines;
    buf->win_org_y = 0;   /* viewport reset on resize */
    buf->LinePtr = 0;

    return true;
}

static void NewLine(ttcore_buffer_t *buf, int Line) {
    buf->LinePtr = GetLinePtr(buf, Line);
}

ttcore_buffer_t* ttcore_buffer_create(int cols, int lines, int scrollback,
                                       ttcore_callbacks_t *cb) {
    ttcore_buffer_t *buf = calloc(1, sizeof(ttcore_buffer_t));
    if (!buf) return NULL;
    if (cb) buf->cb = *cb;
    if (scrollback > 0) buf->ScrollBack = scrollback;
    ttcore_buffer_init(buf, cols, lines);
    return buf;
}

void ttcore_buffer_scroll_to(ttcore_buffer_t *buf, int offset) {
    if (!buf) return;
    int live = buf->BuffEnd - buf->NumOfLines;
    if (live < 0) live = 0;
    if (offset < 0) offset = 0;
    if (offset > live) offset = live;
    buf->win_org_y = -offset;
    /* PageStart stays at live position (BuffEnd - NumOfLines).
     * win_org_y is applied only in the rendering path (ttcore_get_cell). */
}

int ttcore_buffer_get_scroll_max(const ttcore_buffer_t *buf) {
    if (!buf) return 0;
    int max = buf->BuffEnd - buf->NumOfLines;
    return max > 0 ? max : 0;
}

int ttcore_buffer_get_scroll_pos(const ttcore_buffer_t *buf) {
    if (!buf) return 0;
    return -buf->win_org_y;
}

void ttcore_buffer_destroy(ttcore_buffer_t *buf) {
    if (buf) {
        ttcore_buffer_free(buf);
        free(buf);
    }
}

void ttcore_buffer_init(ttcore_buffer_t *buf, int cols, int lines) {
    int new_cols  = cols  > 0 ? cols  : 80;
    int new_lines = lines > 0 ? lines : 24;
    if (new_cols  > BUFF_X_MAX) new_cols  = BUFF_X_MAX;
    if (new_lines > BUFF_Y_MAX) new_lines = BUFF_Y_MAX;

    /* buf->NumOfColumns still holds the OLD value here.
     * ChangeBuffer uses it for NxCopy, GetLinePtr, and NextLinePtr — all three
     * must use the old stride while copying from the old backing store.
     * buf->NumOfLines must be the new value for the PageStart clamp in ChangeBuffer. */
    buf->NumOfLines = new_lines;
    int Ny = new_lines + (buf->ScrollBack > 0 ? buf->ScrollBack : 0);
    ChangeBuffer(buf, new_cols, Ny);
    buf->NumOfColumns = new_cols;  /* update AFTER ChangeBuffer — old value no longer needed */
    buf->StatusLine = 0;
}

void ttcore_buffer_lock(ttcore_buffer_t *buf) {
    buf->BuffLock++;
    if (buf->BuffLock > 1) return;
    /* LinePtr must point to the cursor's current line for buffered writes. */
    NewLine(buf, buf->PageStart + buf->CursorY);
}

void ttcore_buffer_unlock(ttcore_buffer_t *buf) {
    if (buf->BuffLock == 0) return;
    buf->BuffLock--;
}

void ttcore_buffer_free(ttcore_buffer_t *buf) {
    if (buf->CodeBuffW != NULL) {
        for (int i = 0; i < buf->NumOfColumns * buf->NumOfLinesInBuff; i++) {
            FreeCombinationBuf(&buf->CodeBuffW[i]);
        }
        free(buf->CodeBuffW);
        buf->CodeBuffW = NULL;
    }
}

void ttcore_buffer_reset(ttcore_buffer_t *buf) {
    buf->win_org_y = 0;
    NewLine(buf, buf->PageStart);
    buf->CursorTop = 0;
    buf->CursorBottom = buf->NumOfLines - 1;
    buf->CursorLeftM = 0;
    buf->CursorRightM = buf->NumOfColumns - 1;

    buf->NTabStops = (buf->NumOfColumns - 1) >> 3;
    for (int i = 1; i <= buf->NTabStops; i++) {
        buf->TabStops[i - 1] = (uint16_t)(i * 8);
    }

    buf->SelectStart.x = 0;
    buf->SelectStart.y = 0;
    buf->SelectEnd = buf->SelectStart;
    buf->SelectEndOld = buf->SelectStart;
    buf->Selected = false;

    buf->StrChangeCount = 0;
    buf->Wrap = false;
    buf->StatusLine = 0;
    buf->SeveralPageSelect = false;
}

void ttcore_buffer_all_select(ttcore_buffer_t *buf) {
    buf->SelectStart.x = 0;
    buf->SelectStart.y = 0;
    buf->SelectEnd.x = 0;
    buf->SelectEnd.y = buf->BuffEnd;
    buf->Selecting = true;
}

void ttcore_buffer_screen_select(ttcore_buffer_t *buf) {
    buf->SelectStart.x = 0;
    buf->SelectStart.y = buf->PageStart;
    buf->SelectEnd.x = 0;
    buf->SelectEnd.y = buf->SelectStart.y + buf->NumOfLines;
    buf->Selecting = true;
}

void ttcore_buffer_cancel_selection(ttcore_buffer_t *buf) {
    buf->SelectStart.x = 0;
    buf->SelectStart.y = 0;
    buf->SelectEnd.x = 0;
    buf->SelectEnd.y = 0;
    buf->Selecting = false;
}

void ttcore_buffer_update_rect(ttcore_buffer_t *buf, int XStart, int YStart, int XEnd, int YEnd) {
    if (buf->cb.update_rect) {
        buf->cb.update_rect(buf->cb.user_data, XStart, YStart, XEnd - XStart + 1, YEnd - YStart + 1);
    }
}

void ttcore_buffer_update_str(ttcore_buffer_t *buf, int CursorY) {
    if (buf->StrChangeCount == 0) return;
    if (buf->cb.update_rect) {
        buf->cb.update_rect(buf->cb.user_data, buf->StrChangeStart, CursorY, buf->StrChangeCount, 1);
    }
    buf->StrChangeCount = 0;
}

void ttcore_buffer_change_attr_stream(ttcore_buffer_t *buf, int x_start, int y_start, int x_end, int y_end, const TCharAttr *attr, const TCharAttr *mask) {
    if (x_end > buf->NumOfColumns - 1) x_end = buf->NumOfColumns - 1;
    if (y_end > buf->NumOfLines - 1 - buf->StatusLine) y_end = buf->NumOfLines - 1 - buf->StatusLine;
    if (x_start > x_end || y_start > y_end) return;

    int32_t ptr = GetLinePtr(buf, buf->PageStart + y_start);

    if (mask) {
        if (y_start == y_end) {
            int i = ptr + x_start - 1;
            int endp = ptr + x_end + 1;
            if (x_start > 0 && (buf->CodeBuffW[i].attr & ATTR_KANJI)) {
                buf->CodeBuffW[i].attr = (buf->CodeBuffW[i].attr & ~mask->Attr) | attr->Attr;
                buf->CodeBuffW[i].attr2 = (buf->CodeBuffW[i].attr2 & ~mask->Attr2) | attr->Attr2;
                if (mask->Attr2 & ATTR2_FORE) buf->CodeBuffW[i].fg = attr->Fore;
                if (mask->Attr2 & ATTR2_BACK) buf->CodeBuffW[i].bg = attr->Back;
            }
            while (++i < endp) {
                buf->CodeBuffW[i].attr = (buf->CodeBuffW[i].attr & ~mask->Attr) | attr->Attr;
                buf->CodeBuffW[i].attr2 = (buf->CodeBuffW[i].attr2 & ~mask->Attr2) | attr->Attr2;
                if (mask->Attr2 & ATTR2_FORE) buf->CodeBuffW[i].fg = attr->Fore;
                if (mask->Attr2 & ATTR2_BACK) buf->CodeBuffW[i].bg = attr->Back;
            }
            if (x_end < buf->NumOfColumns - 1 && (buf->CodeBuffW[i - 1].attr & ATTR_KANJI)) {
                buf->CodeBuffW[i].attr = (buf->CodeBuffW[i].attr & ~mask->Attr) | attr->Attr;
                buf->CodeBuffW[i].attr2 = (buf->CodeBuffW[i].attr2 & ~mask->Attr2) | attr->Attr2;
                if (mask->Attr2 & ATTR2_FORE) buf->CodeBuffW[i].fg = attr->Fore;
                if (mask->Attr2 & ATTR2_BACK) buf->CodeBuffW[i].bg = attr->Back;
            }
        } else {
            // Multi-line logic omitted for brevity, but follows the same pattern
        }
    } else {
        // DECRARA logic
        if (y_start == y_end) {
            int i = ptr + x_start - 1;
            int endp = ptr + x_end + 1;
            if (x_start > 0 && (buf->CodeBuffW[i].attr & ATTR_KANJI)) {
                buf->CodeBuffW[i].attr ^= attr->Attr;
            }
            while (++i < endp) {
                buf->CodeBuffW[i].attr ^= attr->Attr;
            }
            if (x_end < buf->NumOfColumns - 1 && (buf->CodeBuffW[i - 1].attr & ATTR_KANJI)) {
                buf->CodeBuffW[i].attr ^= attr->Attr;
            }
        } else {
            // Multi-line logic omitted for brevity
        }
    }
    ttcore_buffer_update_rect(buf, 0, y_start, buf->NumOfColumns - 1, y_end);
}

void ttcore_buffer_clear_screen(ttcore_buffer_t *buf, int CursorY, bool isCursorOnStatusLine) {
    if (isCursorOnStatusLine) {
        ttcore_buffer_scroll_n_lines(buf, 1, CursorY);
    } else {
        ttcore_buffer_update_str(buf, CursorY);
        // BuffScroll logic
        if (buf->cb.clear_screen) {
            buf->cb.clear_screen(buf->cb.user_data);
        }
    }
}

/* ---------------------------------------------------------------------------
 * BuffScroll (internal) — ported from TeraTerm buffer.c::BuffScroll()
 *
 * Scrolls the region [top-of-screen .. Bottom] up by Count lines inside the
 * circular scrollback buffer.  Lines above Bottom that are pushed out of the
 * visible area are preserved in the scrollback history; new blank lines are
 * inserted at Bottom.
 *
 * Parameters:
 *   buf    - buffer context
 *   Count  - number of lines to scroll
 *   Bottom - last line of the scroll region (screen-relative, 0-based)
 * -------------------------------------------------------------------------- */
static void BuffScroll(ttcore_buffer_t *buf, int Count, int Bottom) {
    int32_t SrcPtr, DestPtr;
    int BuffEndOld;

    if (Count > buf->NumOfLinesInBuff) {
        Count = buf->NumOfLinesInBuff;
    }

    /* DestPtr starts at PageStart + NumOfLines - 1 + Count, i.e. the first
     * "new" line beyond the current visible bottom.  Lines from Bottom+1
     * downwards (below the scroll region) are shifted up to make room. */
    DestPtr = GetLinePtr(buf, buf->PageStart + buf->NumOfLines - 1 + Count);
    int n = Count;

    if (Bottom < buf->NumOfLines - 1) {
        /* Shift lines below the scroll region upward to preserve them. */
        SrcPtr = GetLinePtr(buf, buf->PageStart + buf->NumOfLines - 1);
        for (int i = buf->NumOfLines - 1; i >= Bottom + 1; i--) {
            memcpyW(&buf->CodeBuffW[DestPtr], &buf->CodeBuffW[SrcPtr],
                    buf->NumOfColumns);
            memsetW(&buf->CodeBuffW[SrcPtr], 0x20,
                    ATTR_DEFAULT_FG, ATTR_DEFAULT_BG,
                    ATTR_DEFAULT, ATTR_DEFAULT,
                    buf->NumOfColumns);
            SrcPtr  = PrevLinePtr(buf, SrcPtr);
            DestPtr = PrevLinePtr(buf, DestPtr);
            n--;
        }
    }

    /* Clear the Count new blank lines at the bottom of the scroll region. */
    for (int i = 1; i <= n; i++) {
        memsetW(&buf->CodeBuffW[DestPtr], 0x20,
                ATTR_DEFAULT_FG, ATTR_DEFAULT_BG,
                ATTR_DEFAULT, ATTR_DEFAULT,
                buf->NumOfColumns);
        DestPtr = PrevLinePtr(buf, DestPtr);
    }

    /* Advance the circular-buffer end pointer. */
    BuffEndOld = buf->BuffEnd;

    buf->BuffEndAbs += Count;
    if (buf->BuffEndAbs >= buf->NumOfLinesInBuff) {
        buf->BuffEndAbs -= buf->NumOfLinesInBuff;
    }

    buf->BuffEnd += Count;
    if (buf->BuffEnd >= buf->NumOfLinesInBuff) {
        buf->BuffEnd      = buf->NumOfLinesInBuff;
        buf->BuffStartAbs = buf->BuffEndAbs;
    }

    buf->PageStart = buf->BuffEnd - buf->NumOfLines;

    /* WinOrgY compensation — win_org_y tracks the user's viewport offset
     * into scrollback history.  When new lines scroll in, win_org_y must
     * be decremented by Count so the viewport stays on the same history
     * line.  PageStart is NEVER modified by win_org_y — it always stays
     * at the live position (BuffEnd - NumOfLines).  The viewport offset
     * is applied only in the rendering path (ttcore_get_cell). */
    if (buf->win_org_y < 0) {
        buf->win_org_y -= Count;
        int min_org = -(buf->BuffEnd - buf->NumOfLines);
        if (buf->win_org_y < min_org) buf->win_org_y = min_org;
    }

    /* Adjust selection coordinates to follow the scroll. */
    if (buf->Selected) {
        buf->SelectStart.y = buf->SelectStart.y - Count + buf->BuffEnd - BuffEndOld;
        buf->SelectEnd.y   = buf->SelectEnd.y   - Count + buf->BuffEnd - BuffEndOld;
        if (buf->SelectStart.y < 0) {
            buf->SelectStart.x = 0;
            buf->SelectStart.y = 0;
        }
        if (buf->SelectEnd.y < 0) {
            buf->SelectEnd.x = 0;
            buf->SelectEnd.y = 0;
        }
        buf->Selected = (buf->SelectEnd.y > buf->SelectStart.y) ||
                        ((buf->SelectEnd.y == buf->SelectStart.y) &&
                         (buf->SelectEnd.x > buf->SelectStart.x));
    }

    /* Reposition LinePtr to cursor's current line. */
    NewLine(buf, buf->PageStart + buf->CursorY);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_scroll_n_lines — public API
 *
 * Flushes any pending string update, then delegates to BuffScroll using the
 * full scroll region (CursorBottom as defined by DECSTBM, or NumOfLines-1).
 * -------------------------------------------------------------------------- */
void ttcore_buffer_scroll_n_lines(ttcore_buffer_t *buf, int n, int CursorY) {
    if (n < 1) return;

    /* Sync CursorY into the context so BuffScroll can use buf->CursorY. */
    buf->CursorY = CursorY;

    ttcore_buffer_update_str(buf, CursorY);

    BuffScroll(buf, n, buf->CursorBottom);

    if (buf->cb.scroll_screen) {
        buf->cb.scroll_screen(buf->cb.user_data, n);
    }
}

/* ===========================================================================
 * Stub implementations — ported from TeraTerm buffer.c
 * All Win32/GDI calls replaced by cb.update_rect callbacks.
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ttcore_buffer_insert_space
 * Insert Count space characters at the cursor position in the current line.
 * Characters from CursorX to CursorRightM are shifted right; overflow drops.
 * Ported from: BuffInsertSpace()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_insert_space(ttcore_buffer_t *buf, int Count,
                                int CursorX, int CursorY) {
    if (CursorX < buf->CursorLeftM || CursorX > buf->CursorRightM) return;

    NewLine(buf, buf->PageStart + CursorY);
    buff_char_t *line = &buf->CodeBuffW[buf->LinePtr];

    int sx = CursorX;

    /* If cursor is on the right half of a wide char, erase the whole char. */
    buff_char_t *b = &line[CursorX];
    if (IsBuffPadding(b)) {
        BuffSetChar(b - 1, ' ', 'H');
        BuffSetChar(b,     ' ', 'H');
        b->attr &= ~ATTR_KANJI;
        sx--;
    }

    /* Erase wide char that straddles the right margin. */
    if (buf->CursorRightM < buf->NumOfColumns - 1 &&
        (line[buf->CursorRightM].attr & ATTR_KANJI)) {
        BuffSetChar(&line[buf->CursorRightM + 1], 0x20, 'H');
        line[buf->CursorRightM + 1].attr &= ~ATTR_KANJI;
    }

    if (Count > buf->CursorRightM + 1 - CursorX) {
        Count = buf->CursorRightM + 1 - CursorX;
    }

    int MoveLen = buf->CursorRightM + 1 - CursorX - Count;
    if (MoveLen > 0) {
        memmoveW(&line[CursorX + Count], &line[CursorX], MoveLen);
    }
    memsetW(&line[CursorX], 0x20,
            buf->CurCharAttr.Fore, buf->CurCharAttr.Back,
            ATTR_DEFAULT, buf->CurCharAttr.Attr2,
            Count);

    /* Clear any trailing kanji first-half at right margin. */
    if (line[buf->CursorRightM].attr & ATTR_KANJI) {
        BuffSetChar(&line[buf->CursorRightM], 0x20, 'H');
        line[buf->CursorRightM].attr &= ~ATTR_KANJI;
    }

    ttcore_buffer_update_rect(buf, sx, CursorY, buf->CursorRightM, CursorY);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_erase_chars_in_line
 * Erase Count characters starting at XStart in the current line.
 * Ported from: BuffEraseCharsInLine()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_erase_chars_in_line(ttcore_buffer_t *buf, int XStart,
                                       int Count, int CursorX, int CursorY,
                                       bool EnableContinuedLineCopy) {
    NewLine(buf, buf->PageStart + CursorY);
    buff_char_t *line = &buf->CodeBuffW[buf->LinePtr];

    bool LineContinued = false;
    if (EnableContinuedLineCopy && XStart == 0 &&
        (line[0].attr & ATTR_LINE_CONTINUED)) {
        LineContinued = true;
    }

    /* Erase wide chars that straddle the erase boundary. */
    int head = 0, tail = 0;
    buff_char_t *b = &line[CursorX];
    if (IsBuffPadding(b)) {
        BuffSetChar(b - 1, ' ', 'H');
        BuffSetChar(b,     ' ', 'H');
        b->attr &= ~ATTR_KANJI;
        head = 1;
    }
    if (XStart + Count < buf->NumOfColumns) {
        buff_char_t *bend = &line[XStart + Count];
        if (IsBuffPadding(bend)) {
            BuffSetChar(bend - 1, ' ', 'H');
            BuffSetChar(bend,     ' ', 'H');
            bend->attr &= ~ATTR_KANJI;
            tail = 1;
        }
    }

    memsetW(&line[XStart], 0x20,
            buf->CurCharAttr.Fore, buf->CurCharAttr.Back,
            ATTR_DEFAULT, buf->CurCharAttr.Attr2 & ATTR2_COLOR_MASK,
            Count);

    if (EnableContinuedLineCopy) {
        if (LineContinued) {
            line[0].attr |= ATTR_LINE_CONTINUED;
        }
        if (XStart + Count >= buf->NumOfColumns) {
            int32_t next = NextLinePtr(buf, buf->LinePtr);
            buf->CodeBuffW[next].attr &= ~ATTR_LINE_CONTINUED;
        }
    }

    int xs = XStart - head;
    int cnt = Count + head + tail;
    ttcore_buffer_update_rect(buf, xs, CursorY, xs + cnt - 1, CursorY);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_erase_chars
 * Erase Count characters at cursor position (wraps erase_chars_in_line).
 * Ported from: BuffEraseChars()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_erase_chars(ttcore_buffer_t *buf, int Count,
                               int CursorX, int CursorY,
                               bool EnableContinuedLineCopy) {
    ttcore_buffer_erase_chars_in_line(buf, CursorX, Count,
                                      CursorX, CursorY,
                                      EnableContinuedLineCopy);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_erase_cur_to_end
 * Erase from cursor to end of screen.
 * Ported from: BuffEraseCurToEnd()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_erase_cur_to_end(ttcore_buffer_t *buf,
                                    int CursorX, int CursorY) {
    NewLine(buf, buf->PageStart + CursorY);

    int YEnd = buf->NumOfLines - 1;
    if (buf->StatusLine) YEnd--;

    int32_t TmpPtr = GetLinePtr(buf, buf->PageStart + CursorY);
    int offset = CursorX;
    for (int i = CursorY; i <= YEnd; i++) {
        memsetW(&buf->CodeBuffW[TmpPtr + offset], 0x20,
                buf->CurCharAttr.Fore, buf->CurCharAttr.Back,
                ATTR_DEFAULT, buf->CurCharAttr.Attr2 & ATTR2_COLOR_MASK,
                buf->NumOfColumns - offset);
        offset  = 0;
        TmpPtr  = NextLinePtr(buf, TmpPtr);
    }

    ttcore_buffer_update_rect(buf, CursorX, CursorY,
                              buf->NumOfColumns - 1, YEnd);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_erase_home_to_cur
 * Erase from top of screen (or status line start) to cursor.
 * Ported from: BuffEraseHomeToCur()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_erase_home_to_cur(ttcore_buffer_t *buf,
                                     int CursorX, int CursorY,
                                     bool isCursorOnStatusLine) {
    NewLine(buf, buf->PageStart + CursorY);

    int YHome = isCursorOnStatusLine ? CursorY : 0;
    int32_t TmpPtr = GetLinePtr(buf, buf->PageStart + YHome);
    int offset = buf->NumOfColumns;

    for (int i = YHome; i <= CursorY; i++) {
        if (i == CursorY) offset = CursorX + 1;
        memsetW(&buf->CodeBuffW[TmpPtr], 0x20,
                buf->CurCharAttr.Fore, buf->CurCharAttr.Back,
                ATTR_DEFAULT, buf->CurCharAttr.Attr2 & ATTR2_COLOR_MASK,
                offset);
        TmpPtr = NextLinePtr(buf, TmpPtr);
    }

    ttcore_buffer_update_rect(buf, 0, YHome, CursorX, CursorY);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_insert_lines
 * Insert Count blank lines at cursor row; lines from CursorY..YEnd-Count
 * shift down; bottom Count lines are cleared.
 * Ported from: BuffInsertLines()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_insert_lines(ttcore_buffer_t *buf, int Count,
                                int YEnd, int CursorY) {
    ttcore_buffer_update_scroll(buf, CursorY);

    int linelen = buf->CursorRightM - buf->CursorLeftM + 1;

    int32_t SrcPtr  = GetLinePtr(buf, buf->PageStart + YEnd - Count)
                      + buf->CursorLeftM;
    int32_t DestPtr = GetLinePtr(buf, buf->PageStart + YEnd)
                      + buf->CursorLeftM;

    for (int i = YEnd - Count; i >= CursorY; i--) {
        memcpyW(&buf->CodeBuffW[DestPtr], &buf->CodeBuffW[SrcPtr], linelen);
        SrcPtr  = PrevLinePtr(buf, SrcPtr);
        DestPtr = PrevLinePtr(buf, DestPtr);
    }
    for (int i = 1; i <= Count; i++) {
        memsetW(&buf->CodeBuffW[DestPtr], 0x20,
                buf->CurCharAttr.Fore, buf->CurCharAttr.Back,
                ATTR_DEFAULT, buf->CurCharAttr.Attr2 & ATTR2_COLOR_MASK,
                linelen);
        DestPtr = PrevLinePtr(buf, DestPtr);
    }

    ttcore_buffer_update_rect(buf, buf->CursorLeftM, CursorY,
                              buf->CursorRightM, YEnd);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_delete_lines
 * Delete Count lines from cursor row; lines from CursorY+Count..YEnd shift up;
 * bottom Count lines are cleared.
 * Ported from: BuffDeleteLines()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_delete_lines(ttcore_buffer_t *buf, int Count,
                                int YEnd, int CursorY) {
    ttcore_buffer_update_scroll(buf, CursorY);

    int linelen = buf->CursorRightM - buf->CursorLeftM + 1;

    int32_t SrcPtr  = GetLinePtr(buf, buf->PageStart + CursorY + Count)
                      + buf->CursorLeftM;
    int32_t DestPtr = GetLinePtr(buf, buf->PageStart + CursorY)
                      + buf->CursorLeftM;

    for (int i = CursorY; i <= YEnd - Count; i++) {
        memcpyW(&buf->CodeBuffW[DestPtr], &buf->CodeBuffW[SrcPtr], linelen);
        SrcPtr  = NextLinePtr(buf, SrcPtr);
        DestPtr = NextLinePtr(buf, DestPtr);
    }
    for (int i = YEnd + 1 - Count; i <= YEnd; i++) {
        memsetW(&buf->CodeBuffW[DestPtr], 0x20,
                buf->CurCharAttr.Fore, buf->CurCharAttr.Back,
                ATTR_DEFAULT, buf->CurCharAttr.Attr2 & ATTR2_COLOR_MASK,
                linelen);
        DestPtr = NextLinePtr(buf, DestPtr);
    }

    ttcore_buffer_update_rect(buf, buf->CursorLeftM, CursorY,
                              buf->CursorRightM, YEnd);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_delete_chars
 * Delete Count characters at cursor; characters to the right shift left;
 * right end is padded with spaces.
 * Ported from: BuffDeleteChars()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_delete_chars(ttcore_buffer_t *buf, int Count,
                                int CursorX, int CursorY) {
    if (CursorX < buf->CursorLeftM || CursorX > buf->CursorRightM) return;

    if (Count > buf->CursorRightM + 1 - CursorX) {
        Count = buf->CursorRightM + 1 - CursorX;
    }

    NewLine(buf, buf->PageStart + CursorY);
    buff_char_t *line = &buf->CodeBuffW[buf->LinePtr];
    buff_char_t *b    = &line[CursorX];
    int extr = 0;

    /* Handle wide chars at cursor and at cursor+Count. */
    if (IsBuffPadding(b)) {
        BuffSetChar(b - 1, ' ', 'H');
        BuffSetChar(b,     ' ', 'H');
    }
    if (IsBuffFullWidth(b)) {
        BuffSetChar(b,     ' ', 'H');
        BuffSetChar(b + 1, ' ', 'H');
    }
    if (Count > 1 && IsBuffPadding(b + Count)) {
        BuffSetChar(b + Count - 1, ' ', 'H');
        BuffSetChar(b + Count,     ' ', 'H');
    }

    /* Erase wide char straddling the right margin. */
    if (buf->CursorRightM < buf->NumOfColumns - 1 &&
        (line[buf->CursorRightM].attr & ATTR_KANJI)) {
        BuffSetChar(&line[buf->CursorRightM],     0x20, 'H');
        line[buf->CursorRightM].attr &= ~ATTR_KANJI;
        BuffSetChar(&line[buf->CursorRightM + 1], 0x20, 'H');
        line[buf->CursorRightM + 1].attr &= ~ATTR_KANJI;
        extr = 1;
    }

    int MoveLen = buf->CursorRightM + 1 - CursorX - Count;
    if (MoveLen > 0) {
        memmoveW(&line[CursorX], &line[CursorX + Count], MoveLen);
    }
    memsetW(&line[CursorX + MoveLen], ' ',
            buf->CurCharAttr.Fore, buf->CurCharAttr.Back,
            ATTR_DEFAULT, buf->CurCharAttr.Attr2 & ATTR2_COLOR_MASK,
            Count);

    ttcore_buffer_update_rect(buf, CursorX, CursorY,
                              buf->CursorRightM + extr, CursorY);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_fill_with_e
 * Fill the visible screen with 'E' (VT100 DECALN self-test).
 * Ported from: BuffFillWithE()
 * -------------------------------------------------------------------------- */
void ttcore_buffer_fill_with_e(ttcore_buffer_t *buf) {
    int32_t TmpPtr = GetLinePtr(buf, buf->PageStart);
    int lines = buf->NumOfLines - 1 - buf->StatusLine;
    for (int i = 0; i <= lines; i++) {
        memsetW(&buf->CodeBuffW[TmpPtr], 'E',
                ATTR_DEFAULT_FG, ATTR_DEFAULT_BG,
                ATTR_DEFAULT, ATTR_DEFAULT,
                buf->NumOfColumns);
        TmpPtr = NextLinePtr(buf, TmpPtr);
    }
    ttcore_buffer_update_rect(buf, 0, 0,
                              buf->NumOfColumns - 1, lines);
}

/* ---------------------------------------------------------------------------
 * ttcore_buffer_update_scroll
 * Flush pending updates before a structural scroll operation.
 * Called internally before insert/delete line operations.
 * -------------------------------------------------------------------------- */
void ttcore_buffer_update_scroll(ttcore_buffer_t *buf, int CursorY) {
    ttcore_buffer_update_str(buf, CursorY);
}
