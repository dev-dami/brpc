<p align="center">
  <h1 align="center">brpc</h1>
  <p align="center">Small, fast JSON-RPC over multiplexed streams for trusted networks.</p>
</p>

<p align="center">
  <a href="https://github.com/dev-dami/brpc/actions"><img src="https://github.com/dev-dami/brpc/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/dev-dami/brpc/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License"></a>
  <a href="https://github.com/dev-dami/brpc"><img src="https://img.shields.io/badge/version-0.2.0-green.svg" alt="Version"></a>
</p>

---

brpc is a small RPC framework for trusted networks. It provides multiplexed streams, JSON-RPC 2.0, and C/Python bindings with minimal dependencies, prioritizing simplicity and low latency.

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

- Human-readable — easy to inspect in logs or packet captures
- No schema compiler needed
- Dynamic payloads without codegen
- Familiar to Python and JavaScript developers

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
- You need backward-compatible protocol versioning
- You're exposing services to the public internet

## Quick start

### C

```c
#include "brpc_rpc.h"
#include <sys/socket.h>
#include <unistd.h>

int handle_add(const brpc_rpc_request_t *req,
               brpc_rpc_response_t *resp, void *ctx) {
    int64_t a = json_get_int(json_array_get(req->params, 0), 0);
    int64_t b = json_get_int(json_array_get(req->params, 1), 0);
    /* Result is set automatically via resp->result */
    return BRPC_RPC_OK;
}

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    /* Server */
    brpc_channel_t ch;
    brpc_channel_init(&ch, sv[0], 1, 0);
    brpc_rpc_server_t srv;
    brpc_rpc_server_init(&srv);
    brpc_rpc_register(&srv, "add", handle_add, NULL);
    brpc_rpc_server_poll(&srv, &ch);  /* recv + dispatch in one call */

    /* Client */
    brpc_channel_t client;
    brpc_channel_init(&client, sv[1], 0, 0);
    brpc_stream_t *s = brpc_channel_open_stream(&client);
    brpc_rpc_client_t cli;
    brpc_rpc_client_init(&cli, &client, s->stream_id);
    char resp[256];
    brpc_rpc_call_timeout(&cli, "add", "[1,2]", resp, sizeof(resp), 1000);
    printf("Result: %s\n", resp);

    brpc_channel_destroy(&ch);
    brpc_channel_destroy(&client);
    close(sv[0]); close(sv[1]);
}
```

### Python

```python
from brpc import RpcServer, Channel
import socket, threading, time

s1, s2 = socket.socketpair()

def server():
    ch = Channel(s1.fileno(), is_server=True)
    srv = RpcServer()
    @srv.method("add")
    def add(p): return p[0].as_int() + p[1].as_int()
    srv.poll(ch)  # recv + dispatch in one call

def client():
    time.sleep(0.01)
    ch = Channel(s2.fileno())
    s = ch.open_stream()
    from brpc import RpcClient
    cli = RpcClient(ch, s.stream_id)
    resp = cli.call("add", [1, 2])
    print(f"Result: {resp}")

t = threading.Thread(target=server, daemon=True)
t.start()
client()
```

### Python asyncio

```python
import asyncio
from brpc import AsyncRpcClient

async def main():
    async with AsyncRpcClient.connect("127.0.0.1", 8000) as cli:
        result = await cli.call("add", [3, 4])
        print(f"Result: {result}")

asyncio.run(main())
```

## API overview

### Channel

```c
brpc_channel_init(&ch, fd, is_server, max_streams);
brpc_channel_init_ex(&ch, fd, &cfg);  /* with TLS, compression */

/* Stream management */
brpc_stream_t *s = brpc_channel_open_stream(&ch);
brpc_stream_t *s = brpc_channel_find_stream(&ch, stream_id);

/* Ready-stream iteration */
brpc_channel_reset_ready_iter(&ch);
while ((s = brpc_channel_next_ready_stream(&ch, last_id))) {
    /* s has data available */
}

/* Send data */
brpc_channel_send_data(&ch, stream_id, data, len, end_stream);

/* Observability */
brpc_stats_t stats;
brpc_channel_stats(&ch, &stats);
```

### RPC

```c
/* Server */
brpc_rpc_server_poll(&srv, &ch);  /* recv + dispatch all streams */

/* Client */
brpc_rpc_call_timeout(&cli, "method", params_json, buf, sizeof(buf), 1000);
brpc_rpc_cancel(&cli);
```

### Error codes

```c
typedef enum brpc_error {
    BRPC_OK                     =   0,
    BRPC_ERROR_CLOSED           =  -1,
    BRPC_ERROR_TIMEOUT          =  -2,
    BRPC_ERROR_IO               =  -4,
    BRPC_ERROR_STREAM_NOT_FOUND =  -6,
    BRPC_ERROR_STREAM_CLOSED    =  -7,
    BRPC_ERROR_MAX_STREAMS      =  -8,
    BRPC_ERROR_TLS              = -10,
} brpc_error_t;

const char *brpc_error_string(err);  /* human-readable */
```

## Architecture

```
┌──────────────────────────────────────────────────┐
│             brpc_rpc (JSON-RPC 2.0)              │
│   Request/response, method dispatch, errors      │
├─────────────────┬────────────────────────────────┤
│  json_hotpath   │       brpc_channel             │
│ Arena parser +  │ Multiplexed TCP channel        │
│ streaming writer│ (TLS, compression, SETTINGS)   │
├─────────────────┼──────────┬─────────────────────┤
│                 │brpc_frame│   brpc_stream        │
│                 │ 10-byte  │ Bidirectional        │
│                 │ LE hdr   │ ring buffers         │
├─────────────────┴──────────┴─────────────────────┤
│                 brpc_prof + brpc_stats            │
│        Microsecond profiling + counters          │
└──────────────────────────────────────────────────┘
```

## Wire format

```
Offset  Size  Field
0       4     Stream ID
4       1     Frame Type (DATA=0, HEADERS=1, RST=2, SETTINGS=3, PING=4, GOAWAY=5, WINDOW_UPDATE=6)
5       1     Flags (END_STREAM=0x01, END_HEADERS=0x02, COMPRESSED=0x04)
6       4     Payload Length
10      N     Payload
```

The wire format is experimental until v1.0.

## Features

| Feature | Status |
|---------|--------|
| JSON parse/serialize | Done |
| Binary framing (10-byte header) | Done |
| Multiplexed streams | Done |
| Flow control (ring buffer + window) | Done |
| JSON-RPC 2.0 dispatch | Done |
| TLS (OpenSSL, client + server) | Done |
| Compression (zlib, negotiated via SETTINGS) | Done |
| RPC timeouts | Done |
| Stream cancellation (RST_STREAM) | Done |
| Connection lifecycle callbacks | Done |
| Event-loop integration (fd, wants_read, wants_write) | Done |
| Protocol versioning (SETTINGS handshake) | Done |
| Observability (bytes, frames, streams counters) | Done |
| Python bindings (sync + asyncio) | Done |
| Authentication | Not planned |

## Performance

Measured on AMD Ryzen 7 5800X, Linux 6.x, `cc -O2`, single thread, `socketpair(AF_UNIX)`:

| Operation | Latency | Throughput |
|-----------|---------|------------|
| JSON parse (28B) | 0.1µs | 6.75M msg/s |
| JSON parse (99B) | 0.4µs | 2.37M msg/s |
| JSON serialize (small) | 0.1µs | 7.79M msg/s |
| Frame encode (128B) | 0.1µs | 12.5M/s |
| Frame decode (128B) | 0.1µs | 10.3M/s |
| Stream write (1KB) | <0.1µs | 24.1M/s |
| Channel round-trip | 2.4µs | 413K/s |

**Memory**: Peak RSS 2MB for all components.

**Methodology**: Microbenchmarks on a single thread. CPU governor set to `performance`. Median of 10M iterations. Numbers represent ideal-case throughput, not production latency under concurrent load. Benchmark code in `bench.c`.

## Comparison

| | brpc | gRPC | Raw TCP + JSON |
|---|---|---|---|
| RTT | 2.4µs | Tens-hundreds of µs | ~1µs |
| Multiplexing | Yes | Yes | No |
| Flow control | Yes | Yes | No |
| TLS | Yes (OpenSSL) | Yes | No |
| Codegen | No | Yes (Protobuf) | No |
| Streaming | Yes | Yes | Manual |
| Languages | C, Python | 10+ | Any |
| JSON-RPC 2.0 | Built-in | No | Manual |

## Installation

### C

```bash
make all            # builds to build/
make test           # runs 114 C tests
make run            # runs the demo
make bench          # runs benchmarks
make clean          # removes build/
```

### Python

```bash
make install-python       # install system-wide
make install-python-dev   # install editable (dev mode)
make python-test          # build + run Python tests
```

## Project structure

```
brpc/
├── include/              # Public C headers
│   ├── brpc_error.h      # Error enum
│   ├── brpc_channel.h    # Channel, config, stats
│   ├── brpc_stream.h     # Stream abstraction
│   ├── brpc_frame.h      # Wire format, SETTINGS keys
│   ├── brpc_rpc.h        # JSON-RPC 2.0
│   ├── brpc_compress.h   # zlib compression
│   ├── brpc_tls.h        # OpenSSL TLS
│   └── json_hotpath.h    # JSON parser/serializer
├── src/                  # Implementation (12 files)
├── tests/                # 114 C tests
├── bench.c               # Benchmarks
├── python/brpc/          # Python package
│   ├── src/brpc/         # ctypes bindings + async
│   └── tests/            # 43 Python tests
├── Makefile
├── build.zig
└── .github/workflows/    # CI
```

## Roadmap

```text
v0.2 (current)
  Stable public API
  Event-loop integration
  Backpressure APIs
  RPC timeouts
  Stream cancellation
  Lifecycle callbacks
  SETTINGS handshake
  TLS (OpenSSL)
  Compression (zlib)
  Protocol versioning
  Async Python
  Observability counters

v0.3
  Stable wire format
  Backward compatibility guarantees

v1.0
  Production stable

Post v1.0
  Rust bindings
  Go bindings
```

## FAQ

**Q: Why not just use gRPC?**
A: If you need cross-language support or a mature ecosystem, use gRPC. brpc is for when you want simplicity + speed on trusted networks.

**Q: Is brpc HTTP/2 compatible?**
A: No. brpc uses its own framing protocol. HTTP/2 clients cannot speak to brpc.

**Q: Can I use this in production?**
A: Yes, in trusted networks or behind TLS termination. The wire format is experimental until v1.0.

**Q: Can RPC calls be cancelled?**
A: Yes. `brpc_rpc_call_timeout()` for timeout-based cancellation, `brpc_rpc_cancel()` for immediate RST_STREAM.

**Q: What happens on disconnect?**
A: No automatic reconnect. All streams fail, pending RPCs return errors. The application creates a new channel.

## License

MIT
