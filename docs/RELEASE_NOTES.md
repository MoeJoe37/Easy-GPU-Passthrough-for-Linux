# Release notes

## 2.4.0

### Safety hardening

- Added a Linux-side GPU temperature guard using the selected PCI device's `hwmon/temp*_input` sensors when available.
- Added GUI settings for:
  - **Enable GPU temperature guard**;
  - **Max GPU temperature**;
  - **VM-stop safety recovery** grace period.
- VM mode is now blocked when the thermal guard can read the GPU temperature and the temperature is at or above the configured limit.
- Preflight warns when Linux cannot read a GPU temperature sensor, because VFIO/guest ownership can hide host-side thermal telemetry.
- Preflight warns when the thermal guard or VM-stop safety timer is disabled.
- When the VM stops, the app now schedules Host recovery for the next restart by default.
- When the VM stops, the app arms a transient systemd timer that calls `gpu-switcher-ctl safetyRecoverHostNow` after the configured grace period.
- **Keep GPU with VM** now explicitly cancels the safety recovery timer and warns that this should only be used when guest/firmware cooling is confirmed.
- Added CLI command `gpu-switcher-ctl safetyRecoverHostNow` for the transient safety timer.
- Updated `docs/RISKS.md` with thermal/idle-VFIO risks and mitigations.

### Notes

- The app still does not overclock, undervolt, write fan curves, change GPU power limits, or flash firmware.
- Runtime validation still requires the target Linux host, GPU, libvirt VM, systemd, VFIO/IOMMU, and guest driver stack.

## 2.3.0

### Added

- Added **Auto setup single GPU** GUI action.
- Added `gpu-switcher-ctl autoSetupSingleGpu [gpuBdf]`.
- Auto-detects the only graphics controller when no GPU BDF is provided.
- Auto-detects the HDMI/DP audio companion function from the same IOMMU group when possible.
- Automatically enables the explicit single-GPU acknowledgement during auto setup.
- Installs the libvirt QEMU hook during auto setup.
- Backs up inactive libvirt XML before patching.
- Adds missing GPU/audio PCI `<hostdev>` entries to inactive libvirt XML.
- Accepts existing libvirt PCI hostdevs regardless of `managed='yes'` or `managed='no'`.
- Runtime-masks `display-manager.service` for VM-mode boots on true single-GPU systems.
- Unbinds VT consoles and common platform framebuffers before VFIO binding on single-GPU transitions.
- Reloads common host GPU modules before attempting host-driver recovery.
- Hook now handles both `stopped:end` and `release:end` libvirt lifecycle events.
- VM stopped hook now accepts either configured VM name or UUID.

### Changed

- Improved systemd ordering so boot handoff runs before the display manager and graphical target.
- Updated README to explain the automated single-GPU flow.
- Rewrote the risk register to show what the app now mitigates and what still requires user/hardware action.

### Still requires real-machine validation

- Actual GPU reset/rebind behavior varies by GPU, kernel, driver, firmware, and motherboard.
- Qt6/libvirt/VFIO runtime testing must be done on the target Linux host.

## 2.2.0

- Added post-VM-close decision flow:
  - restart now to host;
  - return on next restart;
  - keep GPU with VM.
- Changed the hook to record a pending decision instead of forcing automatic host recovery.
- Added CLI aliases for the post-VM-close actions.

## 2.1.0 and earlier

- Initial GUI, helper daemon, CLI, preflight report, inventory, boot-state switching, and libvirt hook support.
