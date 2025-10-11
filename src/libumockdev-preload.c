/*
 * Copyright (C) 2012-2013 Canonical Ltd.
 * Copyright (C) 2018 Martin Pitt
 * Author: Martin Pitt <martin.pitt@ubuntu.com>
 *
 * The initial code for intercepting function calls was inspired and partially
 * copied from kmod's testsuite:
 * Copyright (C) 2012 ProFUSION embedded systems
 * Lucas De Marchi <lucas.demarchi@profusion.mobi>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with umockdev; If not, see <http://www.gnu.org/licenses/>.
 */

/* for getting stat64 */
#define _GNU_SOURCE

#include <features.h>

#ifdef __GLIBC__
/* Remove gcc asm aliasing so that our interposed symbols work as expected */
#include <sys/cdefs.h>

#include <stddef.h>
extern int __REDIRECT_NTH (__ttyname_r_alias, (int __fd, char *__buf,
                                               size_t __buflen), ttyname_r);

#ifdef __REDIRECT
#undef __REDIRECT
#endif
#define __REDIRECT(name, proto, alias) name proto
#ifdef __REDIRECT_NTH
#undef __REDIRECT_NTH
#endif
#define __REDIRECT_NTH(name, proto, alias) name proto __THROW

#endif /* __GLIBC__ */

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <linux/ioctl.h>
#include <linux/un.h>
#include <linux/netlink.h>
#include <linux/input.h>
#include <linux/magic.h>
#include <unistd.h>
#include <pthread.h>

#include "config.h"
#include "debug.h"
#include "utils.h"
#include "ioctl_tree.h"

#ifdef __GLIBC__

/* __USE_TIME64_REDIRECTS was introduced in glibc 2.39.9. With older releases we need to look at
 * __USE_TIME_BITS64 instead, but the latter is always defined in 2.39.9 now. So make some guesswork
 * and define the former when appropriate. */
#if !defined(__USE_TIME64_REDIRECTS) && defined(__USE_TIME_BITS64) && __TIMESIZE == 32
#pragma message "Defining backwards compatibility shim __USE_TIME64_REDIRECTS for glibc < 2.39.9"
#define __USE_TIME64_REDIRECTS 1
#endif

/* Fixup for making a mess with __REDIRECT above */
#ifdef __USE_TIME64_REDIRECTS
#define clock_gettime __clock_gettime64
extern int clock_gettime(clockid_t clockid, struct timespec *tp);
#endif
#endif

/* fix missing O_TMPFILE on some systems */
#ifndef O_TMPFILE
#define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#endif

/********************************
 *
 * Utility functions
 *
 ********************************/

#define UNHANDLED -100

static void *
get_libc_func(const char *f)
{
    void *fp;
    static void *nextlib;

    if (nextlib == NULL)
	nextlib = dlopen("libc.so.6", RTLD_LAZY);

    fp = dlsym(nextlib, f);

    return fp;
}

#define libc_func(name, rettype, ...)			\
    static rettype (*_ ## name) (__VA_ARGS__) = NULL;	\
    if (_ ## name == NULL) {				\
        _ ## name = get_libc_func(#name);		\
        if (_ ## name == NULL) {			\
            fprintf(stderr, "umockdev: could not get libc function "#name"\n"); \
            abort();					\
        }						\
    }

/* return rdev of a file descriptor */
static dev_t
dev_of_fd(int fd)
{
    struct stat st;
    int ret, orig_errno;
    libc_func(fstat, int, int, struct stat*);

    orig_errno = errno;
    ret = _fstat(fd, &st);
    errno = orig_errno;
    if (ret < 0)
	return 0;
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
	return st.st_rdev;
    return 0;
}

static inline int
path_exists(const char *path)
{
    int orig_errno, res;
    libc_func(access, int, const char*, int);

    orig_errno = errno;
    res = _access(path, F_OK);
    errno = orig_errno;
    return res;
}

/* multi-thread locking for trap_path users */
pthread_mutex_t trap_path_lock = PTHREAD_MUTEX_INITIALIZER;
static sigset_t trap_path_sig_restore;

/* multi-thread locking for ioctls */
pthread_mutex_t ioctl_lock = PTHREAD_MUTEX_INITIALIZER;

#define TRAP_PATH_LOCK \
    do { \
        sigset_t sig_set; \
        sigfillset(&sig_set); \
        pthread_mutex_lock (&trap_path_lock); \
        pthread_sigmask(SIG_SETMASK, &sig_set, &trap_path_sig_restore); \
    } while (0)
#define TRAP_PATH_UNLOCK \
    do { \
        pthread_sigmask(SIG_SETMASK, &trap_path_sig_restore, NULL); \
        pthread_mutex_unlock (&trap_path_lock); \
    } while (0)

#define IOCTL_LOCK pthread_mutex_lock (&ioctl_lock)
#define IOCTL_UNLOCK pthread_mutex_unlock (&ioctl_lock)

static size_t trap_path_prefix_len = 0;

static const char *
trap_path(const char *path)
{
    libc_func(realpath, char *, const char *, char *);
    static char abspath_buf[PATH_MAX];
    static char buf[PATH_MAX * 2];
    const char *prefix;
    const char *abspath = NULL;
    size_t path_len;
    int check_exist = 0;

    /* do we need to trap this path? */
    if (path == NULL)
	return path;

    prefix = getenv("UMOCKDEV_DIR");
    if (prefix == NULL)
	return path;

    if (path[0] != '/') {
	int orig_errno = errno;
	abspath = _realpath(path, abspath_buf);
	errno = orig_errno;
	if (abspath)
	    DBG(DBG_PATH, "trap_path relative %s -> absolute %s\n", path, abspath);
    }
    if (!abspath)
	abspath = path;

    if (strncmp(abspath, "/dev/", 5) == 0 || strcmp(abspath, "/dev") == 0 || strncmp(abspath, "/proc/", 6) == 0)
	check_exist = 1;
    else if (strncmp(abspath, "/run/udev/data", 14) == 0)
	check_exist = 0;
    else if (strncmp(abspath, "/sys/", 5) != 0 && strcmp(abspath, "/sys") != 0)
	return path;

    path_len = strlen(abspath);
    trap_path_prefix_len = strlen(prefix);
    if (path_len + trap_path_prefix_len >= sizeof(buf)) {
	errno = ENAMETOOLONG;
	return NULL;
    }

    /* test bed disabled? */
    strcpy(buf, prefix);
    strcpy(buf + trap_path_prefix_len, "/disabled");
    if (path_exists(buf) == 0)
	return path;

    strcpy(buf + trap_path_prefix_len, abspath);

    if (check_exist && path_exists(buf) < 0)
	return path;

    return buf;
}

static bool
get_rdev_maj_min(const char *nodename, uint32_t *major, uint32_t *minor)
{
    static char buf[PATH_MAX];
    static char link[PATH_MAX];
    int name_offset;
    int orig_errno;
    libc_func(readlink, ssize_t, const char*, char*, size_t);

    name_offset = snprintf(buf, sizeof(buf), "%s/dev/.node/", getenv("UMOCKDEV_DIR"));
    buf[sizeof(buf) - 1] = 0;

    /* append nodename and replace / with _ */
    strncpy(buf + name_offset, nodename, sizeof(buf) - name_offset - 1);
    for (size_t i = name_offset; i < sizeof(buf); ++i)
	if (buf[i] == '/')
	    buf[i] = '_';

    /* read major:minor */
    orig_errno = errno;
    ssize_t link_len = _readlink(buf, link, sizeof(link));
    if (link_len < 0) {
	DBG(DBG_PATH, "get_rdev %s: cannot read link %s: %m\n", nodename, buf);
	errno = orig_errno;
	return false;
    }
    link[link_len] = '\0';
    errno = orig_errno;

    if (sscanf(link, "%u:%u", major, minor) != 2) {
	DBG(DBG_PATH, "get_rdev %s: cannot decode major/minor from '%s'\n", nodename, link);
	return false;
    }
    DBG(DBG_PATH, "get_rdev %s: got major/minor %u:%u\n", nodename, *major, *minor);
    return true;
}

static dev_t
get_rdev(const char *nodename)
{
    unsigned major, minor;

    if (get_rdev_maj_min(nodename, &major, &minor))
	return makedev(major, minor);
    else
	return (dev_t) 0;
}

static dev_t
parse_dev_t(const char *value, const char *source, int error)
{
	unsigned long minor, major;
	char *endptr;
	major = strtoul(value, &endptr, 10);
	if (endptr[0] != ':') {
	    if (error) {
		fprintf(stderr, "umockdev: $%s (%s) contains no ':'\n", source, value);
		abort();
	    } else
		return (dev_t) -1;
	}
	minor = strtoul(endptr + 1, &endptr, 10);
	if (endptr[0] != '\0') {
	    if (error) {
		fprintf(stderr, "umockdev: %s (%s) has invalid minor\n", source, value);
		abort();
	    } else
		return (dev_t) -1;
	}
	return makedev(major, minor);
}

static int
is_emulated_device(const char *path, const mode_t st_mode)
{
    libc_func(readlink, ssize_t, const char*, char*, size_t);
    int orig_errno;
    ssize_t res;
    char dest[10];		/* big enough, we are only interested in the prefix */

    /* we use symlinks to the real /dev/pty/ for mocking tty devices, those
     * should appear as char device, not as symlink; but other symlinks should
     * stay symlinks */
    if (S_ISLNK(st_mode)) {
	orig_errno = errno;
	res = _readlink(path, dest, sizeof(dest));
	errno = orig_errno;
	assert(res > 0);

	return (strncmp(dest, "/dev/", 5) == 0);
    }

    /* other file types count as emulated for now */
    return !S_ISDIR(st_mode);
}

/* dirfd helper (openat family): intercept opening /dev and /sys from the root dir */
static const char *
resolve_dirfd_path(int dirfd, const char *pathname)
{
    libc_func(readlink, ssize_t, const char*, char *, size_t);
    const char *p = NULL;
    int trapped = 0;

    if ((strncmp(pathname, "sys", 3) == 0 || strncmp(pathname, "dev", 3) == 0) &&
        (pathname[3] == '/' || pathname[3] == '\0')) {
        static char buf[PATH_MAX], link[PATH_MAX];
        snprintf(buf, sizeof(buf), "/proc/self/fd/%d", dirfd);
        if (_readlink(buf, link, sizeof(link)) == 1 && link[0] == '/') {
            trapped = 1;
            strncpy(link + 1, pathname, sizeof(link) - 2);
            link[sizeof(link) - 1] = 0;
            p = trap_path(link);
        }
    }

    if (!trapped)
        p = trap_path(pathname);

    return p;
}

static bool is_dir_or_contained(const char *path, const char *dir, const char *subdir)
{
    if (!path || !dir)
	return false;

    const ssize_t subdir_len = strlen(subdir);
    const size_t dir_len = strlen(dir);

    return (dir_len + subdir_len <= strlen(path) &&
	    strncmp(path, dir, dir_len) == 0 &&
	    strncmp(path + dir_len, subdir, subdir_len) == 0 &&
	    (path[dir_len + subdir_len] == '\0' || path[dir_len + subdir_len] == '/'));
}

static bool get_fd_path(int fd, char *path_out, size_t path_size)
{
    static char fdpath[PATH_MAX];
    libc_func(readlink, ssize_t, const char*, char *, size_t);

    snprintf(fdpath, sizeof fdpath, "/proc/self/fd/%i", fd);
    int orig_errno = errno;
    ssize_t linklen = _readlink(fdpath, path_out, path_size);
    errno = orig_errno;
    if (linklen < 0 || linklen >= (ssize_t)path_size)
        return false;
    path_out[linklen] = '\0';
    return true;
}

static bool is_fd_in_mock(int fd, const char *subdir)
{
    static char linkpath[PATH_MAX];

    if (!get_fd_path(fd, linkpath, sizeof linkpath)) {
	perror("umockdev: failed to map fd to a path");
	return false;
    }

    return is_dir_or_contained(linkpath, getenv("UMOCKDEV_DIR"), subdir);
}

/* Helper to adjust stat for known emulated devices */
static inline void
adjust_emulated_device_mode_rdev(const char *dev_path, mode_t *st_mode, dev_t *st_rdev)
{
    *st_mode &= ~S_IFREG;
    if (*st_mode & S_ISVTX) {
        *st_mode = S_IFBLK | (*st_mode & ~(S_IFMT | S_ISVTX));
        DBG(DBG_PATH, "  %s is an emulated block device\n", dev_path);
    } else {
        *st_mode = S_IFCHR | (*st_mode & ~S_IFMT);
        DBG(DBG_PATH, "  %s is an emulated char device\n", dev_path);
    }
    *st_rdev = get_rdev(dev_path + 5);
}

/********************************
 *
 * fd -> pointer map
 *
 ********************************/

#define FD_MAP_MAX 50
typedef struct {
    int set[FD_MAP_MAX];
    int fd[FD_MAP_MAX];
    const void *data[FD_MAP_MAX];
} fd_map;

static void
fd_map_add(fd_map * map, int fd, const void *data)
{
    size_t i;
    for (i = 0; i < FD_MAP_MAX; ++i) {
	if (!map->set[i]) {
	    map->set[i] = 1;
	    map->fd[i] = fd;
	    map->data[i] = data;
	    return;
	}
    }

    fprintf(stderr, "libumockdev-preload fd_map_add(): overflow");
    abort();
}

static void
fd_map_remove(fd_map * map, int fd)
{
    size_t i;
    for (i = 0; i < FD_MAP_MAX; ++i) {
	if (map->set[i] && map->fd[i] == fd) {
	    map->set[i] = 0;
	    return;
	}
    }

    fprintf(stderr, "libumockdev-preload fd_map_remove(): did not find fd %i", fd);
    abort();
}

static int
fd_map_get(fd_map * map, int fd, const void **data_out)
{
    size_t i;
    for (i = 0; i < FD_MAP_MAX; ++i) {
	if (map->set[i] && map->fd[i] == fd) {
	    if (data_out != NULL)
		*data_out = map->data[i];
	    return 1;
	}
    }

    if (data_out != NULL)
	*data_out = NULL;
    return 0;
}

/********************************
 *
 * Wrappers for accessing netlink socket
 *
 ********************************/

/* keep track of the last socket fds wrapped by socket(), so that we can
 * identify them in the other functions */
static fd_map wrapped_netlink_sockets;

static void
netlink_close(int fd)
{
    if (fd_map_get(&wrapped_netlink_sockets, fd, NULL)) {
	DBG(DBG_NETLINK, "netlink_close(): closing netlink socket fd %i\n", fd);
	fd_map_remove(&wrapped_netlink_sockets, fd);
    }
}

static int
netlink_socket(int domain, int type, int protocol)
{
    libc_func(socket, int, int, int, int);
    int fd;
    const char *path = getenv("UMOCKDEV_DIR");

    if (domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT && path != NULL) {
	fd = _socket(AF_UNIX, type, 0);
	fd_map_add(&wrapped_netlink_sockets, fd, NULL);
	DBG(DBG_NETLINK, "testbed wrapped socket: intercepting netlink, fd %i\n", fd);
	return fd;
    }

    return UNHANDLED;
}

static int
netlink_bind(int sockfd)
{
    libc_func(bind, int, int, const struct sockaddr *, socklen_t);

    struct sockaddr_un sa;
    const char *path = getenv("UMOCKDEV_DIR");

    if (fd_map_get(&wrapped_netlink_sockets, sockfd, NULL) && path != NULL) {
	DBG(DBG_NETLINK, "testbed wrapped bind: intercepting netlink socket fd %i\n", sockfd);

	/* we create one socket per fd, and send emulated uevents to all of
	 * them; poor man's multicast; this can become more elegant if/when
	 * AF_UNIX multicast lands */
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/event%i", path, sockfd);
	/* clean up from previously closed fds, to avoid "already in use" error */
	unlink(sa.sun_path);
	return _bind(sockfd, (struct sockaddr *)&sa, sizeof(sa));
    }

    return UNHANDLED;
}

static void
netlink_recvmsg(int sockfd, struct msghdr * msg, ssize_t ret)
{
    struct cmsghdr *cmsg;
    struct sockaddr_nl *sender;

    if (fd_map_get(&wrapped_netlink_sockets, sockfd, NULL) && ret > 0) {
	DBG(DBG_NETLINK, "testbed wrapped recvmsg: netlink socket fd %i, got %zi bytes\n", sockfd, ret);

	/* fake sender to be netlink */
	sender = (struct sockaddr_nl *)msg->msg_name;
	sender->nl_family = AF_NETLINK;
	sender->nl_pid = 0;
	sender->nl_groups = 2;	/* UDEV_MONITOR_UDEV */
	msg->msg_namelen = sizeof(sender);

	/* fake sender credentials to be uid 0 */
	cmsg = CMSG_FIRSTHDR(msg);
	if (cmsg != NULL) {
	    const uid_t uid0 = 0;
	    /* don't cast into a struct ucred *, as that increases alignment requirement */
	    memcpy(CMSG_DATA(cmsg) + offsetof(struct ucred, uid), &uid0, sizeof uid0);
	}
    }
}


/********************************
 *
 * ioctl emulation
 *
 ********************************/

static fd_map ioctl_wrapped_fds;

struct ioctl_fd_info {
    char *dev_path;
    int ioctl_sock;
    bool is_emulated;
    pthread_mutex_t sock_lock;
};

/* Helper to adjust fstat results for emulated devices */
static void
fstat_adjust_emulated_device(int fd, mode_t *st_mode, dev_t *st_rdev)
{
    /* Check if this fd is for an emulated device using the ioctl tracking system */
    struct ioctl_fd_info *fdinfo;
    if (fd_map_get(&ioctl_wrapped_fds, fd, (const void **)&fdinfo) && fdinfo->is_emulated) {
        DBG(DBG_PATH, "fstat(%i): adjusting emulated device %s\n", fd, fdinfo->dev_path);
        adjust_emulated_device_mode_rdev(fdinfo->dev_path, st_mode, st_rdev);
        return;
    }

        /* Check if this untracked fd points to an emulated device in the mock testbed */
    if (is_fd_in_mock(fd, "/dev")) {
        static char linkpath[PATH_MAX];

        if (get_fd_path(fd, linkpath, sizeof linkpath)) {
            /* Extract the /dev/... part from the mock path */
            const char *umockdev_dir = getenv("UMOCKDEV_DIR");
            if (umockdev_dir) {
                size_t prefix_len = strlen(umockdev_dir);
                if (strncmp(linkpath, umockdev_dir, prefix_len) == 0 &&
                    strncmp(linkpath + prefix_len, "/dev/", 5) == 0) {
                    const char *dev_path = linkpath + prefix_len;
                    if (is_emulated_device(linkpath, *st_mode)) {
                        DBG(DBG_PATH, "fstat(%i): adjusting untracked emulated device %s\n", fd, dev_path);
                        adjust_emulated_device_mode_rdev(dev_path, st_mode, st_rdev);
                    }
                }
            }
        }
    }
}

static void
ioctl_emulate_open(int fd, const char *dev_path, bool is_emulated)
{
    libc_func(socket, int, int, int, int);
    libc_func(connect, int, int, const struct sockaddr *, socklen_t);
    bool is_default = false;
    int sock;
    int ret;
    struct ioctl_fd_info *fdinfo;
    struct sockaddr_un addr;

    if (strncmp(dev_path, "/dev/", 5) != 0)
	return;

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/ioctl/%s", getenv("UMOCKDEV_DIR"), dev_path);

    if (path_exists (addr.sun_path) != 0) {
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/ioctl/_default", getenv("UMOCKDEV_DIR"));
	is_default = true;
    }

    int orig_errno = errno;
    sock = _socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
	DBG(DBG_IOCTL, "ioctl_emulate_open fd %i (%s): not emulated\n", fd, dev_path);
	errno = orig_errno;
	return;
    }

    ret = _connect(sock, (const struct sockaddr *) &addr, sizeof(addr));
    if (ret == -1) {
	if (!is_default || errno != ENOENT) {
	    fprintf(stderr, "ERROR: libumockdev-preload: Failed to connect to ioctl socket %s for %s: %m\n",
		    addr.sun_path, dev_path);
	    exit(1);
	} else {
	    errno = orig_errno;
	    return;
	}
    }

    fdinfo = mallocx(sizeof(struct ioctl_fd_info));
    fdinfo->is_emulated = is_emulated;
    fdinfo->ioctl_sock = sock;
    fdinfo->dev_path = strdupx(dev_path);
    pthread_mutex_init(&fdinfo->sock_lock, NULL);

    fd_map_add(&ioctl_wrapped_fds, fd, fdinfo);
    DBG(DBG_IOCTL, "ioctl_emulate_open fd %i (%s): connected ioctl socket\n", fd, dev_path);
    errno = orig_errno;
}

static void
ioctl_emulate_close(int fd)
{
    libc_func(close, int, int);
    struct ioctl_fd_info *fdinfo;

    if (fd_map_get(&ioctl_wrapped_fds, fd, (const void **)&fdinfo)) {
	DBG(DBG_IOCTL, "ioctl_emulate_close: closing ioctl socket fd %i\n", fd);
	fd_map_remove(&ioctl_wrapped_fds, fd);
	if (fdinfo->ioctl_sock >= 0)
	    _close(fdinfo->ioctl_sock);
	free(fdinfo->dev_path);
	pthread_mutex_destroy(&fdinfo->sock_lock);
	free(fdinfo);
    }
}

#define IOCTL_REQ_IOCTL 1
#define IOCTL_REQ_RES 2
#define IOCTL_REQ_READ 7
#define IOCTL_REQ_WRITE 8
#define IOCTL_RES_DONE 3
#define IOCTL_RES_RUN 4
#define IOCTL_RES_READ_MEM 5
#define IOCTL_RES_WRITE_MEM 6
#define IOCTL_RES_ABORT 0xff

/* Marshal everything as unsigned long */
struct ioctl_request {
    unsigned long cmd;
    unsigned long arg1;
    unsigned long arg2;
};

static int
remote_emulate(int fd, int cmd, long arg1, long arg2)
{
    libc_func(send, ssize_t, int, const void *, size_t, int);
    libc_func(recv, ssize_t, int, const void *, size_t, int);
    libc_func(ioctl, int, int, IOCTL_REQUEST_TYPE, ...);
    libc_func(read, ssize_t, int, void *, size_t);
    libc_func(write, ssize_t, int, void *, size_t);
    struct ioctl_fd_info *fdinfo;
    struct ioctl_request req;
    sigset_t sig_set, sig_restore;
    int res;

    /* Block all signals while we are talking with the remote process. */
    sigfillset(&sig_set);
    pthread_sigmask(SIG_SETMASK, &sig_set, &sig_restore);

    IOCTL_LOCK;

    if (!fd_map_get(&ioctl_wrapped_fds, fd, (const void **)&fdinfo)) {
	IOCTL_UNLOCK;

	pthread_sigmask(SIG_SETMASK, &sig_restore, NULL);
	return UNHANDLED;
    }
    IOCTL_UNLOCK;

    /* Only pass on ioctl requests for emulated devices */
    if (!fdinfo->is_emulated) {
	pthread_sigmask(SIG_SETMASK, &sig_restore, NULL);
	return UNHANDLED;
    }

    pthread_mutex_lock (&fdinfo->sock_lock);

    /* We force "unsigned int" here to prevent sign extension to long
     * which could confuse the receiving side. */
    req.cmd = cmd;
    req.arg1 = arg1;
    req.arg2 = arg2;

    res = _send(fdinfo->ioctl_sock, &req, sizeof(req), 0);
    if (res < 0)
	goto con_err;

    while (1) {
	res = _recv(fdinfo->ioctl_sock, &req, sizeof(req), 0);
	if (res < 0)
	    goto con_err;
	if (res == 0)
	    goto con_eof;

	switch (req.cmd) {
	    case IOCTL_RES_DONE:
		errno = req.arg2;

		pthread_mutex_unlock (&fdinfo->sock_lock);
		pthread_sigmask(SIG_SETMASK, &sig_restore, NULL);
		/* Force a context switch so that other threads can take the lock */
		usleep(0);
		return req.arg1;

	    case IOCTL_RES_RUN:
		if (cmd == IOCTL_REQ_IOCTL)
		    res = _ioctl(fd, arg1, arg2);
		else if (cmd == IOCTL_REQ_READ)
		    res = _read(fd, (char*) arg1, arg2);
		else if (cmd == IOCTL_REQ_WRITE)
		    res = _write(fd, (char*) arg1, arg2);
		else
		    goto con_err;

		req.cmd = IOCTL_REQ_RES;
		req.arg1 = res;
		req.arg2 = errno;

		res = _send(fdinfo->ioctl_sock, &req, sizeof(req), 0);
		if (res < 0)
		    goto con_err;

		break;

	    case IOCTL_RES_READ_MEM: {
		size_t done = 0;
		do {
		    res = _send(fdinfo->ioctl_sock, (void*) (req.arg1 + done), (size_t) req.arg2 - done, 0);
		    if (res > 0)
			done += res;
		} while (res > 0 && done < req.arg2);
		if (res < 0 && errno == EFAULT) {
		    fprintf(stderr, "ERROR: libumockdev-preload: emulation code requested invalid read from %p + %lx\n",
			    (void*) req.arg1, (unsigned long) req.arg2);
		}
		if (res < 0)
		    goto con_err;

		break;
	    }

	    case IOCTL_RES_WRITE_MEM: {
		size_t done = 0;
		do {
		    res = _recv(fdinfo->ioctl_sock, (void*) (req.arg1 + done), (size_t) req.arg2 - done, 0);
		    if (res > 0)
			done += res;
		} while (res > 0 && done < req.arg2);
		if (res < 0 && errno == EFAULT) {
		    fprintf(stderr, "ERROR: libumockdev-preload: emulation code requested invalid write to %p + %lx\n",
			    (void*) req.arg1, (unsigned long) req.arg2);
		}

		if (res < 0)
		    goto con_err;

		break;
	    }

	    case IOCTL_RES_ABORT:
		fprintf(stderr, "ERROR: libumockdev-preload: Server requested abort on device %s, exiting\n",
			fdinfo->dev_path);
		abort();

	    default:
		fprintf(stderr, "ERROR: libumockdev-preload: Error communicating with ioctl socket, unknown command: %ld (res: %d)\n",
			req.cmd, res);
		abort();
	}
    }

con_eof:
    fprintf(stderr, "ERROR: libumockdev-preload: Error communicating with ioctl socket, received EOF\n");
    pthread_sigmask(SIG_SETMASK, &sig_restore, NULL);
    abort();

con_err:
    fprintf(stderr, "ERROR: libumockdev-preload: Error communicating with ioctl socket, errno: %d\n",
	    errno);
    pthread_sigmask(SIG_SETMASK, &sig_restore, NULL);
    abort();
}

/********************************
 *
 * device/socket script recording
 *
 ********************************/

#define MAX_SCRIPT_SOCKET_LOGFILE 50

enum script_record_format {FMT_DEFAULT, FMT_EVEMU};

static fd_map script_dev_logfile_map;	/* maps a st_rdev to a log file name */
static fd_map script_dev_devpath_map;   /* maps a st_rdev to a device path */
static fd_map script_dev_format_map;   /* maps a st_rdev to a script_record_format */
static int script_dev_logfile_map_inited = 0;
const char* script_socket_logfile[2*MAX_SCRIPT_SOCKET_LOGFILE]; /* list of socket name, log file name */
size_t script_socket_logfile_len = 0;
static fd_map script_recorded_fds;

struct script_record_info {
    FILE *log;			/* output file */
    struct timespec time;	/* time of last operation */
    char op;			/* last operation: 0: none, 'r': read, 'w': write */
    enum script_record_format fmt;
};

/* read UMOCKDEV_SCRIPT_* environment variables and set up dev_logfile_map
 * according to it */
static void
init_script_dev_logfile_map(void)
{
    int i;
    dev_t dev;
    char varname[100];
    const char *devname, *logname, *format;

    script_dev_logfile_map_inited = 1;

    for (i = 0; 1; ++i) {
	snprintf(varname, sizeof(varname), "UMOCKDEV_SCRIPT_RECORD_FILE_%i", i);
	logname = getenv(varname);
	if (logname == NULL)
	    break;
	snprintf(varname, sizeof(varname), "UMOCKDEV_SCRIPT_RECORD_DEV_%i", i);
	devname = getenv(varname);
	if (devname == NULL) {
	    fprintf(stderr, "umockdev: $%s not set\n", varname);
	    exit(1);
	}
	snprintf(varname, sizeof(varname), "UMOCKDEV_SCRIPT_RECORD_FORMAT_%i", i);
	format = getenv(varname);
	if (format == NULL) {
	    fprintf(stderr, "umockdev: $%s not set\n", varname);
	    exit(1);
	}
	dev = parse_dev_t(devname, NULL, 0);
	if (dev != (dev_t) -1) {
	    /* if it's a dev_t, we should record its path */
	    const char *devpath;
	    snprintf(varname, sizeof(varname), "UMOCKDEV_SCRIPT_RECORD_DEVICE_PATH_%i", i);
	    devpath = getenv(varname);
	    if (devpath == NULL) {
		fprintf(stderr, "umockdev: $%s not set\n", varname);
		exit(1);
	    }
	    DBG(DBG_SCRIPT, "init_script_dev_logfile_map: will record script of device %i:%i into %s\n", major(dev), minor(dev),
	    logname);
	    fd_map_add(&script_dev_logfile_map, dev, logname);
	    fd_map_add(&script_dev_devpath_map, dev, devpath);

	    if (strcmp(format, "default") == 0)
		fd_map_add(&script_dev_format_map, dev, (void*) FMT_DEFAULT);
	    else if (strcmp(format, "evemu") == 0)
		fd_map_add(&script_dev_format_map, dev, (void*) FMT_EVEMU);
	    else {
		fprintf(stderr, "umockdev: unknown device script record format '%s'\n", format);
		exit(1);
	    }
	} else {
	    if (strcmp(format, "default") != 0) {
		fprintf(stderr, "umockdev: unknown socket script record format '%s'\n", format);
		exit(1);
	    }

	    /* if it's a path, then we record a socket */
	    if (script_socket_logfile_len < MAX_SCRIPT_SOCKET_LOGFILE) {
		DBG(DBG_SCRIPT, "init_script_dev_logfile_map: will record script of socket %s into %s\n", devname, logname);
		script_socket_logfile[2*script_socket_logfile_len] = devname;
		script_socket_logfile[2*script_socket_logfile_len+1] = logname;
		script_socket_logfile_len++;
	    } else {
		fprintf(stderr, "too many script sockets to record\n");
		abort();
	    }
	}
    }
}

static void
script_start_record(int fd, const char *logname, const char *recording_path, enum script_record_format fmt)
{
    FILE *log;
    libc_func(fopen, FILE*, const char *, const char*);
    struct script_record_info *srinfo;

    if (fd_map_get(&script_recorded_fds, fd, NULL)) {
	fprintf(stderr, "script_start_record: internal error: fd %i is already being recorded\n", fd);
	abort();
    }

    log = _fopen(logname, "a+");
    if (log == NULL) {
	perror("umockdev: failed to open script record file");
	exit(1);
    }

    /* if we have a previous record... */
    fseek(log, 0, SEEK_END);
    if (ftell(log) > 0) {
	DBG(DBG_SCRIPT, "script_start_record: Appending to existing record of format %i for path %s\n", fmt, recording_path ?: "NULL");
	/* ...and we're going to record the device name... */
	if (recording_path) {
	    /* ... ensure we're recording the same device... */
	    char *existing_device_path;
	    char line[1000];
	    libc_func(fgets, char *, char *, int, FILE *);

	    fseek(log, 0, SEEK_SET);
	    while (_fgets(line, sizeof(line), log)) {
		switch (fmt) {
		    case FMT_DEFAULT:
			/* Start by skipping any leading comments */
			if (line[0] == '#')
			    continue;
			if (sscanf(line, "d 0 %ms\n", &existing_device_path) == 1)
			{
			    DBG(DBG_SCRIPT, "script_start_record: recording %s, existing device spec in record %s\n", recording_path, existing_device_path);
			    /* We have an existing "d /dev/something" directive, check it matches */
			    if (strcmp(recording_path, existing_device_path) != 0) {
				fprintf(stderr, "umockdev: attempt to record two different devices to the same script recording\n");
				exit(1);
			    }
			    free(existing_device_path);
			}
			// device specification must be on the first non-comment line
			break;

		    case FMT_EVEMU:
			if (strncmp(line, "E: ", 3) == 0)
			    break;
			if (sscanf(line, "# device %ms\n", &existing_device_path) == 1) {
			    DBG(DBG_SCRIPT, "script_start_record evemu format: recording %s, existing device spec in record %s\n", recording_path, existing_device_path);
			    /* We have an existing "/dev/something" directive, check it matches */
			    if (strcmp(recording_path, existing_device_path) != 0) {
				fprintf(stderr, "umockdev: attempt to record two different devices to the same evemu recording\n");
				exit(1);
			    }
			    free(existing_device_path);
			}
			break;

		    default:
			fprintf(stderr, "umockdev: unknown script format %i\n", fmt);
			abort();
		}
	    }

	    fseek(log, 0, SEEK_END);
	}

	/* ...finally, make sure that we start a new line */
	putc('\n', log);
    } else if (recording_path) { /* this is a new record, start by recording the device path */
	DBG(DBG_SCRIPT, "script_start_record: Starting new record of format %i\n", fmt);
	switch (fmt) {
	    case FMT_DEFAULT:
		fprintf(log, "d 0 %s\n", recording_path);
		break;

	    case FMT_EVEMU:
		fprintf(log, "# EVEMU 1.2\n# device %s\n", recording_path);
		break;

	    default:
		fprintf(stderr, "umockdev: unknown script format %i\n", fmt);
		abort();
	}
    }

    srinfo = mallocx(sizeof(struct script_record_info));
    srinfo->log = log;
    if (clock_gettime(CLOCK_MONOTONIC, &srinfo->time) < 0) {
	fprintf(stderr, "libumockdev-preload: failed to clock_gettime: %m\n");
	abort();
    }
    srinfo->op = 0;
    srinfo->fmt = fmt;
    fd_map_add(&script_recorded_fds, fd, srinfo);
}

static void
script_record_open(int fd)
{
    dev_t fd_dev;
    const char *logname, *recording_path = NULL;
    const void* data = NULL;
    enum script_record_format fmt;
    int r;

    if (!script_dev_logfile_map_inited)
	init_script_dev_logfile_map();

    /* check if the opened device is one we want to record */
    fd_dev = dev_of_fd(fd);
    if (!fd_map_get(&script_dev_logfile_map, fd_dev, (const void **)&logname)) {
	DBG(DBG_SCRIPT, "script_record_open: fd %i on device %i:%i is not recorded\n", fd, major(fd_dev), minor(fd_dev));
	return;
    }
    r = fd_map_get(&script_dev_devpath_map, fd_dev, (const void **)&recording_path);
    assert(r);
    r = fd_map_get(&script_dev_format_map, fd_dev, &data);
    assert(r);
    fmt = (enum script_record_format) (long) data;

    DBG(DBG_SCRIPT, "script_record_open: start recording fd %i on device %i:%i into %s (format %i)\n",
	fd, major(fd_dev), minor(fd_dev), logname, fmt);
    script_start_record(fd, logname, recording_path, fmt);
}

static void
script_record_connect(int sockfd, const struct sockaddr *addr, int res)
{
    size_t i;

    if (addr->sa_family == AF_UNIX && res == 0) {
	const char *sock_path = ((struct sockaddr_un *) addr)->sun_path;

	/* find out where we log it to */
	if (!script_dev_logfile_map_inited)
	    init_script_dev_logfile_map();
	for (i = 0; i < script_socket_logfile_len; ++i) {
	    if (strcmp(script_socket_logfile[2*i], sock_path) == 0) {
		DBG(DBG_SCRIPT, "script_record_connect: starting recording of unix socket %s on fd %i\n", sock_path, sockfd);
		script_start_record(sockfd, script_socket_logfile[2*i+1], NULL, FMT_DEFAULT);
	    }
	}
    }
}


static void
script_record_close(int fd)
{
    libc_func(fclose, int, FILE *);
    struct script_record_info *srinfo;

    if (!fd_map_get(&script_recorded_fds, fd, (const void **)&srinfo))
	return;
    DBG(DBG_SCRIPT, "script_record_close: stop recording fd %i\n", fd);
    _fclose(srinfo->log);
    free(srinfo);
    fd_map_remove(&script_recorded_fds, fd);
}

static unsigned long
update_msec(struct timespec *tm)
{
    struct timespec now;
    long delta;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
	fprintf(stderr, "libumockdev-preload: failed to clock_gettime: %m\n");
	abort();
    }
    delta = (now.tv_sec - tm->tv_sec) * 1000 + now.tv_nsec / 1000000 - tm->tv_nsec / 1000000;
    assert(delta >= 0);
    *tm = now;

    return (unsigned long)delta;
}

static void
script_record_op(char op, int fd, const void *buf, ssize_t size)
{
    struct script_record_info *srinfo;
    unsigned long delta;
    libc_func(fwrite, size_t, const void *, size_t, size_t, FILE *);
    static char header[100];

    if (!fd_map_get(&script_recorded_fds, fd, (const void **)&srinfo))
	return;
    if (size <= 0)
	return;
    DBG(DBG_SCRIPT, "script_record_op %c: got %zi bytes on fd %i (format %i)\n", op, size, fd, srinfo->fmt);

    switch (srinfo->fmt) {
	case FMT_DEFAULT:
	    delta = update_msec(&srinfo->time);
	    DBG(DBG_SCRIPT, "  %lu ms since last operation %c\n", delta, srinfo->op);

	    /* for negligible time deltas, append to the previous stanza, otherwise
	     * create a new record */
	    if (delta >= 10 || srinfo->op != op) {
		if (srinfo->op != 0)
		    putc('\n', srinfo->log);
		snprintf(header, sizeof(header), "%c %lu ", op, delta);
		size_t r = _fwrite(header, strlen(header), 1, srinfo->log);
		assert(r == 1);
	    }

	    /* escape ASCII control chars */
	    const unsigned char *cur = buf;
	    for (ssize_t i = 0; i < size; ++i, ++cur) {
		if (*cur < 32) {
		    putc('^', srinfo->log);
		    putc(*cur + 64, srinfo->log);
		    continue;
		}
		if (*cur == '^') {
		    /* we cannot encode ^ as ^^, as we need that for 0x1E already; so
		     * take the next free code which is 0x60 */
		    putc('^', srinfo->log);
		    putc('`', srinfo->log);
		    continue;
		}
		putc(*cur, srinfo->log);
	    }
	    break;

	case FMT_EVEMU:
	    if (op != 'r') {
		fprintf(stderr, "libumockdev-preload: evemu format only supports reads from the device\n");
		abort();
	    }
	    if (size % sizeof(struct input_event) != 0) {
		fprintf(stderr, "libumockdev-preload: evemu format only supports reading input_event structs\n");
		abort();
	    }
	    const struct input_event *e = buf;
	    while (size > 0) {
		fprintf(srinfo->log, "E: %li.%06li %04"PRIX16" %04"PRIX16 " %"PRIi32"\n",
			(long) e->input_event_sec, (long) e->input_event_usec, e->type, e->code, e->value);
		size -= sizeof(struct input_event);
		e++;
	    }
	    break;

	default:
	    fprintf(stderr, "libumockdev-preload script_record_op(): unsupported format %i\n", srinfo->fmt);
	    abort();
    }

    fflush(srinfo->log);
    srinfo->op = op;
}


/********************************
 *
 * Overridden libc wrappers for pretending that the $UMOCKDEV_DIR test bed is
 * the actual system.
 *
 ********************************/

/* wrapper template for a function with one "const char* path" argument */
#define WRAP_1ARG(rettype, failret, name)   \
rettype name(const char *path)		    \
{ \
    const char *p;			    \
    libc_func(name, rettype, const char*);  \
    rettype r;				    \
    TRAP_PATH_LOCK;			    \
    p = trap_path(path);		    \
    if (p == NULL)			    \
	r = failret;			    \
    else {				    \
	DBG(DBG_PATH, "testbed wrapped " #name "(%s) -> %s\n", path, p);	\
	r = (*_ ## name)(p);		    \
    };					    \
    TRAP_PATH_UNLOCK;			    \
    return r;				    \
}

/* wrapper template for a function with one "const char* path" argument and path return */
#define WRAP_1ARG_PATHRET(rettype, failret, name)   \
rettype name(const char *path)		    \
{ \
    const char *p;			    \
    libc_func(name, rettype, const char*);  \
    rettype r;				    \
    TRAP_PATH_LOCK;			    \
    p = trap_path(path);		    \
    if (p == NULL)			    \
	r = failret;			    \
    else {				    \
	r = (*_ ## name)(p);		    \
	if (p != path && r != NULL)	    \
	    memmove(r, r + trap_path_prefix_len, strlen(r) - trap_path_prefix_len + 1); \
    };					    \
    TRAP_PATH_UNLOCK;			    \
    return r;				    \
}


/* wrapper template for a function with "const char* path" and another argument */
#define WRAP_2ARGS(rettype, failret, name, arg2t) \
rettype name(const char *path, arg2t arg2) \
{ \
    const char *p;					\
    libc_func(name, rettype, const char*, arg2t);	\
    rettype r;						\
    TRAP_PATH_LOCK;					\
    p = trap_path(path);				\
    if (p == NULL)					\
	r = failret;					\
    else						\
	r = (*_ ## name)(p, arg2);			\
    TRAP_PATH_UNLOCK;					\
    return r;						\
}

/* wrapper template for a function with "const char* path", another argument, and path return */
#define WRAP_2ARGS_PATHRET(rettype, failret, name, arg2t) \
rettype name(const char *path, arg2t arg2) \
{ \
    const char *p;					\
    libc_func(name, rettype, const char*, arg2t);	\
    rettype r;						\
    TRAP_PATH_LOCK;					\
    p = trap_path(path);				\
    if (p == NULL)					\
	r = failret;					\
    else {						\
	r = (*_ ## name)(p, arg2);			\
	if (p != path && r != NULL)			\
	    memmove(r, r + trap_path_prefix_len, strlen(r) - trap_path_prefix_len + 1); \
    };					    \
    TRAP_PATH_UNLOCK;					\
    return r;						\
}


/* wrapper template for a function with "const char* path" and two other arguments */
#define WRAP_3ARGS(rettype, failret, name, arg2t, arg3t) \
rettype name(const char *path, arg2t arg2, arg3t arg3) \
{ \
    const char *p;						\
    libc_func(name, rettype, const char*, arg2t, arg3t);	\
    rettype r;							\
    TRAP_PATH_LOCK;						\
    p = trap_path(path);					\
    if (p == NULL)						\
	r = failret;						\
    else							\
	r = (*_ ## name)(p, arg2, arg3);			\
    TRAP_PATH_UNLOCK;						\
    return r;							\
}

/* wrapper template for a function with "const char* path", two other arguments, and path return */
#define WRAP_3ARGS_PATHRET(rettype, failret, name, arg2t, arg3t) \
rettype name(const char *path, arg2t arg2, arg3t arg3) \
{ \
    const char *p;					\
    libc_func(name, rettype, const char*, arg2t, arg3t); \
    rettype r;						\
    TRAP_PATH_LOCK;					\
    p = trap_path(path);				\
    if (p == NULL)					\
	r = failret;					\
    else {						\
	r = (*_ ## name)(p, arg2, arg3);		\
	if (p != path && r != NULL)			\
	    memmove(r, r + trap_path_prefix_len, strlen(r) - trap_path_prefix_len + 1); \
    };					    \
    TRAP_PATH_UNLOCK;					\
    return r;						\
}


/* wrapper template for a function with "const char* path" and three other arguments */
#define WRAP_4ARGS(rettype, failret, name, arg2t, arg3t, arg4t) \
rettype name(const char *path, arg2t arg2, arg3t arg3, arg4t arg4) \
{ \
    const char *p;						\
    libc_func(name, rettype, const char*, arg2t, arg3t, arg4t);	\
    rettype r;							\
    TRAP_PATH_LOCK;						\
    p = trap_path(path);					\
    if (p == NULL)						\
	r = failret;						\
    else							\
	r = (*_ ## name)(p, arg2, arg3, arg4);			\
    TRAP_PATH_UNLOCK;						\
    return r;							\
}

#define STAT_ADJUST_MODE \
    if (ret == 0 && p != path && strncmp(path, "/dev/", 5) == 0 \
	    && is_emulated_device(p, st->st_mode)) \
        adjust_emulated_device_mode_rdev(path, &st->st_mode, &st->st_rdev);

/* wrapper template for stat family; note that we abuse the sticky bit in
 * the emulated /dev to indicate a block device (the sticky bit has no
 * real functionality for device nodes) */
#define WRAP_STAT(prefix, suffix) \
extern int prefix ## stat ## suffix (const char *path, \
                                     struct stat ## suffix *st); \
int prefix ## stat ## suffix (const char *path, struct stat ## suffix *st) \
{ \
    const char *p;								\
    libc_func(prefix ## stat ## suffix, int, const char*, struct stat ## suffix *); \
    int ret;									\
    TRAP_PATH_LOCK;								\
    p = trap_path(path);							\
    if (p == NULL) {								\
	TRAP_PATH_UNLOCK;							\
	return -1;								\
    }										\
    DBG(DBG_PATH, "testbed wrapped " #prefix "stat" #suffix "(%s) -> %s\n", path, p);	\
    ret = _ ## prefix ## stat ## suffix(p, st);					\
    TRAP_PATH_UNLOCK;								\
    STAT_ADJUST_MODE;                                                           \
    return ret;									\
}

/* wrapper template for fstatat family */
#define WRAP_FSTATAT(prefix, suffix) \
extern int prefix ## fstatat ## suffix (int dirfd, const char *path, \
                                        struct stat ## suffix *st, int flags); \
int prefix ## fstatat ## suffix (int dirfd, const char *path, struct stat ## suffix *st, int flags) \
{ \
    const char *p;								\
    libc_func(prefix ## fstatat ## suffix, int, int, const char*, struct stat ## suffix *, int); \
    int ret;									\
    TRAP_PATH_LOCK;								\
    p = resolve_dirfd_path(dirfd, path);					\
    if (p == NULL) {								\
	TRAP_PATH_UNLOCK;							\
	return -1;								\
    }										\
    DBG(DBG_PATH, "testbed wrapped " #prefix "fstatat" #suffix "(%s) -> %s\n", path, p); \
    ret = _ ## prefix ## fstatat ## suffix(dirfd, p, st, flags);		\
    TRAP_PATH_UNLOCK;								\
    STAT_ADJUST_MODE;                                                           \
    return ret;									\
}

/* wrapper template for __xstat family; note that we abuse the sticky bit in
 * the emulated /dev to indicate a block device (the sticky bit has no
 * real functionality for device nodes)
 * This family got deprecated/dropped in glibc 2.32.9000, but we still need
 * to keep it for a while for programs that were built against previous versions */
#define WRAP_VERSTAT(prefix, suffix) \
int prefix ## stat ## suffix (int ver, const char *path, struct stat ## suffix *st); \
int prefix ## stat ## suffix (int ver, const char *path, struct stat ## suffix *st) \
{ \
    const char *p;								\
    libc_func(prefix ## stat ## suffix, int, int, const char*, struct stat ## suffix *); \
    int ret;									\
    TRAP_PATH_LOCK;								\
    p = trap_path(path);							\
    if (p == NULL) {								\
	TRAP_PATH_UNLOCK;							\
	return -1;								\
    }										\
    DBG(DBG_PATH, "testbed wrapped " #prefix "stat" #suffix "(%s) -> %s\n", path, p);	\
    ret = _ ## prefix ## stat ## suffix(ver, p, st);				\
    TRAP_PATH_UNLOCK;								\
    STAT_ADJUST_MODE;                                                           \
    return ret;									\
}

/* wrapper template for __fxstatat family */
#define WRAP_VERFSTATAT(prefix, suffix) \
int prefix ## fxstatat ## suffix (int ver, int dirfd, const char *path, struct stat ## suffix *st, int flags) \
{ \
    const char *p;								\
    libc_func(prefix ## fxstatat ## suffix, int, int, int, const char*, struct stat ## suffix *, int); \
    int ret;									\
    TRAP_PATH_LOCK;								\
    p = resolve_dirfd_path(dirfd, path);					\
    if (p == NULL) {								\
	TRAP_PATH_UNLOCK;							\
	return -1;								\
    }										\
    DBG(DBG_PATH, "testbed wrapped " #prefix "fxstatat" #suffix "(%s) -> %s\n", path, p); \
    ret = _ ## prefix ## fxstatat ## suffix(ver, dirfd, p, st, flags);		\
    TRAP_PATH_UNLOCK;								\
    STAT_ADJUST_MODE;                                                           \
    return ret;									\
}

/* wrapper template for fstat family */
#define WRAP_FSTAT(prefix, suffix) \
extern int prefix ## fstat ## suffix (int fd, struct stat ## suffix *st); \
int prefix ## fstat ## suffix (int fd, struct stat ## suffix *st) \
{ \
    libc_func(prefix ## fstat ## suffix, int, int, struct stat ## suffix *); \
    DBG(DBG_PATH, "testbed wrapped " #prefix "fstat" #suffix "(%i)\n", fd); \
    int ret = _ ## prefix ## fstat ## suffix(fd, st); \
    if (ret == 0) { \
        fstat_adjust_emulated_device(fd, &st->st_mode, &st->st_rdev); \
    } \
    return ret; \
}
/* wrapper template for open family */
#define WRAP_OPEN(prefix, suffix) \
int prefix ## open ## suffix (const char *path, int flags, ...)	    \
{ \
    const char *p;						    \
    libc_func(prefix ## open ## suffix, int, const char*, int, ...);\
    int ret;							    \
    TRAP_PATH_LOCK;						    \
    p = trap_path(path);					    \
    if (p == NULL) {						    \
	TRAP_PATH_UNLOCK;					    \
	return -1;						    \
    }								    \
    DBG(DBG_PATH, "testbed wrapped " #prefix "open" #suffix "(%s) -> %s\n", path, p); \
    if (flags & (O_CREAT | O_TMPFILE)) {			    \
	mode_t mode;						    \
	va_list ap;						    \
	va_start(ap, flags);				    	    \
	mode = va_arg(ap, mode_t);			    	    \
	va_end(ap);					    	    \
	ret = _ ## prefix ## open ## suffix(p, flags, mode);   	    \
    } else							    \
	ret =  _ ## prefix ## open ## suffix(p, flags);		    \
    TRAP_PATH_UNLOCK;						    \
    ioctl_emulate_open(ret, path, path != p);			    \
    if (path == p)						    \
	script_record_open(ret);				    \
    return ret;							    \
}

#define WRAP_OPEN2(prefix, suffix) \
int prefix ## open ## suffix (const char *path, int flags)	    \
{ \
    const char *p;						    \
    libc_func(prefix ## open ## suffix, int, const char*, int);	    \
    int ret;							    \
    TRAP_PATH_LOCK;						    \
    p = trap_path(path);					    \
    if (p == NULL) {						    \
	TRAP_PATH_UNLOCK;					    \
	return -1;						    \
    }								    \
    DBG(DBG_PATH, "testbed wrapped " #prefix "open" #suffix "(%s) -> %s\n", path, p); \
    ret =  _ ## prefix ## open ## suffix(p, flags);		    \
    TRAP_PATH_UNLOCK;						    \
    ioctl_emulate_open(ret, path, path != p);			    \
    if (path == p)						    \
	script_record_open(ret);				    \
    return ret;						    	    \
}

/* wrapper template for fopen family */
#define WRAP_FOPEN(prefix, suffix) \
FILE* prefix ## fopen ## suffix (const char *path, const char *mode)  \
{ \
    const char *p;						    \
    libc_func(prefix ## fopen ## suffix, FILE*, const char*, const char*);\
    FILE *ret;							    \
    TRAP_PATH_LOCK;						    \
    p = trap_path(path);					    \
    if (p == NULL) {						    \
	TRAP_PATH_UNLOCK;					    \
	return NULL;						    \
    }								    \
    DBG(DBG_PATH, "testbed wrapped " #prefix "fopen" #suffix "(%s) -> %s\n", path, p); \
    ret =  _ ## prefix ## fopen ## suffix(p, mode);		    \
    TRAP_PATH_UNLOCK;						    \
    if (ret != NULL) {						    \
	int fd = fileno(ret);					    \
	ioctl_emulate_open(fd, path, path != p);		    \
	if (path == p) {					    \
	    script_record_open(fd);				    \
	}							    \
    }								    \
    return ret;							    \
}

WRAP_1ARG(DIR *, NULL, opendir);
WRAP_1ARG(int, -1, chdir);

WRAP_FOPEN(,);
WRAP_2ARGS(int, -1, mkdir, mode_t);
WRAP_2ARGS(int, -1, chmod, mode_t);
WRAP_2ARGS(int, -1, access, int);
WRAP_STAT(,);
WRAP_STAT(l,);
WRAP_FSTATAT(,);
WRAP_FSTAT(,);

#ifdef __GLIBC__
WRAP_STAT(,64);
WRAP_STAT(l,64);
WRAP_FSTATAT(,64);
WRAP_FSTAT(,64);
WRAP_FOPEN(,64);
#if defined(__USE_FILE_OFFSET64) && defined(__USE_TIME64_REDIRECTS)
#define stat64_time64 stat64
WRAP_STAT(__,64_time64);
WRAP_STAT(__l,64_time64);
WRAP_FSTAT(__,64_time64);
WRAP_FSTATAT(__,64_time64);
#endif
#endif

WRAP_3ARGS(ssize_t, -1, readlink, char *, size_t);

WRAP_4ARGS(ssize_t, -1, getxattr, const char*, void*, size_t);
WRAP_4ARGS(ssize_t, -1, lgetxattr, const char*, void*, size_t);

#ifdef __GLIBC__
WRAP_VERSTAT(__x,);
WRAP_VERSTAT(__x, 64);
WRAP_VERSTAT(__lx,);
WRAP_VERSTAT(__lx, 64);

#ifdef HAVE_FXSTATAT
WRAP_VERFSTATAT(__,);
WRAP_VERFSTATAT(__,64);
#endif

int statx(int dirfd, const char *pathname, int flags, unsigned mask, struct statx * stx)
{
    const char *p;
    libc_func(statx, int, int, const char *, int, unsigned, struct statx *);
    int r;

    TRAP_PATH_LOCK;
    p = trap_path(pathname);
    DBG(DBG_PATH, "testbed wrapped statx (%s) -> %s\n", pathname, p ?: "NULL");
    if (p == NULL)
        r = -1;
    else
        r = _statx(dirfd, p, flags, mask, stx);
    TRAP_PATH_UNLOCK;

    if (r == 0 && p != pathname && strncmp(pathname, "/dev/", 5) == 0
            && is_emulated_device(p, stx->stx_mode)) {
        /* Adjust mode using the common helper - need to handle type conversion for statx */
        mode_t adjusted_mode = stx->stx_mode;
        dev_t dummy_rdev = 0;
        adjust_emulated_device_mode_rdev(pathname, &adjusted_mode, &dummy_rdev);
        stx->stx_mode = adjusted_mode;

        /* Handle statx-specific rdev fields */
        unsigned maj, min;
        if (get_rdev_maj_min(pathname + 5, &maj, &min)) {
            stx->stx_rdev_major = maj;
            stx->stx_rdev_minor = min;
        } else {
            stx->stx_rdev_major = stx->stx_rdev_minor = 0;
        }
    }
    return r;
}

#endif /* __GLIBC__ */

#define WRAP_FSTATFS(suffix) \
int fstatfs ## suffix(int fd, struct statfs ## suffix *buf)	\
{ \
    libc_func(fstatfs ## suffix, int, int, struct statfs ## suffix *buf); \
    int r = _fstatfs ## suffix(fd, buf);			\
    if (r == 0 && is_fd_in_mock (fd, "/sys")) {			\
	DBG(DBG_PATH, "testbed wrapped fstatfs64 (%i) points into mocked /sys; adjusting f_type\n", fd); \
	buf->f_type = SYSFS_MAGIC;				\
    }								\
    return r;							\
}

WRAP_FSTATFS();
#ifdef __GLIBC__
WRAP_FSTATFS(64);
#endif

#define WRAP_STATFS(suffix) \
int statfs ## suffix(const char *path, struct statfs ## suffix *buf) {	\
    libc_func(statfs ## suffix, int, const char*, struct statfs ## suffix *buf); \
    int r;								\
    TRAP_PATH_LOCK;							\
    const char *p = trap_path(path);					\
    if (p == NULL || p == path) {					\
	r = _statfs ## suffix(path, buf);				\
	TRAP_PATH_UNLOCK;						\
	return r;							\
    } \
    DBG(DBG_PATH, "testbed wrapped statfs" #suffix "(%s) -> %s\n", path, p); \
    r = _statfs ## suffix(p, buf);					\
    TRAP_PATH_UNLOCK;							\
    if (r == 0 && is_dir_or_contained(path, "/sys", ""))		\
	buf->f_type = SYSFS_MAGIC;					\
    return r;								\
}

WRAP_STATFS();
#ifdef __GLIBC__
WRAP_STATFS(64);
#endif

int __open_2(const char *path, int flags);
int __open64_2(const char *path, int flags);

WRAP_OPEN(,);
WRAP_OPEN2(__,_2);

/* wrapper template for openat family */
#define WRAP_OPENAT(prefix, suffix) \
int prefix ## openat ## suffix (int dirfd, const char *pathname, int flags, ...)		\
{ \
    const char *p = NULL;									\
    libc_func(prefix ## openat ## suffix, int, int, const char *, int, ...);			\
    int ret;											\
    TRAP_PATH_LOCK;										\
    p = resolve_dirfd_path(dirfd, pathname);							\
    if (p == NULL) { TRAP_PATH_UNLOCK; return -1; }						\
    DBG(DBG_PATH, "testbed wrapped " #prefix "openat" #suffix "(%s) -> %s\n", pathname, p);	\
    if (flags & (O_CREAT | O_TMPFILE)) {							\
	mode_t mode;										\
	va_list ap;										\
	va_start(ap, flags);									\
	mode = va_arg(ap, mode_t);								\
	va_end(ap);										\
	ret = _ ## prefix ## openat ## suffix(dirfd, p, flags, mode);				\
    } else											\
	ret =  _ ## prefix ## openat ## suffix(dirfd, p, flags);				\
    TRAP_PATH_UNLOCK;										\
    return ret;											\
}

WRAP_OPENAT(,);

#ifdef __GLIBC__
WRAP_OPEN(, 64);
WRAP_OPEN2(__,64_2);
WRAP_OPENAT(, 64);
#endif

#if defined(__GLIBC__) && defined(HAVE_OPEN_TREE)
int
open_tree(int dfd, const char *filename, unsigned int flags)
{
    const char *p;
    libc_func(open_tree, int, int, const char *, unsigned int);
    int ret;

    TRAP_PATH_LOCK;
    p = resolve_dirfd_path(dfd, filename);
    if (p == NULL) {
        TRAP_PATH_UNLOCK;
        return -1;
    }
    DBG(DBG_PATH, "testbed wrapped open_tree(%i, %s) -> %s\n", dfd, filename, p);
    ret = _open_tree(dfd, p, flags);
    TRAP_PATH_UNLOCK;
    return ret;
}
#endif

int
inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    const char *p;
    libc_func(inotify_add_watch, int, int, const char*, uint32_t);
    int r;

    TRAP_PATH_LOCK;
    p = trap_path(path);
    if (p == NULL)
	r = -1;
    else
	r = _inotify_add_watch(fd, p, mask);
    TRAP_PATH_UNLOCK;
    return r;
}

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz)
{
    const char *p;
    libc_func(readlinkat, size_t, int, const char*, char*, size_t);
    size_t r;

    TRAP_PATH_LOCK;
    p = trap_path(pathname);
    DBG(DBG_PATH, "testbed wrapped readlinkat (%s) -> %s\n", pathname, p ?: "NULL");
    if (p == NULL)
	r = -1;
    else
	r = _readlinkat(dirfd, p, buf, bufsiz);
    TRAP_PATH_UNLOCK;
    return r;
}

/* A readlinkat fortify wrapper that is used when -D_FORTIFY_SOURCE is used. */
ssize_t __readlinkat_chk(int dirfd, const char *pathname, char *buf, size_t bufsiz, size_t buflen);
ssize_t __readlinkat_chk(int dirfd, const char *pathname, char *buf, size_t bufsiz, size_t buflen)
{
    return readlinkat(dirfd, pathname, buf, bufsiz);
}

WRAP_2ARGS_PATHRET(char *, NULL, realpath, char *);

char *__realpath_chk(const char *path, char *resolved, size_t size);
WRAP_3ARGS_PATHRET(char *, NULL, __realpath_chk, char *, size_t);

#ifdef __GLIBC__
WRAP_1ARG_PATHRET(char *, NULL, canonicalize_file_name);
#endif

char *
getcwd(char *buf, size_t size)
{
    libc_func (getcwd, char*, char*, size_t);
    const char *prefix = getenv("UMOCKDEV_DIR");
    char *r = _getcwd (buf, size);

    if (prefix != NULL && r != NULL) {
	size_t prefix_len = strlen (prefix);
	if (strncmp (r, prefix, prefix_len) == 0) {
	    DBG(DBG_PATH, "testbed wrapped getcwd: %s -> %s\n", r, r + prefix_len);
	    memmove(r, r + prefix_len, strlen(r) - prefix_len + 1); \
	}
    }
    return r;
}

char * __getcwd_chk(char *buf, size_t size, size_t buflen);
char *
__getcwd_chk(char *buf, size_t size, size_t buflen)
{
    libc_func (__getcwd_chk, char*, char*, size_t, size_t);
    const char *prefix = getenv("UMOCKDEV_DIR");
    char *r = ___getcwd_chk (buf, size, buflen);

    if (prefix != NULL && r != NULL) {
	size_t prefix_len = strlen (prefix);
	if (strncmp (r, prefix, prefix_len) == 0) {
	    DBG(DBG_PATH, "testbed wrapped __getcwd_chk: %s -> %s\n", r, r + prefix_len);
	    memmove(r, r + prefix_len, strlen(r) - prefix_len + 1); \
	}
    }
    return r;
}

ssize_t
read(int fd, void *buf, size_t count)
{
    libc_func(read, ssize_t, int, void *, size_t);
    ssize_t res;

    res = remote_emulate(fd, IOCTL_REQ_READ, (long) buf, (long) count);
    if (res != UNHANDLED) {
	DBG(DBG_IOCTL, "ioctl fd %i read of %d bytes: emulated, result %i\n", fd, (int) count, (int) res);
	return res;
    }
    res = _read(fd, buf, count);
    script_record_op('r', fd, buf, res);
    return res;
}

ssize_t
write(int fd, const void *buf, size_t count)
{
    libc_func(write, ssize_t, int, const void *, size_t);
    ssize_t res;

    res = remote_emulate(fd, IOCTL_REQ_WRITE, (long) buf, (long) count);
    if (res != UNHANDLED) {
	DBG(DBG_IOCTL, "ioctl fd %i write of %d bytes: emulated, result %i\n", fd, (int) count, (int) res);
	return res;
    }
    res = _write(fd, buf, count);
    script_record_op('w', fd, buf, res);
    return res;
}

size_t
fread(void *ptr, size_t size, size_t nmemb, FILE * stream)
{
    libc_func(fread, size_t, void *, size_t, size_t, FILE *);
    size_t res;

    res = _fread(ptr, size, nmemb, stream);
    script_record_op('r', fileno(stream), ptr, (res == 0 && ferror(stream)) ? -1 : (ssize_t) (res * size));
    return res;
}

size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE * stream)
{
    libc_func(fwrite, size_t, const void *, size_t, size_t, FILE *);
    size_t res;

    res = _fwrite(ptr, size, nmemb, stream);
    script_record_op('w', fileno(stream), ptr, (res == 0 && ferror(stream)) ? -1 : (ssize_t) (res * size));
    return res;
}

char *
fgets(char *s, int size, FILE * stream)
{
    libc_func(fgets, char *, char *, int, FILE *);
    char *res;
    int len;

    res = _fgets(s, size, stream);
    if (res != NULL) {
	len = strlen(res);
	script_record_op('r', fileno(stream), s, len);
    }
    return res;
}

ssize_t
send(int fd, const void *buf, size_t count, int flags)
{
    libc_func(send, ssize_t, int, const void *, size_t, int);
    ssize_t res;

    res = _send(fd, buf, count, flags);
    script_record_op('w', fd, buf, res);
    return res;
}

ssize_t
recv(int fd, void *buf, size_t count, int flags)
{
    libc_func(recv, ssize_t, int, void *, size_t, int);
    ssize_t res;

    res = _recv(fd, buf, count, flags);
    script_record_op('r', fd, buf, res);
    return res;
}

ssize_t
recvmsg(int sockfd, struct msghdr * msg, int flags)
{
    libc_func(recvmsg, int, int, struct msghdr *, int);
    ssize_t ret = _recvmsg(sockfd, msg, flags);

    netlink_recvmsg(sockfd, msg, ret);

    return ret;
}

extern ssize_t __recvmsg64(int sockfd, struct msghdr * msg, int flags);
ssize_t
__recvmsg64(int sockfd, struct msghdr * msg, int flags)
{
    libc_func(__recvmsg64, int, int, struct msghdr *, int);
    ssize_t ret = ___recvmsg64(sockfd, msg, flags);

    netlink_recvmsg(sockfd, msg, ret);

    return ret;
}

int
socket(int domain, int type, int protocol)
{
    libc_func(socket, int, int, int, int);
    int fd;

    fd = netlink_socket(domain, type, protocol);
    if (fd != UNHANDLED)
	return fd;

    return _socket(domain, type, protocol);
}

int
bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    libc_func(bind, int, int, const struct sockaddr *, socklen_t);
    int res;

    res = netlink_bind(sockfd);
    if (res != UNHANDLED)
	return res;

    return _bind(sockfd, addr, addrlen);
}

int
connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    libc_func(connect, int, int, const struct sockaddr *, socklen_t);
    struct sockaddr_un trapped_addr;
    int res;

    /* playback */
    if (addr->sa_family == AF_UNIX) {
	const char *sock_path = ((struct sockaddr_un *) addr)->sun_path;
	TRAP_PATH_LOCK;
	const char *p = trap_path(sock_path);

	/* could happen with ENOMEM, propagate error */
	if (!p) {
	    TRAP_PATH_UNLOCK;
	    return -1;
	}

	if (p != sock_path) {
	    DBG(DBG_NETLINK, "testbed wrapped connect: redirecting Unix socket %s to %s\n", sock_path, p);
	    trapped_addr.sun_family = AF_UNIX;
	    strncpy(trapped_addr.sun_path, p, sizeof(trapped_addr.sun_path) - 1);
	    trapped_addr.sun_path[sizeof(trapped_addr.sun_path) - 1] = '\0';
	    addr = (struct sockaddr*) &trapped_addr;
	}
	TRAP_PATH_UNLOCK;
    }

    res = _connect(sockfd, addr, addrlen);
    script_record_connect(sockfd, addr, res);

    return res;
}

int
close(int fd)
{
    libc_func(close, int, int);

    netlink_close(fd);
    ioctl_emulate_close(fd);
    script_record_close(fd);

    return _close(fd);
}

int
fclose(FILE * stream)
{
    libc_func(fclose, int, FILE *);
    int fd = fileno(stream);
    if (fd >= 0) {
	netlink_close(fd);
	ioctl_emulate_close(fd);
	script_record_close(fd);
    }

    return _fclose(stream);
}

int
ioctl(int d, IOCTL_REQUEST_TYPE request, ...)
{
    libc_func(ioctl, int, int, IOCTL_REQUEST_TYPE, ...);
    int result;
    va_list ap;
    void* arg;

    /* one cannot reliably forward arbitrary varargs
     * (http://c-faq.com/varargs/handoff.html), but we know that ioctl gets at
     * most one extra argument, and almost all of them are pointers or ints,
     * both of which fit into a void*.
     */
    va_start(ap, request);
    arg = va_arg(ap, void*);
    va_end(ap);

    result = remote_emulate(d, IOCTL_REQ_IOCTL, (unsigned int) request, (long) arg);
    if (result != UNHANDLED) {
	DBG(DBG_IOCTL, "ioctl fd %i request %X: emulated, result %i\n", d, (unsigned) request, result);
	return result;
    }

    /* fallback to call original ioctl */
    result = _ioctl(d, request, arg);
    DBG(DBG_IOCTL, "ioctl fd %i request %X: original, result %i\n", d, (unsigned) request, result);

    return result;
}

#ifdef __GLIBC__

extern int __ioctl_time64 (int __fd, unsigned long int __request, ...) __THROW;
int
__ioctl_time64(int d, IOCTL_REQUEST_TYPE request, ...)
{
    libc_func(__ioctl_time64, int, int, IOCTL_REQUEST_TYPE, ...);
    int result;
    va_list ap;
    void* arg;

    /* one cannot reliably forward arbitrary varargs
     * (http://c-faq.com/varargs/handoff.html), but we know that ioctl gets at
     * most one extra argument, and almost all of them are pointers or ints,
     * both of which fit into a void*.
     */
    va_start(ap, request);
    arg = va_arg(ap, void*);
    va_end(ap);

    result = remote_emulate(d, IOCTL_REQ_IOCTL, (unsigned int) request, (long) arg);
    if (result != UNHANDLED) {
	DBG(DBG_IOCTL, "ioctl fd %i request %X: emulated, result %i\n", d, (unsigned) request, result);
	return result;
    }

    /* fallback to call original ioctl */
    result = ___ioctl_time64(d, request, arg);
    DBG(DBG_IOCTL, "ioctl fd %i request %X: original, result %i\n", d, (unsigned) request, result);

    return result;
}

#endif /* __GLIBC__ */


int
isatty(int fd)
{
    libc_func(isatty, int, int);
    libc_func(readlink, ssize_t, const char*, char*, size_t);
    int result = _isatty(fd);
    char ttyname[1024];
    char ptymap[PATH_MAX];
    char majmin[20];
    char *cp;
    int orig_errno, r;

    if (result != 1) {
	DBG(DBG_PATH, "isatty(%i): real function result: %i, returning that\n", fd, result);
	return result;
    }

    /* isatty() succeeds for our emulated devices, but they should not
     * necessarily appear as TTY; so map the tty name to a major/minor, and
     * only return 1 if it is major 4. */
    orig_errno = errno;
    if (ttyname_r(fd, ttyname, sizeof(ttyname)) != 0) {
	DBG(DBG_PATH, "isatty(%i): is a terminal, but ttyname() failed! %m\n", fd);
	/* *shrug*, what can we do; return original result */
	goto out;
    }

    DBG(DBG_PATH, "isatty(%i): is a terminal, ttyname %s\n", fd, ttyname);
    for (cp = ttyname; *cp; ++cp)
	if (*cp == '/')
	    *cp = '_';
    snprintf(ptymap, sizeof(ptymap), "%s/dev/.ptymap/%s", getenv("UMOCKDEV_DIR"), ttyname);
    r = _readlink(ptymap, majmin, sizeof(majmin));
    if (r < 0) {
	/* failure here is normal for non-emulated devices */
	DBG(DBG_PATH, "isatty(%i): readlink(%s) failed: %m\n", fd, ptymap);
	goto out;
    }
    majmin[r] = '\0';
    if (majmin[0] != '4' || majmin[1] != ':') {
	DBG(DBG_PATH, "isatty(%i): major/minor is %s which is not a tty; returning 0\n", fd, majmin);
	result = 0;
    }

out:
    errno = orig_errno;
    return result;
}

/* vim: set sw=4 noet: */
