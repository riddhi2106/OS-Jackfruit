#!/bin/bash
# scheduler_test.sh - Running Scheduler Experiments

set -e

echo -e "\033[0;34m== Running Scheduler Experiments ==\033[0m"

prompt_screenshot() {
    echo -e "\n\033[0;31m📸 TAKE SCREENSHOT $1: $2\033[0m"
    echo -e "\033[0;31mPress [ENTER] when you have taken the screenshot...\033[0m"
    read -r dummy
}

# Build all workloads
make ci

mkdir -p rootfs-base
if [ ! -d "rootfs-base/bin" ]; then
    echo -e "\033[0;34mDownloading ARM64 Alpine Rootfs...\033[0m"
    wget -qO alpine.tar.gz https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz
    tar -xzf alpine.tar.gz -C rootfs-base
    rm alpine.tar.gz
fi

# Setup rootfs for tests
cp -a rootfs-base rootfs-test1
cp -a rootfs-base rootfs-test2
cp cpu_hog io_pulse rootfs-test1/
cp cpu_hog io_pulse rootfs-test2/

# Start supervisor
sudo ./engine supervisor ./rootfs-base > supervisor_test.log 2>&1 &
SUPERVISOR_PID=$!
sleep 2

echo -e "\n\033[0;32mExperiment 1: Two CPU-bound containers with different nice values\033[0m"
# Container alpha: nice 0 (default)
# Container beta: nice 10 (lower priority)
sudo ./engine start alpha ./rootfs-test1 "/cpu_hog 10" --nice 0
sudo ./engine start beta ./rootfs-test2 "/cpu_hog 10" --nice 10

echo -e "Waiting for both to complete..."
sleep 15
sudo ./engine ps

echo -e "\n\033[0;32mExperiment 2: CPU-bound vs I/O-bound\033[0m"
# Container gamma: cpu_hog
# Container delta: io_pulse
cp -a rootfs-base rootfs-test3
cp cpu_hog io_pulse rootfs-test3/
sudo ./engine start gamma ./rootfs-test1 "/cpu_hog 10"
sudo ./engine start delta ./rootfs-test3 "/io_pulse 10"

echo -e "Waiting for both to complete..."
sleep 15
sudo ./engine ps

prompt_screenshot "7" "Scheduling experiment (Shows terminal output indicating different completion times based on nice values)"

# Cleanup
sudo kill $SUPERVISOR_PID
sudo umount rootfs-test1/proc 2>/dev/null || true
sudo umount rootfs-test2/proc 2>/dev/null || true
sudo umount rootfs-test3/proc 2>/dev/null || true
sudo rm -rf rootfs-test1 rootfs-test2 rootfs-test3

echo -e "\n\033[0;34m== Experiments Complete ==\033[0m"
echo -e "Check the output above and the logs/ directory for your data."
