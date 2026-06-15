CC ?= zig cc
CFLAGS = -Wall -Wextra -O2 -Iinclude
LDFLAGS = -lm -lz -lssl -lcrypto

SRC = src/json_hotpath.c src/brpc_frame.c src/brpc_stream.c src/brpc_channel.c src/brpc_prof.c src/brpc_rpc.c src/brpc_compress.c src/brpc_tls.c src/brpc_error.c

BUILDDIR = build
PYTHON_PKG = python/brpc
PYTHON_LIB = $(PYTHON_PKG)/src/brpc/_libbrpc.so

.PHONY: all clean test run bench python install-python

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

# Build the shared library for Python
python: $(PYTHON_LIB)

$(PYTHON_LIB): $(SRC)
	cc -shared -fPIC -O2 -Iinclude -o $@ $^ $(LDFLAGS)

# Install Python package system-wide
install-python: $(PYTHON_LIB)
	cd $(PYTHON_PKG) && uv pip install .

# Install Python package in development/editable mode
install-python-dev: $(PYTHON_LIB)
	cd $(PYTHON_PKG) && uv pip install -e ".[dev]"

# Build shared library, run Python tests
python-test: $(PYTHON_LIB)
	cd $(PYTHON_PKG) && uv run pytest tests/ -v

clean:
	rm -rf $(BUILDDIR)
