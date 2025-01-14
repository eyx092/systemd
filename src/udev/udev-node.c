/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#include "alloc-util.h"
#include "device-nodes.h"
#include "device-private.h"
#include "device-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "format-util.h"
#include "fs-util.h"
#include "mkdir.h"
#include "path-util.h"
#include "selinux-util.h"
#include "smack-util.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strxcpyx.h"
#include "udev-node.h"
#include "user-util.h"

#define LINK_UPDATE_MAX_RETRIES 128

static int node_symlink(sd_device *dev, const char *node, const char *slink) {
        _cleanup_free_ char *slink_dirname = NULL, *target = NULL;
        const char *id, *slink_tmp;
        struct stat stats;
        int r;

        assert(dev);
        assert(node);
        assert(slink);

        slink_dirname = dirname_malloc(slink);
        if (!slink_dirname)
                return log_oom();

        /* use relative link */
        r = path_make_relative(slink_dirname, node, &target);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to get relative path from '%s' to '%s': %m", slink, node);

        /* preserve link with correct target, do not replace node of other device */
        if (lstat(slink, &stats) == 0) {
                if (S_ISBLK(stats.st_mode) || S_ISCHR(stats.st_mode))
                        return log_device_error_errno(dev, SYNTHETIC_ERRNO(EOPNOTSUPP),
                                                      "Conflicting device node '%s' found, link to '%s' will not be created.", slink, node);
                else if (S_ISLNK(stats.st_mode)) {
                        _cleanup_free_ char *buf = NULL;

                        if (readlink_malloc(slink, &buf) >= 0 &&
                            streq(target, buf)) {
                                log_device_debug(dev, "Preserve already existing symlink '%s' to '%s'", slink, target);
                                (void) label_fix(slink, LABEL_IGNORE_ENOENT);
                                (void) utimensat(AT_FDCWD, slink, NULL, AT_SYMLINK_NOFOLLOW);
                                return 0;
                        }
                }
        } else {
                log_device_debug(dev, "Creating symlink '%s' to '%s'", slink, target);
                do {
                        r = mkdir_parents_label(slink, 0755);
                        if (!IN_SET(r, 0, -ENOENT))
                                break;
                        mac_selinux_create_file_prepare(slink, S_IFLNK);
                        if (symlink(target, slink) < 0)
                                r = -errno;
                        mac_selinux_create_file_clear();
                } while (r == -ENOENT);
                if (r == 0)
                        return 0;
                if (r < 0)
                        log_device_debug_errno(dev, r, "Failed to create symlink '%s' to '%s', trying to replace '%s': %m", slink, target, slink);
        }

        log_device_debug(dev, "Atomically replace '%s'", slink);
        r = device_get_device_id(dev, &id);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to get device id: %m");
        slink_tmp = strjoina(slink, ".tmp-", id);
        (void) unlink(slink_tmp);
        do {
                r = mkdir_parents_label(slink_tmp, 0755);
                if (!IN_SET(r, 0, -ENOENT))
                        break;
                mac_selinux_create_file_prepare(slink_tmp, S_IFLNK);
                if (symlink(target, slink_tmp) < 0)
                        r = -errno;
                mac_selinux_create_file_clear();
        } while (r == -ENOENT);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to create symlink '%s' to '%s': %m", slink_tmp, target);

        if (rename(slink_tmp, slink) < 0) {
                r = log_device_error_errno(dev, errno, "Failed to rename '%s' to '%s': %m", slink_tmp, slink);
                (void) unlink(slink_tmp);
        } else
                /* Tell caller that we replaced already existing symlink. */
                r = 1;

        return r;
}

/* find device node of device with highest priority */
static int link_find_prioritized(sd_device *dev, bool add, const char *stackdir, char **ret) {
        _cleanup_closedir_ DIR *dir = NULL;
        _cleanup_free_ char *target = NULL;
        struct dirent *dent;
        int r, priority = 0;

        assert(!add || dev);
        assert(stackdir);
        assert(ret);

        if (add) {
                const char *devnode;

                r = device_get_devlink_priority(dev, &priority);
                if (r < 0)
                        return r;

                r = sd_device_get_devname(dev, &devnode);
                if (r < 0)
                        return r;

                target = strdup(devnode);
                if (!target)
                        return -ENOMEM;
        }

        dir = opendir(stackdir);
        if (!dir) {
                if (target) {
                        *ret = TAKE_PTR(target);
                        return 0;
                }

                return -errno;
        }

        FOREACH_DIRENT_ALL(dent, dir, break) {
                _cleanup_(sd_device_unrefp) sd_device *dev_db = NULL;
                const char *devnode, *id;
                int db_prio = 0;

                if (dent->d_name[0] == '\0')
                        break;
                if (dent->d_name[0] == '.')
                        continue;

                log_device_debug(dev, "Found '%s' claiming '%s'", dent->d_name, stackdir);

                if (device_get_device_id(dev, &id) < 0)
                        continue;

                /* did we find ourself? */
                if (streq(dent->d_name, id))
                        continue;

                if (sd_device_new_from_device_id(&dev_db, dent->d_name) < 0)
                        continue;

                if (sd_device_get_devname(dev_db, &devnode) < 0)
                        continue;

                if (device_get_devlink_priority(dev_db, &db_prio) < 0)
                        continue;

                if (target && db_prio <= priority)
                        continue;

                log_device_debug(dev_db, "Device claims priority %i for '%s'", db_prio, stackdir);

                r = free_and_strdup(&target, devnode);
                if (r < 0)
                        return r;
                priority = db_prio;
        }

        if (!target)
                return -ENOENT;

        *ret = TAKE_PTR(target);
        return 0;
}

static size_t escape_path(const char *src, char *dest, size_t size) {
        size_t i, j;

        assert(src);
        assert(dest);

        for (i = 0, j = 0; src[i] != '\0'; i++) {
                if (src[i] == '/') {
                        if (j+4 >= size) {
                                j = 0;
                                break;
                        }
                        memcpy(&dest[j], "\\x2f", 4);
                        j += 4;
                } else if (src[i] == '\\') {
                        if (j+4 >= size) {
                                j = 0;
                                break;
                        }
                        memcpy(&dest[j], "\\x5c", 4);
                        j += 4;
                } else {
                        if (j+1 >= size) {
                                j = 0;
                                break;
                        }
                        dest[j] = src[i];
                        j++;
                }
        }
        dest[j] = '\0';
        return j;
}

/* manage "stack of names" with possibly specified device priorities */
static int link_update(sd_device *dev, const char *slink, bool add) {
        _cleanup_free_ char *filename = NULL, *dirname = NULL;
        const char *slink_name, *id;
        char name_enc[PATH_MAX];
        int i, r, retries;

        assert(dev);
        assert(slink);

        slink_name = path_startswith(slink, "/dev");
        if (!slink_name)
                return log_device_debug_errno(dev, SYNTHETIC_ERRNO(EINVAL),
                                              "Invalid symbolic link of device node: %s", slink);

        r = device_get_device_id(dev, &id);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get device id: %m");

        escape_path(slink_name, name_enc, sizeof(name_enc));
        dirname = path_join("/run/udev/links/", name_enc);
        if (!dirname)
                return log_oom();
        filename = path_join(dirname, id);
        if (!filename)
                return log_oom();

        if (!add) {
                if (unlink(filename) == 0)
                        (void) rmdir(dirname);
        } else
                for (;;) {
                        _cleanup_close_ int fd = -1;

                        r = mkdir_parents(filename, 0755);
                        if (!IN_SET(r, 0, -ENOENT))
                                return r;

                        fd = open(filename, O_WRONLY|O_CREAT|O_CLOEXEC|O_TRUNC|O_NOFOLLOW, 0444);
                        if (fd >= 0)
                                break;
                        if (errno != ENOENT)
                                return -errno;
                }

        /* If the database entry is not written yet we will just do one iteration and possibly wrong symlink
         * will be fixed in the second invocation. */
        retries = sd_device_get_is_initialized(dev) > 0 ? LINK_UPDATE_MAX_RETRIES : 1;

        for (i = 0; i < retries; i++) {
                _cleanup_free_ char *target = NULL;
                struct stat st1 = {}, st2 = {};

                r = stat(dirname, &st1);
                if (r < 0 && errno != ENOENT)
                        return -errno;

                r = link_find_prioritized(dev, add, dirname, &target);
                if (r == -ENOENT) {
                        log_device_debug(dev, "No reference left, removing '%s'", slink);
                        if (unlink(slink) == 0)
                                (void) rmdir_parents(slink, "/");

                        break;
                } else if (r < 0)
                        return log_device_error_errno(dev, r, "Failed to determine highest priority symlink: %m");

                r = node_symlink(dev, target, slink);
                if (r < 0) {
                        (void) unlink(filename);
                        break;
                } else if (r == 1)
                        /* We have replaced already existing symlink, possibly there is some other device trying
                         * to claim the same symlink. Let's do one more iteration to give us a chance to fix
                         * the error if other device actually claims the symlink with higher priority. */
                        continue;

                /* Skip the second stat() if the first failed, stat_inode_unmodified() would return false regardless. */
                if ((st1.st_mode & S_IFMT) != 0) {
                        r = stat(dirname, &st2);
                        if (r < 0 && errno != ENOENT)
                                return -errno;

                        if (stat_inode_unmodified(&st1, &st2))
                                break;
                }
        }

        return i < LINK_UPDATE_MAX_RETRIES ? 0 : -ELOOP;
}

int udev_node_update_old_links(sd_device *dev, sd_device *dev_old) {
        const char *name, *devpath;
        int r;

        assert(dev);
        assert(dev_old);

        r = sd_device_get_devpath(dev, &devpath);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devpath: %m");

        /* update possible left-over symlinks */
        FOREACH_DEVICE_DEVLINK(dev_old, name) {
                const char *name_current;
                bool found = false;

                /* check if old link name still belongs to this device */
                FOREACH_DEVICE_DEVLINK(dev, name_current)
                        if (streq(name, name_current)) {
                                found = true;
                                break;
                        }

                if (found)
                        continue;

                log_device_debug(dev, "Updating old name, '%s' no longer belonging to '%s'",
                                 name, devpath);
                r = link_update(dev, name, false);
                if (r < 0)
                        log_device_warning_errno(dev, r,
                                                 "Failed to update device symlink '%s', ignoring: %m",
                                                 name);
        }

        return 0;
}

static int node_permissions_apply(sd_device *dev, bool apply_mac,
                                  mode_t mode, uid_t uid, gid_t gid,
                                  OrderedHashmap *seclabel_list) {
        const char *devnode, *subsystem, *id = NULL;
        bool apply_mode, apply_uid, apply_gid;
        _cleanup_close_ int node_fd = -1;
        struct stat stats;
        dev_t devnum;
        int r;

        assert(dev);

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devname: %m");
        r = sd_device_get_subsystem(dev, &subsystem);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get subsystem: %m");
        r = sd_device_get_devnum(dev, &devnum);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devnum: %m");
        (void) device_get_device_id(dev, &id);

        if (streq(subsystem, "block"))
                mode |= S_IFBLK;
        else
                mode |= S_IFCHR;

        node_fd = open(devnode, O_PATH|O_NOFOLLOW|O_CLOEXEC);
        if (node_fd < 0) {
                if (errno == ENOENT) {
                        log_device_debug_errno(dev, errno, "Device node %s is missing, skipping handling.", devnode);
                        return 0; /* This is necessarily racey, so ignore missing the device */
                }

                return log_device_debug_errno(dev, errno, "Cannot open node %s: %m", devnode);
        }

        if (fstat(node_fd, &stats) < 0)
                return log_device_debug_errno(dev, errno, "cannot stat() node %s: %m", devnode);

        if ((mode != MODE_INVALID && (stats.st_mode & S_IFMT) != (mode & S_IFMT)) || stats.st_rdev != devnum) {
                log_device_debug(dev, "Found node '%s' with non-matching devnum %s, skipping handling.",
                                 devnode, strna(id));
                return 0; /* We might process a device that already got replaced by the time we have a look
                           * at it, handle this gracefully and step away. */
        }

        apply_mode = mode != MODE_INVALID && (stats.st_mode & 0777) != (mode & 0777);
        apply_uid = uid_is_valid(uid) && stats.st_uid != uid;
        apply_gid = gid_is_valid(gid) && stats.st_gid != gid;

        if (apply_mode || apply_uid || apply_gid || apply_mac) {
                bool selinux = false, smack = false;
                const char *name, *label;

                if (apply_mode || apply_uid || apply_gid) {
                        log_device_debug(dev, "Setting permissions %s, uid=" UID_FMT ", gid=" GID_FMT ", mode=%#o",
                                         devnode,
                                         uid_is_valid(uid) ? uid : stats.st_uid,
                                         gid_is_valid(gid) ? gid : stats.st_gid,
                                         mode != MODE_INVALID ? mode & 0777 : stats.st_mode & 0777);

                        r = fchmod_and_chown(node_fd, mode, uid, gid);
                        if (r < 0)
                                log_device_full_errno(dev, r == -ENOENT ? LOG_DEBUG : LOG_ERR, r,
                                                      "Failed to set owner/mode of %s to uid=" UID_FMT
                                                      ", gid=" GID_FMT ", mode=%#o: %m",
                                                      devnode,
                                                      uid_is_valid(uid) ? uid : stats.st_uid,
                                                      gid_is_valid(gid) ? gid : stats.st_gid,
                                                      mode != MODE_INVALID ? mode & 0777 : stats.st_mode & 0777);
                } else
                        log_device_debug(dev, "Preserve permissions of %s, uid=" UID_FMT ", gid=" GID_FMT ", mode=%#o",
                                         devnode,
                                         uid_is_valid(uid) ? uid : stats.st_uid,
                                         gid_is_valid(gid) ? gid : stats.st_gid,
                                         mode != MODE_INVALID ? mode & 0777 : stats.st_mode & 0777);

                /* apply SECLABEL{$module}=$label */
                ORDERED_HASHMAP_FOREACH_KEY(label, name, seclabel_list) {
                        int q;

                        if (streq(name, "selinux")) {
                                selinux = true;

                                q = mac_selinux_apply_fd(node_fd, devnode, label);
                                if (q < 0)
                                        log_device_full_errno(dev, q == -ENOENT ? LOG_DEBUG : LOG_ERR, q,
                                                              "SECLABEL: failed to set SELinux label '%s': %m", label);
                                else
                                        log_device_debug(dev, "SECLABEL: set SELinux label '%s'", label);

                        } else if (streq(name, "smack")) {
                                smack = true;

                                q = mac_smack_apply_fd(node_fd, SMACK_ATTR_ACCESS, label);
                                if (q < 0)
                                        log_device_full_errno(dev, q == -ENOENT ? LOG_DEBUG : LOG_ERR, q,
                                                              "SECLABEL: failed to set SMACK label '%s': %m", label);
                                else
                                        log_device_debug(dev, "SECLABEL: set SMACK label '%s'", label);

                        } else
                                log_device_error(dev, "SECLABEL: unknown subsystem, ignoring '%s'='%s'", name, label);
                }

                /* set the defaults */
                if (!selinux)
                        (void) mac_selinux_fix_fd(node_fd, devnode, LABEL_IGNORE_ENOENT);
                if (!smack)
                        (void) mac_smack_apply_fd(node_fd, SMACK_ATTR_ACCESS, NULL);
        }

        /* always update timestamp when we re-use the node, like on media change events */
        r = futimens_opath(node_fd, NULL);
        if (r < 0)
                log_device_debug_errno(dev, r, "Failed to adjust timestamp of node %s: %m", devnode);

        return r;
}

static int xsprintf_dev_num_path_from_sd_device(sd_device *dev, char **ret) {
        char filename[DEV_NUM_PATH_MAX], *s;
        const char *subsystem;
        dev_t devnum;
        int r;

        assert(ret);

        r = sd_device_get_subsystem(dev, &subsystem);
        if (r < 0)
                return r;

        r = sd_device_get_devnum(dev, &devnum);
        if (r < 0)
                return r;

        xsprintf_dev_num_path(filename,
                              streq(subsystem, "block") ? "block" : "char",
                              devnum);

        s = strdup(filename);
        if (!s)
                return -ENOMEM;

        *ret = s;
        return 0;
}

int udev_node_add(sd_device *dev, bool apply,
                  mode_t mode, uid_t uid, gid_t gid,
                  OrderedHashmap *seclabel_list) {
        const char *devnode, *devlink;
        _cleanup_free_ char *filename = NULL;
        int r;

        assert(dev);

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devnode: %m");

        if (DEBUG_LOGGING) {
                const char *id = NULL;

                (void) device_get_device_id(dev, &id);
                log_device_debug(dev, "Handling device node '%s', devnum=%s", devnode, strna(id));
        }

        r = node_permissions_apply(dev, apply, mode, uid, gid, seclabel_list);
        if (r < 0)
                return r;

        r = xsprintf_dev_num_path_from_sd_device(dev, &filename);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get device path: %m");

        /* always add /dev/{block,char}/$major:$minor */
        (void) node_symlink(dev, devnode, filename);

        /* create/update symlinks, add symlinks to name index */
        FOREACH_DEVICE_DEVLINK(dev, devlink) {
                r = link_update(dev, devlink, true);
                if (r < 0)
                        log_device_warning_errno(dev, r,
                                                 "Failed to update device symlink '%s', ignoring: %m",
                                                 devlink);
        }

        return 0;
}

int udev_node_remove(sd_device *dev) {
        _cleanup_free_ char *filename = NULL;
        const char *devlink;
        int r;

        assert(dev);

        /* remove/update symlinks, remove symlinks from name index */
        FOREACH_DEVICE_DEVLINK(dev, devlink) {
                r = link_update(dev, devlink, false);
                if (r < 0)
                        log_device_warning_errno(dev, r,
                                                 "Failed to update device symlink '%s', ignoring: %m",
                                                 devlink);
        }

        r = xsprintf_dev_num_path_from_sd_device(dev, &filename);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get device path: %m");

        /* remove /dev/{block,char}/$major:$minor */
        (void) unlink(filename);

        return 0;
}
