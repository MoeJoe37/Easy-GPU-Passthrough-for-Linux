# Residual failure modes after the fixes

Even after the safety gates, inventory reporting, and reset logic are added, these problems can still happen:

## 1) Firmware or kernel did not actually expose usable IOMMU groups
**Symptom:** VM mode is blocked and the report says IOMMU is missing or unsafe.

**Fix:** Enable SVM/VT-d/AMD-Vi in firmware and boot with the correct kernel parameters for your platform. Reboot and re-run the compatibility check.

## 2) The GPU and a non-GPU device still share the same IOMMU group
**Symptom:** The app refuses passthrough because the group contains unrelated hardware.

**Fix:** Move the GPU to a different slot if your platform supports ACS separation, or use a board/firmware combination that isolates the device properly.

## 3) The GPU has no clean reset path
**Symptom:** Reset fails and the helper reports that reboot fallback is required.

**Fix:** Use the reboot recovery path, or enable AMD `vendor-reset` when the hardware supports it. For NVIDIA, rely on the kernel reset node when present and keep a host-recovery reboot path available.

## 4) Libvirt XML does not match the selected PCI devices
**Symptom:** The app blocks VM mode because the configured GPU/audio BDFs are not present in the domain XML.

**Fix:** Edit the domain XML or reconfigure the app so the hostdev entries match the exact GPU and audio function being passed through.

## 5) The hook exists but libvirt is not executing it
**Symptom:** VM shutdown does not trigger host recovery.

**Fix:** Confirm the hook is installed at `/etc/libvirt/hooks/qemu`, is executable, and that libvirt is actually calling hooks on your host.

## 6) Host driver rebinding fails
**Symptom:** The helper cannot return the device to the original Linux driver.

**Fix:** Use the reboot fallback to restore the host cleanly. This is the last recovery step by design.

## 7) Diagnostics still depend on the host environment
**Symptom:** The report may classify SSH, fallback display, or hook availability differently across machines.

**Fix:** Treat the diagnostics CLI as a preflight gate, not a guarantee. Re-run it after firmware, kernel, or libvirt changes.


## 8) Boot service does not run early enough
**Symptom:** The GPU is rebound too late and the display manager starts first.

**Fix:** Ensure `gpu-switcher-boot.service` is enabled and ordered before `display-manager.service` and `graphical.target`. If your distro starts a different display manager unit, add it to the unit ordering list.

## 9) VM stop event is recorded but the GUI is not visible
**Symptom:** The VM has stopped, but the host display is unavailable because the only GPU is still assigned to VFIO.

**Fix:** Use SSH or another fallback path to run one of these commands:

```bash
# Reboot immediately and restore GPU ownership to Linux
gpu-switcher-ctl restartHostNow

# Restore GPU ownership to Linux on the next restart
gpu-switcher-ctl returnHostNextRestart

# Keep GPU ownership with VFIO/VM until changed later
gpu-switcher-ctl keepGpuForVm
```

## 10) User chooses to keep the GPU assigned to the VM
**Symptom:** After choosing **Keep GPU with VM**, the next normal boot may still leave the GPU bound for VM/VFIO use until the user manually chooses host recovery.

**Fix:** Open the GUI and press **Reboot to Host**, or run:

```bash
gpu-switcher-ctl rebootToHost
```
