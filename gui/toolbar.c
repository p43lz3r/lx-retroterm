// 2026-03-22 v0.4.0
// toolbar.c — Toolbar widget with serial port configuration.
// Phase G12: priority-sorted device list (ttyACM > ttyUSB > rest),
//            toolbar_next_port() for Ctrl+Shift+N cycling.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "toolbar.h"
#include "serial_conn.h"
#include "ttcore_io.h"

#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static GtkWidget *g_port_combo = NULL;
static GtkWidget *g_baud_combo = NULL;
static GtkWidget *g_dps_combo = NULL;
static GtkWidget *g_flow_combo = NULL;
static GtkWidget *g_dtr_toggle = NULL;
static GtkWidget *g_rts_toggle = NULL;
static GtkWidget *g_connect_btn = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GtkWidget *make_combo(const char *const *items, int n_items) {
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; i < n_items; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), items[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    return combo;
}

static int port_filter(const struct dirent *entry) {
    const char *n = entry->d_name;
    return (strncmp(n, "ttyACM", 6) == 0 ||
            strncmp(n, "ttyUSB", 6) == 0 ||
            strncmp(n, "ttyS", 4) == 0 ||
            strncmp(n, "ttyAMA", 6) == 0 ||
            strncmp(n, "pts/", 4) == 0);
}

static int port_priority(const char *name) {
    if (strncmp(name, "ttyACM", 6) == 0) return 0;
    if (strncmp(name, "ttyUSB", 6) == 0) return 1;
    return 2;
}

static int port_priority_cmp(const void *a, const void *b) {
    const struct dirent *da = *(const struct dirent *const *)a;
    const struct dirent *db = *(const struct dirent *const *)b;
    int pa = port_priority(da->d_name);
    int pb = port_priority(db->d_name);
    if (pa != pb) return pa - pb;
    return strcmp(da->d_name, db->d_name);
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void on_port_popup_shown(GObject *obj, GParamSpec *pspec,
                                gpointer user_data) {
    (void)pspec;
    (void)user_data;
    gboolean shown = FALSE;
    g_object_get(obj, "popup-shown", &shown, NULL);
    if (shown) toolbar_scan_ports();
}

static void on_connect_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;

    if (serial_conn_is_open()) {
        serial_conn_close();
        return;
    }

    const char *port = toolbar_get_port();
    if (!port || port[0] == '\0') return;

    uint32_t baud = toolbar_get_baud();
    uint8_t data_bits, stop_bits, parity;
    toolbar_get_dps(&data_bits, &stop_bits, &parity);
    uint8_t flow = toolbar_get_flow();

    serial_conn_open(port, baud, data_bits, stop_bits, parity, flow);
}

static void on_dtr_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (serial_conn_is_open()) {
        serial_conn_set_dtr(gtk_toggle_button_get_active(btn));
    }
}

static void on_rts_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (serial_conn_is_open()) {
        serial_conn_set_rts(gtk_toggle_button_get_active(btn));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void toolbar_scan_ports(void) {
    if (!g_port_combo) return;

    // Remember current selection
    gchar *current = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(g_port_combo));

    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_port_combo));

    // Scan /dev/ for serial ports, then sort by priority
    struct dirent **namelist = NULL;
    int n = scandir("/dev", &namelist, port_filter, alphasort);
    if (n > 1) {
        qsort(namelist, (size_t)n, sizeof(struct dirent *), port_priority_cmp);
    }
    int selected = -1;
    int first_preferred = -1;  // first ttyACM*/ttyUSB* index
    int count = 0;
    for (int i = 0; i < n; i++) {
        char path[280];
        snprintf(path, sizeof(path), "/dev/%s", namelist[i]->d_name);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_port_combo), path);
        if (current && strcmp(path, current) == 0) {
            selected = count;
        }
        if (first_preferred < 0 && port_priority(namelist[i]->d_name) < 2) {
            first_preferred = count;
        }
        count++;
        free(namelist[i]);
    }
    free(namelist);

    // Also scan /dev/pts/ for PTY slaves
    struct dirent **ptslist = NULL;
    int np = scandir("/dev/pts", &ptslist, NULL, alphasort);
    for (int i = 0; i < np; i++) {
        const char *name = ptslist[i]->d_name;
        // Skip . and .. and ptmx
        if (name[0] >= '0' && name[0] <= '9') {
            char path[280];
            snprintf(path, sizeof(path), "/dev/pts/%s", name);
            gtk_combo_box_text_append_text(
                GTK_COMBO_BOX_TEXT(g_port_combo), path);
            if (current && strcmp(path, current) == 0) {
                selected = count;
            }
            count++;
        }
        free(ptslist[i]);
    }
    free(ptslist);

    // Prefer first ttyACM*/ttyUSB* device; fall back to previous selection
    if (first_preferred >= 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_port_combo), first_preferred);
    } else if (selected >= 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_port_combo), selected);
    } else if (count > 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_port_combo), 0);
    }

    g_free(current);
}

GtkWidget *toolbar_create(void) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 2);
    gtk_widget_set_margin_bottom(box, 2);

    // Port combo (populated by scan)
    g_port_combo = gtk_combo_box_text_new_with_entry();
    gtk_widget_set_size_request(g_port_combo, 180, -1);
    g_signal_connect(g_port_combo, "notify::popup-shown",
                     G_CALLBACK(on_port_popup_shown), NULL);
    gtk_box_pack_start(GTK_BOX(box), g_port_combo, FALSE, FALSE, 0);

    // Baud
    const char *const bauds[] = {
        "115200", "9600", "38400", "57600", "230400", "460800"
    };
    g_baud_combo = make_combo(bauds, 6);
    gtk_box_pack_start(GTK_BOX(box), g_baud_combo, FALSE, FALSE, 0);

    // Data/Parity/Stop
    const char *const dps[] = {"8N1", "8N2", "7E1", "7O1"};
    g_dps_combo = make_combo(dps, 4);
    gtk_box_pack_start(GTK_BOX(box), g_dps_combo, FALSE, FALSE, 0);

    // Flow control
    const char *const flow[] = {"None", "RTS/CTS", "XON/XOFF"};
    g_flow_combo = make_combo(flow, 3);
    gtk_box_pack_start(GTK_BOX(box), g_flow_combo, FALSE, FALSE, 0);

    // Separator
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);

    // DTR / RTS toggle buttons
    g_dtr_toggle = gtk_toggle_button_new_with_label("DTR");
    gtk_widget_set_sensitive(g_dtr_toggle, FALSE);
    g_signal_connect(g_dtr_toggle, "toggled",
                     G_CALLBACK(on_dtr_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(box), g_dtr_toggle, FALSE, FALSE, 0);

    g_rts_toggle = gtk_toggle_button_new_with_label("RTS");
    gtk_widget_set_sensitive(g_rts_toggle, FALSE);
    g_signal_connect(g_rts_toggle, "toggled",
                     G_CALLBACK(on_rts_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(box), g_rts_toggle, FALSE, FALSE, 0);

    // Separator
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);

    // Encoding
    const char *const enc[] = {"UTF-8", "Shift_JIS", "EUC-JP", "ISO 8859-1"};
    gtk_box_pack_start(GTK_BOX(box), make_combo(enc, 4), FALSE, FALSE, 0);

    // Separator
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);

    // Hex Dump / Timestamp toggles
    GtkWidget *hex = gtk_toggle_button_new_with_label("Hex Dump");
    gtk_box_pack_start(GTK_BOX(box), hex, FALSE, FALSE, 0);

    GtkWidget *ts = gtk_toggle_button_new_with_label("Timestamp");
    gtk_box_pack_start(GTK_BOX(box), ts, FALSE, FALSE, 0);

    // Separator
    gtk_box_pack_start(GTK_BOX(box),
        gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 2);

    // Connect button
    g_connect_btn = gtk_button_new_with_label("Connect");
    g_signal_connect(g_connect_btn, "clicked",
                     G_CALLBACK(on_connect_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), g_connect_btn, FALSE, FALSE, 0);

    // Initial port scan
    toolbar_scan_ports();

    return box;
}

const char *toolbar_get_port(void) {
    if (!g_port_combo) return NULL;
    // For combo with entry, get the entry text
    if (GTK_IS_COMBO_BOX_TEXT(g_port_combo)) {
        GtkWidget *entry = gtk_bin_get_child(GTK_BIN(g_port_combo));
        if (GTK_IS_ENTRY(entry)) {
            return gtk_entry_get_text(GTK_ENTRY(entry));
        }
    }
    return NULL;
}

uint32_t toolbar_get_baud(void) {
    if (!g_baud_combo) return 115200;
    gchar *text = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(g_baud_combo));
    if (!text) return 115200;
    uint32_t baud = (uint32_t)atoi(text);
    g_free(text);
    return baud > 0 ? baud : 115200;
}

void toolbar_get_dps(uint8_t *data_bits, uint8_t *stop_bits, uint8_t *parity) {
    *data_bits = 8;
    *stop_bits = 1;
    *parity = TTCORE_IO_PARITY_NONE;

    if (!g_dps_combo) return;
    gchar *text = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(g_dps_combo));
    if (!text) return;

    if (strcmp(text, "8N1") == 0) {
        *data_bits = 8; *stop_bits = 1; *parity = TTCORE_IO_PARITY_NONE;
    } else if (strcmp(text, "8N2") == 0) {
        *data_bits = 8; *stop_bits = 2; *parity = TTCORE_IO_PARITY_NONE;
    } else if (strcmp(text, "7E1") == 0) {
        *data_bits = 7; *stop_bits = 1; *parity = TTCORE_IO_PARITY_EVEN;
    } else if (strcmp(text, "7O1") == 0) {
        *data_bits = 7; *stop_bits = 1; *parity = TTCORE_IO_PARITY_ODD;
    }
    g_free(text);
}

uint8_t toolbar_get_flow(void) {
    if (!g_flow_combo) return TTCORE_IO_FLOW_NONE;
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_flow_combo));
    switch (idx) {
        case 0: return TTCORE_IO_FLOW_NONE;
        case 1: return TTCORE_IO_FLOW_RTSCTS;
        case 2: return TTCORE_IO_FLOW_XONXOFF;
        default: return TTCORE_IO_FLOW_NONE;
    }
}

void toolbar_set_port(const char *port) {
    if (!g_port_combo || !port) return;
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(g_port_combo));
    if (GTK_IS_ENTRY(entry)) {
        gtk_entry_set_text(GTK_ENTRY(entry), port);
    }
}

void toolbar_set_baud(uint32_t baud) {
    if (!g_baud_combo) return;
    char baud_str[16];
    snprintf(baud_str, sizeof(baud_str), "%u", baud);
    // Search for matching entry in combo
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(g_baud_combo));
    GtkTreeIter iter;
    int idx = 0;
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gchar *text = NULL;
            gtk_tree_model_get(model, &iter, 0, &text, -1);
            if (text && strcmp(text, baud_str) == 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_baud_combo), idx);
                g_free(text);
                return;
            }
            g_free(text);
            idx++;
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    // Not found — set first entry (115200) as fallback
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_baud_combo), 0);
}

void toolbar_notify_connected(bool connected) {
    if (g_connect_btn) {
        gtk_button_set_label(GTK_BUTTON(g_connect_btn),
                             connected ? "Disconnect" : "Connect");
    }
    // Disable config combos when connected
    if (g_port_combo) gtk_widget_set_sensitive(g_port_combo, !connected);
    if (g_baud_combo) gtk_widget_set_sensitive(g_baud_combo, !connected);
    if (g_dps_combo) gtk_widget_set_sensitive(g_dps_combo, !connected);
    if (g_flow_combo) gtk_widget_set_sensitive(g_flow_combo, !connected);
    // DTR/RTS only active when connected
    if (g_dtr_toggle) gtk_widget_set_sensitive(g_dtr_toggle, connected);
    if (g_rts_toggle) gtk_widget_set_sensitive(g_rts_toggle, connected);
}

void toolbar_next_port(void) {
    if (!g_port_combo) return;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(g_port_combo));
    int count = gtk_tree_model_iter_n_children(model, NULL);
    if (count <= 1) return;
    int cur = gtk_combo_box_get_active(GTK_COMBO_BOX(g_port_combo));
    int next = (cur + 1) % count;
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_port_combo), next);
}
