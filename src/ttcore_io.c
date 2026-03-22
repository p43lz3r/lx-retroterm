// 2026-03-10 v0.1.0 — ttcore_io: Serial Port I/O (headless, non-blocking, POSIX)
// Ersetzt Win32 commlib.c: CreateFile/DCB/WaitCommEvent -> termios/poll()
// Phase 1: open/close/configure + serielle Signale; poll/write: nächste Phase

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>

#include "ttcore_io.h"

/* ── Puffergrößen (analog zu commlib.c) ────────────────── */

#define TTCORE_IO_TX_BUF_SIZE  4096
#define TTCORE_IO_RX_TMP_SIZE  16384
#define TTCORE_IO_DEV_MAX      256

/* ── Internes Struct ────────────────────────────────────── */

struct TtcoreIo {
    char              device[TTCORE_IO_DEV_MAX]; /* interne Kopie des Gerätepfads  */
    TtcoreIoConfig    cfg;                       /* cfg.device zeigt auf ->device  */
    TtcoreIoCallbacks cbs;                       /* Kopie der Callbacks            */
    TtcoreIoMode      mode;                      /* TERMINAL | TRANSFER            */

    int               fd;                        /* Dateideskriptor (-1 = zu)      */
    bool              is_open;

    struct termios    saved_tios;                /* Zustand vor open() für Restore */
    bool              tios_saved;

    /* TX-Ringpuffer für non-blocking write */
    uint8_t           tx_buf[TTCORE_IO_TX_BUF_SIZE];
    size_t            tx_head;                   /* Lesezeiger                     */
    size_t            tx_tail;                   /* Schreibzeiger                  */
    size_t            tx_count;                  /* Anzahl gepufferter Bytes       */

    /* Temporärer RX-Puffer für poll() */
    uint8_t           rx_tmp[TTCORE_IO_RX_TMP_SIZE];
};

/* ── Baud-Rate-Lookup (POSIX B*-Konstanten) ─────────────── */

static speed_t baud_to_speed(uint32_t baud) {
    switch (baud) {
        case 50:      return B50;
        case 75:      return B75;
        case 110:     return B110;
        case 134:     return B134;
        case 150:     return B150;
        case 200:     return B200;
        case 300:     return B300;
        case 600:     return B600;
        case 1200:    return B1200;
        case 1800:    return B1800;
        case 2400:    return B2400;
        case 4800:    return B4800;
        case 9600:    return B9600;
        case 19200:   return B19200;
        case 38400:   return B38400;
        case 57600:   return B57600;
        case 115200:  return B115200;
        case 230400:  return B230400;
        case 460800:  return B460800;
        case 921600:  return B921600;
        default:      return B0;     /* nicht unterstützt */
    }
}

/* ── Interne Hilfsfunktion: termios anwenden ────────────── */

static int apply_termios(TtcoreIo *io) {
    speed_t speed = baud_to_speed(io->cfg.baud);
    if (speed == B0) return TTCORE_IO_ERR_CONFIG;

    struct termios tios;
    memset(&tios, 0, sizeof(tios));
    cfmakeraw(&tios);

    /* Baud-Rate */
    cfsetispeed(&tios, speed);
    cfsetospeed(&tios, speed);

    /* Datenbits */
    tios.c_cflag &= ~CSIZE;
    tios.c_cflag |= (io->cfg.data_bits == 7) ? CS7 : CS8;

    /* Stoppbits */
    if (io->cfg.stop_bits == 2)
        tios.c_cflag |= CSTOPB;
    else
        tios.c_cflag &= ~CSTOPB;

    /* Parität */
    switch (io->cfg.parity) {
        case TTCORE_IO_PARITY_NONE:
            tios.c_cflag &= ~PARENB;
            break;
        case TTCORE_IO_PARITY_ODD:
            tios.c_cflag |= (PARENB | PARODD);
            tios.c_iflag |= (INPCK | ISTRIP);
            break;
        case TTCORE_IO_PARITY_EVEN:
            tios.c_cflag |= PARENB;
            tios.c_cflag &= ~PARODD;
            tios.c_iflag |= (INPCK | ISTRIP);
            break;
#ifdef CMSPAR
        case TTCORE_IO_PARITY_MARK:
            tios.c_cflag |= (PARENB | PARODD | CMSPAR);
            tios.c_iflag |= (INPCK | ISTRIP);
            break;
        case TTCORE_IO_PARITY_SPACE:
            tios.c_cflag |= (PARENB | CMSPAR);
            tios.c_cflag &= ~PARODD;
            tios.c_iflag |= (INPCK | ISTRIP);
            break;
#else
        case TTCORE_IO_PARITY_MARK:
        case TTCORE_IO_PARITY_SPACE:
            return TTCORE_IO_ERR_CONFIG; /* CMSPAR nicht verfügbar */
#endif
        default:
            return TTCORE_IO_ERR_CONFIG;
    }

    /* Flusskontrolle */
    switch (io->cfg.flow) {
        case TTCORE_IO_FLOW_NONE:
        case TTCORE_IO_FLOW_DSRDTR:     /* DSR/DTR: manuell via ioctl, kein termios */
            tios.c_cflag &= ~CRTSCTS;
            tios.c_iflag &= ~(IXON | IXOFF | IXANY);
            break;
        case TTCORE_IO_FLOW_XONXOFF:
            tios.c_cflag &= ~CRTSCTS;
            tios.c_iflag |= (IXON | IXOFF);
            tios.c_iflag &= ~IXANY;
            break;
        case TTCORE_IO_FLOW_RTSCTS:
            tios.c_cflag |= CRTSCTS;
            tios.c_iflag &= ~(IXON | IXOFF | IXANY);
            break;
        default:
            return TTCORE_IO_ERR_CONFIG;
    }

    /* Empfänger aktivieren, Modem-Steuerleitungen ignorieren */
    tios.c_cflag |= (CREAD | CLOCAL);

    /* Non-blocking reads: VMIN=0, VTIME=0 */
    tios.c_cc[VMIN]  = 0;
    tios.c_cc[VTIME] = 0;

    if (tcsetattr(io->fd, TCSANOW, &tios) != 0)
        return TTCORE_IO_ERR_CONFIG;

    return TTCORE_IO_OK;
}

/* ── Lifecycle ──────────────────────────────────────────── */

TtcoreIo *ttcore_io_create(void) {
    TtcoreIo *io = (TtcoreIo *)calloc(1, sizeof(TtcoreIo));
    if (!io) return NULL;
    io->fd      = -1;
    io->is_open = false;
    io->mode    = TTCORE_IO_MODE_TERMINAL;
    return io;
}

void ttcore_io_destroy(TtcoreIo *io) {
    if (!io) return;
    if (io->is_open) ttcore_io_close(io);
    free(io);
}

/* ── Konfiguration ──────────────────────────────────────── */

int ttcore_io_configure(TtcoreIo *io, const TtcoreIoConfig *cfg) {
    if (!io || !cfg)                           return TTCORE_IO_ERR_BADPARAM;
    if (!cfg->device || cfg->device[0] == '\0') return TTCORE_IO_ERR_BADPARAM;
    if (baud_to_speed(cfg->baud) == B0)         return TTCORE_IO_ERR_CONFIG;
    if (cfg->data_bits != 7 && cfg->data_bits != 8) return TTCORE_IO_ERR_CONFIG;
    if (cfg->stop_bits != 1 && cfg->stop_bits != 2) return TTCORE_IO_ERR_CONFIG;
    if (cfg->parity > TTCORE_IO_PARITY_SPACE)   return TTCORE_IO_ERR_CONFIG;
    if (cfg->flow   > TTCORE_IO_FLOW_DSRDTR)    return TTCORE_IO_ERR_CONFIG;

    /* Device-Pfad intern kopieren */
    strncpy(io->device, cfg->device, TTCORE_IO_DEV_MAX - 1);
    io->device[TTCORE_IO_DEV_MAX - 1] = '\0';

    /* Config speichern, device-Pointer auf interne Kopie */
    io->cfg        = *cfg;
    io->cfg.device = io->device;

    /* Wenn Port bereits offen: sofort anwenden */
    if (io->is_open)
        return apply_termios(io);

    return TTCORE_IO_OK;
}

int ttcore_io_set_callbacks(TtcoreIo *io, const TtcoreIoCallbacks *cbs) {
    if (!io || !cbs) return TTCORE_IO_ERR_BADPARAM;
    io->cbs = *cbs;
    return TTCORE_IO_OK;
}

/* ── Open / Close ────────────────────────────────────────── */

int ttcore_io_open(TtcoreIo *io) {
    if (!io) return TTCORE_IO_ERR_BADPARAM;

    /* Gerätepfad muss konfiguriert sein */
    if (io->device[0] == '\0') return TTCORE_IO_ERR_BADPARAM;

    /* Bereits offen → Fehler */
    if (io->is_open) return TTCORE_IO_ERR_OPEN;

    /* Gerät öffnen: non-blocking, kein controlling terminal */
    int fd = open(io->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (io->cbs.on_error)
            io->cbs.on_error(TTCORE_IO_ERR_OPEN, strerror(errno), io->cbs.ud);
        return TTCORE_IO_ERR_OPEN;
    }

    /* Aktuelle termios sichern für Restore bei close() */
    if (tcgetattr(fd, &io->saved_tios) == 0)
        io->tios_saved = true;

    io->fd = fd;

    /* termios anwenden */
    int r = apply_termios(io);
    if (r != TTCORE_IO_OK) {
        close(fd);
        io->fd = -1;
        io->tios_saved = false;
        if (io->cbs.on_error)
            io->cbs.on_error(r, "tcsetattr failed", io->cbs.ud);
        return TTCORE_IO_ERR_OPEN;
    }

    io->is_open = true;

    if (io->cbs.on_open)
        io->cbs.on_open(io->cbs.ud);

    return TTCORE_IO_OK;
}

void ttcore_io_close(TtcoreIo *io) {
    if (!io || !io->is_open) return;

    /* DTR löschen (wie TeraTerm EscapeCommFunction(CLRDTR)) */
    int bits = TIOCM_DTR;
    ioctl(io->fd, TIOCMBIC, &bits);

    /* termios wiederherstellen */
    if (io->tios_saved) {
        tcsetattr(io->fd, TCSANOW, &io->saved_tios);
        io->tios_saved = false;
    }

    close(io->fd);
    io->fd      = -1;
    io->is_open = false;

    /* TX-Puffer zurücksetzen */
    io->tx_head  = 0;
    io->tx_tail  = 0;
    io->tx_count = 0;

    if (io->cbs.on_close)
        io->cbs.on_close(io->cbs.ud);
}

bool ttcore_io_is_open(const TtcoreIo *io) {
    if (!io) return false;
    return io->is_open;
}

/* ── Modus-Umschaltung ───────────────────────────────────── */

void ttcore_io_set_mode(TtcoreIo *io, TtcoreIoMode mode) {
    if (!io) return;
    io->mode = mode;
}

TtcoreIoMode ttcore_io_get_mode(const TtcoreIo *io) {
    if (!io) return TTCORE_IO_MODE_TERMINAL;
    return io->mode;
}

/* ── Serielle Steuerfunktionen ───────────────────────────── */

int ttcore_io_send_break(TtcoreIo *io, int ms) {
    if (!io)          return TTCORE_IO_ERR_BADPARAM;
    if (!io->is_open) return TTCORE_IO_ERR_CLOSED;

    if (ms <= 0) {
        /* tcsendbreak(fd, 0): Standard-BREAK ~0.25-0.5s */
        return (tcsendbreak(io->fd, 0) == 0) ? TTCORE_IO_OK : TTCORE_IO_ERR_IO;
    }

    /* Präzises BREAK via TIOCSBRK + nanosleep + TIOCCBRK */
    if (ioctl(io->fd, TIOCSBRK, 0) != 0)
        return TTCORE_IO_ERR_IO;

    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);

    return (ioctl(io->fd, TIOCCBRK, 0) == 0) ? TTCORE_IO_OK : TTCORE_IO_ERR_IO;
}

int ttcore_io_set_dtr(TtcoreIo *io, bool active) {
    if (!io)          return TTCORE_IO_ERR_BADPARAM;
    if (!io->is_open) return TTCORE_IO_ERR_CLOSED;
    int bits = TIOCM_DTR;
    int r = ioctl(io->fd, active ? TIOCMBIS : TIOCMBIC, &bits);
    return (r == 0) ? TTCORE_IO_OK : TTCORE_IO_ERR_IO;
}

int ttcore_io_set_rts(TtcoreIo *io, bool active) {
    if (!io)          return TTCORE_IO_ERR_BADPARAM;
    if (!io->is_open) return TTCORE_IO_ERR_CLOSED;
    int bits = TIOCM_RTS;
    int r = ioctl(io->fd, active ? TIOCMBIS : TIOCMBIC, &bits);
    return (r == 0) ? TTCORE_IO_OK : TTCORE_IO_ERR_IO;
}

int ttcore_io_flush(TtcoreIo *io) {
    if (!io)          return TTCORE_IO_ERR_BADPARAM;
    if (!io->is_open) return TTCORE_IO_ERR_CLOSED;
    return (tcflush(io->fd, TCIOFLUSH) == 0) ? TTCORE_IO_OK : TTCORE_IO_ERR_IO;
}

/* ── Write — TX-Ringpuffer füllen ───────────────────────── */

int ttcore_io_write(TtcoreIo *io, const uint8_t *data, size_t len) {
    if (!io || !data) return TTCORE_IO_ERR_BADPARAM;
    if (!io->is_open) return TTCORE_IO_ERR_CLOSED;
    if (len == 0)     return TTCORE_IO_OK;

    /* Atomarität: kein partial write — ERR_FULL wenn kein Platz für alles */
    size_t avail = TTCORE_IO_TX_BUF_SIZE - io->tx_count;
    if (len > avail) return TTCORE_IO_ERR_FULL;

    /* Mit Wrap-Around kopieren */
    size_t first = TTCORE_IO_TX_BUF_SIZE - io->tx_tail;
    if (first > len) first = len;
    memcpy(&io->tx_buf[io->tx_tail], data, first);
    if (first < len)
        memcpy(io->tx_buf, data + first, len - first);

    io->tx_tail   = (io->tx_tail + len) % TTCORE_IO_TX_BUF_SIZE;
    io->tx_count += len;
    return TTCORE_IO_OK;
}

/* ── Poll — Event-Loop-Treiber ──────────────────────────── */

int ttcore_io_poll(TtcoreIo *io, int timeout_ms) {
    if (!io)          return TTCORE_IO_ERR_BADPARAM;
    if (!io->is_open) return TTCORE_IO_ERR_CLOSED;

    struct pollfd pfd;
    pfd.fd      = io->fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;
    if (io->tx_count > 0) pfd.events |= POLLOUT;

    int r = poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) return 0;   /* Signal-Unterbrechung — kein Fehler */
        if (io->cbs.on_error)
            io->cbs.on_error(TTCORE_IO_ERR_IO, strerror(errno), io->cbs.ud);
        return TTCORE_IO_ERR_IO;
    }
    if (r == 0) return 0;               /* Timeout — idle */

    if (pfd.revents & POLLERR) {
        if (io->cbs.on_error)
            io->cbs.on_error(TTCORE_IO_ERR_IO, "poll POLLERR", io->cbs.ud);
        return TTCORE_IO_ERR_IO;
    }

    int total = 0;

    /* ── RX: lesen und nach Modus dispatchen ── */
    if (pfd.revents & POLLIN) {
        ssize_t n = read(io->fd, io->rx_tmp, sizeof(io->rx_tmp));
        if (n > 0) {
            if (io->mode == TTCORE_IO_MODE_TERMINAL) {
                if (io->cbs.on_rx_data)
                    io->cbs.on_rx_data(io->rx_tmp, (size_t)n, io->cbs.ud);
            } else {
                if (io->cbs.on_xfer_data)
                    io->cbs.on_xfer_data(io->rx_tmp, (size_t)n, io->cbs.ud);
            }
            total += (int)n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (io->cbs.on_error)
                io->cbs.on_error(TTCORE_IO_ERR_IO, strerror(errno), io->cbs.ud);
            return TTCORE_IO_ERR_IO;
        }
        /* n == 0: EOF (z.B. PTY-Master geschlossen) — kein Fehler */
    }

    /* ── TX: Ringpuffer in den fd entleeren ── */
    if ((pfd.revents & POLLOUT) && io->tx_count > 0) {
        while (io->tx_count > 0) {
            /* Zusammenhängendes Stück ab tx_head (ohne Wrap) */
            size_t chunk = TTCORE_IO_TX_BUF_SIZE - io->tx_head;
            if (chunk > io->tx_count) chunk = io->tx_count;

            ssize_t written = write(io->fd, &io->tx_buf[io->tx_head], chunk);
            if (written > 0) {
                io->tx_head   = (io->tx_head + (size_t)written) % TTCORE_IO_TX_BUF_SIZE;
                io->tx_count -= (size_t)written;
                total        += (int)written;
            } else if (written == 0 ||
                       errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* OS-Puffer voll — nächstes poll() */
            } else {
                if (io->cbs.on_error)
                    io->cbs.on_error(TTCORE_IO_ERR_IO, strerror(errno), io->cbs.ud);
                return TTCORE_IO_ERR_IO;
            }
        }
    }

    return total;
}
