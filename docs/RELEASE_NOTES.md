# Release Notes

## v2.2.0

- Added a post-VM-shutdown decision workflow.
- The libvirt hook now records `vm-stopped-awaiting-decision` instead of automatically scheduling host recovery.
- Added GUI actions:
  - **Restart now to Host**
  - **Return on next restart**
  - **Keep GPU with VM**
- Added CLI actions:
  - `restartHostNow`
  - `returnHostNextRestart`
  - `keepGpuForVm`
- Added persistent config fields for stopped-VM state:
  - `vmStoppedAwaitingDecision`
  - `lastStoppedVm`
  - `lastStoppedAt`
- Added GUI auto-refresh so stopped-VM state appears without manually pressing refresh.
- Increased GUI helper response timeout for slower `virsh`/diagnostic calls.
- Made the helper socket use Qt world-access local socket options so a normal desktop GUI can talk to the root helper.
- Fixed CLI simulation command routing.
- Bumped CMake project version to `2.2.0`.

## v2.1.0

- Added reboot-handoff workflow for host→VM and VM→host switching.
- Added `nextBootMode` persistence to the shared config model.
- Added boot-time application service for early VFIO binding and host recovery.
- Renamed the main UI actions to reflect reboot-based transitions.
- Improved inventory and preflight reporting with current mode and next boot target.

## v2.0 development snapshot

- Added system inventory reporting for graphics, Secure Boot, IOMMU, SSH, and libvirt hook checks.
- Added richer preflight output shared by the GUI and CLI.
- Extended vendor classification and reporting with NVIDIA hidden-state, audio-companion, and Secure Boot signals.
- Introduced an inventory-first workflow for release v2 planning.
- Added a dry-run simulation command for the host-to-VM transition.

## v1.0.0

- State-machine based host/VM transition flow.
- Vendor-aware GPU detection for AMD and NVIDIA.
- Pre-flight diagnostics CLI.
- Libvirt hook installation and verification.
- Safer recovery paths with reboot fallback when rebind/reset fails.

## Known limitations

- Hardware reset behavior is still GPU- and firmware-dependent.
- Single-GPU systems remain risky without an alternate recovery path.
- Full build/runtime verification must be performed on a machine with Qt6, libvirt, and VFIO-capable hardware.
