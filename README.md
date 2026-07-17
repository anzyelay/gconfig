# libconfig

A GSettings-inspired configuration library for C, featuring schema validation, type-safe access, change callbacks, and file persistence.

## API

| GSettings | libconfig |
|---|---|
| `GSettingsSchema` | `config_schema_entry_t[]` |
| `g_settings_new()` | `config_new()` |
| `g_settings_get_int()` | `config_get_int()` / `config_get_string()` / `config_get_double()` / `config_get_boolean()` |
| `g_settings_set_int()` | `config_set_*()` |
| `g_signal_connect("changed")` | `config_connect_changed()` |
| `g_settings_reset()` | `config_reset()` / `config_reset_all()` |
| `g_settings_sync()` | `config_sync()` |

## Project Structure

```
.
├── include/config.h        # Public API header
├── src/
│   ├── config.c            # Core logic: lifecycle, access, callbacks, sync
│   ├── schema.h/c          # Schema: key definitions, type validation, defaults
│   ├── backend.h/c         # Backend: file persistence with atomic rename
│   └── store.h/c           # Storage: hashmap with typed value union
├── examples/example.c      # Usage example
└── Makefile                # Build: static library, shared library, example
```

## Key Features

- **Schema Validation**: keys must exist in schema, types must match, numeric ranges enforced
- **Change Callbacks**: registered callbacks fire on value changes
- **Atomic Persistence**: writes to `.tmp` then `rename` for crash safety
- **Storage Format**: `key:{type}:{value}` (e.g. `window-width:i:1920`)
- **Zero Dependencies**: pure C11 + POSIX

## Build

```
make
```

Outputs:
- `lib/libconfig.a` (static library)
- `lib/libconfig.so` (shared library)
- `build/example` (demo program)

## Quick Example

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
