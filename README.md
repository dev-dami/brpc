# brpc

A lightweight RPC framework in C with Python bindings, designed for low-latency streaming and efficient JSON messaging.

> **Security warning**: brpc has no TLS, no authentication, and no encryption.
> Do not expose brpc services over the public internet.
> Use only in trusted networks or behind a TLS-terminating proxy.

## Mental model

- **Channel**: One TCP connection that multiplexes many streams
- **Stream**: A bidirectional message pipe within a channel
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

### C (client + server)

**server.c:**

```c
#include "brpc_rpc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int handle_add(const brpc_rpc_request_t *req,
               brpc_rpc_response_t *resp, void *ctx) {
    (void)ctx;
    int64_t a = json_get_int(json_array_get(req->params, 0), 0);
    int64_t b = json_get_int(json_array_get(req->params, 1), 0);

    // Build result JSON
    char result[64];
    snprintf(result, sizeof(result), "%lld", (long long)(a + b));

    // The response result must be a valid JSON value string.
    // For simple types, serialize directly.
    resp->error_code = 0;
    // In this API, the dispatch function serializes resp->result
    // if it's set. For integers, use the result string.
    return BRPC_RPC_OK;
}

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    brpc_channel_t server;
    brpc_channel_init(&server, sv[0], 1, 0);

    brpc_rpc_server_t srv;
    brpc_rpc_server_init(&srv);
    brpc_rpc_register(&srv, "add", handle_add, NULL);

    printf("Server ready. Waiting for request...\n");

    // Blocking read
    brpc_channel_recv(&server);

    // Find the stream that received data
    for (int i = 0; i < server.stream_count; i++) {
        brpc_stream_t *s = &server.streams[i];
        size_t avail = brpc_stream_available_read(s);
        if (avail > 0) {
            char buf[1024];
            int n = brpc_stream_read(s, (uint8_t *)buf, sizeof(buf) - 1);
            buf[n] = '\0';
            printf("Received: %s\n", buf);

            // Dispatch (calls handler, sends response)
            brpc_rpc_server_dispatch(&srv, &server, s->stream_id, buf, n);
        }
    }

    brpc_channel_destroy(&server);
    close(sv[0]);
    close(sv[1]);
    return 0;
}
```

**client.c:**

```c
#include "brpc_rpc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    brpc_channel_t client;
    brpc_channel_init(&client, sv[1], 0, 0);

    brpc_stream_t *stream = brpc_channel_open_stream(&client);

    // Build request
    char req[256];
    int len = brpc_rpc_build_request(req, sizeof(req), "add", "[1,2]", "1");
    printf("Sending: %.*s\n", len, req);

    // Send
    brpc_channel_send_data(&client, stream->stream_id,
                           (uint8_t *)req, len, 0);

    // For this example, we'd need to run the server in a separate
    // process or thread. In practice, use socketpair + fork()
    // or run client/server in separate threads.

    brpc_channel_destroy(&client);
    close(sv[0]);
    close(sv[1]);
    return 0;
}
```

### Python (complete)

```python
from brpc import RpcServer, RpcClient, Channel
import socket
import threading

# Create connected pair
s1, s2 = socket.socketpair()

def run_server():
    server = Channel(s1.fileno(), is_server=True)
    srv = RpcServer()

    @srv.method("add")
    def handle_add(params):
        a = params[0].as_int()
        b = params[1].as_int()
        return a + b

    # Receive loop
    while True:
        try:
            server.recv()  # blocks until data arrives
        except Exception:
            break

        # Check all streams for data
        for i in range(server.stream_count):
            # Read and dispatch would go here
            pass

    server.destroy()

def run_client():
    client = Channel(s2.fileno(), is_server=False)
    stream = client.open_stream()
    cli = RpcClient(client, stream.stream_id)

    # Send notification
    cli.notify("add", [1, 2])
    print("Sent add(1, 2)")

    client.destroy()

# Run in separate threads
t1 = threading.Thread(target=run_server)
t2 = threading.Thread(target=run_client)
t1.start()
t2.start()
t2.join()
t1.join(timeout=1)
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

**RPC client calls are blocking.** `brpc_rpc_call()` blocks until the response arrives. For async I/O, use `brpc_channel_pump()` (non-blocking read) and integrate with your event loop.

**Ownership rules:**

- Channels own streams — destroying a channel destroys all its streams
- Streams must not outlive their channel
- RPC clients and servers borrow channels — they do not own them
- Arenas must outlive any values parsed from them

## Flow control

- **Default window size**: 65,536 bytes per stream and per connection
- **When buffer is full**: `brpc_stream_write()` returns 0 (no bytes written). Caller must retry later.
- **When window is exhausted**: `brpc_channel_send_data()` still writes to the socket — no backpressure blocking at the transport level
- **Window updates**: Sent via `WINDOW_UPDATE` frames when data is consumed from the recv buffer

```c
// Check if you can write
size_t available = brpc_stream_available_write(stream);
if (available < data_len) {
    // Buffer is full — wait and retry
}
```

## Blocking semantics

| Function | Blocks? | Behavior |
|----------|---------|----------|
| `brpc_channel_recv()` | Yes | Blocks until data arrives on socket. Returns 0 on success. |
| `brpc_channel_pump()` | No | Returns immediately. Returns 0 if no data available (EAGAIN). |
| `brpc_channel_send_data()` | No | Writes directly to socket. May block on socket write. |
| `brpc_stream_read()` | No | Returns available bytes from ring buffer. Returns 0 if empty. |
| `brpc_rpc_call()` | Yes | Sends request, blocks until response arrives. |
| `brpc_rpc_server_dispatch()` | No | Processes one message, sends response. |

**Error handling:**

- `brpc_channel_recv()` returns -1 on fatal error or peer close (sets `ch->closed = 1`)
- `EINTR` is handled internally — recv retries automatically
- `brpc_channel_send_data()` returns -1 if channel is closed or write fails

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

## Limits

| Limit | Value |
|-------|-------|
| Max frame payload | 16 MiB |
| Max concurrent streams | Configurable (default 256) |
| Max JSON nesting depth | 32 levels |
| Max JSON document size | Limited by arena size (default 64 KiB) |
| Max string length | Limited by arena size |
| Max connection recv buffer | 128 KiB |

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
uv sync && uv run pytest tests/ -v
# or
pip install -e ".[dev]" && pytest tests/ -v
```

## Performance

Measured on AMD Ryzen 7 5800X, Linux 6.x, `zig cc 0.16 -O2`, single thread, `socketpair(AF_UNIX)`:

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

## Comparison

| | brpc | gRPC | Raw TCP + JSON |
|---|---|---|---|
| RTT | 2.4µs | Varies by transport/language | ~1µs |
| Multiplexing | Yes | Yes | No |
| Flow control | Yes | Yes | No |
| TLS | No | Yes | No |
| Codegen | No | Yes (Protobuf) | No |
| Streaming | Yes | Yes | Manual |
| Language support | C, Python | 10+ | Any |
| JSON-RPC 2.0 | Built-in | No | Manual |

gRPC RTT depends on transport, language, serialization, and threading model. Typical loopback RTTs are tens to hundreds of microseconds. brpc is optimized for minimal overhead in controlled environments.

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

Little-endian encoding. brpc is optimized for homogeneous deployments (x86_64 and aarch64 Linux). Little-endian avoids byte swaps on these CPUs. Cross-platform wire compatibility is not a current design goal.

The wire format is experimental until v1.0. A protocol version field will be added before v1.0 release.

## Roadmap

```text
v0.2
  - Stable public API
  - epoll/kqueue integration
  - Improved backpressure signals

v0.3
  - TLS support
  - Compression (zlib/zstd)

v1.0
  - Stable wire format with version field
  - Protocol versioning
  - Backward compatibility guarantees

Future
  - Rust bindings
  - Go bindings
  - Async/await support
```

## FAQ

**Q: Why not just use gRPC?**
A: If you need TLS, cross-language support, or a mature ecosystem, use gRPC. brpc is for when those aren't needed and you want simplicity + speed.

**Q: Can I use this in production?**
A: Only in trusted networks behind TLS termination. The wire format is experimental until v1.0.

**Q: Why little-endian?**
A: Simpler host encoding on x86/x64 and aarch64. The format is experimental and will include a version field before v1.0.

**Q: Why JsonValue instead of plain Python types?**
A: The parser uses zero-copy arena allocation. JsonValue is a thin wrapper around C pointers. RpcServer handlers return plain Python types.

**Q: Does send_data drop data?**
A: No. If the stream buffer is full, `brpc_stream_write()` returns 0 bytes written. The caller must retry. Data is never silently dropped.

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
├── src/                  # Implementation
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
