# GPU Switcher risk register

This file tracks the failure modes that matter for single-GPU and multi-GPU VFIO passthrough. Version 2.4.0 adds thermal and idle-recovery guardrails on top of the existing IOMMU, libvirt, driver, and reboot-recovery checks.

Important boundary: this app does not overclock, undervolt, change GPU power limits, flash firmware, or write fan curves. It only changes PCI driver ownership. It cannot guarantee that defective hardware, a blocked fan, bad airflow, a failing PSU, or a guest OS driver bug will be safe.

## 1) Firmware or kernel did not expose usable IOMMU groups

**Symptom:** VM mode is blocked and the report says IOMMU is missing or unsafe.

**Status in app:** Blocked by preflight. The app refuses VM mode when `/sys/kernel/iommu_groups` is missing.

**User fix:** Enable SVM/VT-d/AMD-Vi/IOMMU in firmware and boot with the correct kernel parameters for the platform. Reboot and run **Preflight** again.

## 2) The GPU and unrelated hardware share the same IOMMU group

**Symptom:** The app refuses passthrough because the group contains non-GPU hardware.

**Status in app:** Blocked by preflight. The app allows GPU display/audio functions in the same group, but blocks unrelated devices.

**User fix:** Move the GPU to another slot if the motherboard has better ACS separation, change firmware settings if available, or use hardware that isolates the device correctly. The app does not apply ACS override automatically because it can weaken isolation.

## 3) GPU is already too hot before handoff

**Symptom:** Preflight blocks VM mode with `gpu-temperature-too-high`.

**Status in app:** Blocked by thermal guard when Linux exposes a readable `hwmon/temp*_input` sensor for the selected GPU. Default limit is 85 °C and can be changed from the GUI.

**User fix:** Let the GPU cool down, verify the fans spin, clean dust filters, improve case airflow, then run **Preflight** again. Raise the limit only when you know the safe operating range for that exact card.

## 4) Linux cannot read the GPU temperature sensor

**Symptom:** Preflight warns that GPU temperature is unreadable.

**Status in app:** Warned, not blocked. Many passthrough setups lose Linux-side sensor visibility once the GPU belongs to VFIO or the guest. The app therefore keeps VM-stop auto recovery enabled by default.

**User fix:** Verify thermals inside the Windows guest with the vendor driver installed. Keep the default safety auto-recovery timer unless you have another monitoring and cooling plan.

## 5) VM closes and the GPU remains bound to VFIO

**Symptom:** The VM stopped, but the GPU is still assigned to VFIO/VM mode.

**Status in app:** Mitigated. The hook records a stopped-VM decision, schedules Host recovery for the next restart by default, and arms a transient systemd safety timer. If the user does nothing, the timer calls `gsc safetyRecoverHostNow` and reboots back to Host mode.

**User fix:** Choose **Restart now to Host** for the safest path. Choose **Return on next restart** only if you intentionally want to delay the reboot. Choose **Keep GPU with VM** only when you have verified guest/firmware cooling and understand that this cancels the safety timer.

## 6) The GPU has no clean reset path

**Symptom:** Reset fails, GPU rebinding fails, or the host remains headless after VM shutdown.

**Status in app:** Warned by preflight. The app keeps reboot-based recovery as the primary boundary, tries `/reset` when available, supports AMD `vendor-reset` when requested, and keeps remove/rescan fallback for recovery.

**User fix:** Prefer the reboot recovery path. For some AMD cards, install/load `vendor-reset` if the hardware needs it. For NVIDIA, keep the reboot fallback available.

## 7) Libvirt XML does not match the selected PCI devices

**Symptom:** VM mode is blocked because the configured GPU/audio BDFs are not present in the inactive VM XML.

**Status in app:** Fixed by **Auto setup single GPU** when possible. The helper backs up the inactive domain XML and adds missing `<hostdev>` entries for the GPU and companion audio function.

**User fix:** If auto-patching fails, manually edit the VM XML or add the PCI host devices in virt-manager, then run **Preflight** again.

## 8) The hook exists but libvirt is not executing it

**Symptom:** VM shutdown does not create the stopped-VM decision state.

**Status in app:** Partially fixed. **Auto setup single GPU** installs `/etc/libvirt/hooks/qemu`, makes it executable, and the hook handles both `stopped:end` and `release:end` events.

**User fix:** Confirm the hook is installed at `/etc/libvirt/hooks/qemu`, executable, and that the libvirt daemon used by the VM supports QEMU hooks. Restart libvirt after installing the hook when needed.

## 9) Host driver rebinding fails

**Symptom:** The helper cannot return the device to the original Linux driver.

**Status in app:** Mitigated. The helper records the original GPU/audio drivers, clears `driver_override`, reloads common driver modules, tries direct bind, then remove/rescan fallback.

**User fix:** Use **Restart now to Host** or `gsc rebootToHost`. If the card is still wedged, power-cycle the PC.

## 10) Boot service does not run early enough

**Symptom:** The display manager starts first and takes the GPU before VFIO binding.

**Status in app:** Mitigated. `gpu-switcher-boot.service` is ordered before `display-manager.service` and `graphical.target`. In single-GPU VM mode, the helper runtime-masks `display-manager.service` for that boot, unbinds VT consoles/framebuffers, and then binds the GPU to `vfio-pci`.

**User fix:** Enable the boot service:

```bash
sudo systemctl enable gpu-switcher-boot.service
```

If the distro uses a nonstandard display manager unit, add equivalent ordering or adapt the unit.

## 11) VM stop event is recorded but the Linux GUI is not visible

**Symptom:** The VM has stopped, but the host display is unavailable because the only GPU is still assigned to VFIO.

**Status in app:** Expected for true single-GPU mode. The app records the decision state, schedules fail-safe Host recovery, exposes CLI commands for recovery, and can auto-reboot after the configured grace period.

**User fix:** Use SSH, another remote path, or a physical reboot button to run one of these commands:

```bash
# Reboot immediately and restore GPU ownership to Linux
gsc restartHostNow

# Restore GPU ownership to Linux on the next restart
gsc returnHostNextRestart

# Keep GPU ownership with VFIO/VM until changed later; cancels safety timer
gsc keepGpuForVm
```

## 12) User disables thermal guard or auto recovery

**Symptom:** Preflight warns `thermal-guard-disabled` or `auto-recovery-disabled`.

**Status in app:** Warned. The app allows advanced users to disable these features, but does not hide the risk.

**User fix:** Leave thermal guard enabled and keep VM-stop auto recovery above zero unless there is a deliberate external monitoring/recovery plan.

## 13) Auto XML patch is too simple for unusual domain layouts

**Symptom:** Auto setup backs up the XML but `virsh define` fails or the VM still does not boot.

**Status in app:** Partially mitigated. The app inserts standard managed PCI `<hostdev>` entries before `</devices>`, then validates the XML using `virsh dumpxml`.

**User fix:** Use virt-manager or `virsh edit` for complex setups, especially when custom ROM files, multifunction attributes, custom PCI addresses, Looking Glass, evdev input, or nonstandard video devices are needed.

## 14) Secure Boot / signed module issues

**Symptom:** Host NVIDIA/AMD/VFIO/vendor-reset modules do not load or reload cleanly.

**Status in app:** Warned by diagnostics when Secure Boot appears enabled.

**User fix:** Disable Secure Boot or enroll/sign the required modules according to the distro’s module signing process.

## 15) Keyboard/mouse access inside the VM is not configured

**Symptom:** The GPU output appears but the user cannot control Windows.

**Status in app:** Not automated yet. GPU Switcher controls GPU ownership, not USB controller/input-device passthrough.

**User fix:** Configure USB controller passthrough, USB device redirection, evdev input, Looking Glass/SPICE fallback, or a separate keyboard/mouse path before relying on single-GPU mode.
