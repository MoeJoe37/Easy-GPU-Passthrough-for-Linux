# Build

## Requirements
- Qt 6 Core
- Qt 6 Widgets
- Qt 6 Network
- CMake 3.20+
- libvirt CLI (`virsh`) for XML validation and backup

## Build
```bash
cmake -S . -B build
cmake --build build -j
```

## Runtime model
- `gpu-switcher-helperd.service` keeps the local helper socket available for the GUI and CLI.
- `gpu-switcher-boot.service` applies a pending boot target early in the next boot so the GPU is rebound before the graphical session starts.

## Install
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
