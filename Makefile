# ---- Toolchain ----
CC       := gcc
CFLAGS   := -Wall -Wextra -Wpedantic -std=c11
LDFLAGS  :=
LDLIBS   := -lssl -lcrypto -lz -lpthread

# ---- Layout ----
COMMON_DIR   := common
SERVER_DIR   := server
BALANCER_DIR := balancer
BUILD_DIR    := build
BIN_DIR      := bin

# Every component sees the shared headers; each also sees its own.
COMMON_INC   := -I$(COMMON_DIR)/include
SERVER_INC   := $(COMMON_INC) -I$(SERVER_DIR)/include
BALANCER_INC := $(COMMON_INC) -I$(BALANCER_DIR)/include

SERVER_BIN   := $(BIN_DIR)/codo
BALANCER_BIN := $(BIN_DIR)/codo-balancer
COMMON_LIB   := $(BUILD_DIR)/libcommon.a

# ---- Sources / objects / deps ----
COMMON_SRCS   := $(wildcard $(COMMON_DIR)/src/*.c)
SERVER_SRCS   := $(wildcard $(SERVER_DIR)/src/*.c)
BALANCER_SRCS := $(wildcard $(BALANCER_DIR)/src/*.c)

COMMON_OBJS   := $(COMMON_SRCS:$(COMMON_DIR)/src/%.c=$(BUILD_DIR)/common/%.o)
SERVER_OBJS   := $(SERVER_SRCS:$(SERVER_DIR)/src/%.c=$(BUILD_DIR)/server/%.o)
BALANCER_OBJS := $(BALANCER_SRCS:$(BALANCER_DIR)/src/%.c=$(BUILD_DIR)/balancer/%.o)

DEPS := $(COMMON_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(BALANCER_OBJS:.o=.d)

# ---- Default: build both binaries ----
all: $(SERVER_BIN) $(BALANCER_BIN)

server: $(SERVER_BIN)
balancer: $(BALANCER_BIN)

# ---- Shared static library ----
$(COMMON_LIB): $(COMMON_OBJS)
	ar rcs $@ $^

# ---- Link binaries (each against libcommon) ----
$(SERVER_BIN): $(SERVER_OBJS) $(COMMON_LIB) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(SERVER_OBJS) $(COMMON_LIB) -o $@ $(LDLIBS)

$(BALANCER_BIN): $(BALANCER_OBJS) $(COMMON_LIB) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(BALANCER_OBJS) $(COMMON_LIB) -o $@ $(LDLIBS)

# ---- Compile (-MMD -MP emits .d files tracking header dependencies) ----
$(BUILD_DIR)/common/%.o: $(COMMON_DIR)/src/%.c | $(BUILD_DIR)/common
	$(CC) $(CFLAGS) $(COMMON_INC) -MMD -MP -c $< -o $@

$(BUILD_DIR)/server/%.o: $(SERVER_DIR)/src/%.c | $(BUILD_DIR)/server
	$(CC) $(CFLAGS) $(SERVER_INC) -MMD -MP -c $< -o $@

$(BUILD_DIR)/balancer/%.o: $(BALANCER_DIR)/src/%.c | $(BUILD_DIR)/balancer
	$(CC) $(CFLAGS) $(BALANCER_INC) -MMD -MP -c $< -o $@

# ---- Create dirs on demand ----
$(BIN_DIR) $(BUILD_DIR)/common $(BUILD_DIR)/server $(BUILD_DIR)/balancer:
	mkdir -p $@

# ---- Variants (apply to all components) ----
debug:   CFLAGS  += -g -O0 -DDEBUG -fsanitize=address,undefined
debug:   LDFLAGS += -fsanitize=address,undefined
debug:   all

release: CFLAGS += -O2 -DNDEBUG
release: all

# ---- Convenience ----
run: $(SERVER_BIN)
	./$(SERVER_BIN)

run-balancer: $(BALANCER_BIN)
	./$(BALANCER_BIN)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Pull in auto-generated header dependencies
-include $(DEPS)

.PHONY: all server balancer debug release run run-balancer clean
