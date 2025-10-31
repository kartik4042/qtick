#!/bin/bash
# qtick setup script for RHEL 9.6
# Run with sudo: sudo bash scripts/setup.sh

set -e

echo "=== qtick System Setup for RHEL 9.6 ==="

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
  echo "Please run as root: sudo bash $0"
  exit 1
fi

# 1. Install dependencies
echo "[1/7] Installing dependencies..."
dnf install -y \
  gcc-c++ \
  cmake \
  git \
  hiredis-devel \
  spdlog-devel \
  redis \
  perf \
  numactl \
  hwloc \
  cpupower

# 2. Setup Redis directory
echo "[2/7] Setting up Redis..."
mkdir -p /var/run/redis
chmod 777 /var/run/redis
mkdir -p /var/log/redis
chown redis:redis /var/log/redis

# 3. CPU governor to performance
echo "[3/7] Setting CPU governor to performance..."
cpupower frequency-set -g performance || echo "Warning: cpupower failed, continuing..."

# 4. Disable swap
echo "[4/7] Disabling swap..."
swapoff -a
# To make permanent, comment out swap in /etc/fstab
sed -i.bak '/swap/s/^/#/' /etc/fstab

# 5. Set vm parameters
echo "[5/7] Setting kernel parameters..."
sysctl -w vm.swappiness=1
sysctl -w vm.overcommit_memory=1
sysctl -w net.core.somaxconn=65535
sysctl -w net.ipv4.tcp_max_syn_backlog=8192

# Make persistent
cat >> /etc/sysctl.d/99-qtick.conf <<EOF
vm.swappiness=1
vm.overcommit_memory=1
net.core.somaxconn=65535
net.ipv4.tcp_max_syn_backlog=8192
EOF

# 6. Setup kdb+ (download if needed)
echo "[6/7] Checking kdb+..."
if [ ! -d "kdb_api" ]; then
  echo "Creating kdb_api directory. Please download k.h and c.o from:"
  echo "  https://github.com/KxSystems/kdb/tree/master/c/c"
  mkdir -p kdb_api
  echo "Place k.h and c.o (or l64/c.o for Linux) in kdb_api/"
else
  echo "kdb_api directory exists"
fi

# 7. Isolate CPUs (optional - requires reboot)
echo "[7/7] CPU isolation setup..."
echo "To isolate CPUs 2-3 for latency-critical threads:"
echo "  Edit /etc/default/grub and add to GRUB_CMDLINE_LINUX:"
echo "    isolcpus=2-3 nohz_full=2-3 rcu_nocbs=2-3"
echo "  Then run: grub2-mkconfig -o /boot/grub2/grub.cfg"
echo "  And reboot"

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "1. Download kdb+ from https://kx.com (free 32-bit version available)"
echo "2. Place k.h and c.o in kdb_api/"
echo "3. Build: mkdir build && cd build && cmake .. && make -j"
echo "4. Copy Redis config: sudo cp config/redis.conf /etc/redis/redis-qtick.conf"
echo "5. Start Redis: redis-server /etc/redis/redis-qtick.conf"
echo "6. Start RDB: q q/rdb.q -p 5010"
echo "7. Start bridge: ./build/bridge"
echo "8. Start feed: ./build/feed_sim"
echo ""
echo "Performance testing:"
echo "  sudo perf stat -d ./build/bridge"
echo "  taskset -c 2 ./build/bridge"
echo ""