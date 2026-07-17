#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_changed(config_t *cfg, const char *key, void *user_data) {
    (void)cfg;
    (void)user_data;
    printf("[callback] key '%s' changed\n", key);
}

int main(void) {
    static const config_schema_entry_t app_schema[] = {
        { "window-width",   CONFIG_TYPE_INT,     "i:800",   "Window width in pixels",   400, 3840 },
        { "window-height",  CONFIG_TYPE_INT,     "i:600",   "Window height in pixels",  300, 2160 },
        { "theme",          CONFIG_TYPE_STRING,  "s:light", "UI theme name",             0.0, 0.0 },
        { "font-size",      CONFIG_TYPE_DOUBLE,  "d:12.0",  "Font size in points",      8.0, 72.0 },
        { "fullscreen",     CONFIG_TYPE_BOOLEAN, "b:0",     "Start in fullscreen mode", 0.0, 0.0 },
        { "username",       CONFIG_TYPE_STRING,  "s:user",  "Default username",          0.0, 0.0 },
    };

    config_t *cfg = config_new("myapp", "./config-data",
                               app_schema,
                               sizeof(app_schema) / sizeof(app_schema[0]));
    if (!cfg) {
        fprintf(stderr, "Failed to create config\n");
        return 1;
    }

    config_connect_changed(cfg, on_changed, NULL);

    printf("=== Initial values (from schema defaults or saved file) ===\n");
    printf("window-width  = %d\n", config_get_int(cfg, "window-width"));
    printf("window-height = %d\n", config_get_int(cfg, "window-height"));
    printf("theme         = %s\n", config_get_string(cfg, "theme"));
    printf("font-size     = %.1f\n", config_get_double(cfg, "font-size"));
    printf("fullscreen    = %s\n", config_get_boolean(cfg, "fullscreen") ? "true" : "false");
    printf("username      = %s\n", config_get_string(cfg, "username"));

    printf("\n=== Modifying values ===\n");
    config_set_int(cfg, "window-width", 1920);
    config_set_int(cfg, "window-height", 1080);
    config_set_string(cfg, "theme", "dark");
    config_set_double(cfg, "font-size", 14.0);
    config_set_boolean(cfg, "fullscreen", 1);

    printf("window-width  = %d\n", config_get_int(cfg, "window-width"));
    printf("window-height = %d\n", config_get_int(cfg, "window-height"));
    printf("theme         = %s\n", config_get_string(cfg, "theme"));
    printf("font-size     = %.1f\n", config_get_double(cfg, "font-size"));
    printf("fullscreen    = %s\n", config_get_boolean(cfg, "fullscreen") ? "true" : "false");

    printf("\n=== Type validation (expect error) ===\n");
    config_set_string(cfg, "window-width", "abc");

    printf("\n=== Range validation (expect error) ===\n");
    config_set_int(cfg, "font-size", 100);

    printf("\n=== Reset single key ===\n");
    config_reset(cfg, "theme");
    printf("theme (reset)  = %s\n", config_get_string(cfg, "theme"));

    printf("\n=== List all keys ===\n");
    config_list_keys(cfg);

    printf("\n=== Reset all to defaults ===\n");
    config_reset_all(cfg);
    printf("window-width  = %d\n", config_get_int(cfg, "window-width"));
    printf("theme         = %s\n", config_get_string(cfg, "theme"));
    printf("fullscreen    = %s\n", config_get_boolean(cfg, "fullscreen") ? "true" : "false");

    printf("\n=== Persist to file ===\n");
    config_sync(cfg);
    printf("Saved to config-data/myapp.conf\n");

    config_free(cfg);
    printf("\nDone.\n");
    return 0;
}
