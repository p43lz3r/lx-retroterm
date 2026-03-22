#ifndef TTCORE_BUFFER_H
#define TTCORE_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "libttcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Character attribute bit masks */
#define ATTR_DEFAULT       0x00
#define ATTR_DEFAULT_FG    0x00
#define ATTR_DEFAULT_BG    0x00
#define ATTR_BOLD          0x01
#define ATTR_UNDER         0x02
#define ATTR_SPECIAL       0x04
#define ATTR_FONT_MASK     0x07
#define ATTR_BLINK         0x08
#define ATTR_REVERSE       0x10
#define ATTR_LINE_CONTINUED 0x20
#define ATTR_URL           0x40
#define ATTR_KANJI         0x80
#define ATTR_PADDING       0x100

/* Color attribute bit masks */
#define ATTR2_FORE         0x01
#define ATTR2_BACK         0x02
#define ATTR_SGR_MASK      (ATTR_BOLD | ATTR_UNDER | ATTR_BLINK | ATTR_REVERSE)
#define ATTR_COLOR_MASK    (ATTR_BOLD | ATTR_BLINK | ATTR_REVERSE)
#define ATTR2_COLOR_MASK   (ATTR2_FORE | ATTR2_BACK)
#define ATTR2_PROTECT      0x04

typedef struct {
    uint8_t Attr;
    uint8_t Attr2;
    uint16_t AttrEx;
    uint8_t Fore;
    uint8_t Back;
} TCharAttr;

typedef struct {
    uint32_t u32;
    uint32_t u32_last;
    char WidthProperty;
    char cell;
    char Padding;
    char Emoji;
    uint8_t CombinationCharCount16;
    uint8_t CombinationCharSize16;
    uint8_t CombinationCharCount32;
    uint8_t CombinationCharSize32;
    uint16_t *pCombinationChars16;
    uint32_t *pCombinationChars32;
    uint16_t wc2[2];
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
    uint8_t attr2;
    uint16_t ansi_char;
} buff_char_t;

typedef struct {
    int x;
    int y;
} tt_point_t;

typedef struct ttcore_buffer_t {
    buff_char_t *CodeBuffW;
    int32_t LinePtr;
    int32_t BufferSize;
    int NumOfLinesInBuff;
    int BuffStartAbs;
    int BuffEndAbs;
    int BuffEnd;
    int PageStart;

    int NumOfColumns;
    int NumOfLines;
    int ScrollBack;     /* extra lines above NumOfLines allocated in the ring buffer */
    int win_org_y;      /* viewport offset relative to live view: 0 = live, <0 = in history */

    int StatusLine;
    int CursorTop, CursorBottom, CursorLeftM, CursorRightM;
    /* Logical cursor position (0-based, screen-relative).
     * Formerly Win32 globals; moved here during Linux refactor. */
    int CursorX;
    int CursorY;
    bool Wrap;

    uint16_t TabStops[256];
    int NTabStops;

    int BuffLock;

    bool Selected;
    bool Selecting;
    tt_point_t SelectStart;
    tt_point_t SelectEnd;
    tt_point_t ClickCell;
    tt_point_t SelectEndOld;
    uint32_t SelectStartTime;
    bool BoxSelect;
    tt_point_t DblClkStart, DblClkEnd;

    int StrChangeStart;
    int StrChangeCount;

    bool SeveralPageSelect;

    TCharAttr CurCharAttr;

    /* SaveBuff / SaveBuffX / SaveBuffY removed: Win32 clipboard artefact.
     * Clipboard is handled by the UI layer via cb.set_clipboard. */

    int CodePage;

    ttcore_callbacks_t cb;
} ttcore_buffer_t;

ttcore_buffer_t* ttcore_buffer_create(int cols, int lines, int scrollback,
                                       ttcore_callbacks_t *cb);
void ttcore_buffer_destroy(ttcore_buffer_t *buf);

void ttcore_buffer_init(ttcore_buffer_t *buf, int cols, int lines);
void ttcore_buffer_scroll_to(ttcore_buffer_t *buf, int offset);
int  ttcore_buffer_get_scroll_max(const ttcore_buffer_t *buf);
int  ttcore_buffer_get_scroll_pos(const ttcore_buffer_t *buf);
void ttcore_buffer_lock(ttcore_buffer_t *buf);
void ttcore_buffer_unlock(ttcore_buffer_t *buf);
void ttcore_buffer_free(ttcore_buffer_t *buf);
void ttcore_buffer_reset(ttcore_buffer_t *buf);
void ttcore_buffer_all_select(ttcore_buffer_t *buf);
void ttcore_buffer_screen_select(ttcore_buffer_t *buf);
void ttcore_buffer_cancel_selection(ttcore_buffer_t *buf);
void ttcore_buffer_change_select_region(ttcore_buffer_t *buf);
void ttcore_buffer_insert_space(ttcore_buffer_t *buf, int Count, int CursorX, int CursorY);
void ttcore_buffer_erase_cur_to_end(ttcore_buffer_t *buf, int CursorX, int CursorY);
void ttcore_buffer_erase_home_to_cur(ttcore_buffer_t *buf, int CursorX, int CursorY, bool isCursorOnStatusLine);
void ttcore_buffer_insert_lines(ttcore_buffer_t *buf, int Count, int YEnd, int CursorY);
void ttcore_buffer_erase_chars_in_line(ttcore_buffer_t *buf, int XStart, int Count, int CursorX, int CursorY, bool EnableContinuedLineCopy);
void ttcore_buffer_delete_lines(ttcore_buffer_t *buf, int Count, int YEnd, int CursorY);
void ttcore_buffer_delete_chars(ttcore_buffer_t *buf, int Count, int CursorX, int CursorY);
void ttcore_buffer_erase_chars(ttcore_buffer_t *buf, int Count, int CursorX, int CursorY, bool EnableContinuedLineCopy);
void ttcore_buffer_fill_with_e(ttcore_buffer_t *buf);
void ttcore_buffer_draw_line(ttcore_buffer_t *buf, const TCharAttr *Attr, int Direction, int C, int CursorX, int CursorY);
void ttcore_buffer_erase_box(ttcore_buffer_t *buf, int XStart, int YStart, int XEnd, int YEnd);
void ttcore_buffer_fill_box(ttcore_buffer_t *buf, char c, int XStart, int YStart, int XEnd, int YEnd);
void ttcore_buffer_copy_box(ttcore_buffer_t *buf, int SrcXStart, int SrcYStart, int SrcXEnd, int SrcYEnd, int SrcPage, int DstX, int DstY, int DstPage);
void ttcore_buffer_change_attr_box(ttcore_buffer_t *buf, int XStart, int YStart, int XEnd, int YEnd, const TCharAttr *attr, const TCharAttr *mask);
void ttcore_buffer_change_attr_stream(ttcore_buffer_t *buf, int XStart, int YStart, int XEnd, int YEnd, const TCharAttr *attr, const TCharAttr *mask);
void ttcore_buffer_update_rect(ttcore_buffer_t *buf, int XStart, int YStart, int XEnd, int YEnd);
void ttcore_buffer_update_str(ttcore_buffer_t *buf, int CursorY);
void ttcore_buffer_move_cursor(ttcore_buffer_t *buf, int Xnew, int Ynew, int *CursorX, int *CursorY, bool *Wrap);
void ttcore_buffer_move_right(ttcore_buffer_t *buf, int *CursorX, int CursorY);
void ttcore_buffer_scroll_n_lines(ttcore_buffer_t *buf, int n, int CursorY);
void ttcore_buffer_clear_screen(ttcore_buffer_t *buf, int CursorY, bool isCursorOnStatusLine);
void ttcore_buffer_update_scroll(ttcore_buffer_t *buf, int CursorY);

#ifdef __cplusplus
}
#endif

#endif // TTCORE_BUFFER_H
