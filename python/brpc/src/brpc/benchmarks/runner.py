"""
Performance benchmark framework — bRPC vs gRPC.

Measures:
  - JSON parse/serialize throughput
  - Frame encode/decode throughput
  - Stream write/read throughput
  - Channel round-trip latency
  - Throughput (messages/sec)
"""
import time
import statistics
import socket
from dataclasses import dataclass, field
from typing import Callable


@dataclass
class BenchResult:
    name: str
    iterations: int
    times_ns: list[int] = field(default_factory=list)
    throughput: float = 0.0
    avg_ns: float = 0.0
    min_ns: float = 0.0
    max_ns: float = 0.0
    p50_ns: float = 0.0
    p99_ns: float = 0.0

    def compute(self):
        if not self.times_ns:
            return
        self.avg_ns = statistics.mean(self.times_ns)
        self.min_ns = min(self.times_ns)
        self.max_ns = max(self.times_ns)
        self.p50_ns = statistics.median(self.times_ns)
        sorted_times = sorted(self.times_ns)
        p99_idx = int(len(sorted_times) * 0.99)
        self.p99_ns = sorted_times[min(p99_idx, len(sorted_times) - 1)]
        if self.avg_ns > 0:
            self.throughput = 1_000_000_000.0 / self.avg_ns

    def report(self) -> str:
        avg_us = self.avg_ns / 1000.0
        min_us = self.min_ns / 1000.0
        max_us = self.max_ns / 1000.0
        p50_us = self.p50_ns / 1000.0
        p99_us = self.p99_ns / 1000.0
        return (
            f"  {self.name:<40s} "
            f"{self.iterations:>10d} iters  "
            f"avg={avg_us:>8.1f}us  "
            f"min={min_us:>8.1f}us  "
            f"p50={p50_us:>8.1f}us  "
            f"p99={p99_us:>8.1f}us  "
            f"max={max_us:>8.1f}us  "
            f"throughput={self.throughput:>10.0f}/s"
        )


def bench(name: str, fn: Callable, iterations: int = 100_000,
          warmup: int = 1_000) -> BenchResult:
    """Run a benchmark function, collecting timing data."""
    # Warmup
    for _ in range(warmup):
        fn()

    result = BenchResult(name=name, iterations=iterations)

    # Timed run
    for _ in range(iterations):
        t0 = time.perf_counter_ns()
        fn()
        t1 = time.perf_counter_ns()
        result.times_ns.append(t1 - t0)

    result.compute()
    return result


def bench_throughput(name: str, fn: Callable, duration_sec: float = 2.0) -> BenchResult:
    """Run a benchmark for a fixed duration, counting iterations."""
    # Warmup
    deadline = time.perf_counter() + 0.1
    while time.perf_counter() < deadline:
        fn()

    count = 0
    times_ns = []
    deadline = time.perf_counter() + duration_sec
    while time.perf_counter() < deadline:
        t0 = time.perf_counter_ns()
        fn()
        t1 = time.perf_counter_ns()
        times_ns.append(t1 - t0)
        count += 1

    result = BenchResult(name=name, iterations=count, times_ns=times_ns)
    result.compute()
    return result
