# Development Notes

This repository is an ABK custom external module, so `setup.sh` is the only
entry point ABK calls.

## Module Layout

- `files/drivers/abk_meta_mount/`: kernel source copied into the ABK kernel
  tree.
- `files/include/linux/abk_meta_mount.h`: public in-kernel header for the
  built-in provider.
- `scripts/abk_meta_mount_setup.sh`: shared shell helpers used by `setup.sh`.
- `tests/abk_meta_mount_setup_test.sh`: local fixture test.

## Stages

`before_build` is the required stage for a working build. `after_patch` is
optional and only mirrors the source copy earlier in the flow.

### `after_patch`

- Copy the driver tree into `common/drivers/abk_meta_mount`.
- Copy the public header into `common/include/linux/abk_meta_mount.h`.
- Append the Kconfig and Makefile hooks once.
- Patch `common/fs/namespace.c` once so `path_mount()` and `path_umount()` are
  callable by the built-in ABK Meta Mount driver.

### `before_build`

- Repeat the file copy path idempotently.
- Force the required config symbols:
  - `CONFIG_ABK_META_MOUNT`
  - `CONFIG_OVERLAY_FS`
  - `CONFIG_TMPFS`
  - `CONFIG_TMPFS_XATTR`
  - `CONFIG_TMPFS_POSIX_ACL`
  - `CONFIG_PROC_FS`

## Kernel Driver Notes

- The driver is built into the kernel, not installed as a normal module.
- It creates a compatibility module under `/data/adb/modules/meta-abk-mount`
  so KernelSU and ABK can discover a metamodule entry.
- It also creates a helper ordinary module under
  `/data/adb/modules/abk-meta-mount-runtime` so KSU/ABK userland scripts can
  stage ordinary module trees into `/mnt/abk_meta_mount/modules`.
- It takes over `/data/adb/metamodule` only when the existing target is absent,
  missing, disabled, removed, or already points to `meta-abk-mount`.
- It scans ordinary module directories and executes OverlayFS mounts in the
  kernel with `path_mount()`, but consumes staged lowerdirs from
  `/mnt/abk_meta_mount/modules`. Shell is kept only for runtime module file
  generation.
- It exposes runtime control through sysfs and procfs.
- It optionally registers with ABK Control only when `CONFIG_ABK_CONTROL` is
  enabled in the kernel tree.

## Testing

Always run:

```bash
bash -n setup.sh scripts/libabk.sh scripts/abk_meta_mount_setup.sh tests/abk_meta_mount_setup_test.sh
bash tests/abk_meta_mount_setup_test.sh
```

The fixture validates idempotence and the generated kernel-tree layout without
building a full kernel.
