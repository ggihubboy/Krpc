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

## Template

Fill this table after a local run. The previous 2-core VM figure (Login 100000,
QPS ~5000) is historical and should not be reused without re-running.

| Date | Host | CPU | RAM | Kernel | Compiler | Build | Command | QPS | error_rate | p50_us | p99_us | notes |
|------|------|-----|-----|--------|----------|-------|---------|-----|------------|--------|--------|-------|
| | | | | | | Release | `KRPC_BENCH_PAYLOAD=0 ./scripts/bench.sh` | | | | | |
| | | | | | | Release | `KRPC_BENCH_PAYLOAD=32768 ./scripts/bench.sh` | | | | | |
