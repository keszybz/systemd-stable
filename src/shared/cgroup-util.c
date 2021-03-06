/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ftw.h>

#include "cgroup-util.h"
#include "log.h"
#include "set.h"
#include "macro.h"
#include "util.h"
#include "path-util.h"
#include "strv.h"
#include "unit-name.h"
#include "fileio.h"

int cg_enumerate_processes(const char *controller, const char *path, FILE **_f) {
        char *fs;
        int r;
        FILE *f;

        assert(path);
        assert(_f);

        r = cg_get_path(controller, path, "cgroup.procs", &fs);
        if (r < 0)
                return r;

        f = fopen(fs, "re");
        free(fs);

        if (!f)
                return -errno;

        *_f = f;
        return 0;
}

int cg_enumerate_tasks(const char *controller, const char *path, FILE **_f) {
        char *fs;
        int r;
        FILE *f;

        assert(path);
        assert(_f);

        r = cg_get_path(controller, path, "tasks", &fs);
        if (r < 0)
                return r;

        f = fopen(fs, "re");
        free(fs);

        if (!f)
                return -errno;

        *_f = f;
        return 0;
}

int cg_read_pid(FILE *f, pid_t *_pid) {
        unsigned long ul;

        /* Note that the cgroup.procs might contain duplicates! See
         * cgroups.txt for details. */

        errno = 0;
        if (fscanf(f, "%lu", &ul) != 1) {

                if (feof(f))
                        return 0;

                return errno ? -errno : -EIO;
        }

        if (ul <= 0)
                return -EIO;

        *_pid = (pid_t) ul;
        return 1;
}

int cg_enumerate_subgroups(const char *controller, const char *path, DIR **_d) {
        char *fs;
        int r;
        DIR *d;

        assert(path);
        assert(_d);

        /* This is not recursive! */

        r = cg_get_path(controller, path, NULL, &fs);
        if (r < 0)
                return r;

        d = opendir(fs);
        free(fs);

        if (!d)
                return -errno;

        *_d = d;
        return 0;
}

int cg_read_subgroup(DIR *d, char **fn) {
        struct dirent *de;

        assert(d);

        errno = 0;
        while ((de = readdir(d))) {
                char *b;

                if (de->d_type != DT_DIR)
                        continue;

                if (streq(de->d_name, ".") ||
                    streq(de->d_name, ".."))
                        continue;

                if (!(b = strdup(de->d_name)))
                        return -ENOMEM;

                *fn = b;
                return 1;
        }

        if (errno)
                return -errno;

        return 0;
}

int cg_rmdir(const char *controller, const char *path, bool honour_sticky) {
        char *p;
        int r;

        r = cg_get_path(controller, path, NULL, &p);
        if (r < 0)
                return r;

        if (honour_sticky) {
                char *tasks;

                /* If the sticky bit is set don't remove the directory */

                tasks = strappend(p, "/tasks");
                if (!tasks) {
                        free(p);
                        return -ENOMEM;
                }

                r = file_is_priv_sticky(tasks);
                free(tasks);

                if (r > 0) {
                        free(p);
                        return 0;
                }
        }

        r = rmdir(p);
        free(p);

        return (r < 0 && errno != ENOENT) ? -errno : 0;
}

int cg_kill(const char *controller, const char *path, int sig, bool sigcont, bool ignore_self, Set *s) {
        bool done = false;
        int r, ret = 0;
        pid_t my_pid;
        FILE *f = NULL;
        Set *allocated_set = NULL;

        assert(controller);
        assert(path);
        assert(sig >= 0);

        /* This goes through the tasks list and kills them all. This
         * is repeated until no further processes are added to the
         * tasks list, to properly handle forking processes */

        if (!s)
                if (!(s = allocated_set = set_new(trivial_hash_func, trivial_compare_func)))
                        return -ENOMEM;

        my_pid = getpid();

        do {
                pid_t pid = 0;
                done = true;

                if ((r = cg_enumerate_processes(controller, path, &f)) < 0) {
                        if (ret >= 0 && r != -ENOENT)
                                ret = r;

                        goto finish;
                }

                while ((r = cg_read_pid(f, &pid)) > 0) {

                        if (pid == my_pid && ignore_self)
                                continue;

                        if (set_get(s, LONG_TO_PTR(pid)) == LONG_TO_PTR(pid))
                                continue;

                        /* If we haven't killed this process yet, kill
                         * it */
                        if (kill(pid, sig) < 0) {
                                if (ret >= 0 && errno != ESRCH)
                                        ret = -errno;
                        } else if (ret == 0) {

                                if (sigcont)
                                        kill(pid, SIGCONT);

                                ret = 1;
                        }

                        done = false;

                        if ((r = set_put(s, LONG_TO_PTR(pid))) < 0) {
                                if (ret >= 0)
                                        ret = r;

                                goto finish;
                        }
                }

                if (r < 0) {
                        if (ret >= 0)
                                ret = r;

                        goto finish;
                }

                fclose(f);
                f = NULL;

                /* To avoid racing against processes which fork
                 * quicker than we can kill them we repeat this until
                 * no new pids need to be killed. */

        } while (!done);

finish:
        if (allocated_set)
                set_free(allocated_set);

        if (f)
                fclose(f);

        return ret;
}

int cg_kill_recursive(const char *controller, const char *path, int sig, bool sigcont, bool ignore_self, bool rem, Set *s) {
        int r, ret = 0;
        DIR *d = NULL;
        char *fn;
        Set *allocated_set = NULL;

        assert(path);
        assert(controller);
        assert(sig >= 0);

        if (!s)
                if (!(s = allocated_set = set_new(trivial_hash_func, trivial_compare_func)))
                        return -ENOMEM;

        ret = cg_kill(controller, path, sig, sigcont, ignore_self, s);

        if ((r = cg_enumerate_subgroups(controller, path, &d)) < 0) {
                if (ret >= 0 && r != -ENOENT)
                        ret = r;

                goto finish;
        }

        while ((r = cg_read_subgroup(d, &fn)) > 0) {
                char *p = NULL;

                r = asprintf(&p, "%s/%s", path, fn);
                free(fn);

                if (r < 0) {
                        if (ret >= 0)
                                ret = -ENOMEM;

                        goto finish;
                }

                r = cg_kill_recursive(controller, p, sig, sigcont, ignore_self, rem, s);
                free(p);

                if (r != 0 && ret >= 0)
                        ret = r;
        }

        if (r < 0 && ret >= 0)
                ret = r;

        if (rem)
                if ((r = cg_rmdir(controller, path, true)) < 0) {
                        if (ret >= 0 &&
                            r != -ENOENT &&
                            r != -EBUSY)
                                ret = r;
                }

finish:
        if (d)
                closedir(d);

        if (allocated_set)
                set_free(allocated_set);

        return ret;
}

int cg_kill_recursive_and_wait(const char *controller, const char *path, bool rem) {
        unsigned i;

        assert(path);
        assert(controller);

        /* This safely kills all processes; first it sends a SIGTERM,
         * then checks 8 times after 200ms whether the group is now
         * empty, then kills everything that is left with SIGKILL and
         * finally checks 5 times after 200ms each whether the group
         * is finally empty. */

        for (i = 0; i < 15; i++) {
                int sig, r;

                if (i <= 0)
                        sig = SIGTERM;
                else if (i == 9)
                        sig = SIGKILL;
                else
                        sig = 0;

                if ((r = cg_kill_recursive(controller, path, sig, true, true, rem, NULL)) <= 0)
                        return r;

                usleep(200 * USEC_PER_MSEC);
        }

        return 0;
}

int cg_migrate(const char *cfrom, const char *pfrom, const char *cto, const char *pto, bool ignore_self) {
        bool done = false;
        _cleanup_set_free_ Set *s = NULL;
        int r, ret = 0;
        pid_t my_pid;
        _cleanup_fclose_ FILE *f = NULL;

        assert(cfrom);
        assert(pfrom);
        assert(cto);
        assert(pto);

        s = set_new(trivial_hash_func, trivial_compare_func);
        if (!s)
                return -ENOMEM;

        my_pid = getpid();

        do {
                pid_t pid = 0;
                done = true;

                r = cg_enumerate_tasks(cfrom, pfrom, &f);
                if (r < 0) {
                        if (ret >= 0 && r != -ENOENT)
                                ret = r;

                        return ret;
                }

                while ((r = cg_read_pid(f, &pid)) > 0) {

                        /* This might do weird stuff if we aren't a
                         * single-threaded program. However, we
                         * luckily know we are not */
                        if (pid == my_pid && ignore_self)
                                continue;

                        if (set_get(s, LONG_TO_PTR(pid)) == LONG_TO_PTR(pid))
                                continue;

                        r = cg_attach(cto, pto, pid);
                        if (r < 0) {
                                if (ret >= 0 && r != -ESRCH)
                                        ret = r;
                        } else if (ret == 0)
                                ret = 1;

                        done = false;

                        r = set_put(s, LONG_TO_PTR(pid));
                        if (r < 0) {
                                if (ret >= 0)
                                        ret = r;

                                return ret;
                        }
                }

                if (r < 0) {
                        if (ret >= 0)
                                ret = r;

                        return ret;
                }

                fclose(f);
                f = NULL;
        } while (!done);

        return ret;
}

int cg_migrate_recursive(const char *cfrom, const char *pfrom, const char *cto, const char *pto, bool ignore_self, bool rem) {
        int r, ret = 0;
        _cleanup_closedir_ DIR *d = NULL;
        char *fn;

        assert(cfrom);
        assert(pfrom);
        assert(cto);
        assert(pto);

        ret = cg_migrate(cfrom, pfrom, cto, pto, ignore_self);

        r = cg_enumerate_subgroups(cfrom, pfrom, &d);
        if (r < 0) {
                if (ret >= 0 && r != -ENOENT)
                        ret = r;
                return ret;
        }

        while ((r = cg_read_subgroup(d, &fn)) > 0) {
                _cleanup_free_ char *p = NULL;

                p = strjoin(pfrom, "/", fn, NULL);
                free(fn);
                if (!p) {
                        if (ret >= 0)
                                ret = -ENOMEM;

                        return ret;
                }

                r = cg_migrate_recursive(cfrom, p, cto, pto, ignore_self, rem);
                if (r != 0 && ret >= 0)
                        ret = r;
        }

        if (r < 0 && ret >= 0)
                ret = r;

        if (rem) {
                r = cg_rmdir(cfrom, pfrom, true);
                if (r < 0 && ret >= 0 && r != -ENOENT && r != -EBUSY)
                        return r;
        }

        return ret;
}

static const char *normalize_controller(const char *controller) {

        if (streq(controller, SYSTEMD_CGROUP_CONTROLLER))
                return "systemd";
        else if (startswith(controller, "name="))
                return controller + 5;
        else
                return controller;
}

static int join_path(const char *controller, const char *path, const char *suffix, char **fs) {
        char *t = NULL;

        if (!(controller || path))
                return -EINVAL;

        if (controller) {
                if (path && suffix)
                        t = strjoin("/sys/fs/cgroup/", controller, "/", path, "/", suffix, NULL);
                else if (path)
                        t = strjoin("/sys/fs/cgroup/", controller, "/", path, NULL);
                else if (suffix)
                        t = strjoin("/sys/fs/cgroup/", controller, "/", suffix, NULL);
                else
                        t = strjoin("/sys/fs/cgroup/", controller, NULL);
        } else {
                if (path && suffix)
                        t = strjoin(path, "/", suffix, NULL);
                else if (path)
                        t = strdup(path);
        }

        if (!t)
                return -ENOMEM;

        path_kill_slashes(t);

        *fs = t;
        return 0;
}

int cg_get_path(const char *controller, const char *path, const char *suffix, char **fs) {
        const char *p;
        static __thread bool good = false;

        assert(fs);

        if (_unlikely_(!good)) {
                int r;

                r = path_is_mount_point("/sys/fs/cgroup", false);
                if (r <= 0)
                        return r < 0 ? r : -ENOENT;

                /* Cache this to save a few stat()s */
                good = true;
        }

        p = controller ? normalize_controller(controller) : NULL;
        return join_path(p, path, suffix, fs);
}

static int check(const char *p) {
        char *cc;

        assert(p);

        /* Check if this controller actually really exists */
        cc = alloca(sizeof("/sys/fs/cgroup/") + strlen(p));
        strcpy(stpcpy(cc, "/sys/fs/cgroup/"), p);
        if (access(cc, F_OK) < 0)
                return -errno;

        return 0;
}

int cg_get_path_and_check(const char *controller, const char *path, const char *suffix, char **fs) {
        const char *p;
        int r;

        assert(controller);
        assert(fs);

        if (isempty(controller))
                return -EINVAL;

        /* Normalize the controller syntax */
        p = normalize_controller(controller);

        /* Check if this controller actually really exists */
        r = check(p);
        if (r < 0)
                return r;

        return join_path(p, path, suffix, fs);
}

static int trim_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
        char *p;
        bool is_sticky;

        if (typeflag != FTW_DP)
                return 0;

        if (ftwbuf->level < 1)
                return 0;

        p = strappend(path, "/tasks");
        if (!p) {
                errno = ENOMEM;
                return 1;
        }

        is_sticky = file_is_priv_sticky(p) > 0;
        free(p);

        if (is_sticky)
                return 0;

        rmdir(path);
        return 0;
}

int cg_trim(const char *controller, const char *path, bool delete_root) {
        char *fs;
        int r = 0;

        assert(controller);
        assert(path);

        r = cg_get_path(controller, path, NULL, &fs);
        if (r < 0)
                return r;

        errno = 0;
        if (nftw(fs, trim_cb, 64, FTW_DEPTH|FTW_MOUNT|FTW_PHYS) < 0)
                r = errno ? -errno : -EIO;

        if (delete_root) {
                bool is_sticky;
                char *p;

                p = strappend(fs, "/tasks");
                if (!p) {
                        free(fs);
                        return -ENOMEM;
                }

                is_sticky = file_is_priv_sticky(p) > 0;
                free(p);

                if (!is_sticky)
                        if (rmdir(fs) < 0 && errno != ENOENT) {
                                if (r == 0)
                                        r = -errno;
                        }
        }

        free(fs);

        return r;
}

int cg_delete(const char *controller, const char *path) {
        char *parent;
        int r;

        assert(controller);
        assert(path);

        if ((r = path_get_parent(path, &parent)) < 0)
                return r;

        r = cg_migrate_recursive(controller, path, controller, parent, false, true);
        free(parent);

        return r == -ENOENT ? 0 : r;
}

int cg_attach(const char *controller, const char *path, pid_t pid) {
        _cleanup_free_ char *fs = NULL;
        char c[DECIMAL_STR_MAX(pid_t) + 2];
        int r;

        assert(controller);
        assert(path);
        assert(pid >= 0);

        r = cg_get_path_and_check(controller, path, "tasks", &fs);
        if (r < 0)
                return r;

        if (pid == 0)
                pid = getpid();

        snprintf(c, sizeof(c), "%lu\n", (unsigned long) pid);

        return write_string_file(fs, c);
}

int cg_set_group_access(
                const char *controller,
                const char *path,
                mode_t mode,
                uid_t uid,
                gid_t gid) {

        _cleanup_free_ char *fs = NULL;
        int r;

        assert(controller);
        assert(path);

        if (mode != (mode_t) -1)
                mode &= 0777;

        r = cg_get_path(controller, path, NULL, &fs);
        if (r < 0)
                return r;

        return chmod_and_chown(fs, mode, uid, gid);
}

int cg_set_task_access(
                const char *controller,
                const char *path,
                mode_t mode,
                uid_t uid,
                gid_t gid,
                int sticky) {

        _cleanup_free_ char *fs = NULL, *procs = NULL;
        int r;

        assert(controller);
        assert(path);

        if (mode == (mode_t) -1 && uid == (uid_t) -1 && gid == (gid_t) -1 && sticky < 0)
                return 0;

        if (mode != (mode_t) -1)
                mode &= 0666;

        r = cg_get_path(controller, path, "tasks", &fs);
        if (r < 0)
                return r;

        if (sticky >= 0 && mode != (mode_t) -1)
                /* Both mode and sticky param are passed */
                mode |= (sticky ? S_ISVTX : 0);
        else if ((sticky >= 0 && mode == (mode_t) -1) ||
                 (mode != (mode_t) -1 && sticky < 0)) {
                struct stat st;

                /* Only one param is passed, hence read the current
                 * mode from the file itself */

                r = lstat(fs, &st);
                if (r < 0)
                        return -errno;

                if (mode == (mode_t) -1)
                        /* No mode set, we just shall set the sticky bit */
                        mode = (st.st_mode & ~S_ISVTX) | (sticky ? S_ISVTX : 0);
                else
                        /* Only mode set, leave sticky bit untouched */
                        mode = (st.st_mode & ~0777) | mode;
        }

        r = chmod_and_chown(fs, mode, uid, gid);
        if (r < 0)
                return r;

        /* Always keep values for "cgroup.procs" in sync with "tasks" */
        r = cg_get_path(controller, path, "cgroup.procs", &procs);
        if (r < 0)
                return r;

        return chmod_and_chown(procs, mode, uid, gid);
}

int cg_get_by_pid(const char *controller, pid_t pid, char **path) {
        int r;
        char *p = NULL;
        FILE *f;
        char *fs;
        size_t cs;

        assert(controller);
        assert(path);
        assert(pid >= 0);

        if (pid == 0)
                pid = getpid();

        if (asprintf(&fs, "/proc/%lu/cgroup", (unsigned long) pid) < 0)
                return -ENOMEM;

        f = fopen(fs, "re");
        free(fs);

        if (!f)
                return errno == ENOENT ? -ESRCH : -errno;

        cs = strlen(controller);

        while (!feof(f)) {
                char line[LINE_MAX];
                char *l;

                errno = 0;
                if (!(fgets(line, sizeof(line), f))) {
                        if (feof(f))
                                break;

                        r = errno ? -errno : -EIO;
                        goto finish;
                }

                truncate_nl(line);

                if (!(l = strchr(line, ':')))
                        continue;

                l++;
                if (!strneq(l, controller, cs))
                        continue;

                if (l[cs] != ':')
                        continue;

                if (!(p = strdup(l + cs + 1))) {
                        r = -ENOMEM;
                        goto finish;
                }

                *path = p;
                r = 0;
                goto finish;
        }

        r = -ENOENT;

finish:
        fclose(f);

        return r;
}

int cg_install_release_agent(const char *controller, const char *agent) {
        char *fs = NULL, *contents = NULL, *line = NULL, *sc;
        int r;

        assert(controller);
        assert(agent);

        if ((r = cg_get_path(controller, NULL, "release_agent", &fs)) < 0)
                return r;

        if ((r = read_one_line_file(fs, &contents)) < 0)
                goto finish;

        sc = strstrip(contents);
        if (sc[0] == 0) {

                if (asprintf(&line, "%s\n", agent) < 0) {
                        r = -ENOMEM;
                        goto finish;
                }

                r = write_string_file(fs, line);
                if (r < 0)
                        goto finish;

        } else if (!streq(sc, agent)) {
                r = -EEXIST;
                goto finish;
        }

        free(fs);
        fs = NULL;
        if ((r = cg_get_path(controller, NULL, "notify_on_release", &fs)) < 0)
                goto finish;

        free(contents);
        contents = NULL;
        if ((r = read_one_line_file(fs, &contents)) < 0)
                goto finish;

        sc = strstrip(contents);

        if (streq(sc, "0")) {
                if ((r = write_string_file(fs, "1\n")) < 0)
                        goto finish;

                r = 1;
        } else if (!streq(sc, "1")) {
                r = -EIO;
                goto finish;
        } else
                r = 0;

finish:
        free(fs);
        free(contents);
        free(line);

        return r;
}

int cg_is_empty(const char *controller, const char *path, bool ignore_self) {
        pid_t pid = 0, self_pid;
        int r;
        FILE *f = NULL;
        bool found = false;

        assert(path);

        r = cg_enumerate_tasks(controller, path, &f);
        if (r < 0)
                return r == -ENOENT ? 1 : r;

        self_pid = getpid();

        while ((r = cg_read_pid(f, &pid)) > 0) {

                if (ignore_self && pid == self_pid)
                        continue;

                found = true;
                break;
        }

        fclose(f);

        if (r < 0)
                return r;

        return !found;
}

int cg_is_empty_by_spec(const char *spec, bool ignore_self) {
        int r;
        _cleanup_free_ char *controller = NULL, *path = NULL;

        assert(spec);

        r = cg_split_spec(spec, &controller, &path);
        if (r < 0)
                return r;

        return cg_is_empty(controller, path, ignore_self);
}

int cg_is_empty_recursive(const char *controller, const char *path, bool ignore_self) {
        int r;
        DIR *d = NULL;
        char *fn;

        assert(path);

        r = cg_is_empty(controller, path, ignore_self);
        if (r <= 0)
                return r;

        r = cg_enumerate_subgroups(controller, path, &d);
        if (r < 0)
                return r == -ENOENT ? 1 : r;

        while ((r = cg_read_subgroup(d, &fn)) > 0) {
                char *p = NULL;

                r = asprintf(&p, "%s/%s", path, fn);
                free(fn);

                if (r < 0) {
                        r = -ENOMEM;
                        goto finish;
                }

                r = cg_is_empty_recursive(controller, p, ignore_self);
                free(p);

                if (r <= 0)
                        goto finish;
        }

        if (r >= 0)
                r = 1;

finish:

        if (d)
                closedir(d);

        return r;
}

int cg_split_spec(const char *spec, char **controller, char **path) {
        const char *e;
        char *t = NULL, *u = NULL;

        assert(spec);

        if (*spec == '/') {
                if (!path_is_safe(spec))
                        return -EINVAL;

                if (path) {
                        t = strdup(spec);
                        if (!t)
                                return -ENOMEM;

                        *path = t;
                }

                if (controller)
                        *controller = NULL;

                return 0;
        }

        e = strchr(spec, ':');
        if (!e) {
                if (!filename_is_safe(spec))
                        return -EINVAL;

                if (controller) {
                        t = strdup(spec);
                        if (!t)
                                return -ENOMEM;

                        *controller = t;
                }

                if (path)
                        *path = NULL;

                return 0;
        }

        t = strndup(spec, e-spec);
        if (!t)
                return -ENOMEM;
        if (!filename_is_safe(t)) {
                free(t);
                return -EINVAL;
        }

        u = strdup(e+1);
        if (!u) {
                free(t);
                return -ENOMEM;
        }
        if (!path_is_safe(u)) {
                free(t);
                free(u);
                return -EINVAL;
        }

        if (controller)
                *controller = t;
        else
                free(t);

        if (path)
                *path = u;
        else
                free(u);

        return 0;
}

int cg_join_spec(const char *controller, const char *path, char **spec) {
        assert(controller);
        assert(path);

        if (!path_is_absolute(path) ||
            controller[0] == 0 ||
            strchr(controller, ':') ||
            strchr(controller, '/'))
                return -EINVAL;

        if (asprintf(spec, "%s:%s", controller, path) < 0)
                return -ENOMEM;

        return 0;
}

int cg_fix_path(const char *path, char **result) {
        char *t, *c, *p;
        int r;

        assert(path);
        assert(result);

        /* First check if it already is a filesystem path */
        if (path_startswith(path, "/sys/fs/cgroup") &&
            access(path, F_OK) >= 0) {

                t = strdup(path);
                if (!t)
                        return -ENOMEM;

                *result = t;
                return 0;
        }

        /* Otherwise treat it as cg spec */
        r = cg_split_spec(path, &c, &p);
        if (r < 0)
                return r;

        r = cg_get_path(c ? c : SYSTEMD_CGROUP_CONTROLLER, p ? p : "/", NULL, result);
        free(c);
        free(p);

        return r;
}

int cg_get_user_path(char **path) {
        char *root, *p;

        assert(path);

        /* Figure out the place to put user cgroups below. We use the
         * same as PID 1 has but with the "/system" suffix replaced by
         * "/user" */

        if (cg_get_by_pid(SYSTEMD_CGROUP_CONTROLLER, 1, &root) < 0)
                p = strdup("/user");
        else {
                if (endswith(root, "/system"))
                        root[strlen(root) - 7] = 0;
                else if (streq(root, "/"))
                        root[0] = 0;

                p = strappend(root, "/user");
                free(root);
        }

        if (!p)
                return -ENOMEM;

        *path = p;
        return 0;
}

char **cg_shorten_controllers(char **controllers) {
        char **f, **t;

        controllers = strv_uniq(controllers);

        if (!controllers)
                return controllers;

        for (f = controllers, t = controllers; *f; f++) {
                int r;
                const char *p;

                if (streq(*f, "systemd") || streq(*f, SYSTEMD_CGROUP_CONTROLLER)) {
                        free(*f);
                        continue;
                }

                p = normalize_controller(*f);

                r = check(p);
                if (r < 0) {
                        log_debug("Controller %s is not available, removing from controllers list.", *f);
                        free(*f);
                        continue;
                }

                *(t++) = *f;
        }

        *t = NULL;
        return controllers;
}

int cg_pid_get_cgroup(pid_t pid, char **root, char **cgroup) {
        char *cg_process, *cg_init, *p;
        int r;

        assert(pid >= 0);

        if (pid == 0)
                pid = getpid();

        r = cg_get_by_pid(SYSTEMD_CGROUP_CONTROLLER, pid, &cg_process);
        if (r < 0)
                return r;

        r = cg_get_by_pid(SYSTEMD_CGROUP_CONTROLLER, 1, &cg_init);
        if (r < 0) {
                free(cg_process);
                return r;
        }

        if (endswith(cg_init, "/system"))
                cg_init[strlen(cg_init)-7] = 0;
        else if (streq(cg_init, "/"))
                cg_init[0] = 0;

        if (startswith(cg_process, cg_init))
                p = cg_process + strlen(cg_init);
        else
                p = cg_process;

        free(cg_init);

        if (cgroup) {
                char* c;

                c = strdup(p);
                if (!c) {
                        free(cg_process);
                        return -ENOMEM;
                }

                *cgroup = c;
        }

        if (root) {
                cg_process[p-cg_process] = 0;
                *root = cg_process;
        } else
                free(cg_process);

        return 0;
}

static int instance_unit_from_cgroup(char *cgroup){
        char *at;

        assert(cgroup);

        at = strstr(cgroup, "@.");
        if (at) {
                /* This is a templated service */

                char *i;
                char _cleanup_free_ *i2 = NULL, *s = NULL;

                i = strchr(at, '/');
                if (!i || !i[1]) /* disallow empty instances */
                        return -EINVAL;

                s = strndup(at + 1, i - at - 1);
                i2 = strdup(i + 1);
                if (!s || !i2)
                        return -ENOMEM;

                strcpy(at + 1, i2);
                strcat(at + 1, s);
        }

        return 0;
}

/* non-static only for testing purposes */
int cgroup_to_unit(char *cgroup, char **unit){
        int r;
        char *p;

        assert(cgroup);
        assert(unit);

        r = instance_unit_from_cgroup(cgroup);
        if (r < 0)
                return r;

        p = strrchr(cgroup, '/');
        assert(p);

        r = unit_name_is_valid(p + 1, true);
        if (!r)
                return -EINVAL;

        *unit = strdup(p + 1);
        if (!*unit)
                return -ENOMEM;

        return 0;
}

static int cg_pid_get(const char *prefix, pid_t pid, char **unit) {
        int r;
        char _cleanup_free_ *cgroup = NULL;

        assert(pid >= 0);
        assert(unit);

        r = cg_pid_get_cgroup(pid, NULL, &cgroup);
        if (r < 0)
                return r;

        if (!startswith(cgroup, prefix))
                return -ENOENT;

        r = cgroup_to_unit(cgroup, unit);
        return r;
}

int cg_pid_get_unit(pid_t pid, char **unit) {
        return cg_pid_get("/system/", pid, unit);
}

int cg_pid_get_user_unit(pid_t pid, char **unit) {
        return cg_pid_get("/user/", pid, unit);
}

int cg_controller_from_attr(const char *attr, char **controller) {
        const char *dot;
        char *c;

        assert(attr);
        assert(controller);

        if (!filename_is_safe(attr))
                return -EINVAL;

        dot = strchr(attr, '.');
        if (!dot) {
                *controller = NULL;
                return 0;
        }

        c = strndup(attr, dot - attr);
        if (!c)
                return -ENOMEM;

        if (!filename_is_safe(c)) {
                free(c);
                return -EINVAL;
        }

        *controller = c;
        return 1;
}
