// 2026-03-20 v0.1.0
// serial_config_dialog.c — Dialog for detailed serial port configuration.
// Phase G6: full parameter control beyond toolbar combos.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "serial_config_dialog.h"
#include "config.h"
#include "toolbar.h"
#include "terminal_view.h"

#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Baud rate list
// ---------------------------------------------------------------------------

static const char *const kBaudRates[] = {
    "9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"
};
static const int kNumBaudRates = 8;

// ---------------------------------------------------------------------------
// Dialog
// ---------------------------------------------------------------------------

bool serial_config_dialog_show(GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Serial Configuration", parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 350, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_box_pack_start(GTK_BOX(content), grid, FALSE, FALSE, 0);

    int row = 0;

    // Port
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Port:"), 0, row, 1, 1);
    GtkWidget *port_entry = gtk_entry_new();
    const char *saved_port = config_get_string("Connection", "port", "");
    const char *cur_port = toolbar_get_port();
    gtk_entry_set_text(GTK_ENTRY(port_entry),
                       (cur_port && cur_port[0]) ? cur_port : saved_port);
    gtk_grid_attach(GTK_GRID(grid), port_entry, 1, row++, 1, 1);

    // Baud
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Baud Rate:"), 0, row, 1, 1);
    GtkWidget *baud_combo = gtk_combo_box_text_new();
    int saved_baud = config_get_int("Connection", "baud", 115200);
    int baud_active = 4;  // default: 115200
    for (int i = 0; i < kNumBaudRates; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(baud_combo),
                                       kBaudRates[i]);
        if (atoi(kBaudRates[i]) == saved_baud) baud_active = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(baud_combo), baud_active);
    gtk_grid_attach(GTK_GRID(grid), baud_combo, 1, row++, 1, 1);

    // Data Bits
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Data Bits:"), 0, row, 1, 1);
    GtkWidget *data_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(data_combo), "8");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(data_combo), "7");
    int saved_data = config_get_int("Connection", "data_bits", 8);
    gtk_combo_box_set_active(GTK_COMBO_BOX(data_combo), saved_data == 7 ? 1 : 0);
    gtk_grid_attach(GTK_GRID(grid), data_combo, 1, row++, 1, 1);

    // Stop Bits
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Stop Bits:"), 0, row, 1, 1);
    GtkWidget *stop_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(stop_combo), "1");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(stop_combo), "2");
    int saved_stop = config_get_int("Connection", "stop_bits", 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(stop_combo), saved_stop == 2 ? 1 : 0);
    gtk_grid_attach(GTK_GRID(grid), stop_combo, 1, row++, 1, 1);

    // Parity
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Parity:"), 0, row, 1, 1);
    GtkWidget *parity_combo = gtk_combo_box_text_new();
    const char *const parities[] = {"None", "Odd", "Even", "Mark", "Space"};
    for (int i = 0; i < 5; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(parity_combo),
                                       parities[i]);
    int saved_parity = config_get_int("Connection", "parity", 0);
    if (saved_parity < 0 || saved_parity > 4) saved_parity = 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(parity_combo), saved_parity);
    gtk_grid_attach(GTK_GRID(grid), parity_combo, 1, row++, 1, 1);

    // Flow Control
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Flow Control:"),
                    0, row, 1, 1);
    GtkWidget *flow_combo = gtk_combo_box_text_new();
    const char *const flows[] = {"None", "RTS/CTS", "XON/XOFF", "DSR/DTR"};
    for (int i = 0; i < 4; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(flow_combo),
                                       flows[i]);
    int saved_flow = config_get_int("Connection", "flow", 0);
    if (saved_flow < 0 || saved_flow > 3) saved_flow = 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(flow_combo), saved_flow);
    gtk_grid_attach(GTK_GRID(grid), flow_combo, 1, row++, 1, 1);

    // Backspace Key
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Backspace Key:"),
                    0, row, 1, 1);
    GtkWidget *bs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *bs_radio = gtk_radio_button_new_with_label(NULL, "BS (0x08)");
    GtkWidget *del_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(bs_radio), "DEL (0x7F)");
    int saved_bskey = config_get_int("Connection", "backspace_key", 0);
    if (saved_bskey == 1) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(del_radio), TRUE);
    } else {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bs_radio), TRUE);
    }
    gtk_box_pack_start(GTK_BOX(bs_box), bs_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bs_box), del_radio, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), bs_box, 1, row++, 1, 1);

    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));

    bool applied = false;
    if (response == GTK_RESPONSE_OK) {
        // Read values from dialog
        const char *port = gtk_entry_get_text(GTK_ENTRY(port_entry));
        gchar *baud_text = gtk_combo_box_text_get_active_text(
            GTK_COMBO_BOX_TEXT(baud_combo));
        int baud = baud_text ? atoi(baud_text) : 115200;
        g_free(baud_text);

        int data_bits = gtk_combo_box_get_active(GTK_COMBO_BOX(data_combo)) == 1
                        ? 7 : 8;
        int stop_bits = gtk_combo_box_get_active(GTK_COMBO_BOX(stop_combo)) == 1
                        ? 2 : 1;
        int parity = gtk_combo_box_get_active(GTK_COMBO_BOX(parity_combo));
        int flow = gtk_combo_box_get_active(GTK_COMBO_BOX(flow_combo));

        // Save to config
        config_set_string("Connection", "port", port);
        config_set_int("Connection", "baud", baud);
        config_set_int("Connection", "data_bits", data_bits);
        config_set_int("Connection", "stop_bits", stop_bits);
        config_set_int("Connection", "parity", parity);
        config_set_int("Connection", "flow", flow);

        int bskey = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(del_radio)) ? 1 : 0;
        config_set_int("Connection", "backspace_key", bskey);
        config_save();

        terminal_view_set_backspace_del(bskey == 1);

        // Update toolbar to reflect new settings
        toolbar_set_port(port);
        toolbar_set_baud((uint32_t)baud);

        applied = true;
    }

    gtk_widget_destroy(dialog);
    return applied;
}
