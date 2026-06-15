# brpc

A small RPC framework for trusted networks. It provides multiplexed streams, JSON-RPC 2.0, and C/Python bindings with minimal dependencies, prioritizing simplicity and low latency over security features and ecosystem breadth.

> **Note**: brpc has no authentication or authorization.
> Use TLS in production or run behind a TLS-terminating proxy.

## Design philosophy

brpc optimizes for:

- Small codebase with minimal dependencies
- Low latency
- Human-readable JSON payloads
- Explicit tradeoffs documented in code and docs

brpc does NOT optimize for:

- Internet-facing deployments (no auth)
- Enterprise security features
- Cross-language interoperability
- HTTP/2 compatibility

## Why JSON?

JSON is generally slower and larger than Protobuf, but:

- Human-readable JSON payloads — easy to inspect in logs or packet captures
- No schema compiler needed
- Dynamic payloads without codegen
- Familiar to Python and JavaScript developers
- Easy to inspect in logs and traces

brpc prioritizes simplicity and flexibility over maximum serialization efficiency.

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

- You need cross-language interop (C + Python only)
- You need authentication/authorization
- You need async/event-loop integration (Python only via AsyncChannel)
- You need backward-compatible protocol versioning
- You're exposing services to the public internet

## Complete working example

### C (client + server with fork)

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
    printf("Server: %lld + %lld = %lld\n", a, b, a + b);
    return BRPC_RPC_OK;
}

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child = server */
        close(sv[1]);
        brpc_channel_t server;
        brpc_channel_init(&server, sv[0], 1, 0);

        brpc_rpc_server_t srv;
        brpc_rpc_server_init(&srv);
        brpc_rpc_register(&srv, "add", handle_add, NULL);

        /* Blocking recv — waits for client request */
        brpc_channel_recv(&server);

        /* Find stream with data using accessor */
        brpc_stream_t *s = NULL;
        while ((s = brpc_channel_next_ready_stream(&server,
                    s ? s->stream_id : 0)) != NULL) {
            char buf[1024];
            int n = brpc_stream_read(s, (uint8_t *)buf, sizeof(buf) - 1);
            buf[n] = '\0';
            brpc_rpc_server_dispatch(&srv, &server, s->stream_id, buf, n);
        }

        brpc_channel_destroy(&server);
        close(sv[0]);
        _exit(0);
    }

    /* Parent = client */
    close(sv[0]);
    brpc_channel_t client;
    brpc_channel_init(&client, sv[1], 0, 0);
    brpc_stream_t *stream = brpc_channel_open_stream(&client);

    /* Build and send request */
    char req[256];
    int len = brpc_rpc_build_request(req, sizeof(req), "add", "[1,2]", "1");
    brpc_channel_send_data(&client, stream->stream_id,
                           (uint8_t *)req, len, 0);

    /* Wait for server response */
    brpc_channel_recv(&client);
    char resp[1024];
    int n = brpc_stream_read(stream, (uint8_t *)resp, sizeof(resp) - 1);
    resp[n] = '\0';
    printf("Client got: %s\n", resp);

    brpc_channel_destroy(&client);
    close(sv[1]);
    wait(NULL);
    return 0;
}
```

### Python (minimal example)

```python
from brpc import RpcServer, RpcClient, Channel
import socket
import threading
import time

s1, s2 = socket.socketpair()

def run_server():
    server = Channel(s1.fileno(), is_server=True)
    srv = RpcServer()

    @srv.method("add")
    def handle_add(params):
        a = params[0].as_int()
        b = params[1].as_int()
        return a + b

    # Single recv + dispatch
    server.recv()
    # In real code, iterate ready streams and dispatch each
    # The Python Channel object exposes .stream_count and indexing

    server.destroy()

def run_client():
    time.sleep(0.01)  # Let server start
    client = Channel(s2.fileno(), is_server=False)
    stream = client.open_stream()
    cli = RpcClient(client, stream.stream_id)

    cli.notify("add", [1, 2])
    print("Client: sent add(1, 2)")

    client.destroy()

t1 = threading.Thread(target=run_server, daemon=True)
t2 = threading.Thread(target=run_client)
t1.start()
t2.start()
t2.join()
time.sleep(0.1)
s1.close()
s2.close()
```

## Public channel API

Channels use accessor functions — internal struct is opaque:

```c
// Check if closed
if (brpc_channel_is_closed(&server)) { ... }

// Get stream count
int count = brpc_channel_stream_count(&server);

// Iterate streams by index
for (int i = 0; i < count; i++) {
    brpc_stream_t *s = brpc_channel_get_stream(&server, i);
    // ...
}

// Find streams with pending data (preferred)
brpc_stream_t *s = NULL;
while ((s = brpc_channel_next_ready_stream(&server,
            s ? s->stream_id : 0)) != NULL) {
    // s has data available for reading
}
```

## Threading model

**brpc is NOT thread-safe.** All operations on a channel, stream, or RPC object must happen from a single thread.

| Object | Thread-safe? | Notes |
|--------|-------------|-------|
| `brpc_channel_t` | No | Must be accessed from one thread |
| `brpc_stream_t` | No | Owned by channel |
| `brpc_rpc_server_t` | No | Single dispatch thread |
| `brpc_rpc_client_t` | No | Blocking call, single thread |

**Ownership rules:**

- Channels own streams — destroying a channel destroys all its streams
- Streams must not outlive their channel
- RPC clients and servers borrow channels — they do not own them
- Arenas must outlive any values parsed from them

## Flow control

Three separate mechanisms:

**1. Stream ring buffer** (per-direction, in-process):

- Default size: 16 KiB per direction
- When full: `brpc_stream_write()` returns 0 (no bytes written). Caller retries later.
- When empty: `brpc_stream_read()` returns 0.

**2. Flow-control window** (per-stream, wire-level):

- Default: 65,536 bytes
- Peer may send up to window bytes before waiting for WINDOW_UPDATE
- Window is replenished when data is consumed from the recv buffer

**3. Kernel socket buffer** (OS-level):

- `brpc_channel_send_data()` writes directly to the socket
- May block if the kernel send buffer is full (depends on socket mode)
- `brpc_channel_recv()` uses blocking read

## Protocol guarantees

**Ordering:**
- In-order delivery within a stream (A, B, C arrives as A, B, C)
- No ordering guarantees across streams (stream 1 and stream 3 may interleave)

**Atomicity:**
- Each `brpc_channel_send_data()` call produces one DATA frame
- The stream layer presents complete frames — applications never observe partial frame payloads
- `brpc_stream_read()` returns all available bytes up to the requested size

**Stream fairness:**
- Streams are serviced in order of arrival (FIFO per connection)
- No round-robin or priority scheduling
- A flooding stream can delay others on the same connection
- brpc multiplexes streams but does not guarantee fairness
- Heavy traffic on one stream may increase latency for other streams sharing the same channel
- For latency-sensitive workloads, use separate channels

**Stream lifecycle:**
- Stream ID 0 is reserved and invalid
- Client streams use odd IDs (1, 3, 5...)
- Server streams use even IDs (2, 4, 6...)
- Closed streams never reopen
- New streams get monotonically increasing IDs

**Connection shutdown (GOAWAY):**
- GOAWAY prevents creation of new streams
- Existing streams may continue until completion, reset, or transport close
- After GOAWAY: `brpc_channel_open_stream()` fails, pending streams continue, closed channels cannot be reopened

## Blocking semantics

| Function | Blocks? | Notes |
|----------|---------|-------|
| `brpc_channel_recv()` | Yes | Blocks until data arrives on socket |
| `brpc_channel_pump()` | No | Returns immediately (EAGAIN if no data) |
| `brpc_channel_send_data()` | Depends | Blocks if fd is blocking and kernel buffer full |
| `brpc_stream_read()` | No | Returns available bytes from ring buffer |
| `brpc_rpc_call()` | Yes | Sends request, waits for response (no timeout) |
| `brpc_rpc_call_timeout()` | Yes | Like call(), but returns `BRPC_RPC_ERROR_TIMEOUT` after `timeout_ms` |
| `brpc_rpc_notify()` | No | Fire-and-forget, no response expected |

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
    user_id = params["id"].as_int()      # → int
    name = params["name"].as_str()       # → str
    active = params.get("active", None)  # → JsonValue or None

    # Return plain Python types — serialized automatically
    return {"id": user_id, "name": name}
```

**RpcServer methods accept and return plain Python types.** The `params` argument is the only place you see `JsonValue`.

## Limits

| Limit | Value |
|-------|-------|
| Max frame payload | 16 MiB |
| Max concurrent streams | Configurable (default 256) |
| Max channels | Limited by OS file descriptors |
| Max JSON nesting depth | 32 levels |
| Max JSON document size | Limited by arena size (default 64 KiB) |
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

**Methodology**: These are microbenchmarks on a single thread. CPU governor set to `performance`. Median of 10 million iterations per operation. Numbers represent ideal-case throughput and should not be interpreted as production latency under concurrent load. Benchmark code is included in `bench.c`.

## Comparison

| | brpc | gRPC | Raw TCP + JSON |
|---|---|---|---|
| RTT | 2.4µs | Varies by transport/language | ~1µs |
| Multiplexing | Yes | Yes | No |
| Flow control | Yes | Yes | No |
| TLS | Yes (OpenSSL) | Yes | No |
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

brpc targets homogeneous Linux deployments (x86_64 and aarch64). Cross-platform wire compatibility is not a design goal.

Frame headers are packed and unaligned. Do not cast network buffers directly to struct pointers — use the `brpc_frame_decode()` function instead.

The wire format is experimental until v1.0. A protocol version field will be added before v1.0 release.

## Roadmap

```text
v0.2
  - Stable public API
  - epoll/kqueue integration (fd, wants_read, wants_write)
  - Improved backpressure signals (send_window, is_writable)
  - RPC timeouts (brpc_rpc_call_timeout)
  - Stream cancellation (RST_STREAM, brpc_rpc_cancel)
  - Connection lifecycle callbacks (on_disconnect, on_close)
  - SETTINGS handshake with protocol version

v0.3
  - Stable wire format
  - Backward compatibility guarantees

v1.0

Post v1.0
  - Rust bindings
  - Go bindings
  - Async/await support
```

## FAQ

**Q: Why not just use gRPC?**
A: If you need TLS, cross-language support, or a mature ecosystem, use gRPC. brpc is for when those aren't needed and you want simplicity + speed.

**Q: Is brpc HTTP/2 compatible?**
A: No. brpc borrows ideas such as multiplexed streams and flow control, but uses its own framing protocol and wire format. HTTP/2 clients and servers cannot communicate with brpc.

**Q: Can I use this in production?**
A: Yes, in trusted networks or behind TLS termination. brpc now supports TLS via OpenSSL. The wire format is experimental until v1.0.

**Q: Does send_data drop data?**
A: No. If the stream buffer is full, `brpc_stream_write()` returns 0 bytes written. The caller retries. Data is never silently dropped.

**Q: Why JsonValue instead of plain Python types?**
A: The parser uses zero-copy arena allocation. JsonValue wraps C pointers. RpcServer handlers return plain Python types.

**Q: Can RPC calls be cancelled?**
A: Yes. Use `brpc_rpc_call_timeout()` for timeout-based cancellation, or `brpc_rpc_cancel()` to send a RST_STREAM frame that aborts the stream immediately.

**Q: What happens on disconnect?**
A: No automatic reconnect. If a channel closes: all streams fail, pending RPC calls return errors, application creates a new channel. Automatic retries are the application's responsibility.

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
| TLS | Done (OpenSSL, client + server) |
| Authentication | Not implemented |
| Compression | Done (zlib, per-frame COMPRESSED flag) |
| Async/event-loop integration | Python (AsyncChannel, AsyncRpcClient) + C (fd, wants_read, wants_write) |
| Application-level backpressure | Partial (send_window, is_writable, available_write) |
| Stream cancellation (RST_STREAM) | Done |
| RPC timeout | Done |
| Connection lifecycle callbacks | Done (on_new_stream, on_disconnect, on_close) |
| Protocol versioning | Done (SETTINGS frame, BRPC_PROTOCOL_VERSION) |
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
