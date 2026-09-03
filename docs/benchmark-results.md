# Benchmark results

This file records **reproducible** numbers. Do not copy a QPS figure into a resume
without the machine, compiler, build type, and exact command.

## How to run

```bash
# ZooKeeper + two servers + Login/EchoBlob smoke
./scripts/demo.sh

# Login small-packet load
KRPC_BENCH_THREADS=4 KRPC_BENCH_REQUESTS=10000 KRPC_BENCH_PAYLOAD=0 ./scripts/bench.sh

# 32KB EchoBlob
KRPC_BENCH_THREADS=2 KRPC_BENCH_REQUESTS=200 KRPC_BENCH_PAYLOAD=32768 ./scripts/bench.sh
```

`krpc_bench` prints threads, total requests, success/fail, wall QPS, error rate,
and the process-local `RpcMetrics` snapshot (`p50_us` / `p95_us` / `p99_us` are
fixed-bucket upper bounds, not exact histograms).

## Recorded results

These figures were collected locally with zero-copy disabled. Latencies are
fixed-bucket upper bounds. Re-run before quoting results for another machine.

| Date | Host | CPU | RAM | Kernel | Compiler | Build | Command | QPS | error_rate | p50_us | p99_us | notes |
|------|------|-----|-----|--------|----------|-------|---------|-----|------------|--------|--------|-------|
| 2026-09-03 | lhs-virtual-machine | 2 cores | 3969032 kB | Linux 5.15.0-139-generic x86_64 | GCC 9.4.0 | Release | `KRPC_BENCH_THREADS=4 KRPC_BENCH_REQUESTS=10000 KRPC_BENCH_PAYLOAD=0 ./scripts/bench.sh` | 7465 | 0 | <=500 | <=5000 | 40000/40000 success; zero-copy off |
| 2026-09-03 | lhs-virtual-machine | 2 cores | 3969032 kB | Linux 5.15.0-139-generic x86_64 | GCC 9.4.0 | Release | `KRPC_BENCH_THREADS=2 KRPC_BENCH_REQUESTS=200 KRPC_BENCH_PAYLOAD=32768 ./scripts/bench.sh` | 2515 | 0 | <=1000 | <=5000 | 400/400 success; zero-copy off |
