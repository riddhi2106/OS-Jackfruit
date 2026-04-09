#!/bin/bash
# demo.sh - Automated Demonstration Script for Multi-Container Runtime

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}== Multi-Container Runtime Demo ==${NC}"

prompt_screenshot() {
    echo -e "\n${RED}📸 TAKE SCREENSHOT $1: $2${NC}"
    echo -e "${RED}Press [ENTER] when you have taken the screenshot...${NC}"
    read -r dummy
}

# 1. Build project
echo -e "\n${BLUE}[1/8] Building project...${NC}"
make clean
make ci # Build user-space binaries first
sudo make module # Build kernel module

# 2. Load Kernel Module
echo -e "\n${BLUE}[2/8] Loading Kernel Module...${NC}"
sudo rmmod monitor 2>/dev/null || true
sudo insmod monitor.ko
ls -l /dev/container_monitor

# 3. Setup RootFS
echo -e "\n${BLUE}[3/8] Preparing Root Filesystems...${NC}"
mkdir -p rootfs-base
if [ ! -d "rootfs-base/bin" ]; then
    echo -e "${BLUE}Downloading ARM64 Alpine Rootfs...${NC}"
    wget -qO alpine.tar.gz https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz
    tar -xzf alpine.tar.gz -C rootfs-base
    rm alpine.tar.gz
fi

sudo umount rootfs-alpha/proc 2>/dev/null || true
sudo umount rootfs-beta/proc 2>/dev/null || true
sudo umount rootfs-gamma/proc 2>/dev/null || true
rm -rf rootfs-alpha rootfs-beta rootfs-gamma
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

# Copy workloads into rootfs
cp memory_hog cpu_hog io_pulse rootfs-alpha/
cp memory_hog cpu_hog io_pulse rootfs-beta/

# 4. Start Supervisor
echo -e "\n${BLUE}[4/8] Starting Supervisor...${NC}"
sudo ./engine supervisor ./rootfs-base > supervisor.log 2>&1 &
SUPERVISOR_PID=$!
sleep 2

# 5. Launch Containers (Multi-container + Metadata Demo)
echo -e "\n${BLUE}[5/8] Launching Containers...${NC}"
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog 5" --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta "/io_pulse 5" --soft-mib 64 --hard-mib 96

echo -e "\n${GREEN}Check 'ps' output:${NC}"
sudo ./engine ps

prompt_screenshot "1 & 2" "Multi-container supervision & Metadata tracking (Shows 'start' commands and 'ps' output)"

# 6. Logging Pipeline Demo
echo -e "\n${BLUE}[6/8] Verifying Logs...${NC}"
sleep 3
echo -e "${GREEN}Logs for alpha:${NC}"
sudo ./engine logs alpha | head -n 5

prompt_screenshot "3 & 4" "Bounded-buffer logging & CLI IPC (Shows the 'logs' request returning data over the socket)"

# 7. Memory Limit Enforcement Demo
echo -e "\n${BLUE}[7/8] Demonstrating Memory Limits...${NC}"
echo -e "Starting 'gamma' with memory_hog (hard limit 50MiB)..."
cp -a rootfs-base rootfs-gamma
cp memory_hog rootfs-gamma/
sudo ./engine start gamma ./rootfs-gamma "/memory_hog 100" --soft-mib 20 --hard-mib 50
sleep 5
echo -e "${GREEN}Verifying gamma state after limit hit:${NC}"
sudo ./engine ps | grep gamma || true
echo -e "${GREEN}Kernel logs (dmesg):${NC}"
sudo dmesg | tail -n 5 | grep container_monitor || true

prompt_screenshot "5 & 6" "Soft-limit warning & Hard-limit enforcement (Shows the kernel dmesg logs for limits)"

# 8. Clean Teardown
echo -e "\n${BLUE}[8/8] Cleaning up...${NC}"
sudo ./engine stop alpha
sudo ./engine stop beta
sleep 2
sudo ./engine ps

prompt_screenshot "8" "Clean teardown (Shows 'stopped' metadata and reaped processes)"

sudo kill $SUPERVISOR_PID
sudo rmmod monitor
sudo make clean

echo -e "\n${BLUE}== Demo Complete ==${NC}"
echo -e "You can now capture screenshots from the output above."
