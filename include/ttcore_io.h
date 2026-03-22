// 2026-03-10 v0.1.0 — ttcore_io: Serial Port I/O (headless, non-blocking, POSIX)
// Serial port open/close/configure, mode-switch terminal<->transfer
// Replaces Win32 commlib.c: CreateFile/SetCommState/WaitCommEvent -> termios/poll()

#ifndef TTCORE_IO_H
#define TTCORE_IO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaker Handle ─────────────────────────────────────── */

typedef struct TtcoreIo TtcoreIo;

/* ── Fehler-Codes ──────────────────────────────────────── */

#define TTCORE_IO_OK            0
#define TTCORE_IO_ERR_OPEN     -1   /* open()/tcsetattr() fehlgeschlagen      */
#define TTCORE_IO_ERR_CONFIG   -2   /* ungültige Konfiguration                */
#define TTCORE_IO_ERR_IO       -3   /* read()/write()/ioctl() fehlgeschlagen  */
#define TTCORE_IO_ERR_CLOSED   -4   /* Port ist nicht geöffnet                */
#define TTCORE_IO_ERR_BADPARAM -5   /* NULL-Pointer oder ungültiger Parameter */
#define TTCORE_IO_ERR_FULL     -6   /* TX-Puffer voll — Daten nicht gepuffert */

/* ── Parität ───────────────────────────────────────────── */

#define TTCORE_IO_PARITY_NONE   0
#define TTCORE_IO_PARITY_ODD    1
#define TTCORE_IO_PARITY_EVEN   2
#define TTCORE_IO_PARITY_MARK   3   /* Linux: CMSPAR; ERR_CONFIG wenn fehlt   */
#define TTCORE_IO_PARITY_SPACE  4   /* Linux: CMSPAR; ERR_CONFIG wenn fehlt   */

/* ── Flusskontrolle ────────────────────────────────────── */

#define TTCORE_IO_FLOW_NONE     0
#define TTCORE_IO_FLOW_XONXOFF  1   /* termios: IXON | IXOFF                  */
#define TTCORE_IO_FLOW_RTSCTS   2   /* termios: CRTSCTS                       */
#define TTCORE_IO_FLOW_DSRDTR   3   /* Linux: ioctl(TIOCM_DTR) manuell        */

/* ── Modus ─────────────────────────────────────────────── */

typedef enum {
    TTCORE_IO_MODE_TERMINAL = 0,    /* RX -> on_rx_data  -> libttcore         */
    TTCORE_IO_MODE_TRANSFER = 1,    /* RX -> on_xfer_data -> ttcore_transfer  */
} TtcoreIoMode;

/* ── Konfiguration ─────────────────────────────────────── */

typedef struct {
    const char *device;             /* "/dev/ttyS0", "/dev/ttyUSB0" etc.      */
    uint32_t    baud;               /* 9600, 115200, 230400, 460800, 921600   */
    uint8_t     data_bits;          /* 7 | 8                                  */
    uint8_t     stop_bits;          /* 1 | 2                                  */
    uint8_t     parity;             /* TTCORE_IO_PARITY_*                     */
    uint8_t     flow;               /* TTCORE_IO_FLOW_*                       */
    uint32_t    delay_per_char_us;  /* Sendeverzögerung pro Zeichen [µs]      */
    uint32_t    delay_per_line_us;  /* Sendeverzögerung pro Zeile   [µs]      */
} TtcoreIoConfig;

/* ── Callbacks ─────────────────────────────────────────── */

typedef struct {
    /* TERMINAL-Modus: empfangene Bytes an VT-Parser-Schicht */
    void (*on_rx_data)(const uint8_t *data, size_t len, void *ud);
    /* TRANSFER-Modus: empfangene Bytes an Protokoll-Schicht */
    void (*on_xfer_data)(const uint8_t *data, size_t len, void *ud);
    /* Port erfolgreich geöffnet */
    void (*on_open)(void *ud);
    /* Port geschlossen */
    void (*on_close)(void *ud);
    /* Fehler aufgetreten (code = TTCORE_IO_ERR_*, msg = Klartext) */
    void (*on_error)(int code, const char *msg, void *ud);
    void *ud;                       /* Benutzerdaten für alle Callbacks       */
} TtcoreIoCallbacks;

/* ── Lifecycle ─────────────────────────────────────────── */

TtcoreIo *ttcore_io_create(void);
void      ttcore_io_destroy(TtcoreIo *io);

/* Konfigurieren (vor oder nach open — re-applied via tcsetattr wenn offen) */
int  ttcore_io_configure(TtcoreIo *io, const TtcoreIoConfig *cfg);
int  ttcore_io_set_callbacks(TtcoreIo *io, const TtcoreIoCallbacks *cbs);

/* Port öffnen / schließen */
int  ttcore_io_open(TtcoreIo *io);
void ttcore_io_close(TtcoreIo *io);
bool ttcore_io_is_open(const TtcoreIo *io);

/* ── Event-Loop-Treiber ────────────────────────────────── */

/* timeout_ms:  0 = non-blocking, -1 = blockierend, >0 = max. Wartezeit
   Rückgabe:   >0 = Bytes verarbeitet, 0 = idle, <0 = TTCORE_IO_ERR_* */
int  ttcore_io_poll(TtcoreIo *io, int timeout_ms);

/* ── Senden (gepuffert) ────────────────────────────────── */

/* Gibt TTCORE_IO_ERR_FULL zurück wenn TX-Puffer voll — kein silent truncate */
int  ttcore_io_write(TtcoreIo *io, const uint8_t *data, size_t len);

/* ── Modus-Umschaltung ─────────────────────────────────── */

void         ttcore_io_set_mode(TtcoreIo *io, TtcoreIoMode mode);
TtcoreIoMode ttcore_io_get_mode(const TtcoreIo *io);

/* ── Serielle Steuerfunktionen ──────────────────────────── */

/* BREAK senden: ms<=0 → tcsendbreak(0) ~0.3s, ms>0 → TIOCSBRK+nanosleep */
int  ttcore_io_send_break(TtcoreIo *io, int ms);
/* DTR / RTS setzen oder löschen */
int  ttcore_io_set_dtr(TtcoreIo *io, bool active);
int  ttcore_io_set_rts(TtcoreIo *io, bool active);
/* TX- und RX-Puffer leeren (tcflush TCIOFLUSH) */
int  ttcore_io_flush(TtcoreIo *io);

#ifdef __cplusplus
}
#endif

#endif /* TTCORE_IO_H */
