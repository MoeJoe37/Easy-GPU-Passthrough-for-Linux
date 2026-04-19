# GPU Switcher

A C++/Qt helper app for switching one physical GPU between Linux host mode and a Windows VM using VFIO passthrough.

## Safety model
- Refuses VM mode when IOMMU is not available.
- Refuses VM mode when the GPU shares an unsafe IOMMU group with unrelated hardware.
- Refuses single-GPU passthrough unless the user confirms a fallback display or SSH recovery path.
- Validates the libvirt domain XML before switching and backs it up before changing state.
- Uses the kernel `driver_override`, `reset`, `remove`, and `rescan` paths when available.
- Loads `vendor-reset` only for AMD hardware when enabled in config.
- Uses a reboot handoff model for the main host↔VM transition so GPU ownership is applied early in the next boot.
- Keeps reboot as the final recovery path when reset or rebind still fails.

## What it supports
- AMD GPUs
- NVIDIA GPUs
- GPU audio functions as a paired PCI function when present

## Files
- `src/common.h` / `src/common.cpp`
- `src/helperd_main.cpp`
- `src/gui_main.cpp`
- `src/ctl_main.cpp`
- `hooks/qemu`
- `systemd/gpu-switcher-helperd.service`
- `systemd/gpu-switcher-boot.service`
- `docs/RISKS.md`
- `docs/BUILD.md`

## Build
See `docs/BUILD.md`.

## Diagnostics and control
- `gpu-switcher-ctl inventory` prints a system hardware inventory with GPU, IOMMU, Secure Boot, SSH, and libvirt hook status.
- `gpu-switcher-ctl diagnose [gpuBdf]` prints a structured preflight report.
- `gpu-switcher-ctl simulateVm [gpuBdf]` performs a dry-run of the host-to-VM transition and shows the planned phases without touching hardware.
- `gpu-switcher-ctl rebootToVm` schedules the next boot to bind the GPU to VFIO and auto-start the VM if configured.
- `gpu-switcher-ctl rebootToHost` schedules the next boot to rebind the GPU back to the Linux host driver.
- `gpu-switcher-helperd --apply-boot-state` is used by the boot service to apply the pending target early in boot.
- The helper now runs a state-machine flow for VM entry and host recovery.
- The GUI includes inventory, diagnostics, simulation, and reboot-handoff actions that reuse the same shared report logic.

## v2.1 direction
- Expand the reboot-based handoff so the GPU is applied before the display manager starts.
- Keep NVIDIA/AMD vendor handling centralized so the GUI and CLI stay consistent.
- Grow the recovery layer around explicit inventory, reset, and host recovery reporting.
- Enable the boot service (`systemd/gpu-switcher-boot.service`) so pending transitions are applied automatically on restart.

## Release status
- v2.1 development snapshot with reboot-based handoff support
- Known remaining limitations are documented in docs/RISKS.md
