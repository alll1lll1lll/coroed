SRC_DIR   = source
BUILD_DIR = build
OBJ_DIR   = $(BUILD_DIR)/obj
BIN_DIR   = $(BUILD_DIR)/bin

COMPILER           ?= clang
OPTIMIZATION_LEVEL ?= -O3
SANITIZERS         ?=

CC        = $(COMPILER)

CFLAGS    = -g -I $(SRC_DIR)
CFLAGS   += -Wall -Werror
CFLAGS   += -fno-omit-frame-pointer
CFLAGS   += $(OPTIMIZATION_LEVEL)
CFLAGS   += $(SANITIZERS)
CFLAGS   += $(CFLAGS_EXTRA)

LDFLAGS   = $(CFLAGS)

to_objs   = $(patsubst $(SRC_DIR)/%.S, $(OBJ_DIR)/%.o, $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(1)))

# Библиотека корутин
LIB_SRCS := $(shell find $(SRC_DIR)/coroed -iname '*.c') $(shell find $(SRC_DIR)/coroed -iname '*.S')
LIB_OBJS := $(call to_objs, $(LIB_SRCS))

# Тесты
TEST_SRCS := $(SRC_DIR)/main.c $(wildcard $(SRC_DIR)/test_*.c)
TEST_OBJS := $(call to_objs, $(TEST_SRCS))

# HTTP: общий разбор запросов и сервер на корутинах
HTTP_OBJS          := $(OBJ_DIR)/http/http.o
COROED_SERVER_OBJS := $(OBJ_DIR)/http/coroed_server.o

TARGET    = app

run: $(BIN_DIR)/$(TARGET)
	./$(BIN_DIR)/$(TARGET)

compile: clean $(BIN_DIR)/$(TARGET)

server: $(BIN_DIR)/coroed_server

clean:
	rm -rf $(BUILD_DIR)
	rm -rf ./**/*.o

$(BIN_DIR)/$(TARGET): $(BIN_DIR) $(OBJ_DIR) $(LIB_OBJS) $(HTTP_OBJS) $(TEST_OBJS)
	$(CC) $(LIB_OBJS) $(HTTP_OBJS) $(TEST_OBJS) -o $@ $(LDFLAGS)

$(BIN_DIR)/coroed_server: $(BIN_DIR) $(OBJ_DIR) $(LIB_OBJS) $(HTTP_OBJS) $(COROED_SERVER_OBJS)
	$(CC) $(LIB_OBJS) $(HTTP_OBJS) $(COROED_SERVER_OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	mkdir -p $(shell dirname $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S
	mkdir -p $(shell dirname $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR): $(BUILD_DIR)
	mkdir -p $(OBJ_DIR)

$(BIN_DIR): $(BUILD_DIR)
	mkdir -p $(BIN_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

check-style:
	find source -iname '*.h' -o -iname '*.c' \
	| xargs clang-tidy -p compile_commands.json

check-format:
	find source -iname '*.h' -o -iname '*.c' \
	| xargs clang-format -Werror --dry-run --fallback-style=Google --verbose

fix-format:
	find source -iname '*.h' -o -iname '*.c' \
	| xargs clang-format -i --fallback-style=Google --verbose

info:
	$(info CC        = $(CC))
	$(info CFLAGS    = $(CFLAGS))
	$(info LDFLAGS   = $(LDFLAGS))
	$(info SRC_DIR   = $(SRC_DIR))
	$(info BUILD_DIR = $(BUILD_DIR))
	$(info LIB_SRCS  = $(LIB_SRCS))
	$(info TEST_SRCS = $(TEST_SRCS))
	$(info LIB_OBJS  = $(LIB_OBJS))
