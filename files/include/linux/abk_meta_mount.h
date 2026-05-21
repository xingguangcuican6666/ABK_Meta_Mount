/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ABK_META_MOUNT_H
#define _LINUX_ABK_META_MOUNT_H

#include <linux/bits.h>
#include <linux/types.h>

#define ABK_META_MOUNT_ID "meta-abk-mount"

enum abk_meta_mount_target_flags {
	ABK_META_MOUNT_TARGET_DEFAULT = 0,
	ABK_META_MOUNT_TARGET_OPTIONAL = BIT(0),
};

bool abk_meta_mount_is_enabled(void);
bool abk_meta_mount_is_builtin_metamodule_active(void);
bool abk_meta_mount_is_ready(const char *path);
int abk_meta_mount_set_enabled(bool enabled);
int abk_meta_mount_register_target(const char *path, unsigned long flags);
int abk_meta_mount_prepare_all(void);

#endif /* _LINUX_ABK_META_MOUNT_H */
