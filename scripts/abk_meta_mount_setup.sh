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
}

abk_meta_mount_enable_config() {
  abk_enable_config CONFIG_ABK_META_MOUNT
  abk_enable_config CONFIG_OVERLAY_FS
  abk_enable_config CONFIG_TMPFS
  abk_enable_config CONFIG_TMPFS_XATTR
  abk_enable_config CONFIG_TMPFS_POSIX_ACL
  abk_enable_config CONFIG_PROC_FS
}
