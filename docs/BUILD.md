# Build and installation

## Requirements

- CMake 3.20+
- C++20 compiler
- Qt 6 Core
- Qt 6 Widgets
- Qt 6 Network
- systemd
- libvirt / QEMU
- `virsh`
- `lspci` from `pciutils`
- VFIO-capable kernel and IOMMU-enabled firmware

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Install binaries and services

```bash
sudo install -m 0755 build/gpu-switcher-helperd /usr/local/bin/
sudo install -m 0755 build/gpu-switcher-gui /usr/local/bin/
sudo install -m 0755 build/gpu-switcher-ctl /usr/local/bin/
sudo install -m 0644 systemd/gpu-switcher-helperd.service /etc/systemd/system/
sudo install -m 0644 systemd/gpu-switcher-boot.service /etc/systemd/system/
sudo install -m 0755 hooks/qemu /etc/libvirt/hooks/qemu
sudo systemctl daemon-reload
sudo systemctl enable --now gpu-switcher-helperd.service gpu-switcher-boot.service
```

## Runtime model

- `gpu-switcher-helperd.service` runs as root and exposes `/run/gpu-switcher.sock` for the GUI and CLI.
- `gpu-switcher-boot.service` runs early at boot and applies the pending `nextBootMode` before the display manager starts.
- `/etc/libvirt/hooks/qemu` calls `gpu-switcher-ctl on-vm-stopped <domain>` when the VM stops.
- After the VM stops, the helper records a pending user decision instead of automatically rebooting.

## Quick verification

```bash
gpu-switcher-ctl status
gpu-switcher-ctl inventory
gpu-switcher-ctl diagnose
gpu-switcher-ctl simulateVm
```

## Post-VM-close CLI actions

```bash
# Reboot now and return the GPU to Linux
gpu-switcher-ctl restartHostNow

# Return the GPU to Linux on the next restart, without rebooting now
gpu-switcher-ctl returnHostNextRestart

# Keep the GPU assigned to the VM/VFIO until changed manually
gpu-switcher-ctl keepGpuForVm
```
