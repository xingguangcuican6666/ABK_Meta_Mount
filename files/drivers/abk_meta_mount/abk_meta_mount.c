// SPDX-License-Identifier: GPL-2.0
#include <linux/abk_meta_mount.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/kmod.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_ABK_CONTROL)
#include <linux/abk_control.h>
#endif

#define ABK_META_MOUNT_NAME "ABK Meta Mount"
#define ABK_META_MOUNT_VERSION "0.1.0"
#define ABK_META_MOUNT_AUTHOR "ABK"
#define ABK_META_MOUNT_DESC "Built-in KernelSU-compatible metamodule provider"
#define ABK_META_MOUNT_MODULES_DIR "/data/adb/modules"
#define ABK_META_MOUNT_DATA_DIR "/data/adb/modules/" ABK_META_MOUNT_ID
#define ABK_META_MOUNT_WEB_ROOT ABK_META_MOUNT_DATA_DIR "/webroot"
#define ABK_META_MOUNT_MARKER "/data/adb/metamodule"
#define ABK_META_MOUNT_HELPER "/system/bin/sh"
#define ABK_META_MOUNT_RUNTIME_ROOT "/mnt/abk_meta_mount"
#define ABK_META_MOUNT_STAGE_MODULES_DIR ABK_META_MOUNT_RUNTIME_ROOT "/current/modules"
#define ABK_META_MOUNT_OVERLAY_DIR ABK_META_MOUNT_RUNTIME_ROOT "/current/overlay"

struct abk_meta_mount_target {
	char path[64];
	char name[32];
	char last_status[64];
	char *last_lowerdir;
	unsigned long flags;
	bool ready;
	int last_ret;
	unsigned long last_attempt_jiffies;
	struct list_head node;
};

static LIST_HEAD(abk_meta_mount_targets);
static DEFINE_MUTEX(abk_meta_mount_lock);
static struct delayed_work abk_meta_mount_work;
static atomic_t abk_meta_mount_enabled = ATOMIC_INIT(1);
static bool abk_meta_mount_work_initialized;
static int abk_meta_mount_last_shell_ret;
static int abk_meta_mount_last_compat_ret;
static int abk_meta_mount_last_prepare_ret;
static unsigned long abk_meta_mount_last_compat_jiffies;
static bool abk_meta_mount_marker_owned;
#if IS_ENABLED(CONFIG_ABK_CONTROL)
static bool abk_meta_mount_registered_control;
#endif

static int abk_meta_mount_run_shell(const char *script);
static int abk_meta_mount_ensure_compat_module(void);
static int abk_meta_mount_prepare_target(struct abk_meta_mount_target *target);
static bool abk_meta_mount_has_unready_targets(void);
static void abk_meta_mount_schedule_retry(void);
static bool abk_meta_mount_marker_points_to_module(void);

static bool abk_meta_mount_marker_allows_mount(void)
{
	abk_meta_mount_marker_owned = abk_meta_mount_marker_points_to_module();
	return abk_meta_mount_marker_owned;
}

static const char * const abk_meta_mount_default_targets[] = {
	"/system",
	"/vendor",
	"/product",
	"/system_ext",
	"/odm",
	"/oem",
};

static const char * const abk_meta_mount_system_candidates[] = {
	"system",
};

static const char * const abk_meta_mount_vendor_candidates[] = {
	"vendor",
	"system/vendor",
};

static const char * const abk_meta_mount_product_candidates[] = {
	"product",
	"system/product",
};

static const char * const abk_meta_mount_system_ext_candidates[] = {
	"system_ext",
	"system/system_ext",
};

static const char * const abk_meta_mount_odm_candidates[] = {
	"odm",
	"system/odm",
};

static const char * const abk_meta_mount_oem_candidates[] = {
	"oem",
	"system/oem",
};

struct abk_meta_mount_scan_ctx {
	struct dir_context ctx;
	const char * const *candidates;
	size_t candidate_count;
	char *lowerdir;
	size_t lowerdir_size;
	unsigned int layers;
	bool stage_root_ready;
	int ret;
};

static void abk_meta_mount_set_status(struct abk_meta_mount_target *target,
				      const char *status)
{
	strscpy(target->last_status, status ?: "-", sizeof(target->last_status));
}

static int abk_meta_mount_set_lowerdir(struct abk_meta_mount_target *target,
				       const char *lowerdir)
{
	char *copy = NULL;

	if (lowerdir && lowerdir[0]) {
		copy = kstrdup(lowerdir, GFP_KERNEL);
		if (!copy)
			return -ENOMEM;
	}

	kfree(target->last_lowerdir);
	target->last_lowerdir = copy;
	return 0;
}

static bool abk_meta_mount_path_exists(const char *path)
{
	struct path resolved;
	int ret;

	ret = kern_path(path, LOOKUP_FOLLOW, &resolved);
	if (ret)
		return false;
	path_put(&resolved);
	return true;
}

static bool abk_meta_mount_path_is_dir(const char *path)
{
	struct path resolved;
	int ret;

	ret = kern_path(path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &resolved);
	if (ret)
		return false;
	path_put(&resolved);
	return true;
}

static bool abk_meta_mount_paths_equal(const char *left, const char *right)
{
	struct path left_path;
	struct path right_path;
	bool equal = false;

	if (kern_path(left, LOOKUP_FOLLOW, &left_path))
		return false;
	if (kern_path(right, LOOKUP_FOLLOW, &right_path)) {
		path_put(&left_path);
		return false;
	}

	equal = left_path.mnt == right_path.mnt &&
		left_path.dentry == right_path.dentry;
	path_put(&right_path);
	path_put(&left_path);
	return equal;
}

static bool abk_meta_mount_marker_points_to_module(void)
{
	return abk_meta_mount_paths_equal(ABK_META_MOUNT_MARKER,
					  ABK_META_MOUNT_DATA_DIR);
}

static bool abk_meta_mount_stage_root_ready(void)
{
	return abk_meta_mount_path_is_dir(ABK_META_MOUNT_STAGE_MODULES_DIR);
}

static bool abk_meta_mount_file_contains(const char *path, const char *needle)
{
	struct file *file;
	loff_t pos = 0;
	char buf[512];
	ssize_t len;
	bool found = false;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return false;

	len = kernel_read(file, buf, sizeof(buf) - 1, &pos);
	filp_close(file, NULL);
	if (len <= 0)
		return false;

	buf[len] = '\0';
	found = !!strnstr(buf, needle, len);
	return found;
}

static bool abk_meta_mount_is_metamodule_dir(const char *module_dir)
{
	char *prop_path;
	bool ret;

	prop_path = kasprintf(GFP_KERNEL, "%s/module.prop", module_dir);
	if (!prop_path)
		return false;

	ret = abk_meta_mount_file_contains(prop_path, "metamodule=1") ||
	      abk_meta_mount_file_contains(prop_path, "metamodule=true");
	kfree(prop_path);
	return ret;
}

static int abk_meta_mount_mkdir_one(const char *path, umode_t mode)
{
	struct path parent;
	struct dentry *dentry;
	int ret;

	if (abk_meta_mount_path_is_dir(path))
		return 0;

	dentry = kern_path_create(AT_FDCWD, path, &parent, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	ret = vfs_mkdir(mnt_idmap(parent.mnt), d_inode(parent.dentry), dentry, mode);
#else
	ret = vfs_mkdir(mnt_user_ns(parent.mnt), d_inode(parent.dentry), dentry, mode);
#endif
	done_path_create(&parent, dentry);
	return ret == -EEXIST ? 0 : ret;
}

static int abk_meta_mount_mkdir_p(const char *path, umode_t mode)
{
	char *copy;
	char *cursor;
	int ret = 0;

	if (!path || path[0] != '/')
		return -EINVAL;

	copy = kstrdup(path, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;

	for (cursor = copy + 1; ; cursor++) {
		if (*cursor != '/' && *cursor != '\0')
			continue;

		if (cursor != copy + 1) {
			char saved = *cursor;

			*cursor = '\0';
			ret = abk_meta_mount_mkdir_one(copy, mode);
			*cursor = saved;
			if (ret)
				break;
		}

		if (*cursor == '\0')
			break;
	}

	kfree(copy);
	return ret;
}

static const char * const *abk_meta_mount_candidates_for(const char *path,
							 size_t *count,
							 const char **fallback)
{
	if (!strcmp(path, "/system")) {
		*count = ARRAY_SIZE(abk_meta_mount_system_candidates);
		return abk_meta_mount_system_candidates;
	}
	if (!strcmp(path, "/vendor")) {
		*count = ARRAY_SIZE(abk_meta_mount_vendor_candidates);
		return abk_meta_mount_vendor_candidates;
	}
	if (!strcmp(path, "/product")) {
		*count = ARRAY_SIZE(abk_meta_mount_product_candidates);
		return abk_meta_mount_product_candidates;
	}
	if (!strcmp(path, "/system_ext")) {
		*count = ARRAY_SIZE(abk_meta_mount_system_ext_candidates);
		return abk_meta_mount_system_ext_candidates;
	}
	if (!strcmp(path, "/odm")) {
		*count = ARRAY_SIZE(abk_meta_mount_odm_candidates);
		return abk_meta_mount_odm_candidates;
	}
	if (!strcmp(path, "/oem")) {
		*count = ARRAY_SIZE(abk_meta_mount_oem_candidates);
		return abk_meta_mount_oem_candidates;
	}

	fallback[0] = path[0] == '/' ? path + 1 : path;
	*count = 1;
	return fallback;
}

static int abk_meta_mount_lowerdir_prepend(char *lowerdir, size_t lowerdir_size,
					   const char *path)
{
	size_t lower_len;
	size_t path_len;

	if (!path || !path[0])
		return 0;

	lower_len = strlen(lowerdir);
	path_len = strlen(path);
	if (!lower_len) {
		if (path_len + 1 > lowerdir_size)
			return -ENAMETOOLONG;
		memcpy(lowerdir, path, path_len + 1);
		return 0;
	}
	if (path_len + 1 + lower_len + 1 > lowerdir_size)
		return -ENAMETOOLONG;
	memmove(lowerdir + path_len + 1, lowerdir, lower_len + 1);
	memcpy(lowerdir, path, path_len);
	lowerdir[path_len] = ':';
	return 0;
}

static int abk_meta_mount_lowerdir_append(char *lowerdir, size_t lowerdir_size,
					  const char *path)
{
	size_t lower_len;
	size_t path_len;

	if (!path || !path[0])
		return 0;

	lower_len = strlen(lowerdir);
	path_len = strlen(path);
	if (!lower_len) {
		if (path_len + 1 > lowerdir_size)
			return -ENAMETOOLONG;
		memcpy(lowerdir, path, path_len + 1);
		return 0;
	}
	if (lower_len + 1 + path_len + 1 > lowerdir_size)
		return -ENAMETOOLONG;
	lowerdir[lower_len] = ':';
	memcpy(lowerdir + lower_len + 1, path, path_len + 1);
	return 0;
}

static bool abk_meta_mount_prepare_scan_actor(struct dir_context *ctx, const char *name,
					      int namelen, loff_t offset, u64 ino,
					      unsigned int d_type)
{
	struct abk_meta_mount_scan_ctx *scan =
		container_of(ctx, struct abk_meta_mount_scan_ctx, ctx);
	char *module_dir = NULL;
	char *layer_path = NULL;
	char *staged_layer_path = NULL;
	char *marker_path = NULL;
	char module_name[NAME_MAX + 1];
	size_t i;
	bool keep_going = true;
	int ret = 0;

	(void)offset;
	(void)ino;
	(void)d_type;

	if (scan->ret)
		return false;
	if (namelen <= 0 || namelen > NAME_MAX)
		return true;
	if (name[0] == '.' && (namelen == 1 || (namelen == 2 && name[1] == '.')))
		return true;

	memcpy(module_name, name, namelen);
	module_name[namelen] = '\0';

	if (!strcmp(module_name, ABK_META_MOUNT_ID))
		return true;

	module_dir = kasprintf(GFP_KERNEL, ABK_META_MOUNT_MODULES_DIR "/%s",
			      module_name);
	if (!module_dir)
		return true;
	if (!abk_meta_mount_path_is_dir(module_dir))
		goto out;

	marker_path = kasprintf(GFP_KERNEL, "%s/disable", module_dir);
	if (marker_path && abk_meta_mount_path_exists(marker_path))
		goto out;
	kfree(marker_path);
	marker_path = kasprintf(GFP_KERNEL, "%s/remove", module_dir);
	if (marker_path && abk_meta_mount_path_exists(marker_path))
		goto out;
	kfree(marker_path);
	marker_path = kasprintf(GFP_KERNEL, "%s/skip_mount", module_dir);
	if (marker_path && abk_meta_mount_path_exists(marker_path))
		goto out;
	if (abk_meta_mount_is_metamodule_dir(module_dir))
		goto out;

	for (i = 0; i < scan->candidate_count; i++) {
		layer_path = kasprintf(GFP_KERNEL, "%s/%s", module_dir,
				       scan->candidates[i]);
		if (!layer_path)
			continue;
		if (!abk_meta_mount_path_is_dir(layer_path)) {
			kfree(layer_path);
			layer_path = NULL;
			continue;
		}
		staged_layer_path = kasprintf(GFP_KERNEL,
					      ABK_META_MOUNT_STAGE_MODULES_DIR
					      "/%s/%s",
					      module_name, scan->candidates[i]);
		if (staged_layer_path &&
		    abk_meta_mount_path_is_dir(staged_layer_path)) {
			kfree(layer_path);
			layer_path = staged_layer_path;
			staged_layer_path = NULL;
		} else {
			kfree(staged_layer_path);
			staged_layer_path = NULL;
			kfree(layer_path);
			layer_path = NULL;
			continue;
		}
		ret = abk_meta_mount_lowerdir_prepend(scan->lowerdir,
						      scan->lowerdir_size,
						      layer_path);
		kfree(layer_path);
		layer_path = NULL;
		if (ret) {
			scan->ret = ret;
			keep_going = false;
			goto out;
		}
		scan->layers++;
	}

out:
	kfree(marker_path);
	kfree(staged_layer_path);
	kfree(layer_path);
	kfree(module_dir);
	return keep_going;
}

static int abk_meta_mount_collect_lowerdir(const char *target_path,
					   char *lowerdir, size_t lowerdir_size,
					   unsigned int *layers)
{
	struct abk_meta_mount_scan_ctx scan;
	struct file *dir;
	const char *fallback[1] = { NULL };
	int ret;

	if (!lowerdir || !lowerdir_size || !layers)
		return -EINVAL;

	lowerdir[0] = '\0';
	*layers = 0;
	memset(&scan, 0, sizeof(scan));
	scan.ctx.actor = abk_meta_mount_prepare_scan_actor;
	scan.candidates = abk_meta_mount_candidates_for(target_path,
						     &scan.candidate_count,
						     fallback);
	scan.lowerdir = lowerdir;
	scan.lowerdir_size = lowerdir_size;
	scan.stage_root_ready = abk_meta_mount_stage_root_ready();
	if (!scan.stage_root_ready)
		return -ENOENT;

	dir = filp_open(ABK_META_MOUNT_MODULES_DIR, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(dir))
		return PTR_ERR(dir);

	ret = iterate_dir(dir, &scan.ctx);
	filp_close(dir, NULL);
	if (ret)
		return ret;
	if (scan.ret)
		return scan.ret;

	*layers = scan.layers;
	return 0;
}

static int abk_meta_mount_overlay_target(struct abk_meta_mount_target *target,
					 const char *lowerdir)
{
	struct path path;
	char *upperdir;
	char *workdir;
	char *data;
	int len;
	int ret;

	upperdir = kasprintf(GFP_KERNEL,
			    ABK_META_MOUNT_OVERLAY_DIR "/%s/upper",
			    target->name);
	workdir = kasprintf(GFP_KERNEL,
			   ABK_META_MOUNT_OVERLAY_DIR "/%s/work",
			   target->name);
	if (!upperdir || !workdir) {
		ret = -ENOMEM;
		goto out_free_paths;
	}

	ret = abk_meta_mount_mkdir_p(upperdir, 0755);
	if (ret)
		goto out_free_paths;
	ret = abk_meta_mount_mkdir_p(workdir, 0755);
	if (ret)
		goto out_free_paths;

	data = (char *)__get_free_page(GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out_free_paths;
	}
	len = scnprintf(data, PAGE_SIZE, "lowerdir=%s,upperdir=%s,workdir=%s",
			lowerdir, upperdir, workdir);
	if (len >= PAGE_SIZE) {
		ret = -ENAMETOOLONG;
		goto out_free_data;
	}

	ret = kern_path(target->path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (ret)
		goto out_free_data;
	ret = path_mount("KSU", &path, "overlay", 0, data);
	path_put(&path);

out_free_data:
	free_page((unsigned long)data);
out_free_paths:
	kfree(workdir);
	kfree(upperdir);
	return ret;
}

static int abk_meta_mount_umount_target(struct abk_meta_mount_target *target)
{
	struct path path;
	int ret;

	ret = kern_path(target->path, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path);
	if (ret)
		return ret;
	ret = path_umount(&path, 0);
	return ret;
}

static bool abk_meta_mount_is_overlay_mount(const char *path)
{
	struct path resolved;
	bool overlay = false;
	int ret;

	ret = kern_path(path, LOOKUP_FOLLOW, &resolved);
	if (ret)
		return false;

	if (resolved.mnt && resolved.mnt->mnt_sb &&
	    resolved.mnt->mnt_sb->s_type &&
	    !strcmp(resolved.mnt->mnt_sb->s_type->name, "overlay"))
		overlay = true;

	path_put(&resolved);
	return overlay;
}

static bool abk_meta_mount_valid_target_path(const char *path)
{
	const char *cursor;

	if (!path || path[0] != '/')
		return false;

	for (cursor = path; *cursor; cursor++) {
		char ch = *cursor;

		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9') || ch == '/' || ch == '_' ||
		    ch == '-' || ch == '.')
			continue;
		return false;
	}

	return true;
}

static void abk_meta_mount_target_name(const char *path, char *out,
				       size_t out_len)
{
	size_t pos = 0;

	if (!out_len)
		return;

	if (!path || !path[0]) {
		strscpy(out, "root", out_len);
		return;
	}

	for (; *path && pos + 1 < out_len; path++) {
		char ch = *path;

		if (ch == '/')
			ch = '_';
		if (ch == '_') {
			if (!pos)
				continue;
			if (out[pos - 1] == '_')
				continue;
		}
		out[pos++] = ch;
	}
	out[pos] = '\0';

	if (!out[0])
		strscpy(out, "root", out_len);
}

static struct abk_meta_mount_target *
abk_meta_mount_find_target_locked(const char *path)
{
	struct abk_meta_mount_target *target;

	list_for_each_entry(target, &abk_meta_mount_targets, node) {
		if (!strcmp(target->path, path))
			return target;
	}

	return NULL;
}

int abk_meta_mount_register_target(const char *path, unsigned long flags)
{
	struct abk_meta_mount_target *target;

	if (!abk_meta_mount_valid_target_path(path) ||
	    strlen(path) >= sizeof(target->path))
		return -EINVAL;

	mutex_lock(&abk_meta_mount_lock);
	target = abk_meta_mount_find_target_locked(path);
	if (target) {
		target->flags |= flags;
		mutex_unlock(&abk_meta_mount_lock);
		return 0;
	}

	target = kzalloc(sizeof(*target), GFP_KERNEL);
	if (!target) {
		mutex_unlock(&abk_meta_mount_lock);
		return -ENOMEM;
	}

	strscpy(target->path, path, sizeof(target->path));
	abk_meta_mount_target_name(path, target->name, sizeof(target->name));
	target->flags = flags;
	target->last_ret = 0;
	target->last_attempt_jiffies = 0;
	list_add_tail(&target->node, &abk_meta_mount_targets);
	mutex_unlock(&abk_meta_mount_lock);

	pr_info("abk_meta_mount: registered target %s\n", path);
	return 0;
}
EXPORT_SYMBOL_GPL(abk_meta_mount_register_target);

bool abk_meta_mount_is_enabled(void)
{
	bool disabled;

	disabled = abk_meta_mount_path_exists(ABK_META_MOUNT_DATA_DIR "/disable") ||
		   abk_meta_mount_path_exists(ABK_META_MOUNT_DATA_DIR "/remove");
	return !disabled && atomic_read(&abk_meta_mount_enabled) != 0;
}
EXPORT_SYMBOL_GPL(abk_meta_mount_is_enabled);

bool abk_meta_mount_is_builtin_metamodule_active(void)
{
	return abk_meta_mount_is_enabled();
}
EXPORT_SYMBOL_GPL(abk_meta_mount_is_builtin_metamodule_active);

int abk_meta_mount_set_enabled(bool enabled)
{
	struct abk_meta_mount_target *target;
	int ret = 0;
	static const char enable_script[] =
		"MOD='" ABK_META_MOUNT_DATA_DIR "'\n"
		"mkdir -p \"$MOD\"\n"
		"rm -f \"$MOD/disable\" \"$MOD/remove\"\n";
	static const char disable_script[] =
		"MOD='" ABK_META_MOUNT_DATA_DIR "'\n"
		"mkdir -p \"$MOD\"\n"
		"touch \"$MOD/disable\"\n"
		"rm -f \"$MOD/remove\"\n";

	atomic_set(&abk_meta_mount_enabled, enabled ? 1 : 0);
	if (!enabled && abk_meta_mount_work_initialized)
		cancel_delayed_work_sync(&abk_meta_mount_work);

	abk_meta_mount_run_shell(enabled ? enable_script : disable_script);
	if (enabled) {
		if (abk_meta_mount_work_initialized)
			cancel_delayed_work_sync(&abk_meta_mount_work);
		ret = abk_meta_mount_prepare_all();
		if (ret || abk_meta_mount_has_unready_targets()) {
			abk_meta_mount_schedule_retry();
			if (ret && ret != -ENOENT)
				pr_warn("abk_meta_mount: enable prepare failed: %d\n", ret);
		}
	} else {
		mutex_lock(&abk_meta_mount_lock);
		list_for_each_entry(target, &abk_meta_mount_targets, node) {
			if (target->ready || abk_meta_mount_is_overlay_mount(target->path))
				abk_meta_mount_umount_target(target);
			target->ready = false;
			abk_meta_mount_set_status(target, "disabled");
		}
		mutex_unlock(&abk_meta_mount_lock);
	}
	pr_info("abk_meta_mount: %s\n", enabled ? "enabled" : "disabled");
	return ret == -ENOENT ? 0 : ret;
}
EXPORT_SYMBOL_GPL(abk_meta_mount_set_enabled);

bool abk_meta_mount_is_ready(const char *path)
{
	struct abk_meta_mount_target *target;
	bool ready = false;

	if (!path)
		return false;

	mutex_lock(&abk_meta_mount_lock);
	target = abk_meta_mount_find_target_locked(path);
	if (target)
		ready = target->ready;
	mutex_unlock(&abk_meta_mount_lock);

	return ready;
}
EXPORT_SYMBOL_GPL(abk_meta_mount_is_ready);

static int abk_meta_mount_run_shell(const char *script)
{
	static char *envp[] = {
		"HOME=/",
		"PATH=/system/bin:/system/xbin:/vendor/bin:/odm/bin:/sbin",
		NULL,
	};
	char *argv[] = {
		ABK_META_MOUNT_HELPER,
		"-c",
		(char *)script,
		NULL,
	};
	int ret;

	if (!abk_meta_mount_path_exists(ABK_META_MOUNT_HELPER))
		return abk_meta_mount_last_shell_ret = -ENOENT;

	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if (ret < 0) {
		abk_meta_mount_last_shell_ret = ret;
		return ret;
	}
	abk_meta_mount_last_shell_ret = ret ? -EIO : 0;
	return abk_meta_mount_last_shell_ret;
}

static int abk_meta_mount_ensure_compat_module(void)
{
	static const char script[] =
		"set -eu\n"
		"MOD='" ABK_META_MOUNT_DATA_DIR "'\n"
		"WEB='" ABK_META_MOUNT_WEB_ROOT "'\n"
		"MARK='" ABK_META_MOUNT_MARKER "'\n"
		"RUNTIME_BASE='" ABK_META_MOUNT_RUNTIME_ROOT "'\n"
		"CURRENT=\"$RUNTIME_BASE/current\"\n"
		"RUNTIME_DIR=\"$RUNTIME_BASE/run-$(date +%s)-$$\"\n"
		"STAGE=\"$RUNTIME_DIR/modules\"\n"
		"OVERLAY=\"$RUNTIME_DIR/overlay\"\n"
		"mkdir -p \"$MOD\" \"$WEB\"\n"
		"rm -f \"$MOD/.marker_owned\"\n"
		"mkdir -p \"$STAGE\" \"$OVERLAY\"\n"
		"for CUR in " ABK_META_MOUNT_MODULES_DIR "/*; do\n"
		"  [ -d \"$CUR\" ] || continue\n"
		"  MODNAME=${CUR##*/}\n"
		"  [ \"$MODNAME\" = '" ABK_META_MOUNT_ID "' ] && continue\n"
		"  [ -f \"$CUR/disable\" ] && continue\n"
		"  [ -f \"$CUR/remove\" ] && continue\n"
		"  [ -f \"$CUR/skip_mount\" ] && continue\n"
		"  if [ -f \"$CUR/module.prop\" ] && grep -Eq '^metamodule=(1|true)$' \"$CUR/module.prop\"; then\n"
		"    continue\n"
		"  fi\n"
		"  for REL in system vendor product system_ext odm oem system/vendor system/product system/system_ext system/odm system/oem; do\n"
		"    SRC=\"$CUR/$REL\"\n"
		"    DST=\"$STAGE/$MODNAME/$REL\"\n"
		"    [ -d \"$SRC\" ] || continue\n"
		"    mkdir -p \"$DST\"\n"
		"    cp -a \"$SRC\"/. \"$DST\"/\n"
		"  done\n"
		"done\n"
		"ln -sfn \"$RUNTIME_DIR\" \"$CURRENT\"\n"
		"cat > \"$MOD/module.prop\" <<'ABK_META_PROP'\n"
		"id=" ABK_META_MOUNT_ID "\n"
		"name=" ABK_META_MOUNT_NAME "\n"
		"version=" ABK_META_MOUNT_VERSION "\n"
		"versionCode=1\n"
		"author=" ABK_META_MOUNT_AUTHOR "\n"
		"description=" ABK_META_MOUNT_DESC "\n"
		"metamodule=1\n"
		"mount=false\n"
		"skip_mount=true\n"
		"web=1\n"
		"webui=1\n"
		"action=1\n"
		"ABK_META_PROP\n"
		"cat > \"$MOD/metamount.sh\" <<'ABK_META_METAMOUNT'\n"
		"#!/system/bin/sh\n"
		"rm -f /data/adb/modules/meta-abk-mount/disable /data/adb/modules/meta-abk-mount/remove\n"
		"echo 1 > /sys/kernel/abk_meta_mount/enabled 2>/dev/null || true\n"
		"echo 1 > /sys/kernel/abk_meta_mount/prepare 2>/dev/null || true\n"
		"ABK_META_METAMOUNT\n"
		"chmod 755 \"$MOD/metamount.sh\"\n"
		"cat > \"$MOD/action.sh\" <<'ABK_META_ACTION'\n"
		"#!/system/bin/sh\n"
		"if [ -f /proc/abk_meta_mount/status ]; then cat /proc/abk_meta_mount/status; else echo 'ABK Meta Mount status unavailable'; fi\n"
		"ABK_META_ACTION\n"
		"chmod 755 \"$MOD/action.sh\"\n"
		"cat > \"$WEB/index.html\" <<'ABK_META_WEB'\n"
		"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>ABK Meta Mount</title><style>body{font-family:system-ui,sans-serif;margin:20px;line-height:1.45;color:#171717;background:#f7f7f4}main{max-width:760px}button{padding:10px 14px;margin:0 8px 10px 0;border:1px solid #888;background:#fff;border-radius:6px}pre{white-space:pre-wrap;background:#101820;color:#eef5f5;padding:12px;border-radius:6px;min-height:180px;overflow:auto}</style></head><body><main><h1>ABK Meta Mount</h1><p>Built-in KernelSU metamodule provider. Disable is persistent; already-mounted overlays may require reboot to fully unwind.</p><button onclick=\"refresh()\">Refresh</button><button onclick=\"setEnabled(1)\">Enable</button><button onclick=\"setEnabled(0)\">Disable</button><pre id=\"out\">Loading...</pre></main><script>function out(v){document.getElementById('out').textContent=v}function sh(c){try{if(window.ksu&&typeof window.ksu.exec==='function'){return window.ksu.exec(c)}return 'KernelSU WebUI exec API unavailable'}catch(e){return String(e)}}function refresh(){out(sh('echo 1 > /sys/kernel/abk_meta_mount/prepare 2>/dev/null || true; cat /proc/abk_meta_mount/status 2>/dev/null || echo unavailable'))}function setEnabled(v){var c;if(v==1){c='rm -f /data/adb/modules/meta-abk-mount/disable /data/adb/modules/meta-abk-mount/remove; echo 1 > /sys/kernel/abk_meta_mount/enabled; echo 1 > /sys/kernel/abk_meta_mount/prepare 2>/dev/null || true'}else{c='mkdir -p /data/adb/modules/meta-abk-mount; touch /data/adb/modules/meta-abk-mount/disable; echo 0 > /sys/kernel/abk_meta_mount/enabled'}out(sh(c+'; cat /proc/abk_meta_mount/status 2>/dev/null || true'))}refresh()</script></body></html>\n"
		"ABK_META_WEB\n"
		"if [ -e \"$MARK\" ] && [ ! -L \"$MARK\" ]; then exit 0; fi\n"
		"TAKEOVER=0\n"
		"if [ ! -e \"$MARK\" ]; then\n"
		"  TAKEOVER=1\n"
		"elif [ -L \"$MARK\" ]; then\n"
		"  CUR=$(readlink \"$MARK\" 2>/dev/null || true)\n"
		"  if [ \"$CUR\" = \"$MOD\" ]; then\n"
		"    TAKEOVER=1\n"
		"  elif [ -z \"$CUR\" ] || [ ! -d \"$CUR\" ] || [ -f \"$CUR/disable\" ] || [ -f \"$CUR/remove\" ]; then\n"
		"    TAKEOVER=1\n"
		"  fi\n"
		"fi\n"
		"if [ \"$TAKEOVER\" = 1 ]; then ln -sfn \"$MOD\" \"$MARK\" && touch \"$MOD/.marker_owned\"; fi\n";
	int ret;

	abk_meta_mount_last_compat_jiffies = jiffies;
	if (!abk_meta_mount_path_exists("/data/adb"))
		return abk_meta_mount_last_compat_ret = -ENOENT;

	ret = abk_meta_mount_run_shell(script);
	abk_meta_mount_last_compat_ret = ret;
	abk_meta_mount_marker_owned = abk_meta_mount_marker_allows_mount();
	if (ret)
		pr_warn("abk_meta_mount: compat module setup failed: %d\n", ret);
	return ret;
}

static int abk_meta_mount_prepare_target(struct abk_meta_mount_target *target)
{
	char *lowerdir;
	unsigned int layers = 0;
	int ret;

	if (!target)
		return 0;
	target->last_attempt_jiffies = jiffies;
	target->last_ret = 0;
	abk_meta_mount_set_status(target, "checking");
	abk_meta_mount_set_lowerdir(target, NULL);
	if (!abk_meta_mount_marker_allows_mount()) {
		abk_meta_mount_set_status(target, "other_metamodule");
		target->last_ret = 0;
		return 0;
	}
	if (!abk_meta_mount_stage_root_ready()) {
		target->last_ret = -ENOENT;
		abk_meta_mount_set_status(target, "stage_missing");
		return -ENOENT;
	}
	if (target->ready) {
		if (abk_meta_mount_is_overlay_mount(target->path)) {
			abk_meta_mount_set_status(target, "already_overlay");
			return 0;
		}
		target->ready = false;
	}
	if (!abk_meta_mount_path_exists(target->path)) {
		if (!(target->flags & ABK_META_MOUNT_TARGET_OPTIONAL))
			pr_warn("abk_meta_mount: target missing: %s\n", target->path);
		target->last_ret = (target->flags & ABK_META_MOUNT_TARGET_OPTIONAL) ? 0 : -ENOENT;
		abk_meta_mount_set_status(target, "target_missing");
		return (target->flags & ABK_META_MOUNT_TARGET_OPTIONAL) ? 0 : -ENOENT;
	}

	lowerdir = kzalloc(PATH_MAX * 2, GFP_KERNEL);
	if (!lowerdir) {
		target->last_ret = -ENOMEM;
		abk_meta_mount_set_status(target, "oom");
		return -ENOMEM;
	}

	ret = abk_meta_mount_collect_lowerdir(target->path, lowerdir,
					       PATH_MAX * 2, &layers);
	if (ret) {
		target->last_ret = ret;
		abk_meta_mount_set_status(target, "scan_failed");
		kfree(lowerdir);
		return ret;
	}
	if (!layers) {
		target->last_ret = 0;
		abk_meta_mount_set_status(target, "no_candidates");
		kfree(lowerdir);
		return 0;
	}

	ret = abk_meta_mount_lowerdir_append(lowerdir, PATH_MAX * 2,
					      target->path);
	if (ret) {
		target->last_ret = ret;
		abk_meta_mount_set_status(target, "lowerdir_too_long");
		kfree(lowerdir);
		return ret;
	}

	ret = abk_meta_mount_set_lowerdir(target, lowerdir);
	if (ret) {
		target->last_ret = ret;
		abk_meta_mount_set_status(target, "oom");
		kfree(lowerdir);
		return ret;
	}

	if (abk_meta_mount_is_overlay_mount(target->path)) {
		target->ready = true;
		target->last_ret = 0;
		abk_meta_mount_set_status(target, "already_overlay");
		kfree(lowerdir);
		return 0;
	}

	ret = abk_meta_mount_overlay_target(target, lowerdir);
	target->last_ret = ret;
	if (ret) {
		abk_meta_mount_set_status(target, "mount_failed");
		kfree(lowerdir);
		return ret;
	}

	if (abk_meta_mount_is_overlay_mount(target->path)) {
		target->ready = true;
		abk_meta_mount_set_status(target, "mounted");
		pr_info("abk_meta_mount: prepared %s\n", target->path);
	} else {
		ret = -EIO;
		target->last_ret = ret;
		abk_meta_mount_set_status(target, "mount_not_visible");
	}
	kfree(lowerdir);
	return ret;
}

int abk_meta_mount_prepare_all(void)
{
	struct abk_meta_mount_target *target;
	int compat_ret;
	int ret = 0;

	if (!abk_meta_mount_is_enabled()) {
		abk_meta_mount_last_prepare_ret = -EPERM;
		return -EPERM;
	}

	compat_ret = abk_meta_mount_ensure_compat_module();
	if (compat_ret)
		ret = compat_ret;

	mutex_lock(&abk_meta_mount_lock);
	list_for_each_entry(target, &abk_meta_mount_targets, node) {
		int target_ret;

		target_ret = abk_meta_mount_prepare_target(target);
		if (target_ret && !ret)
			ret = target_ret;
	}
	mutex_unlock(&abk_meta_mount_lock);

	abk_meta_mount_last_prepare_ret = ret;
	return ret;
}
EXPORT_SYMBOL_GPL(abk_meta_mount_prepare_all);

static bool abk_meta_mount_has_unready_targets(void)
{
	struct abk_meta_mount_target *target;
	bool unready = false;

	mutex_lock(&abk_meta_mount_lock);
	list_for_each_entry(target, &abk_meta_mount_targets, node) {
		if (!target->ready && target->last_ret) {
			unready = true;
			break;
		}
	}
	mutex_unlock(&abk_meta_mount_lock);

	return unready;
}

static ssize_t enabled_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	(void)kobj;
	(void)attr;
	return sysfs_emit(buf, "%d\n", abk_meta_mount_is_enabled() ? 1 : 0);
}

static ssize_t enabled_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool enabled;
	int ret;

	(void)kobj;
	(void)attr;

	ret = kstrtobool(buf, &enabled);
	if (ret)
		return ret;
	abk_meta_mount_set_enabled(enabled);
	return count;
}

static struct kobj_attribute abk_meta_mount_enabled_attr =
	__ATTR_RW(enabled);

static ssize_t prepare_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool prepare;
	int ret;

	(void)kobj;
	(void)attr;

	ret = kstrtobool(buf, &prepare);
	if (ret)
		return ret;
	if (prepare) {
		ret = abk_meta_mount_prepare_all();
		if (ret == -EPERM)
			return ret;
		if (ret) {
			abk_meta_mount_schedule_retry();
			return count;
		}
		if (abk_meta_mount_has_unready_targets())
			abk_meta_mount_schedule_retry();
	}
	return count;
}

static struct kobj_attribute abk_meta_mount_prepare_attr =
	__ATTR_WO(prepare);

static struct kobject *abk_meta_mount_kobj;

#if IS_ENABLED(CONFIG_PROC_FS)
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static int abk_meta_mount_status_show(struct seq_file *m, void *v)
{
	struct abk_meta_mount_target *target;

	(void)v;

	seq_printf(m, "id=%s\n", ABK_META_MOUNT_ID);
	seq_printf(m, "name=%s\n", ABK_META_MOUNT_NAME);
	seq_printf(m, "version=%s\n", ABK_META_MOUNT_VERSION);
	seq_printf(m, "enabled=%d\n", abk_meta_mount_is_enabled() ? 1 : 0);
	seq_printf(m, "atomic_enabled=%d\n", atomic_read(&abk_meta_mount_enabled));
	seq_printf(m, "metamodule=1\n");
	seq_printf(m, "path_data_adb=%d\n",
		   abk_meta_mount_path_exists("/data/adb") ? 1 : 0);
	seq_printf(m, "path_helper=%d\n",
		   abk_meta_mount_path_exists(ABK_META_MOUNT_HELPER) ? 1 : 0);
	seq_printf(m, "path_module_dir=%d\n",
		   abk_meta_mount_path_exists(ABK_META_MOUNT_DATA_DIR) ? 1 : 0);
	seq_printf(m, "path_action=%d\n",
		   abk_meta_mount_path_exists(ABK_META_MOUNT_DATA_DIR "/action.sh") ? 1 : 0);
	seq_printf(m, "path_web_index=%d\n",
		   abk_meta_mount_path_exists(ABK_META_MOUNT_WEB_ROOT "/index.html") ? 1 : 0);
	seq_printf(m, "disabled_marker=%d\n",
		   abk_meta_mount_path_exists(ABK_META_MOUNT_DATA_DIR "/disable") ? 1 : 0);
	seq_printf(m, "remove_marker=%d\n",
		   abk_meta_mount_path_exists(ABK_META_MOUNT_DATA_DIR "/remove") ? 1 : 0);
	seq_printf(m, "last_shell_ret=%d\n", abk_meta_mount_last_shell_ret);
	seq_printf(m, "last_compat_ret=%d\n", abk_meta_mount_last_compat_ret);
	seq_printf(m, "last_prepare_ret=%d\n", abk_meta_mount_last_prepare_ret);
	seq_printf(m, "marker_owned=%d\n", abk_meta_mount_marker_owned ? 1 : 0);
	seq_printf(m, "last_compat_age_ms=%u\n",
		   abk_meta_mount_last_compat_jiffies ?
		   jiffies_to_msecs(jiffies - abk_meta_mount_last_compat_jiffies) : 0);

	mutex_lock(&abk_meta_mount_lock);
	list_for_each_entry(target, &abk_meta_mount_targets, node)
	{
		unsigned long age_ms = target->last_attempt_jiffies ?
			jiffies_to_msecs(jiffies - target->last_attempt_jiffies) : 0;

		seq_printf(m, "target=%s ready=%d ret=%d age_ms=%lu status=%s lowerdir=%s\n",
			   target->path, target->ready ? 1 : 0,
			   target->last_ret, age_ms,
			   target->last_status[0] ? target->last_status : "-",
			   target->last_lowerdir ? target->last_lowerdir : "-");
	}
	mutex_unlock(&abk_meta_mount_lock);

	return 0;
}

static int abk_meta_mount_status_open(struct inode *inode, struct file *file)
{
	(void)inode;
	return single_open(file, abk_meta_mount_status_show, NULL);
}

static const struct proc_ops abk_meta_mount_status_fops = {
	.proc_open = abk_meta_mount_status_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_dir_entry *abk_meta_mount_proc_dir;
#endif

#if IS_ENABLED(CONFIG_ABK_CONTROL)
static bool abk_meta_mount_control_is_enabled(void *data)
{
	(void)data;
	return abk_meta_mount_is_enabled();
}

static int abk_meta_mount_control_set_enabled(bool enabled, void *data)
{
	(void)data;
	return abk_meta_mount_set_enabled(enabled);
}

static const struct abk_control_ops abk_meta_mount_control_ops = {
	.id = ABK_META_MOUNT_ID,
	.name = ABK_META_MOUNT_NAME,
	.version = ABK_META_MOUNT_VERSION,
	.description = ABK_META_MOUNT_DESC,
#ifdef ABK_CONTROL_OPS_HAS_RUNTIME_UI
	.module_dir = ABK_META_MOUNT_DATA_DIR,
	.web_root = ABK_META_MOUNT_WEB_ROOT,
	.has_web_ui = true,
	.has_action_script = true,
	.action_supported = true,
#endif
	.is_enabled = abk_meta_mount_control_is_enabled,
	.set_enabled = abk_meta_mount_control_set_enabled,
};

static void abk_meta_mount_register_control(void)
{
	int ret;

	ret = abk_control_register(&abk_meta_mount_control_ops);
	if (ret == -EEXIST)
		ret = 0;
	if (ret)
		pr_warn("abk_meta_mount: ABK Control registration failed: %d\n",
			ret);
	else
		abk_meta_mount_registered_control = true;
}
#else
static void abk_meta_mount_register_control(void)
{
}
#endif

static void abk_meta_mount_workfn(struct work_struct *work)
{
	int ret;

	(void)work;

	ret = abk_meta_mount_prepare_all();
	if (ret || abk_meta_mount_has_unready_targets())
		abk_meta_mount_schedule_retry();
}

static void abk_meta_mount_schedule_retry(void)
{
	if (abk_meta_mount_work_initialized && abk_meta_mount_is_enabled() &&
	    !delayed_work_pending(&abk_meta_mount_work))
		schedule_delayed_work(&abk_meta_mount_work, 60 * HZ);
}

static void abk_meta_mount_register_defaults(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(abk_meta_mount_default_targets); i++)
		abk_meta_mount_register_target(abk_meta_mount_default_targets[i],
					       ABK_META_MOUNT_TARGET_OPTIONAL);
}

static int __init abk_meta_mount_init(void)
{
	int ret = 0;

	abk_meta_mount_register_defaults();

	abk_meta_mount_kobj = kobject_create_and_add("abk_meta_mount",
						     kernel_kobj);
	if (abk_meta_mount_kobj) {
		ret = sysfs_create_file(abk_meta_mount_kobj,
					&abk_meta_mount_enabled_attr.attr);
		if (ret)
			pr_warn("abk_meta_mount: enabled sysfs setup failed: %d\n",
				ret);
		ret = sysfs_create_file(abk_meta_mount_kobj,
					&abk_meta_mount_prepare_attr.attr);
		if (ret)
			pr_warn("abk_meta_mount: prepare sysfs setup failed: %d\n",
				ret);
	}

#if IS_ENABLED(CONFIG_PROC_FS)
	abk_meta_mount_proc_dir = proc_mkdir("abk_meta_mount", NULL);
	if (abk_meta_mount_proc_dir)
		proc_create("status", 0444, abk_meta_mount_proc_dir,
			    &abk_meta_mount_status_fops);
#endif

	abk_meta_mount_register_control();

	INIT_DELAYED_WORK(&abk_meta_mount_work, abk_meta_mount_workfn);
	abk_meta_mount_work_initialized = true;
	schedule_delayed_work(&abk_meta_mount_work, 5 * HZ);

	pr_info("abk_meta_mount: initialized\n");
	return 0;
}

static void __exit abk_meta_mount_exit(void)
{
	struct abk_meta_mount_target *target;
	struct abk_meta_mount_target *next;

	if (abk_meta_mount_work_initialized)
		cancel_delayed_work_sync(&abk_meta_mount_work);

#if IS_ENABLED(CONFIG_ABK_CONTROL)
	if (abk_meta_mount_registered_control)
		abk_control_unregister(&abk_meta_mount_control_ops);
#endif

#if IS_ENABLED(CONFIG_PROC_FS)
	if (abk_meta_mount_proc_dir)
		remove_proc_subtree("abk_meta_mount", NULL);
#endif

	if (abk_meta_mount_kobj) {
		sysfs_remove_file(abk_meta_mount_kobj,
				  &abk_meta_mount_prepare_attr.attr);
		sysfs_remove_file(abk_meta_mount_kobj,
				  &abk_meta_mount_enabled_attr.attr);
		kobject_put(abk_meta_mount_kobj);
	}

	mutex_lock(&abk_meta_mount_lock);
	list_for_each_entry_safe(target, next, &abk_meta_mount_targets, node) {
		list_del(&target->node);
		kfree(target->last_lowerdir);
		kfree(target);
	}
	mutex_unlock(&abk_meta_mount_lock);
}

module_init(abk_meta_mount_init);
module_exit(abk_meta_mount_exit);

MODULE_DESCRIPTION("ABK built-in KernelSU-compatible metamodule provider");
MODULE_LICENSE("GPL");
