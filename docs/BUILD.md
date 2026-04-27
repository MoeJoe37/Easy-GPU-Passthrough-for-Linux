# Build and install

## Dependencies

Install the distro packages for:

- CMake 3.20+
- C++20 compiler
- Qt6 Core / Widgets / Network development headers
- libvirt / virsh
- systemd
- pciutils (`lspci`)
- VFIO kernel modules (`vfio`, `vfio-pci`, `vfio_iommu_type1` depending on distro/kernel)

Examples:

```bash
# Fedora / Nobara / Bazzite-style hosts
sudo dnf install cmake gcc-c++ qt6-qtbase-devel libvirt-client libvirt-daemon-kvm pciutils

# Arch-style hosts
sudo pacman -S cmake gcc qt6-base libvirt pciutils

# Debian / Ubuntu-style hosts
sudo apt install cmake g++ qt6-base-dev libvirt-clients libvirt-daemon-system pciutils
```

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Install binaries and units

```bash
sudo install -m 0755 build/gpu-switcher-helperd /usr/local/bin/
sudo install -m 0755 build/gpu-switcher-gui /usr/local/bin/
sudo install -m 0755 build/gsc /usr/local/bin/
sudo install -m 0755 build/gpu-switcher-ctl /usr/local/bin/   # optional legacy CLI name

# `gsc` is the preferred CLI command. `gpu-switcher-ctl` is kept for compatibility.

sudo install -m 0644 systemd/gpu-switcher-helperd.service /etc/systemd/system/
sudo install -m 0644 systemd/gpu-switcher-boot.service /etc/systemd/system/
sudo install -m 0755 hooks/qemu /etc/libvirt/hooks/qemu

sudo systemctl daemon-reload
sudo systemctl enable --now gpu-switcher-helperd.service gpu-switcher-boot.service
```

Restart libvirt after installing the hook when your distro requires it:

```bash
sudo systemctl restart libvirtd 2>/dev/null || sudo systemctl restart virtqemud
```

## First run

```bash
gpu-switcher-gui
```

For single-GPU hosts:

1. enter the VM name or UUID;
2. press **Auto setup single GPU**;
3. run **Preflight**;
4. press **Reboot to VM** only when the report is acceptable.

CLI equivalent:

```bash
gsc autoSetupSingleGpu
gsc diagnose
gsc rebootToVm
```

## Recovery commands

```bash
gsc restartHostNow
gsc returnHostNextRestart
gsc keepGpuForVm
gsc safetyRecoverHostNow
gsc rebootToHost
```

## Safety defaults

Auto setup enables these safety defaults:

- `thermalGuardEnabled=true`
- `maxGpuTempC=85`
- `safetyAutoRecoveryMinutes=10`

The temperature guard reads Linux `hwmon/temp*_input` sensors for the selected GPU before VM handoff. If the VM stops and no user action is taken, the safety recovery timer reboots back to Host mode after the configured grace period.
