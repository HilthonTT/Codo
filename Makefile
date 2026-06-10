# ---- Toolchain ----
CC       := gcc
CFLAGS   := -Wall -Wextra -Wpedantic -std=c11 -Iinclude
LDFLAGS  :=
LDLIBS   := -lssl -lcrypto -lz -lpthread

# ---- Layout ----
SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build
BIN_DIR   := bin
TARGET    := $(BIN_DIR)/codo

# ---- Sources / objects / deps ----
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# ---- Default ----
all: $(TARGET)

# Link
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# Compile (-MMD -MP emits .d files tracking header dependencies)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Create dirs on demand
$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

# ---- Variants ----
debug:   CFLAGS += -g -O0 -DDEBUG -fsanitize=address,undefined
debug:   LDFLAGS += -fsanitize=address,undefined
debug:   $(TARGET)

release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)

# ---- Convenience ----
run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Pull in auto-generated header dependencies
-include $(DEPS)

.PHONY: all clean debug release run
