# brpc

A lightweight RPC framework in C with Python bindings, designed for low-latency streaming and efficient JSON messaging.

## What problem does this solve?

When you need bidirectional streaming between services and your payloads are JSON, the options are:

1. **gRPC** — powerful but heavy. Protobuf codegen, HTTP/2, ~50K LOC. Overkill for many use cases.
2. **Raw TCP + JSON** — simple but no multiplexing, no flow control, no streaming semantics.
3. **WebSocket** — browser-oriented, text-based framing, limited flow control.

brpc sits between 2 and 3: binary-framed multiplexed streams with JSON payloads, no codegen, no HTTP dependency, ~1800 LOC.

## When to use this

- Embedded/edge systems where gRPC is too heavy
- Python services needing C-level JSON parsing speed
- Internal microservice RPC where Protobuf isn't worth the overhead
- Game servers, telemetry pipelines, real-time dashboards
- Any system where you want streaming without HTTP/2 complexity

## When NOT to use this

- You need TLS (not implemented yet)
- You need cross-language interop (currently C + Python only)
- You need authentication/authorization
- You need message-level encryption
- You need backward-compatible protocol versioning

These are all real limitations. gRPC solves them out of the box.

## Quick start

### C

```c
#include "json_hotpath.h"

// Parse JSON
uint8_t arena_buf[4096];
json_arena_t arena;
json_arena_init(&arena, arena_buf, sizeof(arena_buf));

json_parser_t p;
json_value_t *root;
json_parse(&p, "{\"temp\":23.5}", 13, &arena, &root);

double temp = json_get_float(json_obj_get(root, "temp"), 0);

// Serialize JSON
char buf[256];
json_writer_t w;
json_writer_init(&w, buf, sizeof(buf));
json_write_obj_start(&w);
json_write_obj_key(&w, "temp", 4);
json_write_float(&w, 23.5);
json_write_obj_end(&w);
size_t len = json_writer_finish(&w);
// buf now contains: {"temp":23.5}
```

### Python

```python
from brpc import JsonParser, JsonWriter

parser = JsonParser()
v = parser.parse('{"method":"getUser","id":1}')
print(v["method"].as_str())  # "getUser"
print(v["id"].as_int())      # 1

writer = JsonWriter()
writer.obj_start()
writer.obj_key("method"); writer.str("getUser")
writer.obj_key("id"); writer.int(1)
writer.obj_end()
data = writer.finish()  # b'{"method":"getUser","id":1}'
```

### Channel (bidirectional streaming)

```c
#include "brpc_channel.h"

int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

brpc_channel_t client, server;
brpc_channel_init(&client, sv[0], 0, 0);  // client, 256 max streams
brpc_channel_init(&server, sv[1], 1, 0);  // server

brpc_stream_t *s = brpc_channel_open_stream(&client);
brpc_channel_send_data(&client, s->stream_id, payload, len, 0);

brpc_channel_recv(&server);  // blocking read, dispatches to streams
```

## Performance

Measured on x86-64 Linux with `zig cc -O2`:

| Operation | Latency | Throughput |
|-----------|---------|------------|
| JSON parse (28B) | 0.1µs | 6.75M msg/s |
| JSON parse (99B) | 0.4µs | 2.37M msg/s |
| JSON parse (310B) | 2.1µs | 485K msg/s |
| JSON serialize (small) | 0.1µs | 7.79M msg/s |
| Frame encode (128B) | 0.1µs | 12.5M/s |
| Frame decode (128B) | 0.1µs | 10.3M/s |
| Stream write (1KB) | <0.1µs | 24.1M/s |
| Stream read (1KB) | <0.1µs | 23.8M/s |
| Channel round-trip | 2.4µs | 413K/s |

**Memory**: Peak RSS 2MB for all components.

### vs Python stdlib json

| Payload | brpc (C) | stdlib json | Speedup |
|---------|----------|-------------|---------|
| Small (28B) | 0.1µs | 3.0µs | 30x |
| Medium (99B) | 0.4µs | 4.3µs | 11x |
| Large (310B) | 2.1µs | 7.6µs | 3.6x |

These are C-level numbers. Python ctypes FFI adds ~2-3µs overhead per call.

## Architecture

```
┌─────────────────────────────────────────────────┐
│                Application                      │
├──────────────────┬──────────────────────────────┤
│   json_hotpath   │        brpc_channel          │
│  Arena parser +  │  Multiplexed TCP channel     │
│  streaming writer│  (configurable max streams)  │
├──────────────────┼──────────┬───────────────────┤
│                  │brpc_frame│   brpc_stream     │
│                  │ 10-byte  │ Bidirectional     │
│                  │ LE hdr   │ ring buffers      │
├──────────────────┴──────────┴───────────────────┤
│                  brpc_prof                       │
│         Microsecond profiling counters          │
└─────────────────────────────────────────────────┘
```

## Wire format

Every frame is a 10-byte little-endian header:

```
Offset  Size  Field
0       4     Stream ID
4       1     Frame Type (DATA=0, HEADERS=1, RST=2, SETTINGS=3, PING=4, GOAWAY=5, WINDOW_UPDATE=6)
5       1     Flags (END_STREAM=0x01, END_HEADERS=0x02, COMPRESSED=0x04)
6       4     Payload Length
10      N     Payload
```

## Stream lifecycle

```
IDLE → OPEN → HALF_CLOSED_LOCAL → CLOSED
             HALF_CLOSED_REMOTE → CLOSED
```

- Client streams: odd IDs (1, 3, 5...)
- Server streams: even IDs (2, 4, 6...)
- Configurable max concurrent streams via `brpc_channel_init(ch, fd, is_server, max_streams)`

## Limitations

| Feature | Status |
|---------|--------|
| JSON parse/serialize | Done |
| Binary framing | Done |
| Multiplexed streams | Done |
| Flow control | Done |
| Profiling | Done |
| Python bindings | Done |
| TLS | Not implemented |
| Authentication | Not implemented |
| Compression | Not implemented |
| Reconnection | Not implemented |
| Backpressure signaling | Not implemented |
| Protocol versioning | Not implemented |
| Cross-language interop | C + Python only |

## Build

### C

```bash
make all        # builds brpc_demo and test_brpc
make test       # runs 67 C tests
make run        # runs demo
```

### Python

```bash
cd python/brpc
uv sync
uv run pytest tests/ -v  # 31 Python tests
```

### Benchmarks

```bash
make bench      # builds and runs C benchmarks
```

## Project structure

```
brpc/
├── include/              # Public C headers
│   ├── json_hotpath.h    # JSON parser/serializer
│   ├── brpc_frame.h      # Binary framing
│   ├── brpc_stream.h     # Bidirectional stream
│   ├── brpc_channel.h    # Multiplexed channel
│   └── brpc_prof.h       # Profiling
├── src/                  # Implementation (~1800 LOC)
├── tests/                # C tests (67 tests)
├── examples/             # Demo code
├── bench.c               # C benchmarks
├── python/brpc/          # Python package
│   ├── tests/            # Python tests (31 tests)
│   └── benchmarks/       # Python benchmarks
├── build.zig             # Zig build
├── Makefile              # Make build
└── .github/workflows/    # CI
```

## License

MIT
