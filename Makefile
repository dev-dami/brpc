CC ?= zig cc
CFLAGS = -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lm -lz -lssl -lcrypto

SRC = src/json_hotpath.c src/brpc_frame.c src/brpc_stream.c src/brpc_channel.c src/brpc_prof.c src/brpc_rpc.c src/brpc_compress.c src/brpc_tls.c

.PHONY: all clean test run bench

all: brpc_demo test_brpc bench

brpc_demo: examples/main.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_brpc: tests/test_brpc.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

bench: bench.c $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: test_brpc
	./test_brpc

run: brpc_demo
	./brpc_demo

clean:
	rm -f brpc_demo test_brpc bench
