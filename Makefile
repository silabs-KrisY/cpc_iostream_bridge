CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libcpc 2>/dev/null)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread -ldl -lcpc
EXEDIR = exe
TARGET := cpc_iostream_bridge
SOURCES := cpc_iostream_bridge.c

$(EXEDIR)/$(TARGET): $(SOURCES)
	mkdir -p $(EXEDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(EXEDIR)/$(TARGET) *.o
