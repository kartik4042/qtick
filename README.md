# qtick - Ultra-Low-Latency Market Data Pipeline

High-performance tick processing system designed for sub-millisecond end-to-end latency on RHEL 9.6.

## Architecture

```
[Tick Generator] (C++)
   │
   ├─(UNIX Socket)─→ [Redis Pub/Sub]
   │                      │
   └──────────────────────┘
                          │
                    [C++ Ingest Bridge]
                     ├─ Lock-free SPSC Ring
                     ├─ Batch Accumulator
                     └─ kdb+ IPC (binary)
                          │
                    [kdb+ RDB Process]
                     └─ Vectorized Analytics (VWAP, Vol)
```

**Target**: End-to-end <1ms (realistic), tens of µs (with shared-memory ring & tuning)

## Components

- **Feed Simulator** (`feed_sim`): Generates synthetic market ticks, publishes to Redis
- **Ingest Bridge** (`bridge`): Redis subscriber → lock-free ring → batched q IPC
- **kdb+ RDB** (`rdb.q`): Real-time database with vectorized VWAP computation
- **Redis**: Message bus (UNIX socket mode for low latency)

## Prerequisites

### System Requirements
- RHEL 9.6 or compatible Linux
- kdb+ (download from https://kx.com)
- C++20 compiler (GCC 11+ or Clang 12+)

### Dependencies
```bash
sudo dnf install -y gcc-c++ cmake git hiredis-devel spdlog-devel redis perf numactl
```

## Quick Start

### 1. System Setup
```bash
sudo bash scripts/setup.sh
```

This configures:
- CPU governor to performance mode
- Disables swap
- Kernel networking parameters
- Redis directories

### 2. Get kdb+ API Files
Download from https://github.com/KxSystems/kdb/tree/master/c/c

```bash
mkdir -p kdb_api
cd kdb_api
# Download k.h and c.o (l64/c.o for Linux x64)
wget https://raw.githubusercontent.com/KxSystems/kdb/master/c/c/k.h
# Place appropriate c.o for your platform
cd ..
```

### 3. Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..
```

### 4. Run

**Option A: Automated (recommended for first test)**
```bash
bash scripts/run_local.sh
```

**Option B: Manual**

Terminal 1 - Redis:
```bash
redis-server config/redis.conf
```

Terminal 2 - kdb+:
```bash
q q/rdb.q -p 5010
```

Terminal 3 - Bridge:
```bash
./build/bridge /var/run/redis/redis.sock localhost 5010
```

Terminal 4 - Feed:
```bash
./build/feed_sim /var/run/redis/redis.sock
```

## Configuration

### Tuning Parameters

**Ring Buffer** (`common/ring.hpp`):
- `RING_SIZE = 4096` - must be power of 2, increase for burst handling

**Batch Size** (`ingest_bridge/main.cpp`):
- `BATCH_SIZE = 64` - trades latency vs throughput
- Smaller = lower latency, more syscalls
- Larger = higher throughput, slightly higher tail latency

**Symbols** (`common/tick.hpp`):
- Edit `Symbol` enum to add instruments

### Performance Tuning

**CPU Isolation** (requires reboot):
```bash
# Edit /etc/default/grub, add to GRUB_CMDLINE_LINUX:
isolcpus=2-3 nohz_full=2-3 rcu_nocbs=2-3

# Apply and reboot
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
```

**Pin to specific cores**:
```bash
taskset -c 2 ./build/bridge
taskset -c 3 ./build/feed_sim
```

**NUMA binding**:
```bash
numactl --cpunodebind=0 --membind=0 ./build/bridge
```

## Monitoring

### Query kdb+ Stats
```bash
q -p 5010
```
```q
q)getStats[]      / batch/tick counts
q)getVWAP[]       / per-symbol VWAP
q)getTrades[100]  / last 100 ticks
q)count trades    / total tick count
```

### Redis Stats
```bash
redis-cli -s /var/run/redis/redis.sock info stats
```

### System Performance
```bash
# CPU/cache stats
sudo perf stat -d -p $(pgrep bridge)

# Flame graph profiling
sudo perf record -g -p $(pgrep bridge)
sudo perf report

# Hardware counters
sudo perf stat -e cycles,instructions,cache-references,cache-misses ./build/bridge
```

## Benchmarking

### Latency Measurement Points
- **t0**: Tick generation timestamp
- **t1**: Bridge receives from Redis
- **t2**: Pushed to ring buffer
- **t3**: Batch send to q started
- **t4**: q upd() returns
- **End-to-end**: t4 - t0

### Test Scenarios
```bash
# Baseline: Current setup
./build/bridge

# Test batch sizes
# Edit BATCH_SIZE in ingest_bridge/main.cpp: 16, 32, 64, 128, 256

# Measure throughput
# Edit feed_sim/main.cpp: ticks_per_sec = 10000, 50000, 100000
```

### Expected Performance
- **JSON over TCP**: ~500-1000 µs
- **Binary over UNIX socket**: ~100-500 µs  ← current implementation
- **Shared memory ring**: ~10-50 µs (future)

## Project Structure
```
qtick/
├── CMakeLists.txt
├── README.md
├── common/
│   ├── tick.hpp        # Tick struct, batch container
│   ├── ring.hpp        # Lock-free SPSC ring
│   └── timing.hpp      # High-res timestamps
├── src/
│   ├── feed_sim/       # Tick generator
│   │   └── main.cpp
│   └── ingest_bridge/  # Redis→Ring→kdb+ pipeline
│       └── main.cpp
├── q/
│   └── rdb.q          # kdb+ RDB with VWAP
├── config/
│   └── redis.conf     # Redis tuning
├── scripts/
│   ├── setup.sh       # System setup
│   └── run_local.sh   # Test runner
└── kdb_api/           # k.h and c.o (download separately)
```

## Troubleshooting

**Bridge fails to connect to kdb+**:
```bash
# Check q is running
ps aux | grep " q "
netstat -tlnp | grep 5010
```

**Redis socket permission denied**:
```bash
sudo chmod 777 /var/run/redis/redis.sock
```

**Undefined symbol errors**:
```bash
# Make sure k.h and c.o match your platform
# For Linux x64, use l64/c.o from kdb+ download
```

**High drop count in bridge**:
- Increase `RING_SIZE` in code
- Reduce tick generation rate in feed_sim
- Profile with `perf` to find bottleneck

## Next Steps

1. **Measurement**: Add HdrHistogram for P99/P999 latency tracking
2. **Shared Memory**: Replace Redis with `mmap()` ring for sub-10µs
3. **TSC Timestamps**: Replace `clock_gettime` with calibrated `rdtsc()`
4. **SIMD**: Vectorize VWAP computation in q or C++
5. **Multi-Symbol**: Scale to 1000+ symbols with symbol dictionary
6. **Persistence**: Add HDB write-down for historical storage

## References

- kdb+: https://code.kx.com/q/
- hiredis: https://github.com/redis/hiredis
- spdlog: https://github.com/gabime/spdlog
- Linux Performance: https://www.brendangregg.com/linuxperf.html

## License

MIT