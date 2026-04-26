# GPU Switcher

GPU Switcher is a C++20 / Qt6 Linux GUI, CLI, and root helper daemon for controlled GPU ownership switching between a Linux host and a Windows virtual machine that uses VFIO PCI passthrough.

The project now focuses on the common **single-GPU passthrough** workflow: the Linux host can reboot into a VM mode, bind the only GPU to `vfio-pci`, auto-start the configured libvirt VM, then record the VM stop event, schedule a fail-safe host recovery path, and let the user decide what happens next.

## What this app automates

The manual single-GPU VFIO setup usually requires users to:

1. identify the GPU and HDMI/DP audio PCI functions;
2. verify IOMMU and IOMMU group isolation;
3. add matching PCI hostdev entries to the libvirt VM XML;
4. install a QEMU lifecycle hook;
5. stop or avoid the Linux display manager when the GPU is handed to the VM;
6. unbind framebuffer/console users of the GPU;
7. bind the GPU/audio functions to `vfio-pci`;
8. start the Windows VM;
9. check the Linux-visible GPU temperature before handoff when a sensor is available;
10. arm a post-VM safety recovery timer after the VM stops;
11. decide how to recover the GPU after the VM stops.

GPU Switcher turns those steps into GUI and CLI actions.

## Main workflow

### 1. One-time setup

Open the GUI and fill in at least the **VM name** or **VM UUID**.

Then press **Auto setup single GPU**.

The helper will:

1. detect the only graphics controller automatically, or use the GPU PCI BDF you entered;
2. detect the HDMI/DP audio companion function from the same IOMMU group when possible;
3. save the config in `/var/lib/gpu-switcher/config.ini`;
4. enable the explicit single-GPU acknowledgement;
5. enable VM auto-start after reboot into VM mode;
6. install `/etc/libvirt/hooks/qemu`;
7. back up the inactive libvirt XML to `/var/lib/gpu-switcher/backups/`;
8. add missing GPU/audio PCI hostdev entries to the VM XML;
9. enable the default GPU temperature guard at 85 °C;
10. enable the default VM-stop safety recovery timer at 10 minutes;
11. re-run the preflight report.

CLI equivalent:

```bash
gpu-switcher-ctl autoSetupSingleGpu
# or, when more than one GPU exists:
gpu-switcher-ctl autoSetupSingleGpu 0000:01:00.0
```

### 2. Linux host → Windows VM

From the GUI, press **Reboot to VM**.

The helper will:

1. validate IOMMU, IOMMU group safety, VM XML, hook installation, fallback display / SSH acknowledgement, GPU presence, and the temperature guard;
2. store the original Linux GPU/audio drivers;
3. schedule `nextBootMode=vm` in `/var/lib/gpu-switcher/config.ini`;
4. reboot the PC;
5. during boot, runtime-mask `display-manager.service` on single-GPU systems so the Linux desktop does not race the VM for the only GPU;
6. unbind VT consoles and platform framebuffers when using the single-GPU path;
7. bind the configured GPU/audio PCI functions to `vfio-pci`;
8. mark the current mode as `vm`;
9. auto-start the configured libvirt VM when `autoStartVmOnBoot=true`.

### 3. Windows VM closes

The libvirt QEMU hook calls:

```bash
gpu-switcher-ctl on-vm-stopped <domain>
```

The helper does not try an unsafe live handoff back to Linux. It records the stopped-VM decision, schedules Host recovery for the next restart, and arms the safety timer:

```text
state = vm-stopped-awaiting-decision
vmStoppedAwaitingDecision = true
nextBootMode = host
safetyRecoveryDeadline = <UTC deadline when auto recovery is enabled>
```

The GUI/CLI then gives three choices:

| GUI option | CLI command | What it does |
|---|---|---|
| **Restart now to Host** | `gpu-switcher-ctl restartHostNow` | Schedules `nextBootMode=host` and immediately reboots. On the next boot, the GPU is rebound to the original Linux driver. |
| **Return on next restart** | `gpu-switcher-ctl returnHostNextRestart` | Keeps `nextBootMode=host` but does **not** reboot now. The GPU returns to Linux whenever the user restarts later. |
| **Keep GPU with VM** | `gpu-switcher-ctl keepGpuForVm` | Clears the stopped-VM warning, cancels the safety timer, and keeps the GPU assigned to VM/VFIO mode until the user manually chooses **Reboot to Host**. |

If no user action is taken and `safetyAutoRecoveryMinutes` is greater than zero, the transient systemd timer calls `gpu-switcher-ctl safetyRecoverHostNow` and reboots back to Host mode. On a real single-GPU system, the local Linux GUI may not be visible after the VM closes because the GPU is still assigned to VFIO. Keep SSH, another remote path, or a known reboot shortcut available.

### 4. Windows VM / VFIO → Linux host

From the GUI, press **Reboot to Host**, or use one of the post-VM-close recovery options.

The helper will:

1. schedule `nextBootMode=host`;
2. reboot when requested;
3. during boot, clear `driver_override`;
4. unbind the GPU/audio devices from `vfio-pci`;
5. reload the recorded host driver modules when possible;
6. rebind the devices to the recorded original Linux drivers;
7. re-enable VT consoles and unmask the display manager;
8. mark the current mode as `host`.

## Safety model

GPU passthrough can leave a machine without local display output. GPU Switcher is conservative by design:

- refuses VM mode when IOMMU groups are missing;
- refuses VM mode when the GPU shares an unsafe IOMMU group with unrelated hardware;
- refuses single-GPU passthrough unless fallback display / SSH recovery is acknowledged or explicit single-GPU mode is enabled;
- validates the inactive libvirt domain XML before scheduling VM mode;
- backs up the inactive VM XML before auto-patching or VM-mode handoff;
- tracks original Linux drivers before binding to `vfio-pci`;
- uses reboot-based recovery as the primary ownership boundary;
- runtime-masks the display manager only for the VM boot when using the single-GPU path;
- reads Linux `hwmon/temp*_input` GPU sensors when available and blocks VM mode when the configured temperature limit is reached;
- records stopped-VM state, schedules Host recovery for the next restart, and arms a default safety auto-recovery timer;
- requires a stronger confirmation before cancelling the safety timer and keeping the GPU assigned to VM/VFIO.

## Supported hardware paths

### Hardware safety boundary

The app does **not** overclock, undervolt, change GPU power limits, flash firmware, or write fan curves. It only changes PCI driver ownership between the Linux host, `vfio-pci`, and the VM. It reduces risk by blocking unsafe preflight states and by returning to Host mode after VM shutdown when the user does not choose another action, but it cannot guarantee safety for defective hardware, blocked fans, bad case airflow, an undersized PSU, or a guest OS driver that fails to cool the card.

- AMD GPUs
- NVIDIA GPUs
- Intel GPUs where VFIO passthrough is supported by the platform
- GPU HDMI/DP audio companion functions
- Multi-GPU systems
- Single-GPU systems with explicit recovery acknowledgement

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
gpu-switcher-ctl autoSetupSingleGpu [gpuBdf]
gpu-switcher-ctl simulateVm [gpuBdf]
gpu-switcher-ctl rebootToVm
gpu-switcher-ctl rebootToHost
gpu-switcher-ctl restartHostNow
gpu-switcher-ctl returnHostNextRestart
gpu-switcher-ctl keepGpuForVm
gpu-switcher-ctl safetyRecoverHostNow
gpu-switcher-ctl on-vm-stopped <domain>
```

### Important commands

| Command | Purpose |
|---|---|
| `inventory` | Prints GPU, IOMMU, Secure Boot, SSH, hook, current mode, and next boot target information. |
| `diagnose` | Runs the preflight report used by the GUI. |
| `autoSetupSingleGpu` | Detects GPU/audio, saves config, installs hook, backs up XML, and adds missing hostdev entries. |
| `simulateVm` | Dry-runs the host→VM flow without touching hardware. |
| `rebootToVm` | Schedules VM mode and reboots. |
| `rebootToHost` | Schedules host mode and reboots. |
| `restartHostNow` | Post-VM-close action: schedule host recovery and reboot immediately. |
| `returnHostNextRestart` | Post-VM-close action: schedule host recovery for the next restart but do not reboot now. |
| `keepGpuForVm` | Post-VM-close action: cancel the safety timer and leave the GPU assigned to VM/VFIO until changed manually. |
| `safetyRecoverHostNow` | Internal safety-timer action: if a stopped-VM decision is still pending, schedule Host recovery and reboot. |
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

Current project version: **2.4.0**.

## Release status

Development snapshot. Full runtime validation still requires a Linux machine with Qt6, systemd, libvirt, VFIO/IOMMU support, and the target GPU hardware.
