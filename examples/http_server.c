#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include "config.h"

#define PORT 8080
#define MAX_CLIENTS 10
#define BUFSIZE 8192

static config_t *cfg;

static void on_changed(config_t *c, const char *key, void *ud) {
    (void)c; (void)ud;
    printf("[changed] %s\n", key);
}

static const config_schema_entry_t app_schema[] = {
    {"window-width",  CONFIG_TYPE_INT,     "i:800",   "Window width in pixels",  400, 3840},
    {"window-height", CONFIG_TYPE_INT,     "i:600",   "Window height in pixels", 300, 2160},
    {"theme",         CONFIG_TYPE_STRING,  "s:light", "UI theme name",           0.0, 0.0},
    {"font-size",     CONFIG_TYPE_DOUBLE,  "d:12.0",  "Font size in points",    8.0, 72.0},
    {"fullscreen",    CONFIG_TYPE_BOOLEAN, "b:0",     "Start in fullscreen",    0.0, 0.0},
    {"username",      CONFIG_TYPE_STRING,  "s:user",  "Default username",       0.0, 0.0},
};

static void url_decode(char *dst, const char *src, size_t maxlen) {
    char *d = dst;
    const char *s = src;
    while (*s && (size_t)(d - dst) < maxlen - 1) {
        if (*s == '%' && s[1] && s[2]) {
            int hi, lo;
            if (sscanf(s + 1, "%2x", &hi) == 1) {
                *d++ = (char)hi;
                s += 3;
                continue;
            }
        }
        if (*s == '+') { *d++ = ' '; s++; continue; }
        *d++ = *s++;
    }
    *d = '\0';
}

static const char *get_param(const char *req, const char *name) {
    static char buf[512];
    const char *start = req;
    while (*start) {
        if (*start == '?' || *start == '&') {
            start++;
            if (strncmp(start, name, strlen(name)) == 0) {
                const char *val = start + strlen(name);
                if (*val == '=') {
                    val++;
                    const char *end = strpbrk(val, " &");
                    size_t len = end ? (size_t)(end - val) : strlen(val);
                    if (len < sizeof(buf)) {
                        memcpy(buf, val, len);
                        buf[len] = '\0';
                        url_decode(buf, buf, sizeof(buf));
                    }
                    return buf;
                }
            }
        }
        start++;
    }
    return NULL;
}

static void send_response(int fd, int code, const char *ctype, const char *body) {
    char hdr[BUFSIZE];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\nContent-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, ctype, strlen(body));
    write(fd, hdr, n);
    write(fd, body, strlen(body));
}

static void send_json(int fd, const char *json) {
    send_response(fd, 200, "application/json", json);
}

static void handle_api_get(int fd, const char *req) {
    char body[BUFSIZE];
    const char *key = get_param(req, "key");
    if (!key) {
        send_json(fd, "{\"error\":\"missing key parameter\"}");
        return;
    }
    size_t count = sizeof(app_schema) / sizeof(app_schema[0]);
    const config_schema_entry_t *entry = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(app_schema[i].key, key) == 0) { entry = &app_schema[i]; break; }
    }
    if (!entry) {
        snprintf(body, sizeof(body), "{\"error\":\"unknown key '%s'\"}", key);
        send_json(fd, body);
        return;
    }
    switch (entry->type) {
    case CONFIG_TYPE_INT:
        snprintf(body, sizeof(body),
            "{\"key\":\"%s\",\"type\":\"int\",\"value\":%d}", key,
            config_get_int(cfg, key));
        break;
    case CONFIG_TYPE_DOUBLE:
        snprintf(body, sizeof(body),
            "{\"key\":\"%s\",\"type\":\"double\",\"value\":%.15g}", key,
            config_get_double(cfg, key));
        break;
    case CONFIG_TYPE_STRING:
        snprintf(body, sizeof(body),
            "{\"key\":\"%s\",\"type\":\"string\",\"value\":\"%s\"}", key,
            config_get_string(cfg, key));
        break;
    case CONFIG_TYPE_BOOLEAN:
        snprintf(body, sizeof(body),
            "{\"key\":\"%s\",\"type\":\"boolean\",\"value\":%s}", key,
            config_get_boolean(cfg, key) ? "true" : "false");
        break;
    default: break;
    }
    send_json(fd, body);
}

static void handle_api_keys(int fd) {
    char body[BUFSIZE], *p = body;
    p += snprintf(p, sizeof(body) - (p - body), "{\"keys\":[");
    const config_schema_entry_t *entries = schema_get_all(app_schema);
    size_t count = 6;
    int first = 1;
    for (size_t i = 0; i < count; i++) {
        const config_schema_entry_t *e = &entries[i];
        const char *type_str = "unknown";
        switch (e->type) {
        case CONFIG_TYPE_INT: type_str = "int"; break;
        case CONFIG_TYPE_DOUBLE: type_str = "double"; break;
        case CONFIG_TYPE_STRING: type_str = "string"; break;
        case CONFIG_TYPE_BOOLEAN: type_str = "boolean"; break;
        default: break;
        }
        char val_buf[256];
        switch (e->type) {
        case CONFIG_TYPE_INT:
            snprintf(val_buf, sizeof(val_buf), "%d", config_get_int(cfg, e->key)); break;
        case CONFIG_TYPE_DOUBLE:
            snprintf(val_buf, sizeof(val_buf), "%.15g", config_get_double(cfg, e->key)); break;
        case CONFIG_TYPE_STRING:
            snprintf(val_buf, sizeof(val_buf), "%s", config_get_string(cfg, e->key)); break;
        case CONFIG_TYPE_BOOLEAN:
            snprintf(val_buf, sizeof(val_buf), "%s",
                     config_get_boolean(cfg, e->key) ? "true" : "false"); break;
        default: strcpy(val_buf, "null"); break;
        }
        p += snprintf(p, sizeof(body) - (p - body),
            "%s{\"key\":\"%s\",\"type\":\"%s\",\"value\":\"%s\",\"desc\":\"%s\"}",
            first ? "" : ",",
            e->key, type_str, val_buf, e->description ? e->description : "");
        first = 0;
    }
    p += snprintf(p, sizeof(body) - (p - body), "]}");
    send_json(fd, body);
}

static void handle_api_set(int fd, const char *req) {
    const char *key = get_param(req, "key");
    const char *type = get_param(req, "type");
    const char *val = get_param(req, "value");
    if (!key || !type || !val) {
        send_json(fd, "{\"error\":\"missing key/type/value parameters\"}");
        return;
    }
    int ok = 0;
    if (strcmp(type, "int") == 0) {
        ok = config_set_int(cfg, key, atoi(val)) == 0;
    } else if (strcmp(type, "double") == 0) {
        ok = config_set_double(cfg, key, atof(val)) == 0;
    } else if (strcmp(type, "string") == 0) {
        ok = config_set_string(cfg, key, val) == 0;
    } else if (strcmp(type, "boolean") == 0) {
        ok = config_set_boolean(cfg, key, strcmp(val, "true") == 0 || strcmp(val, "1") == 0) == 0;
    }
    char body[256];
    if (ok) {
        config_sync(cfg);
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"key\":\"%s\"}", key);
    } else {
        snprintf(body, sizeof(body),
            "{\"error\":\"failed to set '%s' (type mismatch or validation error)\"}", key);
    }
    send_json(fd, body);
}

static void handle_api_reset(int fd, const char *req) {
    const char *key = get_param(req, "key");
    if (!key) {
        config_reset_all(cfg);
        send_json(fd, "{\"status\":\"ok\",\"message\":\"all keys reset\"}");
    } else {
        config_reset(cfg, key);
        char body[256];
        snprintf(body, sizeof(body),
            "{\"status\":\"ok\",\"key\":\"%s\",\"message\":\"reset to default\"}", key);
        send_json(fd, body);
    }
    config_sync(cfg);
}

static const char *INDEX_HTML =
"<!DOCTYPE html>\n"
"<html lang=\"zh\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>libconfig - GSettings-style C 配置库演示</title>\n"
"<style>\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"background:#f0f2f5;color:#333;line-height:1.6}\n"
".container{max-width:800px;margin:40px auto;padding:0 20px}\n"
"h1{font-size:24px;margin-bottom:8px;color:#1a1a2e}\n"
".subtitle{color:#666;margin-bottom:24px}\n"
".card{background:#fff;border-radius:8px;box-shadow:0 1px 3px rgba(0,0,0,0.1);"
"padding:20px;margin-bottom:16px}\n"
".card h2{font-size:16px;margin-bottom:12px;color:#16213e}\n"
"table{width:100%%;border-collapse:collapse}\n"
"th,td{padding:8px 12px;text-align:left;border-bottom:1px solid #eee}\n"
"th{color:#888;font-weight:500;font-size:13px}\n"
"td{font-size:14px}\n"
".row{cursor:pointer;transition:background .15s}\n"
".row:hover{background:#f7f8fa}\n"
".editing{background:#e8f4fd!important}\n"
"input,select{padding:4px 8px;border:1px solid #d9d9d9;border-radius:4px;"
"font-size:14px;width:100%%}\n"
"button{padding:6px 16px;border:none;border-radius:4px;cursor:pointer;"
"font-size:14px;margin-right:8px}\n"
".btn-save{background:#1890ff;color:#fff}\n"
".btn-save:hover{background:#40a9ff}\n"
".btn-cancel{background:#f0f0f0;color:#333}\n"
".btn-cancel:hover{background:#d9d9d9}\n"
".btn-reset{background:#fff;color:#ff4d4f;border:1px solid #ff4d4f}\n"
".btn-reset:hover{background:#fff1f0}\n"
".toast{position:fixed;top:20px;right:20px;padding:10px 20px;border-radius:4px;"
"color:#fff;font-size:14px;display:none;z-index:1000}\n"
".toast-ok{background:#52c41a}\n"
".toast-err{background:#ff4d4f}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"container\">\n"
"<h1>libconfig</h1>\n"
"<p class=\"subtitle\">类 GSettings 的 C 语言配置库 -- 在线演示</p>\n"
"<div class=\"card\">\n"
"<h2>配置项</h2>\n"
"<table><thead><tr>"
"<th>Key</th><th>类型</th><th>值</th><th>描述</th><th>操作</th>"
"</tr></thead>\n"
"<tbody id=\"keys-body\"><tr><td colspan=\"5\">加载中...</td></tr></tbody>\n"
"</table>\n"
"</div>\n"
"<div class=\"card\">\n"
"<h2>API</h2>\n"
"<pre style=\"font-size:13px;color:#555;overflow-x:auto\">\n"
"GET  /api/keys             列出所有配置项\n"
"GET  /api/get?key=xxx      获取单个配置项\n"
"POST /api/set?key=xxx&amp;type=int|double|string|boolean&amp;value=yyy  设置配置项\n"
"POST /api/reset            重置所有配置项\n"
"POST /api/reset?key=xxx    重置单个配置项\n"
"</pre>\n"
"</div>\n"
"</div>\n"
"<div id=\"toast\" class=\"toast\"></div>\n"
"<script>\n"
"var t=document.getElementById('toast');\n"
"function toast(m,ok){t.textContent=m;t.className='toast toast-'+(ok?'ok':'err');"
"t.style.display='block';setTimeout(function(){t.style.display='none'},2000)}\n"
"function loadKeys(){\n"
"fetch('/api/keys').then(function(r){return r.json()}).then(function(d){\n"
"var b=document.getElementById('keys-body');\n"
"b.innerHTML=d.keys.map(function(k,i){\n"
"return'<tr class=\"row\" id=\"row-'+i+'\">'\n"
"+'<td><code>'+k.key+'</code></td>'\n"
"+'<td>'+k.type+'</td>'\n"
"+'<td id=\"val-'+i+'\">'+k.value+'</td>'\n"
"+'<td style=\"color:#888;font-size:13px\">'+k.desc+'</td>'\n"
"+'<td>'\n"
"+'<button class=\"btn-reset\" onclick=\"resetKey(\\''+k.key+'\\')\">重置</button>'\n"
"+'</td></tr>';\n"
"}).join('');\n"
"}).catch(function(e){toast('加载失败: '+e,false)})\n"
"}\n"
"function resetKey(key){\n"
"fetch('/api/reset?key='+encodeURIComponent(key),{method:'POST'})\n"
".then(function(r){return r.json()}).then(function(d){\n"
"if(d.status==='ok'){loadKeys();toast(key+' 已重置',true)}\n"
"else{toast(d.error,false)}\n"
"})\n"
"}\n"
"loadKeys();\n"
"</script>\n"
"</body>\n"
"</html>\n";

static void handle_root(int fd) {
    send_response(fd, 200, "text/html", INDEX_HTML);
}

static void handle_404(int fd) {
    send_response(fd, 404, "text/plain", "404 Not Found");
}

static void handle_request(int fd) {
    char buf[BUFSIZE];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    if (strncmp(buf, "GET / ", 6) == 0 || strncmp(buf, "GET / HTTP", 10) == 0 ||
        strncmp(buf, "GET /index", 10) == 0 || strncmp(buf, "GET /favicon", 12) == 0) {
        if (strncmp(buf, "GET /favicon", 12) == 0) { handle_404(fd); return; }
        handle_root(fd);
    } else if (strncmp(buf, "GET /api/keys", 13) == 0) {
        handle_api_keys(fd);
    } else if (strncmp(buf, "GET /api/get", 12) == 0) {
        handle_api_get(fd, buf + 4);
    } else if (strncmp(buf, "POST /api/set", 13) == 0) {
        handle_api_set(fd, buf + 5);
    } else if (strncmp(buf, "POST /api/reset", 15) == 0) {
        handle_api_reset(fd, buf + 5);
    } else {
        handle_404(fd);
    }
    close(fd);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    cfg = config_new("myapp", "/tmp/config-data",
                     app_schema, sizeof(app_schema) / sizeof(app_schema[0]));
    if (!cfg) { fprintf(stderr, "Failed to create config\n"); return 1; }
    config_connect_changed(cfg, on_changed, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen"); return 1;
    }

    printf("libconfig HTTP server listening on http://0.0.0.0:%d\n", PORT);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;
        handle_request(client_fd);
    }

    config_free(cfg);
    return 0;
}
