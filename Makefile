CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcpc 2>/dev/null)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread -ldl

TARGET := cpc_iostream_bridge
SOURCES := cpc_iostream_bridge.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET) *.o
