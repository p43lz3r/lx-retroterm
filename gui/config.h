// 2026-03-20 v0.1.0
// config.h — GKeyFile-based persistent configuration (~/.config/ttcore/config.ini).
// Phase G6: session persistence for connection, appearance, window geometry.
//
// Copyright (C) 2026 ttcore-port contributors — BSD 3-Clause

#ifndef GUI_CONFIG_H_
#define GUI_CONFIG_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Load config from ~/.config/ttcore/config.ini.
// Creates directory and file if they don't exist.
// Safe to call multiple times (reloads from disk).
void config_load(void);

// Save current config to disk.
void config_save(void);

// String getters/setters.
// Returned string is valid until the next set/load call for the same key.
const char *config_get_string(const char *group, const char *key,
                               const char *fallback);
void config_set_string(const char *group, const char *key, const char *value);

// Integer getters/setters.
int config_get_int(const char *group, const char *key, int fallback);
void config_set_int(const char *group, const char *key, int value);

// Boolean getters/setters.
bool config_get_bool(const char *group, const char *key, bool fallback);
void config_set_bool(const char *group, const char *key, bool value);

// Double getters/setters (for GdkRGBA channels).
double config_get_double(const char *group, const char *key, double fallback);
void config_set_double(const char *group, const char *key, double value);

#ifdef __cplusplus
}
#endif

#endif  // GUI_CONFIG_H_
