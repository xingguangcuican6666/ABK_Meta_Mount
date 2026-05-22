#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

KERNEL_ROOT="$TMP_DIR/kernel"
DEFCONFIG="$TMP_DIR/gki_defconfig"

mkdir -p "$KERNEL_ROOT/common/drivers" "$KERNEL_ROOT/common/include/linux" "$KERNEL_ROOT/common/fs"
printf 'VERSION = 6\nPATCHLEVEL = 6\nSUBLEVEL = 0\n' > "$KERNEL_ROOT/common/Makefile"
printf 'menu "Device Drivers"\nendmenu\n' > "$KERNEL_ROOT/common/drivers/Kconfig"
printf '# drivers makefile\n' > "$KERNEL_ROOT/common/drivers/Makefile"
cat > "$KERNEL_ROOT/common/fs/namespace.c" <<'EOF_NAMESPACE'
static int path_mount(const char *dev_name, struct path *path,
                      const char *type_page, unsigned long flags,
                      void *data_page)
{
        return 0;
}

static int path_umount(struct path *path, int flags)
{
        return 0;
}
EOF_NAMESPACE
printf '# defconfig\n' > "$DEFCONFIG"

run_stage() {
  local stage="$1"

  (
    cd "$REPO_ROOT"
    KERNEL_ROOT="$KERNEL_ROOT" \
      DEFCONFIG="$DEFCONFIG" \
      CUSTOM_EXTERNAL_MODULE_STAGE="$stage" \
      CONFIG="android15-6.6-0" \
      bash setup.sh
  )
}

assert_file() {
  local file="$1"

  [ -f "$file" ] || {
    printf 'expected file missing: %s\n' "$file" >&2
    exit 1
  }
}

assert_count() {
  local expected="$1"
  local pattern="$2"
  local file="$3"
  local actual

  actual="$(grep -Fxc "$pattern" "$file" || true)"
  if [ "$actual" != "$expected" ]; then
    printf 'expected %s occurrence(s) of %s in %s, got %s\n' \
      "$expected" "$pattern" "$file" "$actual" >&2
    exit 1
  fi
}

assert_contains() {
  local pattern="$1"
  local file="$2"

  grep -Fq "$pattern" "$file" || {
    printf 'expected %s in %s\n' "$pattern" "$file" >&2
    exit 1
  }
}

run_stage after_patch
run_stage after_patch
run_stage before_build
run_stage before_build

assert_file "$KERNEL_ROOT/common/drivers/abk_meta_mount/abk_meta_mount.c"
assert_file "$KERNEL_ROOT/common/drivers/abk_meta_mount/Kconfig"
assert_file "$KERNEL_ROOT/common/drivers/abk_meta_mount/Makefile"
assert_file "$KERNEL_ROOT/common/include/linux/abk_meta_mount.h"

assert_count 1 'source "drivers/abk_meta_mount/Kconfig"' \
  "$KERNEL_ROOT/common/drivers/Kconfig"
assert_count 1 'obj-$(CONFIG_ABK_META_MOUNT) += abk_meta_mount/' \
  "$KERNEL_ROOT/common/drivers/Makefile"
assert_count 1 'int path_mount(const char *dev_name, struct path *path,' \
  "$KERNEL_ROOT/common/fs/namespace.c"
assert_count 1 'int path_umount(struct path *path, int flags)' \
  "$KERNEL_ROOT/common/fs/namespace.c"
assert_count 1 '/* ABK Meta Mount: expose in-kernel mount helpers to the built-in provider. */' \
  "$KERNEL_ROOT/common/fs/namespace.c"

for symbol in \
  CONFIG_ABK_META_MOUNT \
  CONFIG_OVERLAY_FS \
  CONFIG_TMPFS \
  CONFIG_TMPFS_XATTR \
  CONFIG_TMPFS_POSIX_ACL \
  CONFIG_PROC_FS
do
  assert_count 1 "$symbol=y" "$DEFCONFIG"
done

driver="$KERNEL_ROOT/common/drivers/abk_meta_mount/abk_meta_mount.c"
assert_contains '#if IS_ENABLED(CONFIG_ABK_CONTROL)' "$driver"
assert_contains 'abk_control_register(&abk_meta_mount_control_ops)' "$driver"
assert_contains 'ABK_CONTROL_OPS_HAS_RUNTIME_UI' "$driver"
assert_contains '.web_root = ABK_META_MOUNT_WEB_ROOT' "$driver"
assert_contains '.has_web_ui = true' "$driver"
assert_contains 'metamodule=1' "$driver"
assert_contains 'mount=false' "$driver"
assert_contains 'skip_mount=true' "$driver"
assert_contains '/sys/kernel/abk_meta_mount/prepare' "$driver"
assert_contains 'abk_meta_mount_schedule_retry()' "$driver"
assert_contains 'abk_meta_mount_marker_points_to_module()' "$driver"
assert_contains '/mnt/abk_meta_mount' "$driver"
assert_contains 'cp -a \"$SRC\"/. \"$DST\"/' "$driver"
assert_contains '__get_free_page(GFP_KERNEL)' "$driver"
assert_contains 'free_page((unsigned long)data)' "$driver"
assert_contains 'path_mount("KSU", &path, "overlay", 0, data)' "$driver"
assert_contains 'abk_meta_mount_collect_lowerdir(target->path' "$driver"
assert_contains 'status=%s lowerdir=%s' "$driver"
assert_contains 'TAKEOVER=0' "$driver"
assert_contains '[ -z \"$CUR\" ] || [ ! -d \"$CUR\" ] || [ -f \"$CUR/disable\" ] || [ -f \"$CUR/remove\" ]' "$driver"
assert_contains 'touch \"$MOD/.marker_owned\"' "$driver"
if grep -Fq 'mount -t overlay' "$driver"; then
  printf 'driver still contains shell overlay mount\n' >&2
  exit 1
fi

module_conf="$REPO_ROOT/module.conf"
assert_contains 'ABK_MODULE_SUPPORTED_STAGES="before_build,after_patch"' "$module_conf"
assert_contains 'ABK_MODULE_DEFAULT_STAGE="before_build"' "$module_conf"
assert_contains 'ABK_MODULE_RECOMMENDED_STAGES="before_build"' "$module_conf"

printf 'abk_meta_mount setup test passed\n'
