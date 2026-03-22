// 2026-03-20 v0.1.0
// config.c — GKeyFile-based persistent configuration (~/.config/ttcore/config.ini).
// Phase G6: session persistence for connection, appearance, window geometry.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#include "config.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static GKeyFile *g_kf = NULL;
static char g_path[512] = "";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void ensure_keyfile(void) {
    if (g_kf) return;
    g_kf = g_key_file_new();

    // Build path: ~/.config/ttcore/config.ini
    const char *cfg_dir = g_get_user_config_dir();  // XDG_CONFIG_HOME or ~/.config
    snprintf(g_path, sizeof(g_path), "%s/ttcore", cfg_dir);
    mkdir(g_path, 0755);
    snprintf(g_path, sizeof(g_path), "%s/ttcore/config.ini", cfg_dir);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void config_load(void) {
    ensure_keyfile();
    // Load from disk; ignore errors (file may not exist yet)
    g_key_file_load_from_file(g_kf, g_path, G_KEY_FILE_KEEP_COMMENTS, NULL);
}

void config_save(void) {
    ensure_keyfile();
    gsize len = 0;
    gchar *data = g_key_file_to_data(g_kf, &len, NULL);
    if (data) {
        g_file_set_contents(g_path, data, (gssize)len, NULL);
        g_free(data);
    }
}

const char *config_get_string(const char *group, const char *key,
                               const char *fallback) {
    ensure_keyfile();
    gchar *val = g_key_file_get_string(g_kf, group, key, NULL);
    if (!val) return fallback;
    // GKeyFile owns the string until the key is changed or keyfile freed.
    // We return it directly — valid until next set/load for this key.
    // Note: g_key_file_get_string returns a newly allocated string.
    // We store it back so it stays alive (replace existing).
    g_key_file_set_string(g_kf, group, key, val);
    g_free(val);
    // Re-fetch: now g_key_file owns the internal copy.
    // Unfortunately GKeyFile doesn't expose internal storage, so we use
    // a static buffer for the last returned string.
    static char buf[512];
    val = g_key_file_get_string(g_kf, group, key, NULL);
    if (val) {
        snprintf(buf, sizeof(buf), "%s", val);
        g_free(val);
        return buf;
    }
    return fallback;
}

void config_set_string(const char *group, const char *key, const char *value) {
    ensure_keyfile();
    g_key_file_set_string(g_kf, group, key, value);
}

int config_get_int(const char *group, const char *key, int fallback) {
    ensure_keyfile();
    GError *err = NULL;
    int val = g_key_file_get_integer(g_kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return fallback;
    }
    return val;
}

void config_set_int(const char *group, const char *key, int value) {
    ensure_keyfile();
    g_key_file_set_integer(g_kf, group, key, value);
}

bool config_get_bool(const char *group, const char *key, bool fallback) {
    ensure_keyfile();
    GError *err = NULL;
    gboolean val = g_key_file_get_boolean(g_kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return fallback;
    }
    return val;
}

void config_set_bool(const char *group, const char *key, bool value) {
    ensure_keyfile();
    g_key_file_set_boolean(g_kf, group, key, value);
}

double config_get_double(const char *group, const char *key, double fallback) {
    ensure_keyfile();
    GError *err = NULL;
    double val = g_key_file_get_double(g_kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return fallback;
    }
    return val;
}

void config_set_double(const char *group, const char *key, double value) {
    ensure_keyfile();
    g_key_file_set_double(g_kf, group, key, value);
}
