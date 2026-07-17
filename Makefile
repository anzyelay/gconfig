CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11 -g -O2 -fPIC -D_POSIX_C_SOURCE=200809L
INCLUDES = -Iinclude -Isrc
AR       = ar
ARFLAGS  = rcs

SRC_DIR  = src
BUILD_DIR = build
LIB_DIR  = lib

SRCS     = $(SRC_DIR)/config.c $(SRC_DIR)/schema.c $(SRC_DIR)/backend.c $(SRC_DIR)/store.c $(SRC_DIR)/ipc.c
OBJS     = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

LIB_STATIC  = $(LIB_DIR)/libconfig.a
LIB_SHARED  = $(LIB_DIR)/libconfig.so
EXAMPLE     = $(BUILD_DIR)/example
DAEMON      = $(BUILD_DIR)/configd

.PHONY: all clean example http-server daemon

all: $(LIB_STATIC) $(LIB_SHARED) example daemon

daemon: $(BUILD_DIR)/configd.o
	$(CC) $(CFLAGS) $(INCLUDES) $(filter-out $(BUILD_DIR)/config.o, $(OBJS)) $(BUILD_DIR)/configd.o -o $(DAEMON)

$(BUILD_DIR)/configd.o: $(SRC_DIR)/daemon.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

http-server: $(OBJS) examples/http_server.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) examples/http_server.c $(filter-out $(BUILD_DIR)/ipc.o, $(OBJS)) -o $(BUILD_DIR)/http_server

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIB_DIR):
	@mkdir -p $(LIB_DIR)

$(LIB_STATIC): $(OBJS) | $(LIB_DIR)
	$(AR) $(ARFLAGS) $@ $(OBJS)

$(LIB_SHARED): $(OBJS) | $(LIB_DIR)
	$(CC) -shared -o $@ $(OBJS)

example: $(LIB_STATIC) examples/example.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) examples/example.c -L$(LIB_DIR) -lconfig -o $(EXAMPLE) -Wl,-rpath,$(LIB_DIR)

clean:
	rm -rf $(BUILD_DIR) $(LIB_DIR) config-data
