CC ?= zig cc
CFLAGS = -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lm -lz -lssl -lcrypto

SRC = src/json_hotpath.c src/brpc_frame.c src/brpc_stream.c src/brpc_channel.c src/brpc_prof.c src/brpc_rpc.c src/brpc_compress.c src/brpc_tls.c

BUILDDIR = build

.PHONY: all clean test run bench

all: $(BUILDDIR)/brpc_demo $(BUILDDIR)/test_brpc $(BUILDDIR)/bench

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/brpc_demo: examples/main.c $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/test_brpc: tests/test_brpc.c $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/bench: bench.c $(SRC) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(BUILDDIR)/test_brpc
	./$(BUILDDIR)/test_brpc

run: $(BUILDDIR)/brpc_demo
	./$(BUILDDIR)/brpc_demo

clean:
	rm -rf $(BUILDDIR)
