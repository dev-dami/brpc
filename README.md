# brpc

**Binary RPC with JSON hotpath serialization and HTTP/3-inspired multiplexed streaming.**

[![CI](https://github.com/user/brpc/actions/workflows/ci.yml/badge.svg)](https://github.com/user/brpc/actions)
[![Python 3.10+](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/downloads/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

## What is this?

brpc is a lightweight, high-performance RPC framework written in C with Python bindings. It combines:

- **JSON hotpath parser** — zero-allocation arena-based parsing at ~5µs per message
- **Binary framing** — 10-byte LE header with multiplexed streams
- **Bidirectional streaming** — 256 concurrent streams over one TCP connection
- **Flow control** — per-stream and connection-level windows
- **Built-in profiling** — microsecond-granularity counters for every hot path

Think of it as a simpler, faster alternative to gRPC — JSON payloads instead of Protobuf, ~1500 LOC instead of ~50,000.

## Quick Start

### C

```c
#include "json_hotpath.h"

uint8_t arena_buf[4096];
json_arena_t arena;
json_arena_init(&arena, arena_buf, sizeof(arena_buf));

json_parser_t p;
json_value_t *root;
json_parse(&p, "{\"method\":\"getUser\",\"id\":1}", 27, &arena, &root);

int64_t id = json_get_int(json_obj_get(root, "id"), 0);
const char *method = json_get_str(json_obj_get(root, "method"), NULL);
```

### Python

```python
from brpc import JsonParser, JsonWriter

parser = JsonParser()
value = parser.parse('{"method":"getUser","id":1}')
print(value["method"].as_str())  # "getUser"
print(value["id"].as_int())      # 1

writer = JsonWriter()
writer.obj_start()
writer.obj_key("method")
writer.str("getUser")
writer.obj_end()
data = writer.finish()  # b'{"method":"getUser"}'
```

## Architecture

```
┌─────────────────────────────────────────────────┐
│                Application                      │
│           (JSON-RPC over bRPC)                  │
├──────────────────┬──────────────────────────────┤
│   json_hotpath   │        brpc_channel          │
│  Arena parser +  │  Multiplexed TCP channel     │
│  streaming writer│  with stream management      │
├──────────────────┼──────────┬───────────────────┤
│                  │brpc_frame│   brpc_stream     │
│                  │ Binary   │ Bidirectional     │
│                  │ framing  │ ring buffers      │
├──────────────────┴──────────┴───────────────────┤
│                  brpc_prof                       │
│         Microsecond profiling counters          │
└─────────────────────────────────────────────────┘
```

## Install

### C (build from source)

```bash
make all        # builds brpc_demo and test_brpc
make test       # runs 67 C tests
```

### Python

```bash
cd python/brpc
uv sync                              # install with uv
uv run pytest tests/ -v              # run 31 Python tests
uv run python -m brpc.benchmarks.suite  # run benchmarks
```

Or with pip:

```bash
cd python/brpc
pip install -e ".[dev]"
pytest tests/ -v
```

## API Reference

### C API

#### JSON Parser

| Function | Description |
|----------|-------------|
| `json_arena_init(a, buf, size)` | Initialize arena allocator |
| `json_parse(p, input, len, arena, out)` | Parse JSON into value tree |
| `json_obj_get(obj, key)` | Get object value by key |
| `json_get_int(v, fallback)` | Extract int with fallback |
| `json_get_str(v, out_len)` | Get string pointer and length |
| `json_serialize(val, buf, len, out)` | Serialize value tree to JSON |

#### JSON Writer

| Function | Description |
|----------|-------------|
| `json_writer_init(w, buf, cap)` | Initialize writer |
| `json_write_obj_start/end` | Write object delimiters |
| `json_write_obj_key(w, key, len)` | Write key with auto-comma |
| `json_write_arr_start/end` | Write array delimiters |
| `json_writer_finish(w)` | Finalize and get length |

#### Stream

| Function | Description |
|----------|-------------|
| `brpc_stream_init(s, id, buf_size)` | Initialize stream |
| `brpc_stream_write(s, data, len)` | Write to send ring buffer |
| `brpc_stream_read(s, buf, len)` | Read from recv ring buffer |
| `brpc_stream_close(s)` | Half-close local side |

#### Channel

| Function | Description |
|----------|-------------|
| `brpc_channel_init(ch, fd, is_server)` | Initialize channel |
| `brpc_channel_open_stream(ch)` | Open new stream (odd IDs for client) |
| `brpc_channel_send_data(ch, id, data, len, end)` | Send DATA frame |
| `brpc_channel_recv(ch)` | Blocking read + frame dispatch |
| `brpc_channel_pump(ch)` | Non-blocking read (MSG_DONTWAIT) |

### Python API

```python
from brpc import JsonParser, JsonWriter, Stream, Channel, Profiler

# Parse
parser = JsonParser()
v = parser.parse('{"users":[{"name":"Alice"}]}')
v["users"][0]["name"].as_str()  # "Alice"

# Serialize
w = JsonWriter()
w.obj_start()
w.obj_key("method"); w.str("ping")
w.obj_end()
data = w.finish()

# Stream
s = Stream(stream_id=1, buf_size=16384)
s.write(b"data")
buf = s.read()

# Channel
ch = Channel(fd=sock.fileno(), is_server=False)
stream = ch.open_stream()
ch.send_data(stream.stream_id, payload, end_stream=True)
ch.recv()

# Profiling
Profiler.print()
```

## Benchmarks

### JSON Parse (µs, lower is better)

| Payload | brpc | stdlib json | Speedup |
|---------|------|-------------|---------|
| Small (28B) | 4.4 | 3.0 | 0.69x |
| Medium (99B) | 5.2 | 4.3 | 0.82x |
| Large (310B) | 5.4 | 7.6 | **1.40x** |

> Note: brpc has ~2-3µs FFI overhead per call from Python. At the C level, brpc is
> consistently faster. The arena parser shines on large payloads where zero-allocation
> matters.

### Profile Counter Output

```
json_parse      21 calls, avg 0.46µs
json_serialize   1 call,  avg 0.81µs
frame_encode     9 calls, avg 0.27µs
frame_decode     4 calls, avg 0.03µs
stream_write     5 calls, avg 0.04µs
stream_read      2 calls, avg 0.09µs
channel_send     3 calls, avg 7.14µs
channel_recv     1 call,  avg 6.49µs
```

## Project Structure

```
brpc/
├── include/              # Public C headers
│   ├── json_hotpath.h    # JSON parser/serializer
│   ├── brpc_frame.h      # Binary framing protocol
│   ├── brpc_stream.h     # Bidirectional stream
│   ├── brpc_channel.h    # Multiplexed channel
│   └── brpc_prof.h       # Profiling counters
├── src/                  # Implementation
│   ├── json_hotpath.c    # ~680 LOC
│   ├── brpc_frame.c      # ~170 LOC
│   ├── brpc_stream.c     # ~230 LOC
│   ├── brpc_channel.c    # ~630 LOC
│   └── brpc_prof.c       # ~100 LOC
├── examples/             # Demo code
│   └── main.c
├── tests/                # C test suite (67 tests)
│   └── test_brpc.c
├── python/brpc/          # Python package
│   ├── src/brpc/
│   │   ├── __init__.py   # Python API (ctypes)
│   │   └── _libbrpc.so   # Compiled shared library
│   ├── tests/            # Python tests (31 tests)
│   └── benchmarks/       # Performance benchmarks
├── build.zig             # Zig build system
├── Makefile              # Make build system
└── README.md
```

## Wire Format

Every frame on the wire is a 10-byte little-endian header:

```
Offset  Size  Field
0       4     Stream ID (LE32)
4       1     Frame Type
5       1     Flags
6       4     Payload Length (LE32)
10      N     Payload
```

Frame types: `DATA(0)`, `HEADERS(1)`, `RST_STREAM(2)`, `SETTINGS(3)`, `PING(4)`, `GOAWAY(5)`, `WINDOW_UPDATE(6)`

## License

MIT
