#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void on_changed(config_t *cfg, const char *key, void *user_data) {
    const char *proc = (const char *)user_data;
    (void)cfg;
    printf("[%s] CALLBACK: '%s' changed\n", proc, key);
}

int main(int argc, char *argv[]) {
    const char *proc_name = argc > 1 ? argv[1] : "proc";
    int is_setter = (argc > 2 && strcmp(argv[2], "set") == 0);

    static const config_schema_entry_t schema[] = {
        { "theme",     CONFIG_TYPE_STRING,  "s:light", "UI theme", 0.0, 0.0 },
        { "font-size", CONFIG_TYPE_DOUBLE,  "d:12.0",  "Font size", 8.0, 72.0 },
        { "fullscreen",CONFIG_TYPE_BOOLEAN, "b:0",     "Fullscreen", 0.0, 0.0 },
    };

    config_t *cfg = config_new_with_daemon("testapp",
                                           "/tmp/configd-test.sock",
                                           "./config-data",
                                           schema,
                                           sizeof(schema) / sizeof(schema[0]));
    if (!cfg) {
        fprintf(stderr, "[%s] Failed to create config\n", proc_name);
        return 1;
    }

    printf("[%s] Connected: %s\n", proc_name,
           config_daemon_is_connected(cfg) ? "yes" : "no");

    config_connect_changed(cfg, on_changed, (void *)proc_name);

    printf("[%s] Initial: theme=%s, font-size=%.1f, fullscreen=%s\n",
           proc_name,
           config_get_string(cfg, "theme"),
           config_get_double(cfg, "font-size"),
           config_get_boolean(cfg, "fullscreen") ? "true" : "false");

    if (is_setter) {
        printf("[%s] Setting values...\n", proc_name);
        config_set_string(cfg, "theme", "dark");
        config_set_double(cfg, "font-size", 18.0);
        config_set_boolean(cfg, "fullscreen", 1);
        config_sync(cfg);
        printf("[%s] Done setting. Waiting 2s for others to see changes...\n", proc_name);
        sleep(2);
    } else {
        printf("[%s] Listening for notifications (5s)...\n", proc_name);
        for (int i = 0; i < 25; i++) {
            config_daemon_process_events(cfg);
            usleep(200000);
        }
    }

    printf("[%s] Final: theme=%s, font-size=%.1f, fullscreen=%s\n",
           proc_name,
           config_get_string(cfg, "theme"),
           config_get_double(cfg, "font-size"),
           config_get_boolean(cfg, "fullscreen") ? "true" : "false");

    config_free(cfg);
    printf("[%s] Done.\n", proc_name);
    return 0;
}
