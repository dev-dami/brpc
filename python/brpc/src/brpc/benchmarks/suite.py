#!/usr/bin/env python3
"""
brpc benchmark suite — bRPC vs gRPC performance comparison.

Run: python -m brpc.benchmarks.suite
"""
import sys
import os
import time
import socket
import json as stdlib_json

# Ensure brpc is importable
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from brpc import JsonParser, JsonWriter, Stream, Channel, Profiler
from brpc.benchmarks.runner import bench, bench_throughput

# ═══════════════════════════════════════════════════════════════════════════
# Sample payloads
# ═══════════════════════════════════════════════════════════════════════════

SMALL_JSON = '{"method":"ping","id":1}'
MEDIUM_JSON = '{"jsonrpc":"2.0","method":"getUser","id":42,"params":{"userId":42,"includeProfile":true,"fields":["name","email","avatar"]}}'
LARGE_JSON = json = '{"users":[{"id":1,"name":"Alice","email":"alice@example.com","tags":["admin","active"],"score":99.5},{"id":2,"name":"Bob","email":"bob@example.com","tags":["user"],"score":87.3},{"id":3,"name":"Charlie","email":"charlie@example.com","tags":["user","moderator"],"score":92.1},{"id":4,"name":"Diana","email":"diana@example.com","tags":["admin"],"score":95.7},{"id":5,"name":"Eve","email":"eve@example.com","tags":["user","active"],"score":88.9}],"metadata":{"total":5,"page":1,"hasMore":false}}'

# ═══════════════════════════════════════════════════════════════════════════
# bRPC Benchmarks
# ═══════════════════════════════════════════════════════════════════════════

def bench_brpc_json_parse():
    parser = JsonParser()
    results = []
    for label, payload in [("small", SMALL_JSON), ("medium", MEDIUM_JSON), ("large", LARGE_JSON)]:
        r = bench(f"brpc json_parse ({label})", lambda: parser.parse(payload))
        results.append(r)
    return results


def bench_brpc_json_serialize():
    results = []

    # Small
    w = JsonWriter()
    def serialize_small():
        w.reset()
        w.obj_start()
        w.obj_key("method")
        w.str("ping")
        w.obj_key("id")
        w.int(1)
        w.obj_end()
        w.finish()
    results.append(bench("brpc json_serialize (small)", serialize_small))

    # Medium
    def serialize_medium():
        w.reset()
        w.obj_start()
        w.obj_key("jsonrpc")
        w.str("2.0")
        w.obj_key("method")
        w.str("getUser")
        w.obj_key("id")
        w.int(42)
        w.obj_key("params")
        w.obj_start()
        w.obj_key("userId")
        w.int(42)
        w.obj_key("includeProfile")
        w.bool(True)
        w.obj_end()
        w.obj_end()
        w.finish()
    results.append(bench("brpc json_serialize (medium)", serialize_medium))

    return results


def bench_brpc_frame():
    frame_payload = b"x" * 128
    results = []

    # Encode
    from brpc import lib
    import ctypes

    encode_buf = (ctypes.c_ubyte * (10 + len(frame_payload)))()
    def encode_frame():
        lib.brpc_frame_encode(None, encode_buf, 10 + len(frame_payload))
    results.append(bench("brpc frame_encode", encode_frame))

    # Decode
    dec_buf = (ctypes.c_ubyte * (10 + len(frame_payload)))()
    def decode_frame():
        lib.brpc_frame_decode(dec_buf, 10 + len(frame_payload), None)
    results.append(bench("brpc frame_decode", decode_frame))

    return results


def bench_brpc_stream():
    results = []
    s = Stream(1, 65536)
    data = b"x" * 1024

    def stream_write():
        s.write(data)
    results.append(bench("brpc stream_write (1KB)", stream_write))

    # Fill and read
    for _ in range(100):
        s.write(data)
    def stream_read():
        s.read(1024)
    results.append(bench("brpc stream_read (1KB)", stream_read))
    s.destroy()
    return results


def bench_brpc_channel():
    """Benchmark full round-trip over a socketpair."""
    results = []
    s1, s2 = socket.socketpair()

    client = Channel(s1.fileno(), is_server=False)
    server = Channel(s2.fileno(), is_server=True)

    cs = client.open_stream()
    payload = MEDIUM_JSON.encode()

    # Send (client → server)
    def channel_send():
        client.send_data(cs.stream_id, payload, end_stream=True)
    results.append(bench("brpc channel_send", channel_send, iterations=10_000, warmup=100))

    # Full round-trip: send + recv + parse
    Profiler.reset()
    def roundtrip():
        client.send_data(cs.stream_id, payload, end_stream=True)
        server.recv()
    results.append(bench_throughput("brpc roundtrip (send+recv)", roundtrip, duration_sec=2.0))

    client.destroy()
    server.destroy()
    s1.close()
    s2.close()
    return results


# ═══════════════════════════════════════════════════════════════════════════
# stdlib JSON Benchmarks (baseline)
# ═══════════════════════════════════════════════════════════════════════════

def bench_stdlib_json():
    results = []
    for label, payload in [("small", SMALL_JSON), ("medium", MEDIUM_JSON), ("large", LARGE_JSON)]:
        r = bench(f"stdlib json.loads ({label})", lambda: stdlib_json.loads(payload))
        results.append(r)

    obj = {"method": "getUser", "id": 42, "params": {"userId": 42}}
    r = bench("stdlib json.dumps (medium)", lambda: stdlib_json.dumps(obj))
    results.append(r)
    return results


# ═══════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════

def main():
    print("=" * 120)
    print("bRPC Performance Benchmark Suite")
    print("=" * 120)

    all_results = []

    print("\n── JSON Parse ──")
    for r in bench_brpc_json_parse():
        print(r.report())
        all_results.append(r)

    print("\n── JSON Serialize ──")
    for r in bench_brpc_json_serialize():
        print(r.report())
        all_results.append(r)

    print("\n── stdlib JSON (baseline) ──")
    for r in bench_stdlib_json():
        print(r.report())
        all_results.append(r)

    print("\n── Frame Encode/Decode ──")
    for r in bench_brpc_frame():
        print(r.report())
        all_results.append(r)

    print("\n── Stream Write/Read ──")
    for r in bench_brpc_stream():
        print(r.report())
        all_results.append(r)

    print("\n── Channel Round-Trip ──")
    for r in bench_brpc_channel():
        print(r.report())
        all_results.append(r)

    # ── Summary ──────────────────────────────────────────────────────
    print("\n" + "=" * 120)
    print("SUMMARY")
    print("=" * 120)

    # Find speedup vs stdlib
    stdlib_small = next((r for r in all_results if r.name == "stdlib json.loads (small)"), None)
    brpc_small = next((r for r in all_results if r.name == "brpc json_parse (small)"), None)
    if stdlib_small and brpc_small and stdlib_small.avg_ns > 0:
        speedup = stdlib_small.avg_ns / brpc_small.avg_ns
        print(f"\n  brpc vs stdlib json.loads (small):  {speedup:.1f}x {'faster' if speedup > 1 else 'slower'}")

    stdlib_med = next((r for r in all_results if r.name == "stdlib json.loads (medium)"), None)
    brpc_med = next((r for r in all_results if r.name == "brpc json_parse (medium)"), None)
    if stdlib_med and brpc_med and stdlib_med.avg_ns > 0:
        speedup = stdlib_med.avg_ns / brpc_med.avg_ns
        print(f"  brpc vs stdlib json.loads (medium): {speedup:.1f}x {'faster' if speedup > 1 else 'slower'}")

    # Print profiling
    print("\n── bRPC Internal Profiling ──")
    Profiler.print()

    print("\n" + "=" * 120)
    return 0


if __name__ == "__main__":
    sys.exit(main())
