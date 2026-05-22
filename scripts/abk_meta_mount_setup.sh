#!/usr/bin/env bash

abk_meta_mount_common_dir() {
  abk_common_dir
}

abk_meta_mount_copy_tree() {
  local source_dir="$1"
  local target_dir="$2"

  abk_require_dir "$source_dir"
  mkdir -p "$target_dir"
  cp -a "$source_dir"/. "$target_dir"/
  abk_log "synced $source_dir -> $target_dir"
}

abk_meta_mount_copy_file() {
  local source_file="$1"
  local target_file="$2"

  abk_require_file "$source_file"
  mkdir -p "$(dirname "$target_file")"
  cp -a "$source_file" "$target_file"
  abk_log "copied $source_file -> $target_file"
}

abk_meta_mount_install_kernel_files() {
  local common_dir

  common_dir="$(abk_meta_mount_common_dir)"
  abk_require_dir "$common_dir/drivers"
  abk_require_dir "$common_dir/include/linux"
  abk_require_file "$common_dir/drivers/Kconfig"
  abk_require_file "$common_dir/drivers/Makefile"

  abk_meta_mount_copy_tree \
    "$MODULE_DIR/files/drivers/abk_meta_mount" \
    "$common_dir/drivers/abk_meta_mount"
  abk_meta_mount_copy_file \
    "$MODULE_DIR/files/include/linux/abk_meta_mount.h" \
    "$common_dir/include/linux/abk_meta_mount.h"

  abk_append_line_once "$common_dir/drivers/Kconfig" 'source "drivers/abk_meta_mount/Kconfig"'
  abk_append_line_once "$common_dir/drivers/Makefile" 'obj-$(CONFIG_ABK_META_MOUNT) += abk_meta_mount/'

  abk_meta_mount_patch_namespace "$common_dir/fs/namespace.c"
}

abk_meta_mount_patch_namespace() {
  local namespace_file="$1"

  abk_require_file "$namespace_file"

  if grep -Fq 'ABK Meta Mount: expose in-kernel mount helpers' "$namespace_file"; then
    return 0
  fi

  if grep -Fq 'static int path_mount(' "$namespace_file"; then
    perl -0pi -e 's/static int path_mount\(/int path_mount(/' "$namespace_file"
  fi
  if grep -Fq 'static int path_umount(' "$namespace_file"; then
    perl -0pi -e 's/static int path_umount\(/int path_umount(/' "$namespace_file"
  fi

  if ! grep -Fq 'int path_mount(' "$namespace_file"; then
    abk_die "path_mount() not found in $namespace_file"
  fi
  if ! grep -Fq 'int path_umount(' "$namespace_file"; then
    abk_die "path_umount() not found in $namespace_file"
  fi

  cat >> "$namespace_file" <<'ABK_META_MOUNT_NAMESPACE_MARKER'

/* ABK Meta Mount: expose in-kernel mount helpers to the built-in provider. */
ABK_META_MOUNT_NAMESPACE_MARKER
  abk_log "patched $namespace_file for ABK Meta Mount mount helpers"
}

abk_meta_mount_enable_config() {
  abk_enable_config CONFIG_ABK_META_MOUNT
  abk_enable_config CONFIG_OVERLAY_FS
  abk_enable_config CONFIG_TMPFS
  abk_enable_config CONFIG_TMPFS_XATTR
  abk_enable_config CONFIG_TMPFS_POSIX_ACL
  abk_enable_config CONFIG_PROC_FS
}
