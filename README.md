# GPU Switcher

GPU Switcher is a C++20 / Qt6 Linux GUI, CLI, and root helper daemon for controlled GPU ownership switching between a Linux host and a Windows virtual machine that uses VFIO PCI passthrough.

The app uses a reboot-handoff model for the dangerous parts of the transition: it schedules the next boot target, restarts the machine when requested, applies the GPU binding early in boot, and can auto-start the configured libvirt VM after the GPU is attached to `vfio-pci`.

## Main workflow

### 1. Linux host → Windows VM

From the GUI, press **Reboot to VM**.

The helper will:

1. validate IOMMU, IOMMU group safety, VM XML, hook installation, fallback display / SSH acknowledgement, and GPU presence;
2. store the original Linux GPU/audio drivers;
3. schedule `nextBootMode=vm` in `/var/lib/gpu-switcher/config.ini`;
4. reboot the PC;
5. during early boot, bind the configured GPU/audio PCI functions to `vfio-pci`;
6. mark the current mode as `vm`;
7. auto-start the configured libvirt VM when `autoStartVmOnBoot=true`.

### 2. Windows VM closes

The libvirt QEMU hook calls:

```bash
gpu-switcher-ctl on-vm-stopped <domain>
```

The helper **does not automatically return the GPU to Linux**. It records:

```text
state = vm-stopped-awaiting-decision
vmStoppedAwaitingDecision = true
```

The GUI then shows three user choices:

| GUI option | What it does |
|---|---|
| **Restart now to Host** | Schedules `nextBootMode=host` and immediately reboots the PC. On the next boot, the GPU is rebound to the original Linux driver. |
| **Return on next restart** | Schedules `nextBootMode=host` but does **not** reboot now. The GPU returns to Linux whenever the user restarts later. |
| **Keep GPU with VM** | Clears the stopped-VM warning and keeps the GPU assigned to VM/VFIO mode until the user manually chooses **Reboot to Host**. |

### 3. Windows VM / VFIO → Linux host

From the GUI, press **Reboot to Host**, or use one of the post-VM-close recovery options.

The helper will:

1. schedule `nextBootMode=host`;
2. reboot when requested;
3. during early boot, clear `driver_override`;
4. unbind the GPU/audio devices from `vfio-pci`;
5. rebind them to the recorded original Linux drivers;
6. mark the current mode as `host`.

## Safety model

GPU passthrough can leave a machine without local display output. GPU Switcher is designed to be conservative:

- refuses VM mode when IOMMU groups are missing;
- refuses VM mode when the GPU shares an unsafe IOMMU group with unrelated hardware;
- refuses single-GPU passthrough unless fallback display / SSH recovery is acknowledged;
- validates the inactive libvirt domain XML before scheduling VM mode;
- backs up the inactive VM XML before VM-mode handoff;
- tracks original Linux drivers before binding to `vfio-pci`;
- keeps reboot-based recovery as the primary ownership boundary;
- records stopped-VM state and asks the user what to do next instead of forcing an automatic host reboot.

## Supported hardware paths

- AMD GPUs
- NVIDIA GPUs
- GPU HDMI/DP audio companion functions
- Multi-GPU systems
- Single-GPU systems only when the user explicitly acknowledges the recovery risk

## Project layout

```text
.
├── CMakeLists.txt
├── README.md
├── LICENSE
├── docs/
│   ├── BUILD.md
│   ├── RELEASE_NOTES.md
│   └── RISKS.md
├── hooks/
│   └── qemu
├── systemd/
│   ├── gpu-switcher-boot.service
│   └── gpu-switcher-helperd.service
└── src/
    ├── common.h / common.cpp
    ├── gui_main.cpp
    ├── ctl_main.cpp
    └── helperd_main.cpp
```

## Build

See [`docs/BUILD.md`](docs/BUILD.md).

Basic build:

```bash
cmake -S . -B build
cmake --build build -j
```

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

## CLI commands

```bash
gpu-switcher-ctl status
gpu-switcher-ctl inventory
gpu-switcher-ctl diagnose [gpuBdf]
gpu-switcher-ctl simulateVm [gpuBdf]
gpu-switcher-ctl rebootToVm
gpu-switcher-ctl rebootToHost
gpu-switcher-ctl restartHostNow
gpu-switcher-ctl returnHostNextRestart
gpu-switcher-ctl keepGpuForVm
gpu-switcher-ctl on-vm-stopped <domain>
```

### Important commands

| Command | Purpose |
|---|---|
| `inventory` | Prints GPU, IOMMU, Secure Boot, SSH, hook, current mode, and next boot target information. |
| `diagnose` | Runs the preflight report used by the GUI. |
| `simulateVm` | Dry-runs the host→VM flow without touching hardware. |
| `rebootToVm` | Schedules VM mode and reboots. |
| `rebootToHost` | Schedules host mode and reboots. |
| `restartHostNow` | Post-VM-close action: schedule host recovery and reboot immediately. |
| `returnHostNextRestart` | Post-VM-close action: schedule host recovery for the next restart but do not reboot now. |
| `keepGpuForVm` | Post-VM-close action: leave the GPU assigned to VM/VFIO until changed manually. |
| `on-vm-stopped` | Called by the libvirt hook when the VM stops. |

## Runtime files

```text
/var/lib/gpu-switcher/config.ini
/var/lib/gpu-switcher/state.txt
/var/lib/gpu-switcher/backups/
/run/gpu-switcher.sock
/etc/libvirt/hooks/qemu
```

## Version

Current project version: **2.2.0**.

## Release status

Development snapshot. Full runtime validation still requires a Linux machine with Qt6, systemd, libvirt, VFIO/IOMMU support, and the target GPU hardware.
