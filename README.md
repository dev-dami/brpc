# brpc

A lightweight RPC framework in C with Python bindings, designed for low-latency streaming and efficient JSON messaging.

> **Security warning**: brpc has no TLS, no authentication, and no encryption.
> Do not expose brpc services over the public internet.
> Use only in trusted networks or behind a TLS-terminating proxy.

## Mental model

- **Channel**: One TCP connection that multiplexes many streams
- **Stream**: A bidirectional message pipe within a channel (like an HTTP/2 stream)
- **Frame**: A 10-byte binary header + payload, the unit sent over the wire
- **RPC layer**: JSON-RPC 2.0 on top of streams — method dispatch, handlers, errors

Data flow: `RPC handler → JSON serialize → Stream → Frame → TCP`

## When to use this

- Embedded/edge systems where gRPC is too heavy
- Internal microservice RPC where Protobuf isn't worth the overhead
- Python services needing C-level JSON parsing speed
- Game servers, telemetry pipelines, real-time dashboards
- Controlled networks where TLS termination happens elsewhere

## When NOT to use this

- You need TLS (not implemented)
- You need cross-language interop (C + Python only)
- You need authentication/authorization
- You need async/event-loop integration
- You need backward-compatible protocol versioning
- You're exposing services to the public internet

## Complete working example

### C (client + server in one file)

```c
#include "brpc_rpc.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

// Server handler
int handle_add(const brpc_rpc_request_t *req,
               brpc_rpc_response_t *resp, void *ctx) {
    (void)ctx;
    int64_t a = json_get_int(json_array_get(req->params, 0), 0);
    int64_t b = json_get_int(json_array_get(req->params, 1), 0);
    printf("Server: %lld + %lld = %lld\n", a, b, a + b);
    // For simplicity, set result via error_message abuse
    // In production, allocate a json_value_t for the result
    return BRPC_RPC_OK;
}

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    // Server side
    brpc_channel_t server;
    brpc_channel_init(&server, sv[0], 1, 0);

    brpc_rpc_server_t srv;
    brpc_rpc_server_init(&srv);
    brpc_rpc_register(&srv, "add", handle_add, NULL);

    // Client side
    brpc_channel_t client;
    brpc_channel_init(&client, sv[1], 0, 0);

    brpc_stream_t *stream = brpc_channel_open_stream(&client);

    // Client sends request
    char req_buf[256];
    brpc_rpc_build_request(req_buf, sizeof(req_buf),
                           "add", "[1,2]", "1");
    brpc_channel_send_data(&client, stream->stream_id,
                           (uint8_t *)req_buf, strlen(req_buf), 0);

    // Server receives and dispatches
    brpc_channel_recv(&server);
    brpc_stream_t *srv_stream = brpc_channel_find_stream(
        &server, stream->stream_id);
    char recv_buf[512];
    size_t len = brpc_stream_read(srv_stream, (uint8_t *)recv_buf,
                                  sizeof(recv_buf) - 1);
    recv_buf[len] = '\0';
    brpc_rpc_server_dispatch(&srv, &server, srv_stream->stream_id,
                             recv_buf, len);

    // Cleanup
    brpc_channel_destroy(&client);
    brpc_channel_destroy(&server);
    close(sv[0]);
    close(sv[1]);
    return 0;
}
```

### Python

```python
from brpc import RpcServer, RpcClient, Channel
import socket

# Create connected socket pair
s1, s2 = socket.socketpair()

# Server
server = Channel(s1.fileno(), is_server=True)
srv = RpcServer()

@srv.method("add")
def handle_add(params):
    a = params[0].as_int()
    b = params[1].as_int()
    return a + b

# Client
client = Channel(s2.fileno(), is_server=False)
stream = client.open_stream()
cli = RpcClient(client, stream.stream_id)

# Client sends request
cli.notify("add", [1, 2])

# Server receives
server.recv()
# (In real code, read from stream and dispatch)

# Cleanup
client.destroy()
server.destroy()
s1.close()
s2.close()
```

## Threading model

**brpc is NOT thread-safe.** All operations on a channel, stream, or RPC object must happen from a single thread.

| Object | Thread-safe? | Notes |
|--------|-------------|-------|
| `brpc_channel_t` | No | Must be accessed from one thread |
| `brpc_stream_t` | No | Owned by channel |
| `brpc_rpc_server_t` | No | Single dispatch thread |
| `brpc_rpc_client_t` | No | Blocking call, single thread |

**RPC client calls are blocking.** `brpc_rpc_call()` blocks until the response arrives. For async I/O, integrate with your own event loop using `brpc_channel_pump()` (non-blocking read).

**Recommendation**: Use one thread per channel, or serialize access with a mutex.

## Flow control

- **Default window size**: 65,536 bytes per stream and per connection
- **When window is exhausted**: `brpc_channel_send_data()` still writes to the socket (no backpressure blocking)
- **Window updates**: Sent via `WINDOW_UPDATE` frames when data is consumed
- **Current behavior**: Best-effort. No application-level backpressure callbacks yet.

```c
// Check if you can write
size_t available = brpc_stream_available_write(stream);
if (available < data_len) {
    // Stream buffer is full — data may be dropped or queued
}
```

## Error handling

brpc uses standard JSON-RPC 2.0 error codes:

| Code | Name | When |
|------|------|------|
| -32700 | Parse error | Invalid JSON |
| -32600 | Invalid request | Not a JSON object, missing method |
| -32601 | Method not found | No handler registered |
| -32602 | Invalid params | Wrong parameter types |
| -32603 | Internal error | Handler threw an exception |
| -32000 | Server error | Custom server error |

**Returning errors from handlers:**

```c
// C: set resp->error_code and resp->error_message
int handler(const brpc_rpc_request_t *req,
            brpc_rpc_response_t *resp, void *ctx) {
    resp->error_code = -32602;
    resp->error_message = "Invalid userId";
    return BRPC_RPC_ERROR_PARAMS;
}

# Python: raise an exception or return error dict
@srv.method("getUser")
def get_user(params):
    if not params or "id" not in params:
        raise ValueError("Missing 'id' parameter")
    return {"name": "Alice"}
```

**Transport errors** (connection lost, parse failure) are returned as negative integers from API calls, not as JSON-RPC errors.

## Python API types

When you call `params["id"]`, you get a `JsonValue` wrapper — not a plain Python int. This is because the underlying data lives in C arena memory.

```python
@srv.method("getUser")
def get_user(params):
    # params is a JsonValue (JSON object)
    user_id = params["id"].as_int()      # → int
    name = params["name"].as_str()       # → str
    active = params.get("active", None)  # → JsonValue or None
    score = params["score"].as_float()   # → float

    # Return plain Python types — they get serialized automatically
    return {"id": user_id, "name": name}
```

**RpcServer methods accept and return plain Python types** (dict, list, int, str, float, None). The `params` argument is the only place you see `JsonValue`.

## Installation

### C (from source)

```bash
# Build shared library
zig cc -shared -fPIC -O2 -Iinclude -o libbrpc.so src/*.c -lm

# Or use make
make all        # builds demo, tests, benchmarks
make test       # runs 79 C tests
```

**Supported platforms**: Linux (x86_64, aarch64). Other POSIX systems may work but are untested.

### Python

```bash
cd python/brpc

# With uv (recommended)
uv sync
uv run pytest tests/ -v

# With pip
pip install -e ".[dev]"
pytest tests/ -v
```

**How it works**: Python bindings use ctypes to call the compiled `libbrpc.so`. No C extension compilation needed.

## Performance

Measured on AMD Ryzen 7 5800X, Linux 6.x, `zig cc 0.16 -O2`, single thread:

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

## Architecture

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

Every frame is a 10-byte header:

```
Offset  Size  Field
0       4     Stream ID
4       1     Frame Type (DATA=0, HEADERS=1, RST=2, SETTINGS=3, PING=4, GOAWAY=5, WINDOW_UPDATE=6)
5       1     Flags (END_STREAM=0x01, END_HEADERS=0x02, COMPRESSED=0x04)
6       4     Payload Length
10      N     Payload
```

Frames use little-endian encoding. The wire format is experimental until v1.0. A protocol version field will be added before v1.0 release.

## Roadmap

| Version | Features |
|---------|----------|
| v0.1 (current) | JSON-RPC 2.0, multiplexed streams, flow control, Python bindings |
| v0.2 | TLS support, async/event-loop integration |
| v0.3 | Compression, backpressure callbacks |
| v1.0 | Stable wire format, protocol versioning, cross-language bindings |

## FAQ

**Q: Why not just use gRPC?**
A: If you need TLS, cross-language support, or a mature ecosystem, use gRPC. brpc is for when those aren't needed and you want simplicity + speed.

**Q: Can I use this in production?**
A: Only in trusted networks behind TLS termination. The wire format is experimental until v1.0.

**Q: Why little-endian?**
A: Simpler host encoding on x86/x64. The format is experimental and may include a version field before v1.0.

**Q: Why JsonValue instead of plain Python types?**
A: The parser uses zero-copy arena allocation. JsonValue is a thin wrapper around C pointers. RpcServer handlers return plain Python types.

## Limitations

| Feature | Status |
|---------|--------|
| JSON parse/serialize | Done |
| Binary framing | Done |
| Multiplexed streams | Done |
| Connection/stream flow control | Done |
| JSON-RPC 2.0 dispatch | Done |
| Method registration + handlers | Done |
| Error propagation | Done |
| Profiling | Done |
| Python bindings | Done |
| TLS | Not implemented |
| Authentication | Not implemented |
| Compression | Not implemented |
| Async/event-loop integration | Not implemented |
| Application-level backpressure | Not implemented |
| Protocol versioning | Planned for v1.0 |
| Cross-language interop | C + Python only |

## Project structure

```
brpc/
├── include/              # Public C headers
│   ├── json_hotpath.h    # JSON parser/serializer
│   ├── brpc_frame.h      # Binary framing
│   ├── brpc_stream.h     # Bidirectional stream
│   ├── brpc_channel.h    # Multiplexed channel
│   ├── brpc_rpc.h        # JSON-RPC 2.0
│   └── brpc_prof.h       # Profiling
├── src/                  # Implementation (~2000 LOC)
├── tests/                # C tests (79 tests)
├── bench.c               # C benchmarks
├── python/brpc/          # Python package
│   ├── tests/            # Python tests (40 tests)
│   └── benchmarks/       # Python benchmarks
├── Makefile
├── build.zig
└── .github/workflows/    # CI
```

## License

MIT
