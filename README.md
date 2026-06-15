# brpc

A lightweight RPC framework in C with Python bindings, designed for low-latency streaming and efficient JSON messaging.

## What problem does this solve?

When you need bidirectional streaming between services and your payloads are JSON, the options are:

1. **gRPC** — powerful but heavy. Protobuf codegen, HTTP/2, ~50K LOC. Overkill for many use cases.
2. **Raw TCP + JSON** — simple but no multiplexing, no flow control, no streaming semantics.
3. **WebSocket** — browser-oriented, text-based framing, limited flow control.

brpc sits between 2 and 3: binary-framed multiplexed streams with JSON payloads, no codegen, no HTTP dependency, ~2000 LOC.

## What brpc provides

- **JSON-RPC 2.0 abstraction** — method dispatch, handlers, error propagation
- **Arena-based JSON parsing and serialization** — zero-allocation, fast
- **Binary framing** — 10-byte LE header per frame
- **Multiplexed bidirectional streams** — configurable concurrent streams over one TCP connection
- **Flow control** — connection and stream receive windows
- **Python bindings** — ergonomic API with decorators
- **Profiling** — microsecond-granularity counters for every hot path

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
- You need message-level compression
- You need async/event-loop integration (uv, epoll, etc.)
- You need backward-compatible protocol versioning

These are all real limitations. gRPC solves them out of the box.

## Quick start

### RPC (the main API)

**Server:**

```c
#include "brpc_rpc.h"

// Handler: receives JSON-RPC request, writes response
int handle_get_user(const brpc_rpc_request_t *req,
                    brpc_rpc_response_t *resp, void *ctx) {
    int64_t user_id = json_get_int(json_obj_get(req->params, "id"), 0);
    // ... look up user ...
    // resp->result = your_json_value;
    return BRPC_RPC_OK;
}

// Set up server
brpc_rpc_server_t srv;
brpc_rpc_server_init(&srv);
brpc_rpc_register(&srv, "getUser", handle_get_user, NULL);

// In your recv loop:
brpc_rpc_server_dispatch(&srv, &channel, stream_id, recv_buf, recv_len);
```

**Client:**

```c
#include "brpc_rpc.h"

brpc_rpc_client_t cli;
brpc_rpc_client_init(&cli, &channel, stream_id);

// Build params with JSON writer
char params_buf[256];
json_writer_t pw;
json_writer_init(&pw, params_buf, sizeof(params_buf));
json_write_obj_start(&pw);
json_write_obj_key(&pw, "id", 2);
json_write_int(&pw, 1);
json_write_obj_end(&pw);
json_writer_finish(&pw);

// Call (takes raw JSON string)
char response[4096];
brpc_rpc_call(&cli, "getUser", params_buf, response, sizeof(response));

// Or use the ergonomic API with json_value_t
json_arena_t resp_arena;
json_arena_init(&resp_arena, arena_buf, sizeof(arena_buf));
json_value_t *result;
brpc_rpc_call_json(&cli, "getUser", params_value, &resp_arena, &result);
```

### JSON (standalone)

```c
#include "json_hotpath.h"

// Parse
uint8_t arena_buf[4096];
json_arena_t arena;
json_arena_init(&arena, arena_buf, sizeof(arena_buf));
json_parser_t p;
json_value_t *root;
json_parse(&p, "{\"temp\":23.5}", 13, &arena, &root);
double temp = json_get_float(json_obj_get(root, "temp"), 0);

// Serialize
char buf[256];
json_writer_t w;
json_writer_init(&w, buf, sizeof(buf));
json_write_obj_start(&w);
json_write_obj_key(&w, "temp", 4);
json_write_float(&w, 23.5);
json_write_obj_end(&w);
json_writer_finish(&w);
```

### Python

**RPC (the main API):**

```python
from brpc import RpcServer, RpcClient

# Server
srv = RpcServer()

@srv.method("getUser")
def get_user(params):
    user_id = params["id"].as_int()
    return {"name": "Alice", "id": user_id}

# In your recv loop:
response_json = srv.dispatch(data)

# Client
cli = RpcClient(channel, stream_id)
result = cli.call("getUser", {"id": 1})
# result is raw JSON string
```

**JSON (standalone):**

```python
from brpc import JsonParser, JsonWriter

parser = JsonParser()
v = parser.parse('{"method":"getUser","id":1}')
print(v["method"].as_str())  # "getUser"

writer = JsonWriter()
writer.obj_start()
writer.obj_key("method"); writer.str("getUser")
writer.obj_end()
data = writer.finish()
```

### Channel (low-level streaming)

```c
#include "brpc_channel.h"

int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

brpc_channel_t client, server;
brpc_channel_init(&client, sv[0], 0, 0);  // client, 256 max streams
brpc_channel_init(&server, sv[1], 1, 0);  // server

brpc_stream_t *s = brpc_channel_open_stream(&client);
brpc_channel_send_data(&client, s->stream_id, payload, len, 0);
brpc_channel_recv(&server);
```

## Performance

Measured on x86-64 Linux, `zig cc -O2`, single thread, `socketpair(AF_UNIX)`:

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

C-level numbers. Python ctypes FFI adds ~2-3µs overhead per call.

## Comparison

| | brpc | gRPC | Raw TCP + JSON |
|---|---|---|---|
| RTT (socketpair) | 2.4µs | ~100µs+ | ~1µs |
| Multiplexing | Yes | Yes | No |
| Flow control | Yes | Yes | No |
| TLS | No | Yes | No |
| Codegen | No | Yes (Protobuf) | No |
| Streaming | Yes | Yes | Manual |
| Language support | C, Python | 10+ | Any |
| LOC | ~2000 | ~50,000+ | ~200 |
| JSON-RPC 2.0 | Built-in | No | Manual |

brpc trades TLS, cross-language support, and ecosystem for simplicity and speed.

```
┌─────────────────────────────────────────────────┐
│              brpc_rpc (JSON-RPC 2.0)            │
│    Request/response, method dispatch, errors    │
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

Frames use little-endian encoding. The wire format is experimental until v1.0.

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
| Connection/stream flow control windows | Done |
| JSON-RPC 2.0 dispatch | Done |
| Method registration + handlers | Done |
| Error propagation | Done |
| Profiling | Done |
| Python bindings | Done |
| TLS | Not implemented |
| Authentication | Not implemented |
| Compression | Not implemented |
| Application-level backpressure callbacks | Not implemented |
| Async/event-loop integration | Not implemented |
| Protocol versioning | Not implemented |
| Cross-language interop | C + Python only |

## Build

### C

```bash
make all        # builds brpc_demo, test_brpc, bench
make test       # runs 79 C tests
make run        # runs demo
make bench      # runs C benchmarks
```

### Python

```bash
cd python/brpc
uv sync
uv run pytest tests/ -v  # 31 Python tests
```

## Project structure

```
brpc/
├── include/              # Public C headers
│   ├── json_hotpath.h    # JSON parser/serializer
│   ├── brpc_frame.h      # Binary framing
│   ├── brpc_stream.h     # Bidirectional stream
│   ├── brpc_channel.h    # Multiplexed channel
│   ├── brpc_rpc.h        # JSON-RPC 2.0 abstraction
│   └── brpc_prof.h       # Profiling
├── src/                  # Implementation (~2000 LOC)
├── tests/                # C tests (79 tests)
├── bench.c               # C benchmarks
├── examples/             # Demo code
├── python/brpc/          # Python package
│   ├── tests/            # Python tests (31 tests)
│   └── benchmarks/       # Python benchmarks
├── build.zig             # Zig build
├── Makefile              # Make build
└── .github/workflows/    # CI
```

## License

MIT
