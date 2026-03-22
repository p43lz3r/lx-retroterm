// 2026-03-11 v0.3.0 — ttcore_transfer.h: File Transfer Protocols (XMODEM/YMODEM/ZMODEM)
// Headless, callback-based, non-blocking state machine port of TeraTerm ttpfile/
// No Win32, no globals, no GUI, no threads — pure C99 state machine

#ifndef TTCORE_TRANSFER_H
#define TTCORE_TRANSFER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaker Handle ──────────────────────────────────────── */

typedef struct TtcoreTransfer TtcoreTransfer;

/* ── Protokoll ──────────────────────────────────────────── */

typedef enum {
    TTCORE_XFER_PROTO_XMODEM = 0,
    TTCORE_XFER_PROTO_YMODEM,
    TTCORE_XFER_PROTO_ZMODEM,
} TtcoreXferProto;

/* ── Richtung ───────────────────────────────────────────── */

typedef enum {
    TTCORE_XFER_SEND = 0,
    TTCORE_XFER_RECV,
} TtcoreXferDir;

/* ── XMODEM-Optionen ─────────────────────────────────────── */

typedef enum {
    TTCORE_XMODEM_CHECKSUM    = 1,   /* 128-Byte Blöcke, Prüfsumme   */
    TTCORE_XMODEM_CRC         = 2,   /* 128-Byte Blöcke, CRC-16      */
    TTCORE_XMODEM_1K_CRC      = 3,   /* 1024-Byte Blöcke, CRC-16    */
    TTCORE_XMODEM_1K_CHECKSUM = 4,   /* 1024-Byte Blöcke, Prüfsumme  */
} TtcoreXmodemOpt;

/* ── YMODEM-Optionen ─────────────────────────────────────── */

typedef enum {
    TTCORE_YMODEM_1K     = 1,   /* Standard 1K-Blöcke, CRC-16    */
    TTCORE_YMODEM_G      = 2,   /* YMODEM-G: streaming, no retry  */
    TTCORE_YMODEM_SINGLE = 3,   /* Single-file (kein Batch)       */
} TtcoreYmodemOpt;

/* ── Ergebnis-Codes ─────────────────────────────────────── */

typedef enum {
    TTCORE_XFER_OK          =  0,   /* parse() → weiter          */
    TTCORE_XFER_DONE        =  1,   /* Übertragung abgeschlossen */
    TTCORE_XFER_CANCELED    =  2,   /* Abgebrochen               */
    TTCORE_XFER_ERR_PROTO   = -1,
    TTCORE_XFER_ERR_IO      = -2,
    TTCORE_XFER_ERR_TIMEOUT = -3,
    TTCORE_XFER_ERR_NOMEM   = -4,
} TtcoreXferResult;

/* ── Fortschritt ─────────────────────────────────────────── */

typedef struct {
    long  bytes_transferred;
    long  file_size;          /* -1 = unbekannt (XMODEM recv)  */
    long  packet_num;
    char  filename[256];      /* UTF-8; leer bei XMODEM        */
} TtcoreXferStatus;

/* ── Callbacks ───────────────────────────────────────────── */

typedef struct {
    /* Transport: Bytes an Gegenstelle senden (→ ttcore_io_write) */
    int  (*write_bytes)(void *user, const uint8_t *buf, size_t len);
    /* Transport: 1 Byte aus RX-Puffer lesen; 1=ok, 0=leer */
    int  (*read_byte)(void *user, uint8_t *b);
    /* Transport: RX-Puffer komplett leeren */
    void (*flush_rx)(void *user);

    /* Datei-I/O — alle Pfade UTF-8 */
    int    (*file_open_read) (void *user, const char *path);
    int    (*file_open_write)(void *user, const char *path);
    size_t (*file_read) (void *user, uint8_t *buf, size_t len);
    size_t (*file_write)(void *user, const uint8_t *buf, size_t len);
    void   (*file_close)(void *user);
    long   (*file_size)(void *user, const char *path); /* -1 = unbekannt */

    /* Timeout setzen: Aufrufer ruft nach ms ttcore_xfer_timeout(); 0=abbrechen */
    void (*set_timeout)(void *user, int ms);

    /* YMODEM Batch-Send: nächste Datei anfragen; gibt 1=ok 0=kein weiteres */
    int  (*file_next_send)(void *user, char *path_out, size_t path_max);
    /* YMODEM Recv: Ziel-Pfad bestimmen (darf NULL sein → cfg.filepath nutzen) */
    int  (*file_recv_path)(void *user, const char *name_from_hdr,
                           char *path_out, size_t path_max);
    /* YMODEM: mtime lesen/setzen (darf NULL sein) */
    long (*file_get_mtime)(void *user, const char *path);
    void (*file_set_mtime)(void *user, const char *path, long mtime);

    /* ZMODEM: Datei-Seek für ZRPOS-Resume; darf NULL sein */
    int  (*file_seek)(void *user, int32_t offset);

    /* Fortschritt/Abschluss — darf NULL sein */
    void (*on_progress)(void *user, const TtcoreXferStatus *status);
    void (*on_done)(void *user, TtcoreXferResult result);

    void *user;
} TtcoreXferCallbacks;

/* ── Konfiguration ───────────────────────────────────────── */

typedef struct {
    TtcoreXferProto proto;
    TtcoreXferDir   dir;
    int             opt;            /* TtcoreXmodemOpt; 0 = default CRC    */
    char            filepath[512];  /* Send: Quelldatei; Recv: Zieldatei   */
    bool            text_mode;      /* CRLF-Konvertierung + 0x1A-Trim      */

    /* Timeout-Werte [ms]; 0 = Standardwert verwenden */
    int timeout_init_ms;       /* warten auf ersten NAK/C vom Empfänger   */
    int timeout_init_crc_ms;   /* warten auf CRC-Anforderung 'C'          */
    int timeout_short_ms;      /* zwischen Bytes innerhalb eines Pakets   */
    int timeout_long_ms;       /* nach ACK — auf nächstes Paket           */
    int timeout_vlong_ms;      /* Gesamt-Timeout für Sende-Seite          */
} TtcoreXferConfig;

/* ── Lifecycle ───────────────────────────────────────────── */

TtcoreTransfer  *ttcore_xfer_create (const TtcoreXferConfig    *cfg,
                                     const TtcoreXferCallbacks *cbs);
void             ttcore_xfer_destroy(TtcoreTransfer *xfer);

/* State machine antreiben — aufrufen wenn RX-Daten verfügbar */
TtcoreXferResult ttcore_xfer_parse  (TtcoreTransfer *xfer);

/* Timeout signalisieren — vom Event-Loop nach set_timeout()-Ablauf */
void             ttcore_xfer_timeout(TtcoreTransfer *xfer);

/* Transfer abbrechen */
void             ttcore_xfer_cancel (TtcoreTransfer *xfer);

/* Letzter bekannter Status */
const TtcoreXferStatus *ttcore_xfer_status(const TtcoreTransfer *xfer);

#ifdef __cplusplus
}
#endif

#endif /* TTCORE_TRANSFER_H */
