// 2026-03-22 v0.12.0
// menu.c — GtkMenuBar with all application menu entries.
// Phase G12: Keyboard shortcuts (Ctrl+Shift+*), menu cleanup (hide placeholders).
// Phase G12b: Help menu visible — About dialog + Keyboard Shortcuts dialog.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "menu.h"
#include "config.h"
#include "serial_config_dialog.h"
#include "session_log.h"
#include "view_settings.h"
#include "toolbar_toggle.h"
#include "terminal_view.h"
#include "serial_conn.h"
#include "status_bar.h"
#include "text_sender.h"
#include "hex_upload.h"
#include "xmodem_gui.h"
#include "vtparse.h"
#include "toolbar.h"

static void on_noop(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
}

static void on_quit(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkApplication *app = GTK_APPLICATION(user_data);
    g_application_quit(G_APPLICATION(app));
}

static GtkWidget *make_item(const char *label, GCallback cb, gpointer data) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", cb, data);
    return item;
}

static GtkWidget *make_check_item(const char *label) {
    GtkWidget *item = gtk_check_menu_item_new_with_label(label);
    g_signal_connect(item, "toggled", G_CALLBACK(on_noop), NULL);
    return item;
}

// ---------------------------------------------------------------------------
// Edit menu callbacks (G10b)
// ---------------------------------------------------------------------------

static void on_copy(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    terminal_view_copy_selection();
}

static GtkWidget *g_copy_item = NULL;

static void on_clear_screen(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    terminal_view_clear_screen();
}

static GtkWidget *g_clear_screen_item = NULL;

static void on_edit_menu_map(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    if (g_copy_item) {
        gtk_widget_set_sensitive(g_copy_item,
                                 terminal_view_has_selection());
    }
    if (g_clear_screen_item) {
        gtk_widget_set_sensitive(g_clear_screen_item,
                                 serial_conn_is_open());
    }
}

static GtkWidget *build_edit_menu(GtkAccelGroup *accel) {
    GtkWidget *menu = gtk_menu_new();

    g_copy_item = make_item("Copy", G_CALLBACK(on_copy), NULL);
    gtk_widget_add_accelerator(g_copy_item, "activate", accel,
        GDK_KEY_C, (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_copy_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    g_clear_screen_item = make_item("Clear Screen",
                                     G_CALLBACK(on_clear_screen), NULL);
    gtk_widget_add_accelerator(g_clear_screen_item, "activate", accel,
        GDK_KEY_L,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_clear_screen_item);

    // Update sensitivity each time menu is shown
    g_signal_connect(menu, "map", G_CALLBACK(on_edit_menu_map), NULL);

    GtkWidget *mi = gtk_menu_item_new_with_label("Edit");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    return mi;
}

// ---------------------------------------------------------------------------
// Connection menu callbacks
// ---------------------------------------------------------------------------

static void on_connect_toggle(GtkMenuItem *item, gpointer user_data) {
    (void)item;
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

static void on_disconnect(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    if (serial_conn_is_open()) {
        serial_conn_close();
    }
}

static void on_rescan_ports(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    toolbar_scan_ports();
}

// ---------------------------------------------------------------------------
// Connection → Serial Config / Session callbacks
// ---------------------------------------------------------------------------

static void on_serial_config(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);
    serial_config_dialog_show(GTK_WINDOW(window));
}

static void on_session_save(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    const char *port = toolbar_get_port();
    if (port && port[0]) {
        config_set_string("Connection", "port", port);
    }
    config_set_int("Connection", "baud", (int)toolbar_get_baud());
    config_save();
}

static void on_session_restore(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    config_load();
    const char *port = config_get_string("Connection", "port", NULL);
    if (port) toolbar_set_port(port);
    int baud = config_get_int("Connection", "baud", 0);
    if (baud > 0) toolbar_set_baud((uint32_t)baud);
}

// ---------------------------------------------------------------------------
// Cursor Style submenu state + callbacks
// ---------------------------------------------------------------------------

static int g_menu_cursor_shape = VTPARSER_CURSOR_BLOCK;
static bool g_menu_cursor_blink = false;

static void on_cursor_shape_block(GtkCheckMenuItem *item, gpointer user_data) {
    (void)user_data;
    if (!gtk_check_menu_item_get_active(item)) return;
    g_menu_cursor_shape = VTPARSER_CURSOR_BLOCK;
    terminal_view_set_cursor_style(g_menu_cursor_shape, g_menu_cursor_blink);
}

static void on_cursor_shape_underline(GtkCheckMenuItem *item,
                                       gpointer user_data) {
    (void)user_data;
    if (!gtk_check_menu_item_get_active(item)) return;
    g_menu_cursor_shape = VTPARSER_CURSOR_UNDERLINE_STEADY;
    terminal_view_set_cursor_style(g_menu_cursor_shape, g_menu_cursor_blink);
}

static void on_cursor_blink_toggled(GtkCheckMenuItem *item,
                                     gpointer user_data) {
    (void)user_data;
    g_menu_cursor_blink = gtk_check_menu_item_get_active(item);
    terminal_view_set_cursor_style(g_menu_cursor_shape, g_menu_cursor_blink);
}

// ---------------------------------------------------------------------------
// View menu callbacks
// ---------------------------------------------------------------------------

static void on_appearance(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);
    if (view_settings_show_dialog(GTK_WINDOW(window))) {
        terminal_view_apply_settings();
    }
}

static void on_toggle_toolbar(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *revealer = GTK_WIDGET(user_data);
    toolbar_toggle_toggle(revealer);
}

static void on_auto_hide_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    (void)user_data;
    view_settings_set_auto_hide(
        gtk_check_menu_item_get_active(item));
}

// ---------------------------------------------------------------------------
// Menu builders
// ---------------------------------------------------------------------------

// Hidden File menu items — stored for future gtk_widget_show()
static GtkWidget *g_menu_file_new_window = NULL;
static GtkWidget *g_menu_file_open_log = NULL;
static GtkWidget *g_menu_file_save_log = NULL;
static GtkWidget *g_menu_file_separator = NULL;

static GtkWidget *build_file_menu(GtkApplication *app,
                                   GtkAccelGroup *accel) {
    GtkWidget *menu = gtk_menu_new();

    g_menu_file_new_window = make_item("New Window", G_CALLBACK(on_noop), NULL);
    gtk_widget_set_no_show_all(g_menu_file_new_window, TRUE);
    gtk_widget_hide(g_menu_file_new_window);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_file_new_window);

    g_menu_file_open_log = make_item("Open Log", G_CALLBACK(on_noop), NULL);
    gtk_widget_set_no_show_all(g_menu_file_open_log, TRUE);
    gtk_widget_hide(g_menu_file_open_log);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_file_open_log);

    g_menu_file_save_log = make_item("Save Log", G_CALLBACK(on_noop), NULL);
    gtk_widget_set_no_show_all(g_menu_file_save_log, TRUE);
    gtk_widget_hide(g_menu_file_save_log);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_file_save_log);

    g_menu_file_separator = gtk_separator_menu_item_new();
    gtk_widget_set_no_show_all(g_menu_file_separator, TRUE);
    gtk_widget_hide(g_menu_file_separator);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_file_separator);

    GtkWidget *quit_item = make_item("Quit", G_CALLBACK(on_quit), app);
    gtk_widget_add_accelerator(quit_item, "activate", accel,
        GDK_KEY_q, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    GtkWidget *mi = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    return mi;
}

// Hidden Connection menu items
static GtkWidget *g_menu_conn_auto_reconnect = NULL;

static GtkWidget *build_connection_menu(GtkAccelGroup *accel,
                                         GtkWidget *window) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *connect_item =
        make_item("Connect", G_CALLBACK(on_connect_toggle), NULL);
    gtk_widget_add_accelerator(connect_item, "activate", accel,
        GDK_KEY_O,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), connect_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Disconnect", G_CALLBACK(on_disconnect), NULL));

    GtkWidget *rescan_item =
        make_item("Rescan Ports", G_CALLBACK(on_rescan_ports), NULL);
    gtk_widget_add_accelerator(rescan_item, "activate", accel,
        GDK_KEY_D,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), rescan_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    g_menu_conn_auto_reconnect = make_check_item("Auto-Reconnect");
    gtk_widget_set_no_show_all(g_menu_conn_auto_reconnect, TRUE);
    gtk_widget_hide(g_menu_conn_auto_reconnect);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_conn_auto_reconnect);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *serial_cfg_item =
        make_item("Serial Config\342\200\246",
                   G_CALLBACK(on_serial_config), window);
    gtk_widget_add_accelerator(serial_cfg_item, "activate", accel,
        GDK_KEY_S,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), serial_cfg_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Session Save", G_CALLBACK(on_session_save), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Session Restore", G_CALLBACK(on_session_restore), NULL));

    GtkWidget *mi = gtk_menu_item_new_with_label("Connection");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    return mi;
}

// ---------------------------------------------------------------------------
// Send Text dialog helper — shared by Send Text… and Send Text File…
// ---------------------------------------------------------------------------

// Show a dialog with line-ending + delay controls.  If |prefill| is non-NULL
// the text view is pre-populated and made read-only (file mode).
// Returns true if user clicked OK; *out_text, *out_eol, *out_delay filled.
// Caller must g_free(*out_text).
static bool show_send_text_dialog(GtkWindow *parent, const char *prefill,
                                   char **out_text, TextSenderEol *out_eol,
                                   int *out_delay) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        prefill ? "Send Text File" : "Send Text\342\200\246",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Send",   GTK_RESPONSE_OK,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 500, 400);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(content), 6);
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);

    // Text view in a scrolled window
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget *text_view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    if (prefill) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
        gtk_text_buffer_set_text(buf, prefill, -1);
        gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    }
    gtk_container_add(GTK_CONTAINER(scroll), text_view);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    // Parameters row
    GtkWidget *param_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(content), param_box, FALSE, FALSE, 0);

    // Line-ending selector
    gtk_box_pack_start(GTK_BOX(param_box),
        gtk_label_new("Line Ending:"), FALSE, FALSE, 0);
    GtkWidget *eol_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(eol_combo), "CR");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(eol_combo), "LF");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(eol_combo), "CR+LF");
    gtk_combo_box_set_active(GTK_COMBO_BOX(eol_combo), 0);
    gtk_box_pack_start(GTK_BOX(param_box), eol_combo, FALSE, FALSE, 0);

    // Inter-line delay
    gtk_box_pack_start(GTK_BOX(param_box),
        gtk_label_new("Delay (ms):"), FALSE, FALSE, 0);
    GtkWidget *delay_spin = gtk_spin_button_new_with_range(0, 5000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(delay_spin), 0);
    gtk_box_pack_start(GTK_BOX(param_box), delay_spin, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    bool ok = false;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buf, &start, &end);
        *out_text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
        *out_eol = (TextSenderEol)gtk_combo_box_get_active(
            GTK_COMBO_BOX(eol_combo));
        *out_delay = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(delay_spin));
        ok = true;
    }
    gtk_widget_destroy(dlg);
    return ok;
}

static void on_send_text(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);
    char *text = NULL;
    TextSenderEol eol;
    int delay;
    if (show_send_text_dialog(GTK_WINDOW(window), NULL,
                               &text, &eol, &delay)) {
        if (text && text[0]) {
            text_sender_start(text, eol, delay);
        }
        g_free(text);
    }
}

static void on_send_text_file(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Select Text File", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_OK,
        NULL);

    // Text file filter
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Text files");
    gtk_file_filter_add_mime_type(filter, "text/plain");
    gtk_file_filter_add_pattern(filter, "*.txt");
    gtk_file_filter_add_pattern(filter, "*.bas");
    gtk_file_filter_add_pattern(filter, "*.asm");
    gtk_file_filter_add_pattern(filter, "*.inc");
    gtk_file_filter_add_pattern(filter, "*.hex");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy(chooser);
        return;
    }

    char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);

    if (!filepath) return;

    char *contents = NULL;
    gsize length = 0;
    GError *err = NULL;
    if (!g_file_get_contents(filepath, &contents, &length, &err)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not read file: %s",
                 err ? err->message : "unknown error");
        status_bar_set_error(msg);
        if (err) g_error_free(err);
        g_free(filepath);
        return;
    }
    g_free(filepath);

    char *text = NULL;
    TextSenderEol eol;
    int delay;
    if (show_send_text_dialog(GTK_WINDOW(window), contents,
                               &text, &eol, &delay)) {
        if (text && text[0]) {
            text_sender_start(text, eol, delay);
        }
        g_free(text);
    }
    g_free(contents);
}

static void on_cancel_transfer(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    xmodem_gui_cancel();
    text_sender_cancel();
}

// ---------------------------------------------------------------------------
// XMODEM Send callback
// ---------------------------------------------------------------------------

static void on_xmodem_send(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);

    if (xmodem_gui_is_active() || text_sender_is_active()) {
        status_bar_set_error("A transfer is already in progress");
        return;
    }
    if (!serial_conn_is_open()) {
        status_bar_set_error("Not connected");
        return;
    }

    // File chooser
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Select File for XMODEM Send", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_OK,
        NULL);

    GtkFileFilter *bin_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(bin_filter, "Binary files");
    gtk_file_filter_add_pattern(bin_filter, "*.bin");
    gtk_file_filter_add_pattern(bin_filter, "*.com");
    gtk_file_filter_add_pattern(bin_filter, "*.hex");
    gtk_file_filter_add_pattern(bin_filter, "*.rom");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), bin_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy(chooser);
        return;
    }

    char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (!filepath) return;

    // Protocol option dialog
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "XMODEM Options", GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Send",   GTK_RESPONSE_OK,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(content), 6);
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);

    GtkWidget *crc_radio = gtk_radio_button_new_with_label(
        NULL, "XMODEM-CRC (128 byte blocks)");
    gtk_box_pack_start(GTK_BOX(content), crc_radio, FALSE, FALSE, 0);

    GtkWidget *crc1k_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(crc_radio), "XMODEM-1K CRC (1024 byte blocks)");
    gtk_box_pack_start(GTK_BOX(content), crc1k_radio, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    bool ok = false;
    TtcoreXmodemOpt opt = TTCORE_XMODEM_CRC;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        ok = true;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(crc1k_radio))) {
            opt = TTCORE_XMODEM_1K_CRC;
        }
    }
    gtk_widget_destroy(dlg);

    if (ok) {
        xmodem_gui_send_start(filepath, opt);
    }

    g_free(filepath);
}

// ---------------------------------------------------------------------------
// XMODEM Receive callback
// ---------------------------------------------------------------------------

static void on_xmodem_recv(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);

    if (xmodem_gui_is_active() || text_sender_is_active()) {
        status_bar_set_error("A transfer is already in progress");
        return;
    }
    if (!serial_conn_is_open()) {
        status_bar_set_error("Not connected");
        return;
    }

    // File chooser (Save mode)
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Save Received File", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save",   GTK_RESPONSE_OK,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(
        GTK_FILE_CHOOSER(chooser), TRUE);

    GtkFileFilter *bin_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(bin_filter, "Binary files");
    gtk_file_filter_add_pattern(bin_filter, "*.bin");
    gtk_file_filter_add_pattern(bin_filter, "*.com");
    gtk_file_filter_add_pattern(bin_filter, "*.hex");
    gtk_file_filter_add_pattern(bin_filter, "*.rom");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), bin_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy(chooser);
        return;
    }

    char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (!filepath) return;

    // Protocol option dialog
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "XMODEM Receive Options", GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel",  GTK_RESPONSE_CANCEL,
        "_Receive", GTK_RESPONSE_OK,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(content), 6);
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);

    GtkWidget *crc_radio = gtk_radio_button_new_with_label(
        NULL, "XMODEM-CRC (128 byte blocks)");
    gtk_box_pack_start(GTK_BOX(content), crc_radio, FALSE, FALSE, 0);

    GtkWidget *crc1k_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(crc_radio), "XMODEM-1K CRC (1024 byte blocks)");
    gtk_box_pack_start(GTK_BOX(content), crc1k_radio, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    bool ok = false;
    TtcoreXmodemOpt opt = TTCORE_XMODEM_CRC;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        ok = true;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(crc1k_radio))) {
            opt = TTCORE_XMODEM_1K_CRC;
        }
    }
    gtk_widget_destroy(dlg);

    if (ok) {
        xmodem_gui_recv_start(filepath, opt);
    }

    g_free(filepath);
}

// ---------------------------------------------------------------------------
// Intel HEX Upload callback
// ---------------------------------------------------------------------------

static void on_hex_upload(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);

    if (text_sender_is_active()) {
        status_bar_set_error("A send operation is already in progress");
        return;
    }
    if (!serial_conn_is_open()) {
        status_bar_set_error("Not connected");
        return;
    }

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Select Intel HEX File", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_OK,
        NULL);

    GtkFileFilter *hex_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(hex_filter, "Intel HEX files");
    gtk_file_filter_add_pattern(hex_filter, "*.hex");
    gtk_file_filter_add_pattern(hex_filter, "*.ihx");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), hex_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy(chooser);
        return;
    }

    char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (!filepath) return;

    // Delay parameter dialog
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "HEX Upload Parameters", GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Upload", GTK_RESPONSE_OK,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(content), 6);
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content), hbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox),
        gtk_label_new("Inter-line delay (ms):"), FALSE, FALSE, 0);
    GtkWidget *delay_spin = gtk_spin_button_new_with_range(0, 500, 1);
    int saved_delay = config_get_int("Transfer", "hex_delay_ms", 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(delay_spin), saved_delay);
    gtk_box_pack_start(GTK_BOX(hbox), delay_spin, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    bool ok = false;
    int delay_ms = saved_delay;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        delay_ms = gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(delay_spin));
        ok = true;
    }
    gtk_widget_destroy(dlg);

    if (ok) {
        config_set_int("Transfer", "hex_delay_ms", delay_ms);
        hex_upload_start(filepath, delay_ms);
    }

    g_free(filepath);
}

// ---------------------------------------------------------------------------
// Session Log callbacks
// ---------------------------------------------------------------------------

static GtkWidget *g_log_item = NULL;

static void update_log_menu_label(void) {
    if (!g_log_item) return;
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_log_item),
                            session_log_is_active()
                                ? "Stop Log"
                                : "Start Log\342\200\246");
}

static void on_start_log(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *window = GTK_WIDGET(user_data);

    if (session_log_is_active()) {
        session_log_stop();
        status_bar_set_progress(NULL);
        update_log_menu_label();
        return;
    }

    // File chooser (Save)
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Session Log File", GTK_WINDOW(window),
        GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_OK,
        NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(
        GTK_FILE_CHOOSER(chooser), TRUE);

    GtkFileFilter *log_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(log_filter, "Log files");
    gtk_file_filter_add_pattern(log_filter, "*.log");
    gtk_file_filter_add_pattern(log_filter, "*.txt");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), log_filter);

    GtkFileFilter *all_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(all_filter, "All files");
    gtk_file_filter_add_pattern(all_filter, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all_filter);

    // Restore last used path
    const char *last_path = config_get_string("Log", "auto_path", "");
    if (last_path && last_path[0]) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), last_path);
    }

    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_OK) {
        gtk_widget_destroy(chooser);
        return;
    }

    char *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (!filepath) return;

    // Options dialog
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Log Options", GTK_WINDOW(window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Start",  GTK_RESPONSE_OK,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(content), 6);
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);

    GtkWidget *tx_check =
        gtk_check_button_new_with_label("Log transmitted data (TX)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tx_check),
                                 config_get_bool("Log", "log_tx", false));
    gtk_box_pack_start(GTK_BOX(content), tx_check, FALSE, FALSE, 0);

    GtkWidget *ts_check =
        gtk_check_button_new_with_label("Timestamps per line");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ts_check),
                                 config_get_bool("Log", "timestamps", true));
    gtk_box_pack_start(GTK_BOX(content), ts_check, FALSE, FALSE, 0);

    GtkWidget *append_check =
        gtk_check_button_new_with_label("Append to existing file");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(append_check),
                                 config_get_bool("Log", "append", false));
    gtk_box_pack_start(GTK_BOX(content), append_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    GtkWidget *auto_check =
        gtk_check_button_new_with_label("Auto-start logging on connect");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_check),
                                 config_get_bool("Log", "auto_start", false));
    gtk_box_pack_start(GTK_BOX(content), auto_check, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);

    bool ok = false;
    bool log_tx = false;
    bool timestamps = true;
    bool append = false;
    bool auto_start = false;

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        ok = true;
        log_tx = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tx_check));
        timestamps = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ts_check));
        append = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(append_check));
        auto_start = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(auto_check));
    }
    gtk_widget_destroy(dlg);

    if (ok) {
        // Persist settings
        config_set_string("Log", "auto_path", filepath);
        config_set_bool("Log", "log_tx", log_tx);
        config_set_bool("Log", "timestamps", timestamps);
        config_set_bool("Log", "append", append);
        config_set_bool("Log", "auto_start", auto_start);
        config_save();

        if (session_log_start(filepath, append, log_tx, timestamps)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Logging to file");
            status_bar_set_progress(msg);
            update_log_menu_label();
        } else {
            status_bar_set_error("Could not open log file");
        }
    }

    g_free(filepath);
}

// ---------------------------------------------------------------------------
// Transfer menu builder
// ---------------------------------------------------------------------------

// Hidden Transfer menu items
static GtkWidget *g_menu_xfer_ymodem = NULL;
static GtkWidget *g_menu_xfer_zmodem = NULL;

static GtkWidget *build_transfer_menu(GtkAccelGroup *accel,
                                       GtkWidget *window) {
    GtkWidget *menu = gtk_menu_new();

    GtkWidget *xm_send_item =
        make_item("Send File (XMODEM)\342\200\246",
                   G_CALLBACK(on_xmodem_send), window);
    gtk_widget_add_accelerator(xm_send_item, "activate", accel,
        GDK_KEY_X,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), xm_send_item);

    g_menu_xfer_ymodem = make_item("Send File (YMODEM)",
                                    G_CALLBACK(on_noop), NULL);
    gtk_widget_set_no_show_all(g_menu_xfer_ymodem, TRUE);
    gtk_widget_hide(g_menu_xfer_ymodem);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_xfer_ymodem);

    g_menu_xfer_zmodem = make_item("Send File (ZMODEM)",
                                    G_CALLBACK(on_noop), NULL);
    gtk_widget_set_no_show_all(g_menu_xfer_zmodem, TRUE);
    gtk_widget_hide(g_menu_xfer_zmodem);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_menu_xfer_zmodem);

    GtkWidget *hex_item =
        make_item("Send Intel HEX\342\200\246",
                   G_CALLBACK(on_hex_upload), window);
    gtk_widget_add_accelerator(hex_item, "activate", accel,
        GDK_KEY_H,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), hex_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *send_text_item =
        make_item("Send Text\342\200\246",
                   G_CALLBACK(on_send_text), window);
    gtk_widget_add_accelerator(send_text_item, "activate", accel,
        GDK_KEY_T,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), send_text_item);

    GtkWidget *send_file_item =
        make_item("Send Text File\342\200\246",
                   G_CALLBACK(on_send_text_file), window);
    gtk_widget_add_accelerator(send_file_item, "activate", accel,
        GDK_KEY_F,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), send_file_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Cancel Transfer", G_CALLBACK(on_cancel_transfer), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *xm_recv_item =
        make_item("Receive File (XMODEM)\342\200\246",
                   G_CALLBACK(on_xmodem_recv), window);
    gtk_widget_add_accelerator(xm_recv_item, "activate", accel,
        GDK_KEY_R,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), xm_recv_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    g_log_item = make_item("Start Log\342\200\246",
                            G_CALLBACK(on_start_log), window);
    gtk_widget_add_accelerator(g_log_item, "activate", accel,
        GDK_KEY_G,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_log_item);

    GtkWidget *mi = gtk_menu_item_new_with_label("Transfer");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    return mi;
}

static GtkWidget *build_view_menu(GtkAccelGroup *accel, GtkWidget *window,
                                   GtkWidget *revealer) {
    GtkWidget *menu = gtk_menu_new();

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Appearance\342\200\246",
                   G_CALLBACK(on_appearance), window));

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *toggle_item =
        make_item("Toggle Toolbar", G_CALLBACK(on_toggle_toolbar), revealer);
    gtk_widget_add_accelerator(toggle_item, "activate", accel,
        GDK_KEY_B,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle_item);

    GtkWidget *auto_hide_item =
        gtk_check_menu_item_new_with_label("Auto-hide Toolbar");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(auto_hide_item),
                                   view_settings_get_auto_hide());
    g_signal_connect(auto_hide_item, "toggled",
                     G_CALLBACK(on_auto_hide_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), auto_hide_item);

    // Cursor Style submenu
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    GtkWidget *cursor_menu = gtk_menu_new();

    GtkWidget *block_item =
        gtk_radio_menu_item_new_with_label(NULL, "Block");
    GSList *shape_group =
        gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(block_item));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(block_item), TRUE);
    g_signal_connect(block_item, "toggled",
                     G_CALLBACK(on_cursor_shape_block), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(cursor_menu), block_item);

    GtkWidget *underline_item =
        gtk_radio_menu_item_new_with_label(shape_group, "Underline");
    g_signal_connect(underline_item, "toggled",
                     G_CALLBACK(on_cursor_shape_underline), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(cursor_menu), underline_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(cursor_menu),
        gtk_separator_menu_item_new());

    GtkWidget *blink_item =
        gtk_check_menu_item_new_with_label("Blink");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(blink_item), FALSE);
    g_signal_connect(blink_item, "toggled",
                     G_CALLBACK(on_cursor_blink_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(cursor_menu), blink_item);

    GtkWidget *cursor_mi = gtk_menu_item_new_with_label("Cursor Style");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(cursor_mi), cursor_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), cursor_mi);

    GtkWidget *mi = gtk_menu_item_new_with_label("View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    return mi;
}

static GtkWidget *build_tools_menu(void) {
    GtkWidget *menu = gtk_menu_new();

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Command Palette", G_CALLBACK(on_noop), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Hex Dump Toggle", G_CALLBACK(on_noop), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Timestamp Toggle", G_CALLBACK(on_noop), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Terminal Reset", G_CALLBACK(on_noop), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Break Signal", G_CALLBACK(on_noop), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Macros...", G_CALLBACK(on_noop), NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Settings...", G_CALLBACK(on_noop), NULL));

    GtkWidget *mi = gtk_menu_item_new_with_label("Tools");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    return mi;
}

// ---------------------------------------------------------------------------
// Help menu callbacks (G12b)
// ---------------------------------------------------------------------------

static void on_keyboard_shortcuts(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *parent = GTK_WIDGET(user_data);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Keyboard Shortcuts", GTK_WINDOW(parent),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);
    gtk_box_set_spacing(GTK_BOX(content), 4);

    static const char *shortcuts[][2] = {
        {"Ctrl+Shift+O", "Connect / Disconnect"},
        {"Ctrl+Shift+D", "Rescan Ports"},
        {"Ctrl+Shift+N", "Next Device"},
        {"Ctrl+Shift+S", "Serial Config\342\200\246"},
        {"Ctrl+Shift+C", "Copy"},
        {"Ctrl+Shift+L", "Clear Screen"},
        {"Ctrl+Shift+T", "Send Text\342\200\246"},
        {"Ctrl+Shift+F", "Send Text File\342\200\246"},
        {"Ctrl+Shift+H", "Send Intel HEX\342\200\246"},
        {"Ctrl+Shift+X", "XMODEM Send"},
        {"Ctrl+Shift+R", "XMODEM Receive"},
        {"Ctrl+Shift+G", "Start / Stop Log"},
        {"Ctrl+Shift+B", "Toggle Toolbar"},
        {"Page Up / Down", "Scroll History"},
        {"Ctrl+Q", "Quit"},
    };
    enum { NUM_SHORTCUTS = sizeof(shortcuts) / sizeof(shortcuts[0]) };

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);

    for (int i = 0; i < NUM_SHORTCUTS; i++) {
        char markup[128];
        snprintf(markup, sizeof(markup), "<b>%s</b>", shortcuts[i][0]);
        GtkWidget *key_label = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(key_label), markup);
        gtk_widget_set_halign(key_label, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), key_label, 0, i, 1, 1);

        GtkWidget *desc_label = gtk_label_new(shortcuts[i][1]);
        gtk_widget_set_halign(desc_label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), desc_label, 1, i, 1, 1);
    }

    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void on_about(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    GtkWidget *parent = GTK_WIDGET(user_data);

    const char *authors[] = {"p43lz3r", NULL};

    gtk_show_about_dialog(GTK_WINDOW(parent),
        "program-name",  "ttcore-gui",
        "version",       "0.8",
        "comments",      "Serial terminal for retro devices\n\n"
                         "Linux port of TeraTerm 5 core \342\200\224 "
                         "headless C99 library with GTK3 frontend\n\n"
                         "Based on the amazing TeraTerm 5 "
                         "by TeraTerm Project",
        "copyright",     "\302\251 2026 p43lz3r",
        "license-type",  GTK_LICENSE_MIT_X11,
        "authors",       authors,
        "website-label", "Project Website",
        NULL);
}

static GtkWidget *build_help_menu(GtkWidget *window) {
    GtkWidget *menu = gtk_menu_new();

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("Keyboard Shortcuts\342\200\246",
                   G_CALLBACK(on_keyboard_shortcuts), window));

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
        make_item("About", G_CALLBACK(on_about), window));

    GtkWidget *mi = gtk_menu_item_new_with_label("Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    return mi;
}

// ---------------------------------------------------------------------------
// Next Device accelerator closure (Ctrl+Shift+N)
// ---------------------------------------------------------------------------

static gboolean on_next_device_accel(GtkAccelGroup *group, GObject *accel_obj,
                                      guint keyval, GdkModifierType modifier) {
    (void)group;
    (void)accel_obj;
    (void)keyval;
    (void)modifier;
    if (serial_conn_is_open()) return TRUE;
    toolbar_next_port();
    return TRUE;
}

// ---------------------------------------------------------------------------
// Hidden top-level menu items
// ---------------------------------------------------------------------------
static GtkWidget *g_menu_tools = NULL;

GtkWidget *menu_create(GtkApplication *app, GtkAccelGroup *accel_group,
                        GtkWidget *window, GtkWidget *revealer) {
    GtkWidget *bar = gtk_menu_bar_new();

    gtk_menu_shell_append(GTK_MENU_SHELL(bar),
        build_file_menu(app, accel_group));
    gtk_menu_shell_append(GTK_MENU_SHELL(bar),
        build_edit_menu(accel_group));
    gtk_menu_shell_append(GTK_MENU_SHELL(bar),
        build_connection_menu(accel_group, window));
    gtk_menu_shell_append(GTK_MENU_SHELL(bar),
        build_transfer_menu(accel_group, window));
    gtk_menu_shell_append(GTK_MENU_SHELL(bar),
        build_view_menu(accel_group, window, revealer));

    g_menu_tools = build_tools_menu();
    gtk_widget_set_no_show_all(g_menu_tools, TRUE);
    gtk_widget_hide(g_menu_tools);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), g_menu_tools);

    gtk_menu_shell_append(GTK_MENU_SHELL(bar), build_help_menu(window));

    // Next Device: Ctrl+Shift+N (no menu item — toolbar action)
    GClosure *next_closure = g_cclosure_new(
        G_CALLBACK(on_next_device_accel), NULL, NULL);
    gtk_accel_group_connect(accel_group, GDK_KEY_N,
        (GdkModifierType)(GDK_CONTROL_MASK | GDK_SHIFT_MASK),
        GTK_ACCEL_VISIBLE, next_closure);

    return bar;
}
