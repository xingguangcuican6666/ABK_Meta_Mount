# ABK Meta Mount

ABK Meta Mount is an ABK custom external module that injects a built-in
KernelSU-compatible metamodule provider into `common/drivers`.

The kernel driver creates a runtime compatibility module at
`/data/adb/modules/meta-abk-mount`, writes `module.prop` with `metamodule=1`,
and owns `/data/adb/metamodule` when no other valid metamodule is already active.
This makes KernelSU and ABK's runtime module list see a metamodule, while the
actual provider is built into the kernel.

## Features

- Copies `drivers/abk_meta_mount` and `include/linux/abk_meta_mount.h` into the
  ABK kernel tree.
- Adds `CONFIG_ABK_META_MOUNT` to `common/drivers/Kconfig` and `Makefile`.
- Enables OverlayFS, tmpfs, tmpfs xattrs, tmpfs POSIX ACLs, and procfs during
  the `before_build` stage.
- Creates a KernelSU-compatible module directory containing:
  - `module.prop` with `metamodule=1`, `mount=false`, `web=1`, and `action=1`
  - `metamount.sh`
  - `action.sh`
  - `webroot/index.html`
- Provides runtime controls:
  - `/sys/kernel/abk_meta_mount/enabled`
  - `/sys/kernel/abk_meta_mount/prepare`
  - `/proc/abk_meta_mount/status`
- Mounts enabled ordinary module layers over `/system`, `/vendor`, `/product`,
  `/system_ext`, `/odm`, and `/oem` with OverlayFS.
- Optionally registers with ABK Control through `abk_control_register()` only
  when `CONFIG_ABK_CONTROL` is enabled.

## ABK Usage

Use `before_build` at minimum. `after_patch` is optional if you want the source tree copied earlier:

```text
https://github.com/your-name/ABK_Meta_Mount.git;before_build
```

`before_build` installs the source files, Kconfig hooks, and required defconfig
symbols. `after_patch` is optional and only repeats the source copy earlier in
the build flow.

## Runtime Behavior

On boot, the driver waits until `/data/adb` and `/system/bin/sh` are available.
It then writes the compatibility module files and creates
`/data/adb/metamodule -> /data/adb/modules/meta-abk-mount` if that marker is
absent, already points to this module, points to a missing module, or points to
a module with `disable` or `remove`.

If `/data/adb/metamodule` already points to another enabled metamodule, ABK
Meta Mount still generates its compatibility module directory, but does not take
over the marker and does not mount overlays.

Disabling through WebUI, ABK Control, or
`echo 0 > /sys/kernel/abk_meta_mount/enabled` writes
`/data/adb/modules/meta-abk-mount/disable` and attempts to unmount active
overlays. A reboot may still be needed to fully unwind already-mounted
partitions.

## ABK Control

This repository does not include ABK Control. If the separate ABK Control
module is also built and provides `CONFIG_ABK_CONTROL`, this driver registers:

- id: `meta-abk-mount`
- name: `ABK Meta Mount`
- version: `0.1.0`
- description: `Built-in KernelSU-compatible metamodule provider`
- module dir: `/data/adb/modules/meta-abk-mount`
- WebUI/action support
- enable/disable callbacks

When ABK Control is not built, the registration code is compiled out.

## Verification

Run the local fixture test:

```bash
bash -n setup.sh scripts/libabk.sh scripts/abk_meta_mount_setup.sh tests/abk_meta_mount_setup_test.sh
bash tests/abk_meta_mount_setup_test.sh
```

The fixture checks that setup is idempotent, required files are copied, config
symbols are written once, and ABK Control registration remains conditional.

## References

- KernelSU metamodule guide: <https://kernelsu.org/guide/metamodule.html>
- KernelSU module WebUI guide: <https://kernelsu.org/guide/module-webui.html>
