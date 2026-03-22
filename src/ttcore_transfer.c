// 2026-03-11 v0.3.0 — ttcore_transfer.c: File Transfer Protocols
// XMODEM/YMODEM/ZMODEM port of TeraTerm ttpfile/xmodem.c + ymodem.c + zmodem.c
// Win32-free, callback-based C99 state machine

#include "ttcore_transfer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Control Characters ─────────────────────────────────── */

#define XCTL_SOH  0x01u
#define XCTL_STX  0x02u
#define XCTL_EOT  0x04u
#define XCTL_ACK  0x06u
#define XCTL_BS   0x08u
#define XCTL_NAK  0x15u
#define XCTL_CAN  0x18u

/* ── Timeout Defaults [ms] ──────────────────────────────── */

#define TOUT_INIT_DEFAULT       10000
#define TOUT_INIT_CRC_DEFAULT    3000
#define TOUT_SHORT_DEFAULT       3000
#define TOUT_LONG_DEFAULT       10000
#define TOUT_VLONG_DEFAULT      30000

/* ── Internal Protocol Vtable ───────────────────────────── */

typedef struct TtcoreProtoOp_ {
    bool             (*init)   (TtcoreTransfer *xfer);
    TtcoreXferResult (*parse)  (TtcoreTransfer *xfer);
    void             (*timeout)(TtcoreTransfer *xfer);
    void             (*cancel) (TtcoreTransfer *xfer);
    void             (*destroy)(TtcoreTransfer *xfer);
} TtcoreProtoOp;

/* ── Opaque Handle ──────────────────────────────────────── */

struct TtcoreTransfer {
    TtcoreXferConfig      cfg;
    TtcoreXferCallbacks   cbs;
    const TtcoreProtoOp  *op;
    void                 *proto_state;
    TtcoreXferStatus      status;
    TtcoreXferResult      result;
};

/* ════════════════════════════════════════════════════════════
 *  CRC helpers
 * ════════════════════════════════════════════════════════════ */

static uint16_t update_crc16(uint8_t b, uint16_t crc)
{
    int i;
    crc ^= (uint16_t)((uint16_t)b << 8);
    for (i = 0; i < 8; i++) {
        if (crc & 0x8000u)
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        else
            crc = (uint16_t)(crc << 1);
    }
    return crc;
}

/* ════════════════════════════════════════════════════════════
 *  XMODEM private state
 * ════════════════════════════════════════════════════════════ */

typedef enum { XS_FLUSH = 0, XS_NORMAL, XS_CANCELED } XState;
typedef enum { XPM_SOH = 0, XPM_BLK, XPM_BLK2, XPM_DATA } XPktReadMode;
typedef enum { XNAK_NAK = 0, XNAK_C } XNakMode;

typedef struct {
    uint8_t      pkt_in[1030];
    uint8_t      pkt_out[1030];
    int          pkt_buf_count;
    int          pkt_buf_ptr;
    uint8_t      pkt_num;
    uint8_t      pkt_num_sent;
    int          pkt_num_offset;
    XPktReadMode pkt_read_mode;
    uint16_t     data_len;
    uint8_t      check_len;
    XNakMode     nak_mode;
    int          nak_count;
    bool         cr_recv;
    int          can_count;
    bool         file_open;
    int32_t      file_size;
    int32_t      byte_count;
    XState       state;
    bool         success;
    bool         text_convert_crlf;
    bool         text_trim_1a;
    int          tout_short;
    int          tout_long;
    int          tout_init;
    int          tout_init_crc;
    int          tout_vlong;
} XVar;

/* ── XMODEM I/O helpers ─────────────────────────────────── */

static int x_read_byte(TtcoreTransfer *xfer, uint8_t *b)
{
    return xfer->cbs.read_byte(xfer->cbs.user, b);
}

static int x_write(TtcoreTransfer *xfer, const uint8_t *buf, size_t len)
{
    int rc = xfer->cbs.write_bytes(xfer->cbs.user, buf, len);
    return (rc >= 0) ? 1 : 0;
}

static void x_flush_rx(TtcoreTransfer *xfer)
{
    xfer->cbs.flush_rx(xfer->cbs.user);
}

static void x_set_timeout(TtcoreTransfer *xfer, int ms)
{
    if (xfer->cbs.set_timeout)
        xfer->cbs.set_timeout(xfer->cbs.user, ms);
}

/* ── XMODEM option / check helpers ─────────────────────── */

static void x_set_opt(TtcoreTransfer *xfer, int opt)
{
    XVar *xv = (XVar *)xfer->proto_state;
    xfer->cfg.opt = opt;
    switch (opt) {
    case TTCORE_XMODEM_CHECKSUM:
        xv->data_len = 128;  xv->check_len = 1; break;
    case TTCORE_XMODEM_CRC:
        xv->data_len = 128;  xv->check_len = 2; break;
    case TTCORE_XMODEM_1K_CRC:
        xv->data_len = 1024; xv->check_len = 2; break;
    case TTCORE_XMODEM_1K_CHECKSUM:
        xv->data_len = 1024; xv->check_len = 1; break;
    default:
        xv->data_len = 128;  xv->check_len = 2; break;
    }
}

static uint16_t x_calc_check(const XVar *xv, const uint8_t *pkt)
{
    int i;
    if (xv->check_len == 1) {
        uint16_t sum = 0;
        for (i = 0; i < (int)xv->data_len; i++)
            sum += pkt[3 + i];
        return sum & 0xffu;
    }
    uint16_t crc = 0;
    for (i = 0; i < (int)xv->data_len; i++)
        crc = update_crc16(pkt[3 + i], crc);
    return crc;
}

static bool x_check_packet(const XVar *xv)
{
    uint16_t check = x_calc_check(xv, xv->pkt_in);
    if (xv->check_len == 1)
        return (uint8_t)check == xv->pkt_in[xv->data_len + 3];
    return ((check >> 8)   == xv->pkt_in[xv->data_len + 3]) &&
           ((check & 0xffu) == xv->pkt_in[xv->data_len + 4]);
}

static void x_update_progress(TtcoreTransfer *xfer)
{
    const XVar *xv = (const XVar *)xfer->proto_state;
    xfer->status.bytes_transferred = xv->byte_count;
    xfer->status.file_size = (xfer->cfg.dir == TTCORE_XFER_SEND)
                             ? xv->file_size : -1;
    xfer->status.packet_num = xv->pkt_num_offset + xv->pkt_num;
    if (xfer->cbs.on_progress)
        xfer->cbs.on_progress(xfer->cbs.user, &xfer->status);
}

static void x_cancel_wire(TtcoreTransfer *xfer)
{
    XVar *xv = (XVar *)xfer->proto_state;
    static const uint8_t cancel_seq[] = {
        XCTL_CAN, XCTL_CAN, XCTL_CAN, XCTL_CAN, XCTL_CAN,
        XCTL_BS,  XCTL_BS,  XCTL_BS,  XCTL_BS,  XCTL_BS
    };
    x_write(xfer, cancel_seq, sizeof(cancel_seq));
    xv->state = XS_CANCELED;
}

static void x_send_nak(TtcoreTransfer *xfer)
{
    XVar *xv = (XVar *)xfer->proto_state;
    uint8_t b;
    int     t;

    x_flush_rx(xfer);

    xv->nak_count--;
    if (xv->nak_count < 0) {
        if (xv->nak_mode == XNAK_C) {
            /* fall back from CRC to checksum, give 10 more tries */
            x_set_opt(xfer, TTCORE_XMODEM_CHECKSUM);
            xv->nak_mode  = XNAK_NAK;
            xv->nak_count = 9;
        } else {
            x_cancel_wire(xfer);
            return;
        }
    }

    if (xv->nak_mode == XNAK_NAK) {
        b = XCTL_NAK;
        t = ((xv->pkt_num == 0) && (xv->pkt_num_offset == 0))
            ? xv->tout_init : xv->tout_long;
    } else {
        b = 'C';
        t = xv->tout_init_crc;
    }
    x_write(xfer, &b, 1);
    xv->pkt_read_mode = XPM_SOH;
    x_set_timeout(xfer, t);
}

/* ════════════════════════════════════════════════════════════
 *  XMODEM receive packet state machine
 *  Returns TTCORE_XFER_OK      → continue
 *          TTCORE_XFER_DONE    → EOT received, transfer complete
 *          TTCORE_XFER_CANCELED → CAN sequence or out-of-seq error
 * ════════════════════════════════════════════════════════════ */

static TtcoreXferResult x_read_packet(TtcoreTransfer *xfer)
{
    XVar   *xv = (XVar *)xfer->proto_state;
    uint8_t b, d;
    int     i, c;
    bool    got_pkt = false;

    /* Read one byte at a time at the TOP of the loop so no byte is consumed
     * as a look-ahead past the packet boundary (e.g. EOT after last block). */
    while (!got_pkt && x_read_byte(xfer, &b) > 0) {

        switch (xv->pkt_read_mode) {

        case XPM_SOH:
            switch (b) {
            case XCTL_SOH:
                xv->pkt_in[0] = b;
                xv->pkt_read_mode = XPM_BLK;
                if (xfer->cfg.opt == TTCORE_XMODEM_1K_CRC)
                    x_set_opt(xfer, TTCORE_XMODEM_CRC);
                else if (xfer->cfg.opt == TTCORE_XMODEM_1K_CHECKSUM)
                    x_set_opt(xfer, TTCORE_XMODEM_CHECKSUM);
                x_set_timeout(xfer, xv->tout_short);
                break;
            case XCTL_STX:
                xv->pkt_in[0] = b;
                xv->pkt_read_mode = XPM_BLK;
                if (xfer->cfg.opt == TTCORE_XMODEM_CRC)
                    x_set_opt(xfer, TTCORE_XMODEM_1K_CRC);
                else if (xfer->cfg.opt == TTCORE_XMODEM_CHECKSUM)
                    x_set_opt(xfer, TTCORE_XMODEM_1K_CHECKSUM);
                x_set_timeout(xfer, xv->tout_short);
                break;
            case XCTL_EOT: {
                uint8_t ack = XCTL_ACK;
                xv->success = true;
                x_write(xfer, &ack, 1);
                if (xv->file_open) {
                    xfer->cbs.file_close(xfer->cbs.user);
                    xv->file_open = false;
                }
                return TTCORE_XFER_DONE;
            }
            case XCTL_CAN:
                xv->can_count++;
                if (xv->can_count <= 2)
                    continue;
                return TTCORE_XFER_CANCELED;
            default:
                x_flush_rx(xfer);
                return TTCORE_XFER_OK;
            }
            xv->can_count = 0;
            break;

        case XPM_BLK:
            xv->pkt_in[1] = b;
            xv->pkt_read_mode = XPM_BLK2;
            x_set_timeout(xfer, xv->tout_short);
            break;

        case XPM_BLK2:
            xv->pkt_in[2] = b;
            if ((b ^ xv->pkt_in[1]) == 0xffu) {
                xv->pkt_buf_ptr   = 3;
                xv->pkt_buf_count = (int)xv->data_len + (int)xv->check_len;
                xv->pkt_read_mode = XPM_DATA;
                x_set_timeout(xfer, xv->tout_short);
            } else {
                x_send_nak(xfer);
            }
            break;

        case XPM_DATA:
            xv->pkt_in[xv->pkt_buf_ptr++] = b;
            xv->pkt_buf_count--;
            got_pkt = (xv->pkt_buf_count == 0);
            if (got_pkt)
                xv->pkt_read_mode = XPM_SOH;  /* ready for next packet */
            x_set_timeout(xfer, got_pkt ? xv->tout_long : xv->tout_short);
            break;
        }
    }

    if (!got_pkt)
        return TTCORE_XFER_OK;

    /* Block 0 at start = spurious retransmit; NAK and wait */
    if ((xv->pkt_in[1] == 0) && (xv->pkt_num == 0) && (xv->pkt_num_offset == 0)) {
        xv->nak_count = (xv->nak_mode == XNAK_NAK) ? 10 : 3;
        x_send_nak(xfer);
        return TTCORE_XFER_OK;
    }

    if (!x_check_packet(xv)) {
        x_send_nak(xfer);
        return TTCORE_XFER_OK;
    }

    d = (uint8_t)(xv->pkt_in[1] - xv->pkt_num);
    if (d > 1) {
        x_cancel_wire(xfer);
        return TTCORE_XFER_CANCELED;
    }

    /* ACK */
    b = XCTL_ACK;
    x_write(xfer, &b, 1);
    xv->nak_mode  = XNAK_NAK;
    xv->nak_count = 10;

    if (d == 0)
        return TTCORE_XFER_OK;  /* duplicate — already written, no write again */

    xv->pkt_num = xv->pkt_in[1];
    if (xv->pkt_num == 0)
        xv->pkt_num_offset += 256;

    /* determine actual data length (trim 0x1A padding if text mode) */
    c = (int)xv->data_len;
    if (xv->text_trim_1a) {
        while ((c > 0) && (xv->pkt_in[2 + c] == 0x1au))
            c--;
    }

    /* write to file */
    if (xv->text_convert_crlf) {
        for (i = 0; i < c; i++) {
            b = xv->pkt_in[3 + i];
            if ((b == 0x0au) && (!xv->cr_recv)) {
                /* bare LF → prepend CR */
                uint8_t cr = 0x0du;
                xfer->cbs.file_write(xfer->cbs.user, &cr, 1);
            }
            if (xv->cr_recv && (b != 0x0au)) {
                /* bare CR → append LF */
                uint8_t lf = 0x0au;
                xfer->cbs.file_write(xfer->cbs.user, &lf, 1);
            }
            xv->cr_recv = (b == 0x0du);
            xfer->cbs.file_write(xfer->cbs.user, &b, 1);
        }
    } else {
        xfer->cbs.file_write(xfer->cbs.user, &xv->pkt_in[3], (size_t)c);
    }

    xv->byte_count += c;
    x_set_timeout(xfer, xv->tout_long);
    x_update_progress(xfer);
    return TTCORE_XFER_OK;
}

/* ════════════════════════════════════════════════════════════
 *  XMODEM send packet state machine
 * ════════════════════════════════════════════════════════════ */

static TtcoreXferResult x_send_packet(TtcoreTransfer *xfer)
{
    XVar   *xv = (XVar *)xfer->proto_state;
    uint8_t b;
    int     i;
    bool    send_flag = false;
    bool    got_c     = false;

    if (xv->pkt_buf_count == 0) {
        /* consume ACK / NAK / C / CAN from receiver */
        for (;;) {
            i = x_read_byte(xfer, &b);
            if (i == 0)
                break;

            switch (b) {
            case XCTL_ACK:
                if (!xv->file_open) {
                    xv->success = true;
                    return TTCORE_XFER_DONE;
                } else if (xv->pkt_num_sent == (uint8_t)(xv->pkt_num + 1u)) {
                    xv->pkt_num = xv->pkt_num_sent;
                    if (xv->pkt_num == 0)
                        xv->pkt_num_offset += 256;
                    send_flag = true;
                }
                break;
            case XCTL_NAK:
                if ((xv->pkt_num == 0) && (xv->pkt_num_offset == 0) &&
                    (xv->pkt_num_sent == 0)) {
                    if (!got_c) {
                        /* receiver does not want CRC */
                        if (xfer->cfg.opt == TTCORE_XMODEM_CRC)
                            x_set_opt(xfer, TTCORE_XMODEM_CHECKSUM);
                        else if (xfer->cfg.opt == TTCORE_XMODEM_1K_CRC)
                            x_set_opt(xfer, TTCORE_XMODEM_1K_CHECKSUM);
                    }
                }
                send_flag = true;
                break;
            case XCTL_CAN:
                xv->can_count++;
                if (xv->can_count <= 2)
                    continue;
                return TTCORE_XFER_CANCELED;
            case 'C':
                /* CRC mode requested */
                if ((xv->pkt_num == 0) && (xv->pkt_num_offset == 0) &&
                    (xv->pkt_num_sent == 0)) {
                    if (xfer->cfg.opt == TTCORE_XMODEM_CHECKSUM)
                        x_set_opt(xfer, TTCORE_XMODEM_CRC);
                    else if (xfer->cfg.opt == TTCORE_XMODEM_1K_CHECKSUM)
                        x_set_opt(xfer, TTCORE_XMODEM_1K_CRC);
                    send_flag = true;
                    got_c     = true;
                }
                break;
            default:
                break;
            }
            xv->can_count = 0;
        }

        if (!send_flag)
            return TTCORE_XFER_OK;

        x_set_timeout(xfer, xv->tout_vlong);
        x_flush_rx(xfer);

        if (xv->pkt_num_sent == xv->pkt_num) {
            /* build new packet */
            xv->pkt_num_sent++;
            xv->pkt_out[0] = (xv->data_len == 128u) ? XCTL_SOH : XCTL_STX;
            xv->pkt_out[1] = xv->pkt_num_sent;
            xv->pkt_out[2] = (uint8_t)(~xv->pkt_num_sent);

            i = 1;
            while ((i <= (int)xv->data_len) && xv->file_open) {
                uint8_t fb;
                if (xfer->cbs.file_read(xfer->cbs.user, &fb, 1) != 1)
                    break;
                xv->pkt_out[2 + i] = fb;
                i++;
                xv->byte_count++;
            }

            if (i > 1) {
                /* pad remainder with 0x1A */
                while (i <= (int)xv->data_len) {
                    xv->pkt_out[2 + i] = 0x1au;
                    i++;
                }
                uint16_t check = x_calc_check(xv, xv->pkt_out);
                if (xv->check_len == 1) {
                    xv->pkt_out[xv->data_len + 3] = (uint8_t)check;
                } else {
                    xv->pkt_out[xv->data_len + 3] = (uint8_t)(check >> 8);
                    xv->pkt_out[xv->data_len + 4] = (uint8_t)(check & 0xffu);
                }
                xv->pkt_buf_count = 3 + (int)xv->data_len + (int)xv->check_len;
            } else {
                /* EOF — send EOT */
                if (xv->file_open) {
                    xfer->cbs.file_close(xfer->cbs.user);
                    xv->file_open = false;
                }
                xv->pkt_out[0]    = XCTL_EOT;
                xv->pkt_buf_count = 1;
            }
        } else {
            /* resend last packet */
            xv->pkt_buf_count = (xv->pkt_out[0] == XCTL_EOT)
                                 ? 1
                                 : 3 + (int)xv->data_len + (int)xv->check_len;
        }
        xv->pkt_buf_ptr = 0;
    }

    /* flush any stray NAK/C that arrived during packet building */
    x_flush_rx(xfer);

    /* drain pkt_out into TX */
    i = 1;
    while ((xv->pkt_buf_count > 0) && (i > 0)) {
        b = xv->pkt_out[xv->pkt_buf_ptr];
        i = x_write(xfer, &b, 1);
        if (i > 0) {
            xv->pkt_buf_count--;
            xv->pkt_buf_ptr++;
        }
    }

    if (xv->pkt_buf_count == 0)
        x_update_progress(xfer);

    return TTCORE_XFER_OK;
}

/* ════════════════════════════════════════════════════════════
 *  XMODEM vtable
 * ════════════════════════════════════════════════════════════ */

static bool xmodem_init(TtcoreTransfer *xfer)
{
    XVar                   *xv  = (XVar *)xfer->proto_state;
    const TtcoreXferConfig *cfg = &xfer->cfg;

    xv->tout_init     = cfg->timeout_init_ms     ? cfg->timeout_init_ms     : TOUT_INIT_DEFAULT;
    xv->tout_init_crc = cfg->timeout_init_crc_ms ? cfg->timeout_init_crc_ms : TOUT_INIT_CRC_DEFAULT;
    xv->tout_short    = cfg->timeout_short_ms    ? cfg->timeout_short_ms    : TOUT_SHORT_DEFAULT;
    xv->tout_long     = cfg->timeout_long_ms     ? cfg->timeout_long_ms     : TOUT_LONG_DEFAULT;
    xv->tout_vlong    = cfg->timeout_vlong_ms    ? cfg->timeout_vlong_ms    : TOUT_VLONG_DEFAULT;

    xv->text_convert_crlf = cfg->text_mode;
    xv->text_trim_1a      = cfg->text_mode;

    x_set_opt(xfer, cfg->opt ? cfg->opt : TTCORE_XMODEM_CRC);

    if (cfg->dir == TTCORE_XFER_SEND) {
        long sz = xfer->cbs.file_size(xfer->cbs.user, cfg->filepath);
        if (!xfer->cbs.file_open_read(xfer->cbs.user, cfg->filepath))
            return false;
        xv->file_open = true;
        xv->file_size = (sz >= 0) ? (int32_t)sz : -1;
        x_flush_rx(xfer);
        x_set_timeout(xfer, xv->tout_vlong);
    } else {
        if (!xfer->cbs.file_open_write(xfer->cbs.user, cfg->filepath))
            return false;
        xv->file_open = true;
        xv->file_size = -1;

        if (cfg->opt == TTCORE_XMODEM_CHECKSUM ||
            cfg->opt == TTCORE_XMODEM_1K_CHECKSUM) {
            xv->nak_mode  = XNAK_NAK;
            xv->nak_count = 10;
        } else {
            xv->nak_mode  = XNAK_C;
            xv->nak_count = 3;
        }
        x_send_nak(xfer);  /* send first NAK or 'C' */
    }

    xv->state = XS_NORMAL;
    return true;
}

static TtcoreXferResult xmodem_parse(TtcoreTransfer *xfer)
{
    XVar *xv = (XVar *)xfer->proto_state;
    switch (xv->state) {
    case XS_FLUSH:
        x_flush_rx(xfer);
        xv->state = XS_NORMAL;
        return TTCORE_XFER_OK;
    case XS_NORMAL:
        return (xfer->cfg.dir == TTCORE_XFER_RECV)
               ? x_read_packet(xfer)
               : x_send_packet(xfer);
    case XS_CANCELED:
        x_flush_rx(xfer);
        return TTCORE_XFER_CANCELED;
    }
    return TTCORE_XFER_CANCELED;
}

static void xmodem_timeout(TtcoreTransfer *xfer)
{
    XVar *xv = (XVar *)xfer->proto_state;
    if (xfer->cfg.dir == TTCORE_XFER_RECV)
        x_send_nak(xfer);
    else
        xv->state = XS_CANCELED;
}

static void xmodem_cancel(TtcoreTransfer *xfer)
{
    x_cancel_wire(xfer);
}

static void xmodem_destroy(TtcoreTransfer *xfer)
{
    XVar *xv = (XVar *)xfer->proto_state;
    if (xv && xv->file_open && xfer->cbs.file_close) {
        xfer->cbs.file_close(xfer->cbs.user);
        xv->file_open = false;
    }
    free(xfer->proto_state);
    xfer->proto_state = NULL;
}

static const TtcoreProtoOp kXmodemOps = {
    xmodem_init,
    xmodem_parse,
    xmodem_timeout,
    xmodem_cancel,
    xmodem_destroy,
};

/* ════════════════════════════════════════════════════════════
 *  YMODEM private state
 * ════════════════════════════════════════════════════════════ */

typedef enum {
    YS_WAIT_C = 0,     /* SEND: waiting for 'C'/'G' from receiver      */
    YS_SEND_BLK0,      /* SEND: have sent block0, waiting for ACK       */
    YS_SEND_DATA,      /* SEND: sending data blocks                     */
    YS_SEND_EOT1,      /* SEND: sent first EOT, waiting for NAK         */
    YS_SEND_EOT2,      /* SEND: sent second EOT, waiting for ACK        */
    YS_SEND_BATCH,     /* SEND: waiting for 'C'/'G' after file done     */
    YS_SEND_NULL_BLK0, /* SEND: sent null block0, waiting for ACK       */

    YS_RECV_WAIT_BLK0, /* RECV: waiting for block0                      */
    YS_RECV_DATA,      /* RECV: receiving data blocks                   */
    YS_RECV_EOT1,      /* RECV: got first EOT, sent NAK                 */
    YS_RECV_BATCH,     /* RECV: sent ACK+C after file done, next file   */

    YS_CANCELED,
} YState;

typedef enum { YPM_SOH = 0, YPM_BLK, YPM_BLK2, YPM_DATA } YPktReadMode;

typedef struct {
    /* packet assembly (shared RECV+SEND) */
    uint8_t      pkt_in[1030];
    uint8_t      pkt_out[1030];
    int          pkt_buf_count;
    int          pkt_buf_ptr;
    YPktReadMode pkt_read_mode;
    uint16_t     cur_data_len;  /* actual len of current pkt (128 or 1024) */

    /* block numbers */
    uint8_t      pkt_num;       /* last accepted block number */
    uint8_t      pkt_num_sent;  /* last sent block number */
    int          pkt_num_offset;

    /* recv state */
    long          recv_filesize; /* -1 = unknown */
    char          recv_filename[512];
    unsigned long recv_mtime;

    /* send state */
    bool         file_open;
    long         file_size;
    int32_t      byte_count;
    bool         send_eot;      /* have started EOT sequence */
    bool         last_send_eot; /* true when sending null block0 */
    bool         ymodem_g;      /* YMODEM-G stream mode */

    /* retry */
    int          nak_count;
    int          can_count;

    /* timeout values */
    int          tout_short;
    int          tout_long;
    int          tout_init;
    int          tout_vlong;

    YState       state;
    bool         success;

    /* current filepath being sent/received */
    char         filepath[512];
} YVar;

/* ── YMODEM I/O helpers (reuse x_ helpers via xfer->cbs) ── */

static int y_read_byte(TtcoreTransfer *xfer, uint8_t *b)
{
    return xfer->cbs.read_byte(xfer->cbs.user, b);
}

static int y_write(TtcoreTransfer *xfer, const uint8_t *buf, size_t len)
{
    int rc = xfer->cbs.write_bytes(xfer->cbs.user, buf, len);
    return (rc >= 0) ? 1 : 0;
}

static void y_flush_rx(TtcoreTransfer *xfer)
{
    xfer->cbs.flush_rx(xfer->cbs.user);
}

static void y_set_timeout(TtcoreTransfer *xfer, int ms)
{
    if (xfer->cbs.set_timeout)
        xfer->cbs.set_timeout(xfer->cbs.user, ms);
}

static void y_cancel_wire(TtcoreTransfer *xfer)
{
    YVar *yv = (YVar *)xfer->proto_state;
    static const uint8_t cancel_seq[] = {
        XCTL_CAN, XCTL_CAN, XCTL_CAN, XCTL_CAN, XCTL_CAN,
        XCTL_BS,  XCTL_BS,  XCTL_BS,  XCTL_BS,  XCTL_BS
    };
    y_write(xfer, cancel_seq, sizeof(cancel_seq));
    yv->state = YS_CANCELED;
}

static void y_update_progress(TtcoreTransfer *xfer)
{
    const YVar *yv = (const YVar *)xfer->proto_state;
    xfer->status.bytes_transferred = yv->byte_count;
    xfer->status.file_size         = yv->recv_filesize > 0
                                     ? yv->recv_filesize
                                     : (yv->file_size > 0 ? yv->file_size : -1);
    xfer->status.packet_num        = yv->pkt_num_offset + yv->pkt_num;
    if (xfer->cbs.on_progress)
        xfer->cbs.on_progress(xfer->cbs.user, &xfer->status);
}

/* ── YMODEM: send one handshake byte ('C' or 'G') ───────── */
static void y_send_handshake(TtcoreTransfer *xfer)
{
    YVar  *yv = (YVar *)xfer->proto_state;
    uint8_t b = yv->ymodem_g ? 'G' : 'C';
    y_flush_rx(xfer);
    y_write(xfer, &b, 1);
    yv->pkt_read_mode = YPM_SOH;
    y_set_timeout(xfer, yv->tout_init);
}

/* ── YMODEM RECV: parse one incoming packet ──────────────── */
static TtcoreXferResult y_recv_read_packet(TtcoreTransfer *xfer)
{
    YVar   *yv = (YVar *)xfer->proto_state;
    uint8_t b;
    bool    got_pkt = false;

    while (!got_pkt && y_read_byte(xfer, &b) > 0) {
        switch (yv->pkt_read_mode) {

        case YPM_SOH:
            switch (b) {
            case XCTL_SOH:
                yv->pkt_in[0]    = b;
                yv->cur_data_len = 128;
                yv->pkt_read_mode = YPM_BLK;
                y_set_timeout(xfer, yv->tout_short);
                break;
            case XCTL_STX:
                yv->pkt_in[0]    = b;
                yv->cur_data_len = 1024;
                yv->pkt_read_mode = YPM_BLK;
                y_set_timeout(xfer, yv->tout_short);
                break;
            case XCTL_EOT:
                if (yv->state == YS_RECV_DATA) {
                    /* two-phase EOT: send NAK, wait for second EOT */
                    uint8_t nak = XCTL_NAK;
                    y_write(xfer, &nak, 1);
                    yv->state = YS_RECV_EOT1;
                    y_set_timeout(xfer, yv->tout_short);
                } else if (yv->state == YS_RECV_EOT1) {
                    /* second EOT: ACK, close file, send 'C' for batch */
                    uint8_t ack = XCTL_ACK;
                    y_write(xfer, &ack, 1);
                    if (yv->file_open && xfer->cbs.file_close) {
                        xfer->cbs.file_close(xfer->cbs.user);
                        yv->file_open = false;
                    }
                    /* send handshake for next file */
                    y_send_handshake(xfer);
                    yv->pkt_num        = 0;
                    yv->pkt_num_offset = 0;
                    yv->byte_count     = 0;
                    yv->state = YS_RECV_BATCH;
                }
                return TTCORE_XFER_OK;
            case XCTL_CAN:
                yv->can_count++;
                if (yv->can_count <= 2)
                    continue;
                return TTCORE_XFER_CANCELED;
            default:
                y_flush_rx(xfer);
                return TTCORE_XFER_OK;
            }
            yv->can_count = 0;
            break;

        case YPM_BLK:
            yv->pkt_in[1]     = b;
            yv->pkt_read_mode = YPM_BLK2;
            y_set_timeout(xfer, yv->tout_short);
            break;

        case YPM_BLK2:
            yv->pkt_in[2] = b;
            if ((b ^ yv->pkt_in[1]) == 0xffu) {
                yv->pkt_buf_ptr   = 3;
                yv->pkt_buf_count = (int)yv->cur_data_len + 2; /* CRC */
                yv->pkt_read_mode = YPM_DATA;
                y_set_timeout(xfer, yv->tout_short);
            } else {
                /* bad complement — send NAK */
                if (!yv->ymodem_g) {
                    uint8_t nak = XCTL_NAK;
                    y_write(xfer, &nak, 1);
                } else {
                    return TTCORE_XFER_CANCELED;
                }
            }
            break;

        case YPM_DATA:
            yv->pkt_in[yv->pkt_buf_ptr++] = b;
            yv->pkt_buf_count--;
            got_pkt = (yv->pkt_buf_count == 0);
            if (got_pkt)
                yv->pkt_read_mode = YPM_SOH;
            y_set_timeout(xfer, got_pkt ? yv->tout_long : yv->tout_short);
            break;
        }
    }

    if (!got_pkt)
        return TTCORE_XFER_OK;

    /* Verify CRC */
    {
        uint16_t crc = 0;
        int      i;
        for (i = 0; i < (int)yv->cur_data_len; i++)
            crc = update_crc16(yv->pkt_in[3 + i], crc);
        uint16_t recv_crc = ((uint16_t)yv->pkt_in[3 + yv->cur_data_len] << 8) |
                             yv->pkt_in[3 + yv->cur_data_len + 1];
        if (crc != recv_crc) {
            if (!yv->ymodem_g) {
                uint8_t nak = XCTL_NAK;
                y_write(xfer, &nak, 1);
                return TTCORE_XFER_OK;
            }
            return TTCORE_XFER_CANCELED;
        }
    }

    uint8_t blk = yv->pkt_in[1];

    /* ── Block 0 (file info) ─────────────────────────────── */
    if (blk == 0) {
        /* null block0 → end of batch */
        if (yv->pkt_in[3] == 0) {
            uint8_t ack = XCTL_ACK;
            y_write(xfer, &ack, 1);
            yv->success = true;
            return TTCORE_XFER_DONE;
        }

        /* parse filename + size from data */
        const char *name = (const char *)&yv->pkt_in[3];
        size_t      nlen = strnlen(name, (size_t)yv->cur_data_len);
        if (nlen < (size_t)yv->cur_data_len) {
            const char *sizestr = name + nlen + 1;
            long         fsz   = -1;
            unsigned long mtime = 0;
            unsigned int  mode  = 0;
            sscanf(sizestr, "%ld%lo%o", &fsz, &mtime, &mode);
            yv->recv_filesize = fsz;
            yv->recv_mtime    = (unsigned long)mtime;
        } else {
            yv->recv_filesize = -1;
            yv->recv_mtime    = 0;
        }

        /* copy filename to status + yv */
        strncpy(yv->recv_filename, name, sizeof(yv->recv_filename) - 1);
        yv->recv_filename[sizeof(yv->recv_filename) - 1] = '\0';
        strncpy(xfer->status.filename, yv->recv_filename,
                sizeof(xfer->status.filename) - 1);
        xfer->status.file_size = yv->recv_filesize;

        /* determine destination path */
        char dest[512];
        if (xfer->cbs.file_recv_path &&
            xfer->cbs.file_recv_path(xfer->cbs.user, yv->recv_filename,
                                     dest, sizeof(dest))) {
            strncpy(yv->filepath, dest, sizeof(yv->filepath) - 1);
        } else {
            strncpy(yv->filepath, xfer->cfg.filepath,
                    sizeof(yv->filepath) - 1);
        }
        yv->filepath[sizeof(yv->filepath) - 1] = '\0';

        /* open output file */
        if (!xfer->cbs.file_open_write(xfer->cbs.user, yv->filepath))
            return TTCORE_XFER_ERR_IO;
        yv->file_open = true;

        /* ACK block0, send handshake for data phase */
        uint8_t ack = XCTL_ACK;
        y_write(xfer, &ack, 1);
        y_send_handshake(xfer);
        yv->state = YS_RECV_DATA;
        return TTCORE_XFER_OK;
    }

    /* ── Data block ──────────────────────────────────────── */
    uint8_t d = (uint8_t)(blk - yv->pkt_num);
    if (d > 1) {
        if (!yv->ymodem_g)
            y_cancel_wire(xfer);
        return TTCORE_XFER_CANCELED;
    }

    /* ACK (YMODEM-G: no ACK for data blocks) */
    if (!yv->ymodem_g) {
        uint8_t ack = XCTL_ACK;
        y_write(xfer, &ack, 1);
    }

    if (d == 0)
        return TTCORE_XFER_OK; /* duplicate */

    yv->pkt_num = blk;
    if (yv->pkt_num == 0)
        yv->pkt_num_offset += 256;

    /* trim to filesize if known */
    int write_len = (int)yv->cur_data_len;
    if (yv->recv_filesize > 0) {
        long remaining = yv->recv_filesize - yv->byte_count;
        if (remaining < write_len)
            write_len = (int)remaining;
    }
    if (write_len > 0) {
        xfer->cbs.file_write(xfer->cbs.user, &yv->pkt_in[3], (size_t)write_len);
        yv->byte_count += write_len;
    }

    y_set_timeout(xfer, yv->tout_long);
    y_update_progress(xfer);
    return TTCORE_XFER_OK;
}

/* ── YMODEM SEND ─────────────────────────────────────────── */

/* Build YMODEM block0 into yv->pkt_out; returns total packet size */
static int y_build_block0(TtcoreTransfer *xfer)
{
    YVar   *yv = (YVar *)xfer->proto_state;
    uint8_t data[128];
    memset(data, 0, sizeof(data));

    /* Only send filename for non-null block */
    if (!yv->last_send_eot) {
        /* extract basename from filepath */
        const char *name = yv->filepath;
        const char *p    = name;
        while (*p) {
            if (*p == '/' || *p == '\\') name = p + 1;
            p++;
        }
        size_t nlen = strlen(name);
        if (nlen >= 125) nlen = 125;
        memcpy(data, name, nlen);
        if (yv->file_size >= 0) {
            snprintf((char *)data + nlen + 1,
                     sizeof(data) - nlen - 1,
                     "%ld", yv->file_size);
        }
    }
    /* null block0 if last_send_eot: all zeros already */

    /* SOH, blk=0, ~blk=0xFF, 128 data bytes, CRC16 */
    yv->pkt_out[0] = XCTL_SOH;
    yv->pkt_out[1] = 0x00;
    yv->pkt_out[2] = 0xFF;
    memcpy(&yv->pkt_out[3], data, 128);
    uint16_t crc = 0;
    int i;
    for (i = 0; i < 128; i++)
        crc = update_crc16(data[i], crc);
    yv->pkt_out[131] = (uint8_t)(crc >> 8);
    yv->pkt_out[132] = (uint8_t)(crc & 0xffu);
    return 133;
}

/* Build YMODEM data block (1K) into yv->pkt_out */
static int y_build_data_block(TtcoreTransfer *xfer)
{
    YVar   *yv = (YVar *)xfer->proto_state;
    int     i  = 1;
    uint8_t fb;

    yv->pkt_num_sent++;
    yv->pkt_out[0] = XCTL_STX;
    yv->pkt_out[1] = yv->pkt_num_sent;
    yv->pkt_out[2] = (uint8_t)(~yv->pkt_num_sent);

    while ((i <= 1024) && yv->file_open) {
        if (xfer->cbs.file_read(xfer->cbs.user, &fb, 1) != 1)
            break;
        yv->pkt_out[2 + i] = fb;
        i++;
        yv->byte_count++;
    }

    if (i == 1) {
        /* EOF: no data was read */
        return 0;
    }
    /* pad with 0x1A */
    while (i <= 1024) {
        yv->pkt_out[2 + i] = 0x1au;
        i++;
    }
    uint16_t crc = 0;
    int j;
    for (j = 0; j < 1024; j++)
        crc = update_crc16(yv->pkt_out[3 + j], crc);
    yv->pkt_out[1027] = (uint8_t)(crc >> 8);
    yv->pkt_out[1028] = (uint8_t)(crc & 0xffu);
    return 1029;
}

static TtcoreXferResult y_send_parse(TtcoreTransfer *xfer)
{
    YVar   *yv = (YVar *)xfer->proto_state;
    uint8_t b;
    int     i;

    /* Drain available control bytes */
    for (;;) {
        i = y_read_byte(xfer, &b);
        if (i == 0) break;

        switch (b) {
        case XCTL_CAN:
            yv->can_count++;
            if (yv->can_count > 2)
                return TTCORE_XFER_CANCELED;
            continue;
        default:
            yv->can_count = 0;
            break;
        }

        switch (yv->state) {

        case YS_WAIT_C:
            if (b == 'C' || b == 'G') {
                if (b == 'G') yv->ymodem_g = true;
                y_flush_rx(xfer);
                /* build and send block0 */
                int plen = y_build_block0(xfer);
                yv->pkt_buf_count = plen;
                yv->pkt_buf_ptr   = 0;
                /* drain pkt_out */
                while (yv->pkt_buf_count > 0) {
                    uint8_t bb = yv->pkt_out[yv->pkt_buf_ptr];
                    if (y_write(xfer, &bb, 1) > 0) {
                        yv->pkt_buf_count--;
                        yv->pkt_buf_ptr++;
                    } else {
                        break;
                    }
                }
                y_set_timeout(xfer, yv->tout_long);
                yv->state = YS_SEND_BLK0;
            }
            break;

        case YS_SEND_BLK0:
            if (b == XCTL_ACK) {
                /* wait for 'C' or 'G' to start data phase */
                y_set_timeout(xfer, yv->tout_long);
            } else if (b == 'C' || b == 'G') {
                /* start data phase */
                y_flush_rx(xfer);
                yv->state = YS_SEND_DATA;

                /* build and send first data block */
                int plen = y_build_data_block(xfer);
                if (plen == 0) {
                    /* empty file: go to EOT */
                    if (yv->file_open) {
                        xfer->cbs.file_close(xfer->cbs.user);
                        yv->file_open = false;
                    }
                    uint8_t eot = XCTL_EOT;
                    y_write(xfer, &eot, 1);
                    y_set_timeout(xfer, yv->tout_long);
                    yv->state = YS_SEND_EOT1;
                } else {
                    yv->pkt_buf_count = plen;
                    yv->pkt_buf_ptr   = 0;
                    while (yv->pkt_buf_count > 0) {
                        uint8_t bb = yv->pkt_out[yv->pkt_buf_ptr];
                        if (y_write(xfer, &bb, 1) > 0) {
                            yv->pkt_buf_count--;
                            yv->pkt_buf_ptr++;
                        } else break;
                    }
                    y_set_timeout(xfer, yv->tout_long);
                }
            } else if (b == XCTL_NAK) {
                /* NAK on block0: resend */
                y_flush_rx(xfer);
                int plen = y_build_block0(xfer);
                yv->pkt_buf_count = plen;
                yv->pkt_buf_ptr   = 0;
                while (yv->pkt_buf_count > 0) {
                    uint8_t bb = yv->pkt_out[yv->pkt_buf_ptr];
                    if (y_write(xfer, &bb, 1) > 0) {
                        yv->pkt_buf_count--;
                        yv->pkt_buf_ptr++;
                    } else break;
                }
                y_set_timeout(xfer, yv->tout_long);
            }
            break;

        case YS_SEND_DATA:
            if (b == XCTL_ACK) {
                /* advance: send next block or EOT */
                yv->pkt_num = yv->pkt_num_sent;
                if (yv->pkt_num == 0) yv->pkt_num_offset += 256;
                y_update_progress(xfer);

                y_flush_rx(xfer);
                int plen = y_build_data_block(xfer);
                if (plen == 0) {
                    /* EOF */
                    if (yv->file_open) {
                        xfer->cbs.file_close(xfer->cbs.user);
                        yv->file_open = false;
                    }
                    uint8_t eot = XCTL_EOT;
                    y_write(xfer, &eot, 1);
                    y_set_timeout(xfer, yv->tout_long);
                    yv->state = YS_SEND_EOT1;
                } else {
                    yv->pkt_buf_count = plen;
                    yv->pkt_buf_ptr   = 0;
                    while (yv->pkt_buf_count > 0) {
                        uint8_t bb = yv->pkt_out[yv->pkt_buf_ptr];
                        if (y_write(xfer, &bb, 1) > 0) {
                            yv->pkt_buf_count--;
                            yv->pkt_buf_ptr++;
                        } else break;
                    }
                    y_set_timeout(xfer, yv->tout_long);
                }
            } else if (b == XCTL_NAK) {
                if (yv->ymodem_g)
                    return TTCORE_XFER_CANCELED;
                /* resend last packet */
                y_flush_rx(xfer);
                yv->pkt_buf_count = 1029; /* 1K+overhead */
                yv->pkt_buf_ptr   = 0;
                while (yv->pkt_buf_count > 0) {
                    uint8_t bb = yv->pkt_out[yv->pkt_buf_ptr];
                    if (y_write(xfer, &bb, 1) > 0) {
                        yv->pkt_buf_count--;
                        yv->pkt_buf_ptr++;
                    } else break;
                }
                y_set_timeout(xfer, yv->tout_long);
            }
            break;

        case YS_SEND_EOT1:
            /* two-phase EOT: expect NAK, then resend EOT */
            if (b == XCTL_NAK) {
                uint8_t eot = XCTL_EOT;
                y_write(xfer, &eot, 1);
                y_set_timeout(xfer, yv->tout_long);
                yv->state = YS_SEND_EOT2;
            } else if (b == XCTL_ACK) {
                /* some receivers skip two-phase */
                yv->state = YS_SEND_BATCH;
            }
            break;

        case YS_SEND_EOT2:
            if (b == XCTL_ACK) {
                y_set_timeout(xfer, yv->tout_long);
                yv->state = YS_SEND_BATCH;
            }
            break;

        case YS_SEND_BATCH:
            if (b == 'C' || b == 'G') {
                /* check for next file */
                char next_path[512];
                bool have_next = false;
                if (xfer->cbs.file_next_send &&
                    xfer->cbs.file_next_send(xfer->cbs.user,
                                             next_path, sizeof(next_path))) {
                    strncpy(yv->filepath, next_path, sizeof(yv->filepath) - 1);
                    yv->filepath[sizeof(yv->filepath) - 1] = '\0';
                    /* open new file */
                    long sz = xfer->cbs.file_size(xfer->cbs.user, yv->filepath);
                    if (xfer->cbs.file_open_read(xfer->cbs.user, yv->filepath)) {
                        yv->file_open      = true;
                        yv->file_size      = sz;
                        yv->byte_count     = 0;
                        yv->pkt_num        = 0;
                        yv->pkt_num_sent   = 0;
                        yv->pkt_num_offset = 0;
                        have_next = true;
                        yv->last_send_eot  = false;
                    }
                }

                if (!have_next) {
                    yv->last_send_eot = true;
                }

                y_flush_rx(xfer);
                int plen = y_build_block0(xfer);
                yv->pkt_buf_count = plen;
                yv->pkt_buf_ptr   = 0;
                while (yv->pkt_buf_count > 0) {
                    uint8_t bb = yv->pkt_out[yv->pkt_buf_ptr];
                    if (y_write(xfer, &bb, 1) > 0) {
                        yv->pkt_buf_count--;
                        yv->pkt_buf_ptr++;
                    } else break;
                }
                y_set_timeout(xfer, yv->tout_long);
                yv->state = yv->last_send_eot ? YS_SEND_NULL_BLK0 : YS_SEND_BLK0;
            }
            break;

        case YS_SEND_NULL_BLK0:
            if (b == XCTL_ACK) {
                yv->success = true;
                return TTCORE_XFER_DONE;
            }
            break;

        default:
            break;
        }
    }
    return TTCORE_XFER_OK;
}

/* ════════════════════════════════════════════════════════════
 *  YMODEM vtable
 * ════════════════════════════════════════════════════════════ */

static bool ymodem_init(TtcoreTransfer *xfer)
{
    YVar                   *yv  = (YVar *)xfer->proto_state;
    const TtcoreXferConfig *cfg = &xfer->cfg;

    yv->tout_init  = cfg->timeout_init_ms  ? cfg->timeout_init_ms  : TOUT_INIT_DEFAULT;
    yv->tout_short = cfg->timeout_short_ms ? cfg->timeout_short_ms : TOUT_SHORT_DEFAULT;
    yv->tout_long  = cfg->timeout_long_ms  ? cfg->timeout_long_ms  : TOUT_LONG_DEFAULT;
    yv->tout_vlong = cfg->timeout_vlong_ms ? cfg->timeout_vlong_ms : TOUT_VLONG_DEFAULT;

    yv->ymodem_g = (cfg->opt == TTCORE_YMODEM_G);

    strncpy(yv->filepath, cfg->filepath, sizeof(yv->filepath) - 1);
    yv->filepath[sizeof(yv->filepath) - 1] = '\0';

    yv->recv_filesize = -1;

    if (cfg->dir == TTCORE_XFER_RECV) {
        /* send initial handshake ('C' or 'G') */
        y_send_handshake(xfer);
        yv->state = YS_RECV_WAIT_BLK0;
    } else {
        /* send mode: wait for receiver's 'C'/'G' */
        long sz = xfer->cbs.file_size(xfer->cbs.user, cfg->filepath);
        if (!xfer->cbs.file_open_read(xfer->cbs.user, cfg->filepath))
            return false;
        yv->file_open = true;
        yv->file_size = sz;
        y_set_timeout(xfer, yv->tout_vlong);
        yv->state = YS_WAIT_C;
    }
    return true;
}

static TtcoreXferResult ymodem_parse(TtcoreTransfer *xfer)
{
    YVar *yv = (YVar *)xfer->proto_state;
    if (yv->state == YS_CANCELED)
        return TTCORE_XFER_CANCELED;

    if (xfer->cfg.dir == TTCORE_XFER_RECV)
        return y_recv_read_packet(xfer);
    return y_send_parse(xfer);
}

static void ymodem_timeout(TtcoreTransfer *xfer)
{
    YVar *yv = (YVar *)xfer->proto_state;
    if (xfer->cfg.dir == TTCORE_XFER_RECV) {
        y_send_handshake(xfer);
    } else {
        yv->state = YS_CANCELED;
    }
}

static void ymodem_cancel(TtcoreTransfer *xfer)
{
    y_cancel_wire(xfer);
}

static void ymodem_destroy(TtcoreTransfer *xfer)
{
    YVar *yv = (YVar *)xfer->proto_state;
    if (yv && yv->file_open && xfer->cbs.file_close) {
        xfer->cbs.file_close(xfer->cbs.user);
        yv->file_open = false;
    }
    free(xfer->proto_state);
    xfer->proto_state = NULL;
}

static const TtcoreProtoOp kYmodemOps = {
    ymodem_init,
    ymodem_parse,
    ymodem_timeout,
    ymodem_cancel,
    ymodem_destroy,
};

/* ════════════════════════════════════════════════════════════
 *  ZMODEM — private state & implementation
 * ════════════════════════════════════════════════════════════ */

/* ── Control constants ──────────────────────────────────── */

#define ZPAD   0x2Au   /* '*'  */
#define ZDLE   0x18u   /* CAN  */
#define ZDLEE  0x58u   /* ZDLE^0x40 */
#define ZBIN   0x41u   /* 'A' binary header, CRC-16  */
#define ZHEX   0x42u   /* 'B' hex header,    CRC-16  */
#define ZBIN32 0x43u   /* 'C' binary header, CRC-32  */

/* Header type values */
#define ZRQINIT    0u
#define ZRINIT     1u
#define ZSINIT     2u
#define ZACK       3u
#define ZFILE      4u
#define ZSKIP      5u
#define ZNAK       6u
#define ZABORT     7u
#define ZFIN       8u
#define ZRPOS      9u
#define ZDATA     10u
#define ZEOF      11u
#define ZFERR     12u
#define ZCRC      13u
#define ZCHALLENGE 14u
#define ZCOMPL    15u
#define ZCAN_HDR  16u
#define ZFREECNT  17u
#define ZCOMMAND  18u
#define ZSTDERR   19u

/* Data subpacket terminators (after ZDLE) */
#define ZCRCE  0x68u   /* 'h' end-of-frame,   no response */
#define ZCRCG  0x69u   /* 'i' intermediate,   no response */
#define ZCRCQ  0x6Au   /* 'j' intermediate,   send ZACK   */
#define ZCRCW  0x6Bu   /* 'k' end-of-frame,   send ZACK   */
#define ZRUB0  0x6Cu   /* 'l' escape for 0x7F */
#define ZRUB1  0x6Du   /* 'm' escape for 0xFF */

/* ZRINIT capability flags (ZF0 byte) */
#define ZCANFDX  0x01u
#define ZCANOVIO 0x02u
#define ZCANFC32 0x20u
#define ZESCCTL  0x40u

/* Header position macros */
#define ZF0  3
#define ZF1  2
#define ZF2  1
#define ZF3  0
#define ZP0  0
#define ZP1  1
#define ZP2  2
#define ZP3  3

/* Default timeouts [ms] */
#define ZTOUT_INIT   10000
#define ZTOUT_FIN     3000
#define ZTOUT_DATA   10000

/* Max data subpacket size */
#define ZMAX_DATA  1024

/* ── State enums ────────────────────────────────────────── */

typedef enum {
    ZS_RECV_INIT     = 1,  /* sent ZRINIT, waiting for ZFILE  */
    ZS_RECV_INIT2    = 2,  /* got ZSINIT, sent ZACK           */
    ZS_RECV_DATA     = 3,  /* receiving ZDATA subpackets      */
    ZS_RECV_FIN      = 4,  /* sent ZFIN, waiting for "OO"     */
    ZS_SEND_INIT     = 5,  /* (send side — future)            */
    ZS_SEND_INIT_HDR = 6,
    ZS_SEND_INIT_DAT = 7,
    ZS_SEND_FILE_HDR = 8,
    ZS_SEND_FILE_DAT = 9,
    ZS_SEND_DATA_HDR = 10,
    ZS_SEND_DATA_DAT = 11,
    ZS_SEND_DATA_DAT2= 12,
    ZS_SEND_EOF      = 13,
    ZS_SEND_FIN      = 14,
    ZS_CANCEL        = 15,
    ZS_END           = 16,
} ZState_E;

typedef enum {
    ZPS_GET_PAD    = 1,  /* waiting for '*' (0x2A)           */
    ZPS_GET_DLE    = 2,  /* got PAD, waiting for ZDLE        */
    ZPS_HDR_FRM    = 3,  /* got ZDLE, reading frame type     */
    ZPS_GET_BIN    = 4,  /* reading binary header bytes      */
    ZPS_GET_HEX    = 5,  /* reading hex-encoded header bytes */
    ZPS_GET_HEX_EOL= 6,  /* consuming header EOL             */
    ZPS_GET_DATA   = 7,  /* reading data subpacket bytes     */
    ZPS_GET_CRC    = 8,  /* reading CRC bytes after ZCRC*    */
} ZStatePkt_E;

/* Internal packet-parse result */
typedef enum {
    ZPKT_NEED_MORE = 0,
    ZPKT_HDR_OK,
    ZPKT_DATA_OK,
    ZPKT_CRC_ERR,
    ZPKT_CANCEL,
} ZPktResult;

/* ── TZVar ──────────────────────────────────────────────── */

typedef struct {
    /* Packet I/O buffers */
    uint8_t  pkt_in[1032];
    uint8_t  pkt_out[1032];
    int      pkt_in_ptr;
    int      pkt_out_ptr;
    int      pkt_in_count;
    int      pkt_out_count;

    /* Header bytes */
    uint8_t  rx_hdr[4];
    uint8_t  tx_hdr[4];
    uint8_t  rx_type;
    uint8_t  term;

    /* State machines */
    ZState_E    z_state;
    ZStatePkt_E z_pkt_state;

    /* Flags */
    bool     sending;
    bool     bin_flag;
    bool     crc32;
    bool     ctl_esc;
    bool     hex_lo;
    bool     quoted;
    bool     cr_recv;
    bool     file_open;

    /* CRC state */
    uint16_t crc16;
    uint32_t crc32_val;

    /* Transfer bookkeeping */
    int32_t  pos;
    int32_t  last_pos;
    int32_t  file_size;
    int32_t  win_size;
    uint32_t file_mtime;
    int32_t  byte_count;

    /* Config */
    int      max_data_len;
    int      timeout_ms;
    int      tout_init;
    int      tout_fin;
    int      can_count;
    uint8_t  last_sent;
    int      z_mode;   /* 0=recv, 1=send */

    /* Timing */
    uint64_t start_ms;
    int      prog_stat;

    /* Retry counter for ZRPOS */
    int      rpos_retry;

    /* Callbacks (pointer into parent xfer) */
    const TtcoreXferCallbacks *cbs;
} TZVar;

#define ZM_RECV 0
#define ZM_SEND 1

/* ── CRC-32 (IEEE 802.3 / ISO-HDLC, poly 0xEDB88320) ────── */
/* Inline loop — no table, no truncation bugs. */
static uint32_t crc32_update(uint32_t crc, uint8_t b)
{
    int i;
    crc ^= b;
    for (i = 0; i < 8; i++)
        crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    return crc;
}

/* ── monotonic millisecond timestamp ────────────────────── */

#include <time.h>
static uint64_t z_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* ── I/O helpers ────────────────────────────────────────── */

static int z_write(TtcoreTransfer *xfer, const uint8_t *buf, size_t len)
{
    int rc = xfer->cbs.write_bytes(xfer->cbs.user, buf, len);
    return (rc >= 0) ? 1 : 0;
}

static void z_set_timeout(TtcoreTransfer *xfer, int ms)
{
    if (xfer->cbs.set_timeout)
        xfer->cbs.set_timeout(xfer->cbs.user, ms);
}

/* Read one byte; discard XON(0x11), XOFF(0x13), 0x81, 0x83.
 * Returns 1=got byte, 0=empty. */
static int z_read1byte(TtcoreTransfer *xfer, uint8_t *b)
{
    for (;;) {
        if (!xfer->cbs.read_byte(xfer->cbs.user, b))
            return 0;
        uint8_t v = *b & 0x7Fu;
        if (v == 0x11u || v == 0x13u)
            continue;
        return 1;
    }
}

/* ── ZDLE escape decoder ─────────────────────────────────── */

static uint8_t z_get_escaped(uint8_t b)
{
    if (b == ZRUB0) return 0x7Fu;
    if (b == ZRUB1) return 0xFFu;
    return b ^ 0x40u;
}

/* ── Escape decision ─────────────────────────────────────── */

static bool z_need_escape(const TZVar *zv, uint8_t b)
{
    switch (b) {
    case 0x0Du: case 0x8Du:   /* CR / CR|0x80  */
    case 0x0Au: case 0x8Au:   /* LF / LF|0x80  */
    case 0x10u: case 0x90u:   /* DLE            */
    case 0x11u: case 0x91u:   /* XON            */
    case 0x13u: case 0x93u:   /* XOFF           */
    case 0x1Du: case 0x9Du:   /* GS             */
    case ZDLE:                 /* 0x18           */
        return true;
    default:
        if (zv->ctl_esc && ((b & 0x60u) == 0u))
            return true;
        return false;
    }
}

/* Append one byte to pkt_out with ZDLE-escaping. */
static void z_put_bin(TZVar *zv, uint8_t b)
{
    if (z_need_escape(zv, b)) {
        zv->pkt_out[zv->pkt_out_count++] = ZDLE;
        b ^= 0x40u;
    }
    zv->last_sent = b;
    zv->pkt_out[zv->pkt_out_count++] = b;
}

/* ── Hex output ──────────────────────────────────────────── */

static void z_put_hex(TZVar *zv, uint8_t b)
{
    static const char kHex[] = "0123456789abcdef";
    zv->pkt_out[zv->pkt_out_count++] = (uint8_t)kHex[b >> 4];
    zv->pkt_out[zv->pkt_out_count++] = (uint8_t)kHex[b & 0x0Fu];
}

/* ── Store / recall 32-bit position in header bytes ─────── */

static void z_sto_hdr(TZVar *zv, int32_t pos)
{
    uint32_t p = (uint32_t)pos;
    zv->tx_hdr[ZP0] = (uint8_t)(p        & 0xFFu);
    zv->tx_hdr[ZP1] = (uint8_t)((p >>  8) & 0xFFu);
    zv->tx_hdr[ZP2] = (uint8_t)((p >> 16) & 0xFFu);
    zv->tx_hdr[ZP3] = (uint8_t)((p >> 24) & 0xFFu);
}

static int32_t z_rcl_hdr(const TZVar *zv)
{
    uint32_t v = (uint32_t)zv->rx_hdr[ZP3];
    v = (v << 8) | (uint32_t)zv->rx_hdr[ZP2];
    v = (v << 8) | (uint32_t)zv->rx_hdr[ZP1];
    v = (v << 8) | (uint32_t)zv->rx_hdr[ZP0];
    return (int32_t)v;
}

/* ── ZHEX header sender ──────────────────────────────────── */

/* Sends: ** ZDLE 'B' type_hex hdr[0..3]_hex crc16_hex CR LF|0x80 XON */
static void z_send_zhex_hdr(TtcoreTransfer *xfer, TZVar *zv, uint8_t type)
{
    int i;
    zv->pkt_out_count = 0;
    zv->pkt_out[zv->pkt_out_count++] = ZPAD;
    zv->pkt_out[zv->pkt_out_count++] = ZPAD;
    zv->pkt_out[zv->pkt_out_count++] = ZDLE;
    zv->pkt_out[zv->pkt_out_count++] = ZHEX;
    z_put_hex(zv, type);
    uint16_t crc = update_crc16(type, 0);
    for (i = 0; i < 4; i++) {
        z_put_hex(zv, zv->tx_hdr[i]);
        crc = update_crc16(zv->tx_hdr[i], crc);
    }
    z_put_hex(zv, (uint8_t)(crc >> 8));
    z_put_hex(zv, (uint8_t)(crc & 0xFFu));
    zv->pkt_out[zv->pkt_out_count++] = 0x0Du; /* CR          */
    zv->pkt_out[zv->pkt_out_count++] = 0x8Au; /* LF | 0x80   */
    if (type != ZFIN && type != ZACK)
        zv->pkt_out[zv->pkt_out_count++] = 0x11u; /* XON     */
    z_write(xfer, zv->pkt_out, (size_t)zv->pkt_out_count);
}

/* ── ZBIN32 header sender (CRC-32) ──────────────────────── */

/* Sends: ZPAD ZDLE 'C' type(escaped) hdr[0..3](escaped) crc32[0..3](escaped)
 * CRC-32 covers [type, hdr[0..3]] (5 bytes), finalized (xorout), stored LE. */
static void z_send_zbin32_hdr(TtcoreTransfer *xfer, TZVar *zv, uint8_t type)
{
    int i;
    zv->pkt_out_count = 0;
    zv->pkt_out[zv->pkt_out_count++] = ZPAD;
    zv->pkt_out[zv->pkt_out_count++] = ZDLE;
    zv->pkt_out[zv->pkt_out_count++] = ZBIN32;
    uint32_t crc = 0xFFFFFFFFu;
    z_put_bin(zv, type);
    crc = crc32_update(crc, type);
    for (i = 0; i < 4; i++) {
        z_put_bin(zv, zv->tx_hdr[i]);
        crc = crc32_update(crc, zv->tx_hdr[i]);
    }
    crc ^= 0xFFFFFFFFu;   /* finalize */
    z_put_bin(zv, (uint8_t)(crc        & 0xFFu));
    z_put_bin(zv, (uint8_t)((crc >>  8) & 0xFFu));
    z_put_bin(zv, (uint8_t)((crc >> 16) & 0xFFu));
    z_put_bin(zv, (uint8_t)((crc >> 24) & 0xFFu));
    z_write(xfer, zv->pkt_out, (size_t)zv->pkt_out_count);
}

/* ── High-level send helpers ─────────────────────────────── */

static void z_send_rinit(TtcoreTransfer *xfer, TZVar *zv)
{
    memset(zv->tx_hdr, 0, 4);
    zv->tx_hdr[ZF0] = ZCANFDX | ZCANOVIO | ZCANFC32;
    if (zv->ctl_esc)
        zv->tx_hdr[ZF0] |= ZESCCTL;
    z_send_zhex_hdr(xfer, zv, ZRINIT);
    z_set_timeout(xfer, zv->tout_init);
}

static void z_send_rpos(TtcoreTransfer *xfer, TZVar *zv)
{
    z_sto_hdr(zv, zv->pos);
    z_send_zhex_hdr(xfer, zv, ZRPOS);
    z_set_timeout(xfer, zv->timeout_ms);
}

static void z_send_ack(TtcoreTransfer *xfer, TZVar *zv)
{
    z_sto_hdr(zv, zv->pos);
    z_send_zhex_hdr(xfer, zv, ZACK);
    z_set_timeout(xfer, zv->timeout_ms);
}

static void z_send_nak(TtcoreTransfer *xfer, TZVar *zv)
{
    memset(zv->tx_hdr, 0, 4);
    z_send_zhex_hdr(xfer, zv, ZNAK);
}

static void z_send_fin(TtcoreTransfer *xfer, TZVar *zv)
{
    memset(zv->tx_hdr, 0, 4);
    z_send_zhex_hdr(xfer, zv, ZFIN);
}

/* Cancel: 8× ZDLE + 8× BS */
static void z_send_cancel(TtcoreTransfer *xfer)
{
    int i;
    uint8_t buf[16];
    for (i = 0; i < 8; i++) buf[i]     = ZDLE;
    for (i = 0; i < 8; i++) buf[8 + i] = 0x08u;
    z_write(xfer, buf, 16);
}

/* ── CRC verification for received header ───────────────── */

static bool z_check_hdr(TZVar *zv)
{
    int i;
    if (zv->crc32) {
        uint32_t c = 0xFFFFFFFFu;
        for (i = 0; i < 9; i++)
            c = crc32_update(c, zv->pkt_in[i]);
        return (c == 0xDEBB20E3u);
    }
    uint16_t c = 0;
    for (i = 0; i < 7; i++)
        c = update_crc16(zv->pkt_in[i], c);
    return (c == 0);
}

/* Extract rx_type + rx_hdr from pkt_in after successful header parse. */
static void z_extract_hdr(TZVar *zv)
{
    int i;
    zv->rx_type = zv->pkt_in[0];
    for (i = 0; i < 4; i++)
        zv->rx_hdr[i] = zv->pkt_in[i + 1];
}

/* ── Packet parser (inner state machine) ─────────────────── */

/* Feed one byte into the packet parser.
 * Returns ZPKT_NEED_MORE until a complete header or data unit is ready. */
static ZPktResult z_parse_pkt_byte(TZVar *zv, uint8_t b)
{
    switch (zv->z_pkt_state) {

    /* ── find ZPAD ────────────────────────────────────────── */
    case ZPS_GET_PAD:
        if (b == ZPAD)
            zv->z_pkt_state = ZPS_GET_DLE;
        return ZPKT_NEED_MORE;

    /* ── find second ZPAD or ZDLE ────────────────────────── */
    case ZPS_GET_DLE:
        if (b == ZPAD)
            return ZPKT_NEED_MORE;     /* another '*', keep waiting */
        if (b == ZDLE) {
            zv->z_pkt_state = ZPS_HDR_FRM;
            return ZPKT_NEED_MORE;
        }
        zv->z_pkt_state = ZPS_GET_PAD;
        return ZPKT_NEED_MORE;

    /* ── read frame type ─────────────────────────────────── */
    case ZPS_HDR_FRM:
        zv->quoted    = false;
        zv->pkt_in_ptr = 0;
        switch (b) {
        case ZBIN:
            zv->crc32      = false;
            zv->pkt_in_count = 7;   /* type(1)+hdr(4)+crc16(2) */
            zv->z_pkt_state  = ZPS_GET_BIN;
            break;
        case ZHEX:
            zv->crc32      = false;
            zv->hex_lo     = false;
            zv->pkt_in_count = 7;
            zv->z_pkt_state  = ZPS_GET_HEX;
            break;
        case ZBIN32:
            zv->crc32      = true;
            zv->pkt_in_count = 9;   /* type(1)+hdr(4)+crc32(4) */
            zv->z_pkt_state  = ZPS_GET_BIN;
            break;
        default:
            zv->z_pkt_state = ZPS_GET_PAD;
            break;
        }
        return ZPKT_NEED_MORE;

    /* ── binary header ───────────────────────────────────── */
    case ZPS_GET_BIN:
        if (b == ZDLE) {
            zv->quoted = true;
            return ZPKT_NEED_MORE;
        }
        if (zv->quoted) {
            b = z_get_escaped(b);
            zv->quoted = false;
        }
        zv->pkt_in[zv->pkt_in_ptr++] = b;
        zv->pkt_in_count--;
        if (zv->pkt_in_count == 0) {
            zv->z_pkt_state = ZPS_GET_PAD;
            if (!z_check_hdr(zv))
                return ZPKT_CRC_ERR;
            z_extract_hdr(zv);
            return ZPKT_HDR_OK;
        }
        return ZPKT_NEED_MORE;

    /* ── hex header ──────────────────────────────────────── */
    case ZPS_GET_HEX: {
        uint8_t nib;
        if (b >= '0' && b <= '9')      nib = b - '0';
        else if (b >= 'a' && b <= 'f') nib = (uint8_t)(b - 'a' + 10);
        else {
            zv->z_pkt_state = ZPS_GET_PAD;
            return ZPKT_NEED_MORE;  /* invalid hex char — resync */
        }
        if (!zv->hex_lo) {
            zv->pkt_in[zv->pkt_in_ptr] = (uint8_t)(nib << 4);
            zv->hex_lo = true;
        } else {
            zv->pkt_in[zv->pkt_in_ptr] |= nib;
            zv->hex_lo = false;
            zv->pkt_in_ptr++;
            zv->pkt_in_count--;
            if (zv->pkt_in_count == 0) {
                zv->z_pkt_state  = ZPS_GET_HEX_EOL;
                zv->pkt_in_count = 2;  /* CR + LF|0x80 */
            }
        }
        return ZPKT_NEED_MORE;
    }

    /* ── hex header EOL ──────────────────────────────────── */
    case ZPS_GET_HEX_EOL:
        zv->pkt_in_count--;
        if (zv->pkt_in_count <= 0) {
            zv->z_pkt_state = ZPS_GET_PAD;
            if (!z_check_hdr(zv))
                return ZPKT_CRC_ERR;
            z_extract_hdr(zv);
            return ZPKT_HDR_OK;
        }
        return ZPKT_NEED_MORE;

    /* ── data subpacket ──────────────────────────────────── */
    case ZPS_GET_DATA:
        if (b == ZDLE) {
            zv->quoted = true;
            return ZPKT_NEED_MORE;
        }
        if (zv->quoted) {
            switch (b) {
            case ZCRCE: case ZCRCG: case ZCRCQ: case ZCRCW:
                zv->term = b;
                /* include terminator in CRC */
                if (zv->crc32)
                    zv->crc32_val = crc32_update(zv->crc32_val, b);
                else
                    zv->crc16 = update_crc16(b, zv->crc16);
                zv->pkt_in_count = zv->crc32 ? 4 : 2;
                zv->z_pkt_state  = ZPS_GET_CRC;
                zv->quoted = false;
                return ZPKT_NEED_MORE;
            default:
                b = z_get_escaped(b);
                break;
            }
            zv->quoted = false;
        }
        if (zv->crc32)
            zv->crc32_val = crc32_update(zv->crc32_val, b);
        else
            zv->crc16 = update_crc16(b, zv->crc16);
        if (zv->pkt_in_ptr < ZMAX_DATA) {
            zv->pkt_in[zv->pkt_in_ptr++] = b;
        } else {
            /* overflow — drop frame */
            zv->z_pkt_state = ZPS_GET_PAD;
        }
        return ZPKT_NEED_MORE;

    /* ── CRC bytes ───────────────────────────────────────── */
    case ZPS_GET_CRC:
        if (b == ZDLE) {
            zv->quoted = true;
            return ZPKT_NEED_MORE;
        }
        if (zv->quoted) {
            b = z_get_escaped(b);
            zv->quoted = false;
        }
        if (zv->crc32)
            zv->crc32_val = crc32_update(zv->crc32_val, b);
        else
            zv->crc16 = update_crc16(b, zv->crc16);
        zv->pkt_in_count--;
        if (zv->pkt_in_count > 0)
            return ZPKT_NEED_MORE;
        /* CRC complete — verify */
        zv->z_pkt_state = ZPS_GET_PAD;
        if (zv->crc32) {
            if (zv->crc32_val != 0xDEBB20E3u)
                return ZPKT_CRC_ERR;
        } else {
            if (zv->crc16 != 0)
                return ZPKT_CRC_ERR;
        }
        return ZPKT_DATA_OK;

    default:
        zv->z_pkt_state = ZPS_GET_PAD;
        return ZPKT_NEED_MORE;
    }
}

/* ── progress update ─────────────────────────────────────── */

static void z_update_progress(TtcoreTransfer *xfer, TZVar *zv)
{
    xfer->status.bytes_transferred = zv->byte_count;
    xfer->status.file_size         = zv->file_size > 0 ? zv->file_size : -1;
    xfer->status.packet_num        = 0;
    if (xfer->cbs.on_progress)
        xfer->cbs.on_progress(xfer->cbs.user, &xfer->status);
}

/* ── Prepare data-subpacket state ────────────────────────── */

static void z_begin_data_pkt(TZVar *zv)
{
    zv->quoted        = false;
    zv->pkt_in_ptr    = 0;
    zv->pkt_in_count  = 0;
    zv->crc16         = 0;
    zv->crc32_val     = 0xFFFFFFFFu;
    zv->z_pkt_state   = ZPS_GET_DATA;
}

/* ── Write data subpacket to file (binary or text) ────────── */

static void z_write_data(TtcoreTransfer *xfer, TZVar *zv)
{
    int i;
    if (zv->bin_flag) {
        xfer->cbs.file_write(xfer->cbs.user, zv->pkt_in,
                              (size_t)zv->pkt_in_ptr);
    } else {
        for (i = 0; i < zv->pkt_in_ptr; i++) {
            uint8_t b = zv->pkt_in[i];
            if (b == 0x0Au && !zv->cr_recv) {
                uint8_t cr = 0x0Du;
                xfer->cbs.file_write(xfer->cbs.user, &cr, 1);
            }
            if (zv->cr_recv && b != 0x0Au) {
                uint8_t lf = 0x0Au;
                xfer->cbs.file_write(xfer->cbs.user, &lf, 1);
            }
            zv->cr_recv = (b == 0x0Du);
            xfer->cbs.file_write(xfer->cbs.user, &b, 1);
        }
    }
    zv->byte_count += zv->pkt_in_ptr;
    zv->pos        += zv->pkt_in_ptr;
}

/* ── Parse ZFILE data packet ─────────────────────────────── */

static bool z_parse_zfile(TtcoreTransfer *xfer, TZVar *zv)
{
    char dest[512];
    const char *name;
    size_t nlen;
    long fsz;
    unsigned long mtime;
    unsigned int  mode;

    /* pkt_in contains: filename\0size_octal_mode\0 */
    zv->pkt_in[zv->pkt_in_ptr] = 0;
    name = (const char *)zv->pkt_in;
    nlen = strnlen(name, (size_t)zv->pkt_in_ptr);

    /* copy filename to status */
    strncpy(xfer->status.filename, name, sizeof(xfer->status.filename) - 1);
    xfer->status.filename[sizeof(xfer->status.filename) - 1] = '\0';

    /* parse size + mtime */
    fsz   = -1;
    mtime = 0;
    mode  = 0;
    if (nlen < (size_t)zv->pkt_in_ptr) {
        const char *sizestr = name + nlen + 1;
        sscanf(sizestr, "%ld%lo%o", &fsz, &mtime, &mode);
    }
    zv->file_size   = (fsz > 0) ? (int32_t)fsz : -1;
    zv->file_mtime  = (uint32_t)mtime;
    xfer->status.file_size = zv->file_size;

    /* determine output path */
    dest[0] = '\0';
    if (xfer->cbs.file_recv_path &&
        xfer->cbs.file_recv_path(xfer->cbs.user, name, dest, sizeof(dest))) {
        /* use provided path */
    } else {
        strncpy(dest, xfer->cfg.filepath, sizeof(dest) - 1);
        dest[sizeof(dest) - 1] = '\0';
    }

    /* open output file */
    if (!xfer->cbs.file_open_write(xfer->cbs.user, dest))
        return false;
    zv->file_open  = true;
    zv->pos        = 0;
    zv->byte_count = 0;
    zv->cr_recv    = false;
    return true;
}

/* ── Outer ZMODEM receive state machine ──────────────────── */

/* ════════════════════════════════════════════════════════════
 *  ZMODEM send helpers
 * ════════════════════════════════════════════════════════════ */

/* Send ZRQINIT (ZHEX), go to ZS_SEND_INIT. */
static void z_send_zrqinit(TtcoreTransfer *xfer, TZVar *zv)
{
    memset(zv->tx_hdr, 0, 4);
    z_send_zhex_hdr(xfer, zv, ZRQINIT);
    zv->z_state = ZS_SEND_INIT;
    z_set_timeout(xfer, zv->tout_init);
}

/* Send ZFILE header (ZBIN32) for the current file, go to ZS_SEND_FILE_HDR.
 * tx_hdr[ZF0] must be set before calling (ZCBIN or ZCNL). */
static void z_send_zfile_hdr(TtcoreTransfer *xfer, TZVar *zv)
{
    z_sto_hdr(zv, 0);
    zv->tx_hdr[ZF0] = 0x01u;  /* ZCBIN — binary */
    z_send_zbin32_hdr(xfer, zv, ZFILE);
    zv->z_state = ZS_SEND_FILE_HDR;
}

/* Build and send ZFILE data subpacket (filename\0size\0), CRC32, ZCRCW.
 * Transitions to ZS_SEND_FILE_DAT. */
static void z_send_zfile_dat(TtcoreTransfer *xfer, TZVar *zv)
{
    int i, n;
    long fsz;
    char szstr[64];
    uint32_t crc;

    /* build payload: filename\0size\0 */
    n = 0;
    const char *fname = xfer->cfg.filepath;
    /* use basename only */
    const char *slash = fname;
    for (i = 0; fname[i]; i++)
        if (fname[i] == '/') slash = fname + i + 1;
    for (i = 0; slash[i]; i++)
        zv->pkt_out[n++] = (uint8_t)slash[i];
    zv->pkt_out[n++] = 0;  /* NUL after name */

    fsz = xfer->cbs.file_size(xfer->cbs.user, xfer->cfg.filepath);
    zv->file_size = (int32_t)(fsz > 0 ? fsz : 0);
    xfer->status.file_size = zv->file_size;

    long mtime = 0;
    if (xfer->cbs.file_get_mtime)
        mtime = xfer->cbs.file_get_mtime(xfer->cbs.user, xfer->cfg.filepath);
    snprintf(szstr, sizeof(szstr), "%ld %lo", (long)zv->file_size, mtime);
    for (i = 0; szstr[i]; i++)
        zv->pkt_out[n++] = (uint8_t)szstr[i];
    zv->pkt_out[n++] = 0;  /* NUL after size+mtime */

    /* Now build the escaped ZDLE+CRC32 subpacket output */
    uint8_t out[1300];
    int on = 0;

    crc = 0xFFFFFFFFu;
    for (i = 0; i < n; i++) {
        crc = crc32_update(crc, zv->pkt_out[i]);
        if (z_need_escape(zv, zv->pkt_out[i])) {
            out[on++] = ZDLE;
            out[on++] = zv->pkt_out[i] ^ 0x40u;
        } else {
            out[on++] = zv->pkt_out[i];
        }
    }
    /* ZDLE ZCRCW terminator */
    crc = crc32_update(crc, ZCRCW);
    out[on++] = ZDLE;
    out[on++] = ZCRCW;
    crc ^= 0xFFFFFFFFu;  /* finalize */

    /* 4 CRC32 bytes LE (escaped) */
    uint8_t crc_b[4] = {
        (uint8_t)(crc        & 0xFFu),
        (uint8_t)((crc >>  8) & 0xFFu),
        (uint8_t)((crc >> 16) & 0xFFu),
        (uint8_t)((crc >> 24) & 0xFFu)
    };
    for (i = 0; i < 4; i++) {
        if (z_need_escape(zv, crc_b[i])) {
            out[on++] = ZDLE;
            out[on++] = crc_b[i] ^ 0x40u;
        } else {
            out[on++] = crc_b[i];
        }
    }

    z_write(xfer, out, (size_t)on);
    zv->z_state = ZS_SEND_FILE_DAT;
    z_set_timeout(xfer, zv->timeout_ms);
}

/* Send ZDATA header (ZBIN32) at current pos, go to ZS_SEND_DATA_HDR. */
static void z_send_zdata_hdr(TtcoreTransfer *xfer, TZVar *zv)
{
    z_sto_hdr(zv, zv->pos);
    z_send_zbin32_hdr(xfer, zv, ZDATA);
    zv->z_state = ZS_SEND_DATA_HDR;
}

/* Read one data subpacket from file, escape it, compute CRC32, send.
 * Terminator: ZCRCE at EOF, ZCRCQ at window boundary, ZCRCG otherwise.
 * State after: ZS_SEND_DATA_DAT (ZCRCG/ZCRCE) or ZS_SEND_DATA_DAT2 (ZCRCQ). */
static void z_send_data_dat(TtcoreTransfer *xfer, TZVar *zv)
{
    int i, c;
    uint8_t b;
    uint8_t out[2300];
    int on = 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t term;

    /* EOF check */
    if (zv->pos >= zv->file_size) {
        /* send ZEOF */
        z_sto_hdr(zv, zv->pos);
        z_send_zhex_hdr(xfer, zv, ZEOF);
        zv->z_state = ZS_SEND_EOF;
        z_set_timeout(xfer, zv->timeout_ms);
        return;
    }

    /* read up to max_data_len bytes from file */
    int nread = 0;
    uint8_t fbuf[1024];
    c = (int)xfer->cbs.file_read(xfer->cbs.user, fbuf,
                                  (size_t)zv->max_data_len);
    nread = c;

    /* build escaped output + CRC */
    for (i = 0; i < nread; i++) {
        b = fbuf[i];
        crc = crc32_update(crc, b);
        if (z_need_escape(zv, b)) {
            out[on++] = ZDLE;
            out[on++] = b ^ 0x40u;
        } else {
            out[on++] = b;
        }
    }

    zv->pos += nread;
    zv->byte_count = zv->pos;

    /* choose terminator */
    if (zv->pos >= zv->file_size) {
        term = ZCRCE;
    } else if (zv->win_size >= 0 &&
               (zv->pos - zv->last_pos) > zv->win_size) {
        term = ZCRCQ;
    } else {
        term = ZCRCG;
    }

    crc = crc32_update(crc, term);
    out[on++] = ZDLE;
    out[on++] = term;
    crc ^= 0xFFFFFFFFu;  /* finalize */

    uint8_t crc_b[4] = {
        (uint8_t)(crc        & 0xFFu),
        (uint8_t)((crc >>  8) & 0xFFu),
        (uint8_t)((crc >> 16) & 0xFFu),
        (uint8_t)((crc >> 24) & 0xFFu)
    };
    for (i = 0; i < 4; i++) {
        if (z_need_escape(zv, crc_b[i])) {
            out[on++] = ZDLE;
            out[on++] = crc_b[i] ^ 0x40u;
        } else {
            out[on++] = crc_b[i];
        }
    }

    z_write(xfer, out, (size_t)on);
    z_update_progress(xfer, zv);

    if (term == ZCRCQ) {
        zv->z_state = ZS_SEND_DATA_DAT2;  /* wait for ZACK */
        z_set_timeout(xfer, zv->timeout_ms);
    } else {
        zv->z_state = ZS_SEND_DATA_DAT;
    }
}

/* ── Outer ZMODEM send state machine ──────────────────────── */

static TtcoreXferResult z_send_parse(TtcoreTransfer *xfer)
{
    TZVar *zv = (TZVar *)xfer->proto_state;
    uint8_t b;

    if (zv->z_state == ZS_END)
        return TTCORE_XFER_DONE;
    if (zv->z_state == ZS_CANCEL)
        return TTCORE_XFER_CANCELED;

    /* Read all available input */
    while (z_read1byte(xfer, &b) > 0) {

        /* CAN-sequence detector */
        if (b == ZDLE) {
            zv->can_count--;
            if (zv->can_count <= 0) {
                z_send_cancel(xfer);
                zv->z_state = ZS_CANCEL;
                return TTCORE_XFER_CANCELED;
            }
        } else {
            zv->can_count = 5;
        }

        ZPktResult pr = z_parse_pkt_byte(zv, b);

        switch (pr) {
        case ZPKT_NEED_MORE:
            break;

        case ZPKT_CANCEL:
            z_send_cancel(xfer);
            zv->z_state = ZS_CANCEL;
            return TTCORE_XFER_CANCELED;

        case ZPKT_CRC_ERR:
            /* ignore bad headers on send side */
            break;

        case ZPKT_HDR_OK:
            switch (zv->rx_type) {

            case ZRINIT:
                if (zv->z_state == ZS_SEND_INIT) {
                    /* First ZRINIT: open file and start sending */
                    if (zv->file_open) {
                        xfer->cbs.file_close(xfer->cbs.user);
                        zv->file_open = false;
                    }
                    zv->ctl_esc = ((zv->rx_hdr[ZF0] & ZESCCTL) != 0);
                    if (!xfer->cbs.file_open_read(xfer->cbs.user,
                                                   xfer->cfg.filepath)) {
                        z_send_cancel(xfer);
                        zv->z_state = ZS_CANCEL;
                        return TTCORE_XFER_CANCELED;
                    }
                    zv->file_open  = true;
                    zv->pos        = 0;
                    zv->last_pos   = 0;
                    zv->byte_count = 0;
                    strncpy(xfer->status.filename, xfer->cfg.filepath,
                            sizeof(xfer->status.filename) - 1);
                    z_send_zfile_hdr(xfer, zv);
                } else if (zv->z_state == ZS_SEND_EOF) {
                    /* Receiver acknowledged ZEOF — no more files: send ZFIN */
                    if (zv->file_open) {
                        xfer->cbs.file_close(xfer->cbs.user);
                        zv->file_open = false;
                    }
                    memset(zv->tx_hdr, 0, 4);
                    z_send_zhex_hdr(xfer, zv, ZFIN);
                    zv->z_state = ZS_SEND_FIN;
                    z_set_timeout(xfer, zv->tout_fin);
                }
                break;

            case ZRPOS:
                /* Receiver requesting position (resume or error) */
                switch (zv->z_state) {
                case ZS_SEND_FILE_DAT:
                case ZS_SEND_DATA_HDR:
                case ZS_SEND_DATA_DAT:
                case ZS_SEND_DATA_DAT2:
                case ZS_SEND_EOF: {
                    int32_t rpos = z_rcl_hdr(zv);
                    if (rpos != zv->pos && xfer->cbs.file_seek)
                        xfer->cbs.file_seek(xfer->cbs.user, rpos);
                    zv->pos      = rpos;
                    zv->last_pos = rpos;
                    z_send_zdata_hdr(xfer, zv);
                    break;
                }
                default:
                    break;
                }
                break;

            case ZACK:
                /* ACK after ZCRCQ checkpoint */
                if (zv->z_state == ZS_SEND_DATA_DAT2) {
                    int32_t ack_pos = z_rcl_hdr(zv);
                    zv->last_pos = ack_pos;
                    if (zv->pos == ack_pos) {
                        zv->z_state = ZS_SEND_DATA_DAT;
                    } else {
                        /* position mismatch — seek and resend */
                        if (xfer->cbs.file_seek)
                            xfer->cbs.file_seek(xfer->cbs.user, ack_pos);
                        zv->pos = ack_pos;
                        z_send_zdata_hdr(xfer, zv);
                    }
                }
                break;

            case ZFIN:
                /* Receiver acknowledges ZFIN — send "OO" and finish */
                if (zv->z_state == ZS_SEND_FIN) {
                    static const uint8_t oo[] = {'O', 'O'};
                    z_write(xfer, oo, 2);
                    zv->z_state = ZS_END;
                    return TTCORE_XFER_DONE;
                }
                break;

            case ZNAK:
                /* NAK — resend current header */
                switch (zv->z_state) {
                case ZS_SEND_FILE_HDR:
                    z_send_zfile_hdr(xfer, zv);
                    break;
                case ZS_SEND_FILE_DAT:
                    z_send_zfile_hdr(xfer, zv);
                    break;
                default:
                    break;
                }
                break;

            case ZABORT:
            case ZFERR:
                /* receiver aborted — send ZFIN */
                memset(zv->tx_hdr, 0, 4);
                z_send_zhex_hdr(xfer, zv, ZFIN);
                zv->z_state = ZS_SEND_FIN;
                break;

            default:
                break;
            }
            break;  /* ZPKT_HDR_OK */

        case ZPKT_DATA_OK:
            /* Send side doesn't receive data subpackets */
            zv->z_pkt_state = ZPS_GET_PAD;
            break;
        }
    }

    /* After consuming all input: if in a state that requires
     * sending without waiting for response, do it now. */
    switch (zv->z_state) {
    case ZS_SEND_FILE_HDR:
        z_send_zfile_dat(xfer, zv);
        break;
    case ZS_SEND_DATA_HDR:
    case ZS_SEND_DATA_DAT:
        z_send_data_dat(xfer, zv);
        break;
    default:
        break;
    }

    return TTCORE_XFER_OK;
}

static TtcoreXferResult z_recv_parse(TtcoreTransfer *xfer)
{
    TZVar *zv = (TZVar *)xfer->proto_state;
    uint8_t b;

    /* Check for end state */
    if (zv->z_state == ZS_END)
        return TTCORE_XFER_DONE;
    if (zv->z_state == ZS_CANCEL)
        return TTCORE_XFER_CANCELED;

    while (z_read1byte(xfer, &b) > 0) {

        /* RecvFIN: wait for "OO" from sender — handle BEFORE CAN detector
         * so that non-ZDLE bytes don't reset can_count */
        if (zv->z_state == ZS_RECV_FIN) {
            if (b == 'O') {
                zv->can_count--;
                if (zv->can_count <= 0) {
                    zv->z_state = ZS_END;
                    return TTCORE_XFER_DONE;
                }
            }
            continue;
        }

        /* CAN-sequence detector: 8× ZDLE → cancel */
        if (b == ZDLE) {
            zv->can_count--;
            if (zv->can_count <= 0) {
                z_send_cancel(xfer);
                zv->z_state = ZS_CANCEL;
                return TTCORE_XFER_CANCELED;
            }
        } else {
            zv->can_count = 5;
        }

        ZPktResult pr = z_parse_pkt_byte(zv, b);

        switch (pr) {
        case ZPKT_NEED_MORE:
            break;

        case ZPKT_CANCEL:
            z_send_cancel(xfer);
            zv->z_state = ZS_CANCEL;
            return TTCORE_XFER_CANCELED;

        case ZPKT_CRC_ERR:
            /* CRC error on header */
            switch (zv->z_state) {
            case ZS_RECV_INIT:
            case ZS_RECV_INIT2:
                z_send_rinit(xfer, zv);
                break;
            case ZS_RECV_DATA:
                z_send_rpos(xfer, zv);
                break;
            default:
                break;
            }
            break;

        case ZPKT_HDR_OK:
            /* Dispatch on header type */
            switch (zv->rx_type) {

            case ZRQINIT:
                if (zv->z_state == ZS_RECV_INIT)
                    z_send_rinit(xfer, zv);
                break;

            case ZSINIT:
                /* sender wants to send SINIT data — ACK, absorb data */
                zv->ctl_esc |= ((zv->rx_hdr[ZF0] & ZESCCTL) != 0);
                z_send_ack(xfer, zv);
                zv->z_state = ZS_RECV_INIT2;
                z_begin_data_pkt(zv);
                break;

            case ZFILE:
                if (zv->z_state != ZS_RECV_INIT &&
                    zv->z_state != ZS_RECV_INIT2)
                    break;
                zv->bin_flag = (zv->rx_hdr[ZF0] != 0x02u); /* ZCNL==2 */
                z_set_timeout(xfer, zv->tout_init);
                z_begin_data_pkt(zv);
                break;

            case ZDATA:
                if (zv->z_state != ZS_RECV_DATA)
                    break;
                if (z_rcl_hdr(zv) != zv->pos) {
                    z_send_rpos(xfer, zv);
                    break;
                }
                /* Resume: seek file to current position (ZRPOS recovery) */
                if (zv->pos > 0 && xfer->cbs.file_seek)
                    xfer->cbs.file_seek(xfer->cbs.user, zv->pos);
                z_set_timeout(xfer, zv->timeout_ms);
                z_begin_data_pkt(zv);
                break;

            case ZEOF: {
                int32_t eof_pos = z_rcl_hdr(zv);
                if (zv->z_state != ZS_RECV_DATA)
                    break;
                if (eof_pos != zv->pos) {
                    z_send_rpos(xfer, zv);
                    break;
                }
                /* flush pending CR in text mode */
                if (!zv->bin_flag && zv->cr_recv) {
                    uint8_t lf = 0x0Au;
                    xfer->cbs.file_write(xfer->cbs.user, &lf, 1);
                    zv->cr_recv = false;
                }
                if (zv->file_open) {
                    xfer->cbs.file_close(xfer->cbs.user);
                    zv->file_open = false;
                    if (zv->file_mtime && xfer->cbs.file_set_mtime)
                        xfer->cbs.file_set_mtime(xfer->cbs.user,
                                                  xfer->cfg.filepath,
                                                  (long)zv->file_mtime);
                }
                /* ready for next file */
                zv->z_state = ZS_RECV_INIT;
                z_send_rinit(xfer, zv);
                break;
            }

            case ZFIN:
                /* transfer complete */
                zv->can_count = 2;  /* expect "OO" = 2 chars */
                zv->z_state   = ZS_RECV_FIN;
                z_send_fin(xfer, zv);
                z_set_timeout(xfer, zv->tout_fin);
                break;

            case ZABORT:
            case ZFERR:
                z_send_cancel(xfer);
                zv->z_state = ZS_CANCEL;
                return TTCORE_XFER_CANCELED;

            default:
                break;
            }
            break;  /* ZPKT_HDR_OK */

        case ZPKT_DATA_OK:
            /* Data subpacket complete and CRC OK */
            switch (zv->rx_type) {

            case ZSINIT:
                /* absorb SINIT data — already ACK'd, back to RECV_INIT */
                zv->z_state = ZS_RECV_INIT;
                z_send_rinit(xfer, zv);
                break;

            case ZFILE: {
                bool ok = z_parse_zfile(xfer, zv);
                if (!ok) {
                    /* can't open file */
                    memset(zv->tx_hdr, 0, 4);
                    z_send_zhex_hdr(xfer, zv, ZSKIP);
                    zv->z_state = ZS_RECV_INIT;
                    break;
                }
                /* send ZRPOS(0) to start data flow; stay in ZPS_GET_PAD
                 * so the ZDATA header is parsed as a header, not as data */
                zv->z_state = ZS_RECV_DATA;
                z_sto_hdr(xfer->proto_state, 0);
                z_send_zhex_hdr(xfer, zv, ZRPOS);
                z_set_timeout(xfer, zv->timeout_ms);
                break;
            }

            case ZDATA:
                z_write_data(xfer, zv);
                z_update_progress(xfer, zv);
                /* dispatch on terminator */
                switch (zv->term) {
                case ZCRCG:
                    /* no ACK, continue streaming */
                    z_begin_data_pkt(zv);
                    z_set_timeout(xfer, zv->timeout_ms);
                    break;
                case ZCRCQ:
                    z_send_ack(xfer, zv);
                    z_begin_data_pkt(zv);
                    break;
                case ZCRCE:
                    /* end-of-frame, wait for ZEOF header */
                    zv->z_pkt_state = ZPS_GET_PAD;
                    z_set_timeout(xfer, zv->timeout_ms);
                    break;
                case ZCRCW:
                    z_send_ack(xfer, zv);
                    zv->z_pkt_state = ZPS_GET_PAD;
                    z_set_timeout(xfer, zv->timeout_ms);
                    break;
                default:
                    zv->z_pkt_state = ZPS_GET_PAD;
                    break;
                }
                break;

            default:
                zv->z_pkt_state = ZPS_GET_PAD;
                break;
            }
            break;  /* ZPKT_DATA_OK */
        }
    }
    return TTCORE_XFER_OK;
}

/* ── ZMODEM vtable functions ─────────────────────────────── */

static bool zmodem_init(TtcoreTransfer *xfer)
{
    TZVar *zv = (TZVar *)xfer->proto_state;

    zv->cbs         = &xfer->cbs;
    zv->tout_init   = xfer->cfg.timeout_init_ms  ? xfer->cfg.timeout_init_ms  : ZTOUT_INIT;
    zv->tout_fin    = xfer->cfg.timeout_long_ms  ? xfer->cfg.timeout_long_ms  : ZTOUT_FIN;
    zv->timeout_ms  = xfer->cfg.timeout_long_ms  ? xfer->cfg.timeout_long_ms  : ZTOUT_DATA;
    zv->max_data_len = ZMAX_DATA;
    zv->can_count   = 5;
    zv->z_pkt_state = ZPS_GET_PAD;
    zv->crc32_val   = 0xFFFFFFFFu;
    zv->win_size    = -1;
    zv->start_ms    = z_now_ms();

    if (xfer->cfg.dir == TTCORE_XFER_RECV) {
        zv->z_mode  = ZM_RECV;
        zv->z_state = ZS_RECV_INIT;
        z_send_rinit(xfer, zv);
    } else {
        zv->z_mode  = ZM_SEND;
        z_send_zrqinit(xfer, zv);
    }
    return true;
}

static TtcoreXferResult zmodem_parse(TtcoreTransfer *xfer)
{
    TZVar *zv = (TZVar *)xfer->proto_state;
    if (zv->z_mode == ZM_SEND)
        return z_send_parse(xfer);
    return z_recv_parse(xfer);
}

static void zmodem_timeout(TtcoreTransfer *xfer)
{
    TZVar *zv = (TZVar *)xfer->proto_state;
    switch (zv->z_state) {
    /* recv side */
    case ZS_RECV_INIT:
    case ZS_RECV_INIT2:
        z_send_rinit(xfer, zv);
        break;
    case ZS_RECV_DATA:
        zv->rpos_retry++;
        if (zv->rpos_retry > 5) {
            z_send_cancel(xfer);
            zv->z_state = ZS_CANCEL;
        } else {
            z_send_rpos(xfer, zv);
        }
        break;
    case ZS_RECV_FIN:
        zv->z_state = ZS_END;
        break;
    /* send side */
    case ZS_SEND_INIT:
        z_send_zrqinit(xfer, zv);
        break;
    case ZS_SEND_FILE_DAT:
    case ZS_SEND_DATA_DAT2:
    case ZS_SEND_EOF:
        /* no response from receiver — resend ZDATA header to retry */
        z_send_zdata_hdr(xfer, zv);
        break;
    case ZS_SEND_FIN:
        /* resend ZFIN */
        memset(zv->tx_hdr, 0, 4);
        z_send_zhex_hdr(xfer, zv, ZFIN);
        break;
    default:
        break;
    }
}

static void zmodem_cancel(TtcoreTransfer *xfer)
{
    TZVar *zv = (TZVar *)xfer->proto_state;
    z_send_cancel(xfer);
    zv->z_state = ZS_CANCEL;
}

static void zmodem_destroy(TtcoreTransfer *xfer)
{
    TZVar *zv = (TZVar *)xfer->proto_state;
    if (zv && zv->file_open && xfer->cbs.file_close) {
        xfer->cbs.file_close(xfer->cbs.user);
        zv->file_open = false;
    }
    free(xfer->proto_state);
    xfer->proto_state = NULL;
}

static const TtcoreProtoOp kZmodemOps = {
    zmodem_init,
    zmodem_parse,
    zmodem_timeout,
    zmodem_cancel,
    zmodem_destroy,
};

/* ════════════════════════════════════════════════════════════
 *  Public API
 * ════════════════════════════════════════════════════════════ */

TtcoreTransfer *ttcore_xfer_create(const TtcoreXferConfig    *cfg,
                                   const TtcoreXferCallbacks *cbs)
{
    if (!cfg || !cbs)
        return NULL;
    if (!cbs->write_bytes || !cbs->read_byte || !cbs->flush_rx)
        return NULL;
    if (!cbs->file_open_read || !cbs->file_open_write ||
        !cbs->file_read      || !cbs->file_write      ||
        !cbs->file_close     || !cbs->file_size)
        return NULL;

    TtcoreTransfer *xfer = (TtcoreTransfer *)calloc(1, sizeof(*xfer));
    if (!xfer)
        return NULL;

    xfer->cfg = *cfg;
    xfer->cbs = *cbs;

    switch (cfg->proto) {
    case TTCORE_XFER_PROTO_XMODEM: {
        XVar *xv = (XVar *)calloc(1, sizeof(*xv));
        if (!xv) { free(xfer); return NULL; }
        xfer->proto_state = xv;
        xfer->op          = &kXmodemOps;
        break;
    }
    case TTCORE_XFER_PROTO_YMODEM: {
        YVar *yv = (YVar *)calloc(1, sizeof(*yv));
        if (!yv) { free(xfer); return NULL; }
        xfer->proto_state = yv;
        xfer->op          = &kYmodemOps;
        break;
    }
    case TTCORE_XFER_PROTO_ZMODEM: {
        TZVar *zv = (TZVar *)calloc(1, sizeof(*zv));
        if (!zv) { free(xfer); return NULL; }
        xfer->proto_state = zv;
        xfer->op          = &kZmodemOps;
        break;
    }
    default:
        free(xfer);
        return NULL;
    }

    xfer->result           = TTCORE_XFER_OK;
    xfer->status.file_size = -1;

    if (!xfer->op->init(xfer)) {
        xfer->op->destroy(xfer);
        free(xfer);
        return NULL;
    }
    return xfer;
}

void ttcore_xfer_destroy(TtcoreTransfer *xfer)
{
    if (!xfer)
        return;
    if (xfer->op && xfer->op->destroy)
        xfer->op->destroy(xfer);
    free(xfer);
}

TtcoreXferResult ttcore_xfer_parse(TtcoreTransfer *xfer)
{
    if (!xfer)
        return TTCORE_XFER_ERR_PROTO;
    if (xfer->result != TTCORE_XFER_OK)
        return xfer->result;

    TtcoreXferResult r = xfer->op->parse(xfer);
    if (r != TTCORE_XFER_OK) {
        xfer->result = r;
        if (xfer->cbs.on_done)
            xfer->cbs.on_done(xfer->cbs.user, r);
    }
    return r;
}

void ttcore_xfer_timeout(TtcoreTransfer *xfer)
{
    if (!xfer || xfer->result != TTCORE_XFER_OK)
        return;
    xfer->op->timeout(xfer);
}

void ttcore_xfer_cancel(TtcoreTransfer *xfer)
{
    if (!xfer || xfer->result != TTCORE_XFER_OK)
        return;
    xfer->op->cancel(xfer);
    xfer->result = TTCORE_XFER_CANCELED;
    if (xfer->cbs.on_done)
        xfer->cbs.on_done(xfer->cbs.user, TTCORE_XFER_CANCELED);
}

const TtcoreXferStatus *ttcore_xfer_status(const TtcoreTransfer *xfer)
{
    if (!xfer)
        return NULL;
    return &xfer->status;
}
