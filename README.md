# libconfig

A GSettings-inspired configuration library for C, featuring schema validation, type-safe access, change callbacks, file persistence, **multi-process IPC via config daemon**, and a **gsettings-style CLI tool**.

## API

| GSettings | libconfig |
|---|---|
| `GSettingsSchema` | `config_schema_entry_t[]` |
| `g_settings_new()` | `config_new()` / `config_new_with_daemon()` |
| `g_settings_get_int()` | `config_get_int()` / `config_get_string()` / `config_get_double()` / `config_get_boolean()` |
| `g_settings_set_int()` | `config_set_*()` |
| `g_signal_connect("changed")` | `config_connect_changed()` |
| `g_settings_reset()` | `config_reset()` / `config_reset_all()` |
| `g_settings_sync()` | `config_sync()` |
| `dconf` (IPC service) | `configd` (config daemon) |
| `gsettings` CLI | `config-cli` |

## Project Structure

```
.
├── include/config.h        # Public API header
├── src/
│   ├── config.c            # Core logic: lifecycle, access, callbacks, sync
│   ├── schema.h/c          # Schema: key definitions, type validation, defaults
│   ├── backend.h/c         # Backend: file persistence with atomic rename
│   ├── store.h/c           # Storage: hashmap with typed value union
│   ├── ipc.h/c             # IPC client: Unix socket communication protocol
│   └── daemon.c            # Config daemon: multi-process central hub
├── tools/
│   └── config-cli.c        # gsettings-style CLI tool
├── examples/
│   ├── example.c           # Direct file-mode usage example
│   └── daemon_test.c       # Multi-process daemon mode demo
└── Makefile
```

## Key Features

- **Schema Validation**: keys must exist in schema, types must match, numeric ranges enforced
- **Change Callbacks**: registered callbacks fire on value changes
- **Atomic Persistence**: writes to `.tmp` then `rename` for crash safety
- **Storage Format**: `key:{type}:{value}` (e.g. `window-width:i:1920`)
- **Multi-Process IPC**: config daemon (`configd`) with Unix socket, supports cross-process change notifications
- **CLI Tool**: gsettings-style `config-cli` with get/set/reset/list-keys/monitor commands
- **Zero Dependencies**: pure C11 + POSIX

## Build

```
make
```

Outputs:
- `lib/libconfig.a` (static library)
- `lib/libconfig.so` (shared library)
- `build/example` (file-mode demo)
- `build/configd` (config daemon)
- `build/config-cli` (CLI tool)

## Quick Example

### File Mode (standalone)

```c
#include "config.h"

static const config_schema_entry_t schema[] = {
    { "width",  CONFIG_TYPE_INT,    "i:800", "Window width",  400, 3840 },
    { "theme",  CONFIG_TYPE_STRING, "s:dark", "UI theme",     0.0, 0.0  },
    { "fullscreen", CONFIG_TYPE_BOOLEAN, "b:0", "Fullscreen", 0.0, 0.0 },
};

config_t *cfg = config_new("myapp", "./config", schema,
                           sizeof(schema) / sizeof(schema[0]));
config_set_int(cfg, "width", 1920);
printf("%d\n", config_get_int(cfg, "width"));  // 1920
config_sync(cfg);  // persist to ./config/myapp.conf
config_free(cfg);
```

### Daemon Mode (multi-process)

```c
#include "config.h"

static void on_changed(config_t *cfg, const char *key, void *data) {
    printf("[%s] changed\n", key);
}

config_t *cfg = config_new_with_daemon("myapp", "/tmp/configd.sock",
                                       "./config", schema, schema_len);
config_connect_changed(cfg, on_changed, NULL);
config_set_string(cfg, "theme", "dark");
config_sync(cfg);

// Poll for cross-process notifications
config_daemon_process_events(cfg);

config_free(cfg);
```

### CLI Tool

```bash
# Start the daemon
./build/configd -d ./config-data -s /tmp/configd.sock &

# Get/set values
config-cli -s /tmp/configd.sock set myapp theme dark -t string
config-cli -s /tmp/configd.sock get myapp theme
# Output: 'dark'

# List all keys
config-cli -s /tmp/configd.sock list-keys myapp
# Output:
#   width     int      800
#   theme     string   'dark'

# Monitor real-time changes
config-cli -s /tmp/configd.sock monitor myapp
```
