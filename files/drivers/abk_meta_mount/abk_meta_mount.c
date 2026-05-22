// SPDX-License-Identifier: GPL-2.0
#include <linux/abk_meta_mount.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
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
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_ABK_CONTROL)
#include <linux/abk_control.h>
#endif

#define ABK_META_MOUNT_NAME "ABK Meta Mount"
#define ABK_META_MOUNT_VERSION "0.1.0"
#define ABK_META_MOUNT_AUTHOR "ABK"
#define ABK_META_MOUNT_DESC "Built-in KernelSU-compatible metamodule provider"
#define ABK_META_MOUNT_ROOT "/dev/abk_meta_mount"
#define ABK_META_MOUNT_DATA_DIR "/data/adb/modules/" ABK_META_MOUNT_ID
#define ABK_META_MOUNT_WEB_ROOT ABK_META_MOUNT_DATA_DIR "/webroot"
#define ABK_META_MOUNT_MARKER "/data/adb/metamodule"
#define ABK_META_MOUNT_HELPER "/system/bin/sh"

struct abk_meta_mount_target {
	char path[64];
	char name[32];
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
#if IS_ENABLED(CONFIG_ABK_CONTROL)
static bool abk_meta_mount_registered_control;
#endif

static int abk_meta_mount_run_shell(const char *script);
static int abk_meta_mount_ensure_compat_module(void);
static int abk_meta_mount_prepare_target(struct abk_meta_mount_target *target);
static bool abk_meta_mount_has_unready_targets(void);
static void abk_meta_mount_schedule_retry(void);

static const char * const abk_meta_mount_default_targets[] = {
	"/system",
	"/vendor",
	"/product",
	"/system_ext",
	"/odm",
	"/oem",
};

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
		"rm -f \"$MOD/remove\"\n"
		"for p in /system /vendor /product /system_ext /odm /oem; do umount \"$p\" 2>/dev/null || true; done\n";

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
		list_for_each_entry(target, &abk_meta_mount_targets, node)
			target->ready = false;
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
		"mkdir -p \"$MOD\" \"$WEB\"\n"
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
		"[ \"$TAKEOVER\" = 1 ] && ln -sfn \"$MOD\" \"$MARK\"\n";
	int ret;

	abk_meta_mount_last_compat_jiffies = jiffies;
	if (!abk_meta_mount_path_exists("/data/adb"))
		return abk_meta_mount_last_compat_ret = -ENOENT;

	ret = abk_meta_mount_run_shell(script);
	abk_meta_mount_last_compat_ret = ret;
	if (ret)
		pr_warn("abk_meta_mount: compat module setup failed: %d\n", ret);
	return ret;
}

static int abk_meta_mount_prepare_target(struct abk_meta_mount_target *target)
{
	char script[4096];
	int len;
	int ret;

	if (!target)
		return 0;
	target->last_attempt_jiffies = jiffies;
	target->last_ret = 0;
	if (target->ready) {
		if (abk_meta_mount_is_overlay_mount(target->path))
			return 0;
		target->ready = false;
	}
	if (!abk_meta_mount_path_exists(target->path)) {
		if (!(target->flags & ABK_META_MOUNT_TARGET_OPTIONAL))
			pr_warn("abk_meta_mount: target missing: %s\n", target->path);
		target->last_ret = (target->flags & ABK_META_MOUNT_TARGET_OPTIONAL) ? 0 : -ENOENT;
		return (target->flags & ABK_META_MOUNT_TARGET_OPTIONAL) ? 0 : -ENOENT;
	}

	len = scnprintf(script, sizeof(script),
		 "set -eu\n"
		 "T='%s'\n"
		 "N='%s'\n"
		 "R='" ABK_META_MOUNT_ROOT "'\n"
		 "MOD='" ABK_META_MOUNT_DATA_DIR "'\n"
		 "MARK='" ABK_META_MOUNT_MARKER "'\n"
		 "D=\"$R/$N\"\n"
		 "STATUS=\"$D/status\"\n"
		 "LOWERFILE=\"$D/lowerdir\"\n"
		 "mkdir -p \"$D\"\n"
		 ": > \"$STATUS\"\n"
		 ": > \"$LOWERFILE\"\n"
		 "if [ -e \"$MARK\" ] && [ ! -L \"$MARK\" ]; then printf 'foreign_marker\\n' > \"$STATUS\"; exit 0; fi\n"
		 "if [ -L \"$MARK\" ]; then\n"
		 "  CUR=$(readlink \"$MARK\" 2>/dev/null || true)\n"
		 "  if [ \"$CUR\" != \"$MOD\" ]; then\n"
		 "    [ -z \"$CUR\" ] || [ ! -d \"$CUR\" ] || [ -f \"$CUR/disable\" ] || [ -f \"$CUR/remove\" ] || { printf 'other_metamodule\\n' > \"$STATUS\"; exit 0; }\n"
		 "    ln -sfn \"$MOD\" \"$MARK\"\n"
		 "  fi\n"
		 "fi\n"
		 "REL=\"${T#/}\"\n"
		 "case \"$T\" in\n"
		 "  /system) CANDS='system' ;;\n"
		 "  /vendor) CANDS='vendor system/vendor' ;;\n"
		 "  /product) CANDS='product system/product' ;;\n"
		 "  /system_ext) CANDS='system_ext system/system_ext' ;;\n"
		 "  /odm) CANDS='odm system/odm' ;;\n"
		 "  /oem) CANDS='oem system/oem' ;;\n"
		 "  *) CANDS=\"$REL\" ;;\n"
		 "esac\n"
		 "LOWERS=''\n"
		 "for M in /data/adb/modules/*; do\n"
		 "  [ -d \"$M\" ] || continue\n"
		 "  [ \"${M##*/}\" = '" ABK_META_MOUNT_ID "' ] && continue\n"
		 "  [ -f \"$M/disable\" ] && continue\n"
		 "  [ -f \"$M/remove\" ] && continue\n"
		 "  [ -f \"$M/skip_mount\" ] && continue\n"
		 "  if grep -Eq '^metamodule=(1|true)$' \"$M/module.prop\" 2>/dev/null; then continue; fi\n"
		 "  for C in $CANDS; do\n"
		 "    [ -d \"$M/$C\" ] || continue\n"
		 "    LOWERS=\"$M/$C${LOWERS:+:$LOWERS}\"\n"
		 "  done\n"
		 "done\n"
		 "if [ -z \"$LOWERS\" ]; then\n"
		 "  printf 'no_candidates\\n' > \"$STATUS\"\n"
		 "  exit 0\n"
		 "fi\n"
		 "LOWERS=\"$LOWERS:$T\"\n"
		 "printf '%%s\\n' \"$LOWERS\" > \"$LOWERFILE\"\n"
		 "mkdir -p \"$R/$N/upper\" \"$R/$N/work\"\n"
		 "if grep -F \" $T overlay \" /proc/mounts >/dev/null 2>&1; then\n"
		 "  printf 'already_overlay\\n' > \"$STATUS\"\n"
		 "  exit 0\n"
		 "fi\n"
		 "if mount -t overlay KSU -o lowerdir=\"$LOWERS\",upperdir=\"$R/$N/upper\",workdir=\"$R/$N/work\" \"$T\" 2>/dev/null; then\n"
		 "  if grep -F \" $T overlay \" /proc/mounts >/dev/null 2>&1; then\n"
		 "    printf 'mounted\\n' > \"$STATUS\"\n"
		 "    exit 0\n"
		 "  fi\n"
		 "fi\n"
		 "printf 'mount_failed\\n' > \"$STATUS\"\n"
		 "exit 4\n",
		 target->path, target->name);
	if (len < 0 || len >= sizeof(script)) {
		target->last_ret = -E2BIG;
		return -E2BIG;
	}

	ret = abk_meta_mount_run_shell(script);
	target->last_ret = ret;
	if (ret)
		return ret;

	if (abk_meta_mount_is_overlay_mount(target->path)) {
		target->ready = true;
		pr_info("abk_meta_mount: prepared %s\n", target->path);
	}
	return 0;
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
		if (!target->ready) {
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

static void abk_meta_mount_seq_print_file(struct seq_file *m, const char *path)
{
	struct file *file;
	loff_t pos = 0;
	char buf[512];
	ssize_t len;

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file)) {
		seq_puts(m, "-");
		return;
	}

	len = kernel_read(file, buf, sizeof(buf) - 1, &pos);
	filp_close(file, NULL);
	if (len <= 0) {
		seq_puts(m, "-");
		return;
	}

	buf[len] = '\0';
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	seq_puts(m, buf);
}

static int abk_meta_mount_status_show(struct seq_file *m, void *v)
{
	struct abk_meta_mount_target *target;
	char status_path[128];
	char lower_path[128];

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
	seq_printf(m, "last_compat_age_ms=%u\n",
		   abk_meta_mount_last_compat_jiffies ?
		   jiffies_to_msecs(jiffies - abk_meta_mount_last_compat_jiffies) : 0);

	mutex_lock(&abk_meta_mount_lock);
	list_for_each_entry(target, &abk_meta_mount_targets, node)
	{
		unsigned long age_ms = target->last_attempt_jiffies ?
			jiffies_to_msecs(jiffies - target->last_attempt_jiffies) : 0;

		scnprintf(status_path, sizeof(status_path),
			  ABK_META_MOUNT_ROOT "/%s/status", target->name);
		scnprintf(lower_path, sizeof(lower_path),
			  ABK_META_MOUNT_ROOT "/%s/lowerdir", target->name);
		seq_printf(m, "target=%s ready=%d ret=%d age_ms=%lu status=",
			   target->path, target->ready ? 1 : 0,
			   target->last_ret, age_ms);
		abk_meta_mount_seq_print_file(m, status_path);
		seq_puts(m, " lowerdir=");
		abk_meta_mount_seq_print_file(m, lower_path);
		seq_putc(m, '\n');
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
		kfree(target);
	}
	mutex_unlock(&abk_meta_mount_lock);
}

module_init(abk_meta_mount_init);
module_exit(abk_meta_mount_exit);

MODULE_DESCRIPTION("ABK built-in KernelSU-compatible metamodule provider");
MODULE_LICENSE("GPL");
