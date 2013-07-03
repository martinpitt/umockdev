/*
 * Copyright (C) 2012-2013 Canonical Ltd.
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

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <linux/un.h>
#include <linux/netlink.h>
#include <unistd.h>

#include "ioctl_tree.h"

#ifdef DEBUG
#    define DBG(...) fprintf(stderr, __VA_ARGS__)
#    define IFDBG(x) x
#else
#    define DBG(...) {}
#    define IFDBG(x) {}
#endif

/* global state for recording ioctls */
int ioctl_record_fd = -1;
FILE *ioctl_record_log;
ioctl_tree *ioctl_record;

/********************************
 *
 * Utility functions
 *
 ********************************/

static void *
get_libc_func(const char *f)
{
    void *fp;
    static void *nextlib;

    if (nextlib == NULL) {
#ifdef RTLD_NEXT
	nextlib = RTLD_NEXT;
#else
	nextlib = dlopen("libc.so.6", RTLD_LAZY);
#endif
    }

    fp = dlsym(nextlib, f);
    assert(fp);

    return fp;
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
static fd_map wrapped_sockets;

int
socket(int domain, int type, int protocol)
{
    static int (*_socket) (int, int, int);
    int fd;
    _socket = get_libc_func("socket");

    if (domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT) {
	fd = _socket(AF_UNIX, type, 0);
	fd_map_add(&wrapped_sockets, fd, NULL);
	DBG("testbed wrapped socket: intercepting netlink, fd %i\n", fd);
	return fd;
    }

    return _socket(domain, type, protocol);
}

int
bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    static int (*_bind) (int, const struct sockaddr *, socklen_t);
    struct sockaddr_un sa;
    const char *path = getenv("UMOCKDEV_DIR");

    _bind = get_libc_func("bind");
    if (fd_map_get(&wrapped_sockets, sockfd, NULL) && path != NULL) {
	DBG("testbed wrapped bind: intercepting netlink socket fd %i\n", sockfd);

	/* we create one socket per fd, and send emulated uevents to all of
	 * them; poor man's multicast; this can become more elegant if/when
	 * AF_UNIX multicast lands */
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/event%i", path, sockfd);
	/* clean up from previously closed fds, to avoid "already in use" error */
	unlink(sa.sun_path);
	return _bind(sockfd, (struct sockaddr *)&sa, sizeof(sa));
    }

    return _bind(sockfd, addr, addrlen);
}

ssize_t
recvmsg(int sockfd, struct msghdr * msg, int flags)
{
    static int (*_recvmsg) (int, struct msghdr *, int);
    ssize_t ret;
    struct cmsghdr *cmsg;
    struct sockaddr_nl *sender;

    _recvmsg = get_libc_func("recvmsg");
    ret = _recvmsg(sockfd, msg, flags);

    if (fd_map_get(&wrapped_sockets, sockfd, NULL) && ret > 0) {
	DBG("testbed wrapped recvmsg: netlink socket fd %i, got %zi bytes\n", sockfd, ret);

	/* fake sender to be netlink */
	sender = (struct sockaddr_nl *)msg->msg_name;
	sender->nl_family = AF_NETLINK;
	sender->nl_pid = 0;
	sender->nl_groups = 2;	/* UDEV_MONITOR_UDEV */
	msg->msg_namelen = sizeof(sender);

	/* fake sender credentials to be uid 0 */
	cmsg = CMSG_FIRSTHDR(msg);
	if (cmsg != NULL) {
	    struct ucred *cred = (struct ucred *)CMSG_DATA(cmsg);
	    cred->uid = 0;
	}
    }
    return ret;
}

/********************************
 *
 * ioctl recording
 *
 ********************************/

static void
ioctl_record_open(int fd)
{
    static dev_t record_rdev = (dev_t) - 1;
    struct stat st;
    int ret, orig_errno;

    if (fd < 0)
	return;

    /* lazily initialize record_rdev */
    if (record_rdev == (dev_t) - 1) {
	const char *dev = getenv("UMOCKDEV_IOCTL_RECORD_DEV");

	if (dev != NULL) {
	    record_rdev = (dev_t) atoi(dev);
	} else {
	    /* not recording */
	    record_rdev = 0;
	}
    }

    if (record_rdev == 0)
	return;

    /* check if the opened device is the one we want to record */
    orig_errno = errno;
    ret = fstat(fd, &st);
    errno = orig_errno;
    if (ret < 0)
	return;
    if (!(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) || st.st_rdev != record_rdev)
	return;

    /* recording is already in progress? e. g. libmtp opens the device
     * multiple times */
    /*
       if (ioctl_record_fd >= 0) {
       fprintf(stderr, "umockdev: recording for this device is already ongoing, stopping recording of previous open()\n");
       ioctl_record_close();
       }
     */

    ioctl_record_fd = fd;

    /* lazily open the record file */
    if (ioctl_record_log == NULL) {
	const char *path = getenv("UMOCKDEV_IOCTL_RECORD_FILE");
	if (path == NULL) {
	    fprintf(stderr, "umockdev: $UMOCKDEV_IOCTL_RECORD_FILE not set\n");
	    exit(1);
	}
	if (getenv("UMOCKDEV_DIR") != NULL) {
	    fprintf(stderr, "umockdev: $UMOCKDEV_DIR cannot be used while recording\n");
	    exit(1);
	}
	ioctl_record_log = fopen(path, "a+");
	if (ioctl_record_log == NULL) {
	    perror("umockdev: failed to open ioctl record file");
	    exit(1);
	}

	/* load an already existing log */
	ioctl_record = ioctl_tree_read(ioctl_record_log);
    }
}

static void
ioctl_record_close(void)
{
    /* recorded anything? */
    if (ioctl_record != NULL) {
	rewind(ioctl_record_log);
	assert(ftruncate(fileno(ioctl_record_log), 0) == 0);
	ioctl_tree_write(ioctl_record_log, ioctl_record);
	fflush(ioctl_record_log);
    }
}

static void
record_ioctl(unsigned long request, void *arg, int result)
{
    ioctl_tree *node;

    assert(ioctl_record_log != NULL);

    node = ioctl_tree_new_from_bin(request, arg, result);
    if (node == NULL)
	return;
    ioctl_tree_insert(ioctl_record, node);
    /* handle initial node */
    if (ioctl_record == NULL)
	ioctl_record = node;
}

/********************************
 *
 * ioctl emulation
 *
 ********************************/

static fd_map ioctl_wrapped_fds;

struct ioctl_fd_info {
    ioctl_tree *tree;
    ioctl_tree *last;
};

static void
ioctl_wrap_open(int fd, const char *dev_path)
{
    FILE *f;
    static char ioctl_path[PATH_MAX];
    struct ioctl_fd_info *fdinfo;

    if (strncmp(dev_path, "/dev/", 5) != 0)
	return;

    fdinfo = malloc(sizeof(struct ioctl_fd_info));
    fdinfo->tree = NULL;
    fdinfo->last = NULL;
    fd_map_add(&ioctl_wrapped_fds, fd, fdinfo);

    /* check if we have an ioctl tree for this */
    snprintf(ioctl_path, sizeof(ioctl_path), "%s/ioctl/%s", getenv("UMOCKDEV_DIR"), dev_path);

    f = fopen(ioctl_path, "r");
    if (f == NULL)
	return;

    fdinfo->tree = ioctl_tree_read(f);
    fclose(f);
    if (fdinfo->tree == NULL) {
        fprintf(stderr, "ERROR: libumockdev-preload: failed to load ioctl record file for %s: empty or invalid format?", dev_path);
	exit(1);
    }
    DBG("ioctl_wrap_open fd %i (%s): loaded ioctl tree\n", fd, dev_path);
}

static int
ioctl_emulate(int fd, unsigned long request, void *arg)
{
    ioctl_tree *ret;
    int ioctl_result = -2;
    struct ioctl_fd_info *fdinfo;

    if (fd_map_get(&ioctl_wrapped_fds, fd, (const void **)&fdinfo)) {
	/* check our ioctl tree */
	ret = ioctl_tree_execute(fdinfo->tree, fdinfo->last, request, arg, &ioctl_result);
	if (ret != NULL)
	    fdinfo->last = ret;
    }

    /* -2 means "unhandled" */
    return ioctl_result;
}

/* note, the actual definition of ioctl is a varargs function; one cannot
 * reliably forward arbitrary varargs (http://c-faq.com/varargs/handoff.html),
 * but we know that ioctl gets at most one extra argument, and almost all of
 * them are pointers or ints, both of which fit into a void*.
 */
int ioctl(int d, unsigned long request, void *arg);
int
ioctl(int d, unsigned long request, void *arg)
{
    static int (*_fn) (int, unsigned long, void *);
    int result;

    result = ioctl_emulate(d, request, arg);
    if (result != -2) {
	DBG("ioctl fd %i request %lX: emulated, result %i\n", d, request, result);
	return result;
    }

    /* call original ioctl */
    _fn = get_libc_func("ioctl");
    result = _fn(d, request, arg);
    DBG("ioctl fd %i request %lX: original, result %i\n", d, request, result);

    if (result != -1 && ioctl_record_fd == d)
	record_ioctl(request, arg, result);

    return result;
}

/********************************
 *
 * Wrappers for accessing files
 *
 ********************************/

static inline int
path_exists(const char *path)
{
    int orig_errno, res;

    orig_errno = errno;
    res = access(path, F_OK);
    errno = orig_errno;
    return res;
}

static const char *
trap_path(const char *path)
{
    static char buf[PATH_MAX * 2];
    const char *prefix;
    size_t path_len, prefix_len;
    int check_exist = 0;

    /* do we need to trap this path? */
    if (path == NULL)
	return path;

    prefix = getenv("UMOCKDEV_DIR");
    if (prefix == NULL)
	return path;

    if (strncmp(path, "/dev/", 5) == 0 || strcmp(path, "/dev") == 0)
	check_exist = 1;
    else if (strncmp(path, "/sys/", 5) != 0 && strcmp(path, "/sys") != 0)
	return path;

    path_len = strlen(path);
    prefix_len = strlen(prefix);
    if (path_len + prefix_len >= sizeof(buf)) {
	errno = ENAMETOOLONG;
	return NULL;
    }

    /* test bed disabled? */
    strcpy(buf, prefix);
    strcpy(buf + prefix_len, "/disabled");
    if (path_exists(buf) == 0)
	return path;

    strcpy(buf + prefix_len, path);

    if (check_exist && path_exists(buf) < 0)
	return path;

    return buf;
}

/* wrapper template for a function with one "const char* path" argument */
#define WRAP_1ARG(rettype, failret, name) \
rettype name(const char *path) \
{ \
    const char *p;			\
    static rettype (*_fn)(const char*);	\
    _fn = get_libc_func(#name);		\
    p = trap_path(path);		\
    if (p == NULL)			\
	return failret;			\
    return (*_fn)(p);			\
}

/* wrapper template for a function with "const char* path" and another argument */
#define WRAP_2ARGS(rettype, failret, name, arg2t) \
rettype name(const char *path, arg2t arg2) \
{ \
    const char *p;					\
    static rettype (*_fn)(const char*, arg2t arg2);	\
    _fn = get_libc_func(#name);				\
    p = trap_path(path);				\
    if (p == NULL)					\
	return failret;					\
    return (*_fn)(p, arg2);				\
}

/* wrapper template for a function with "const char* path" and two other arguments */
#define WRAP_3ARGS(rettype, failret, name, arg2t, arg3t) \
rettype name(const char *path, arg2t arg2, arg3t arg3) \
{ \
    const char *p;						    \
    static rettype (*_fn)(const char*, arg2t arg2, arg3t arg3);	    \
    _fn = get_libc_func(#name);					    \
    p = trap_path(path);					    \
    if (p == NULL)						    \
	return failret;						    \
    return (*_fn)(p, arg2, arg3);				    \
}

static dev_t
get_rdev (const char* nodename)
{
    static char buf[PATH_MAX];
    static char link[PATH_MAX];
    int name_offset;
    int i, major, minor, orig_errno;

    name_offset = snprintf(buf, sizeof(buf), "%s/dev/.node/", getenv("UMOCKDEV_DIR"));
    buf[sizeof(buf) - 1] = 0;

    /* append nodename and replace / with _ */
    strncpy(buf + name_offset, nodename, sizeof(buf) - name_offset - 1);
    for (i = name_offset; i < sizeof(buf); ++i)
	if (buf[i] == '/')
	    buf[i] = '_';

    /* read major:minor */
    orig_errno = errno;
    if (readlink(buf, link, sizeof(link)) < 0) {
	DBG("get_rdev %s: cannot read link %s: %m\n", nodename, buf);
	errno = orig_errno;
	return (dev_t) 0;
    }
    errno = orig_errno;
    if (sscanf(link, "%i:%i", &major, &minor) != 2) {
	DBG("get_rdev %s: cannot decode major/minor from '%s'\n", nodename, link);
	return (dev_t) 0;
    }
    DBG("get_rdev %s: got major/minor %i:%i\n", nodename, major, minor);
    return makedev(major, minor);
}

/* wrapper template for __xstat family; note that we abuse the sticky bit in
 * the emulated /dev to indicate a block device (the sticky bit has no
 * real functionality for device nodes) */
#define WRAP_VERSTAT(prefix, suffix) \
int prefix ## stat ## suffix (int ver, const char *path, struct stat ## suffix *st) \
{ \
    const char *p;								\
    static int (*_fn)(int ver, const char *path, struct stat ## suffix *buf);   \
    int ret;									\
    _fn = get_libc_func(#prefix "stat" #suffix);				\
    p = trap_path(path);							\
    if (p == NULL)								\
	return -1;								\
    DBG("testbed wrapped " #prefix "stat" #suffix "(%s) -> %s\n", path, p);	\
    ret = _fn(ver, p, st);							\
    if (ret == 0 && p != path && strncmp(path, "/dev/", 5) == 0			\
	&& S_ISREG(st->st_mode)) {						\
	st->st_mode &= ~S_IFREG;						\
	if (st->st_mode &  S_ISVTX) {						\
	    st->st_mode &= ~S_ISVTX; st->st_mode |= S_IFBLK;			\
	    DBG("  %s is an emulated block device\n", path);			\
	} else {								\
	    st->st_mode |= S_IFCHR;						\
	    DBG("  %s is an emulated char device\n", path);			\
	}									\
	st->st_rdev = get_rdev(path + 5);					\
    }										\
    return ret;									\
}

/* wrapper template for open family */
#define WRAP_OPEN(prefix, suffix) \
int prefix ## open ## suffix (const char *path, int flags, ...)	    \
{ \
    const char *p;						    \
    static int (*_fn)(const char *path, int flags, ...);	    \
    int ret;							    \
    _fn = get_libc_func(#prefix "open" #suffix);		    \
    p = trap_path(path);					    \
    if (p == NULL)						    \
	return -1;						    \
    DBG("testbed wrapped " #prefix "open" #suffix "(%s) -> %s\n", path, p); \
    if (flags & O_CREAT) {					    \
	mode_t mode;						    \
	va_list ap;						    \
	va_start(ap, flags);				    	    \
	mode = va_arg(ap, mode_t);			    	    \
	va_end(ap);					    	    \
	ret = _fn(p, flags, mode);			    	    \
    } else							    \
	ret = _fn(p, flags);					    \
    if (path != p)						    \
	ioctl_wrap_open(ret, path);				    \
    else							    \
	ioctl_record_open(ret);					    \
    return ret;						    	    \
}

WRAP_1ARG(DIR *, NULL, opendir);

WRAP_2ARGS(FILE *, NULL, fopen, const char *);
WRAP_2ARGS(FILE *, NULL, fopen64, const char *);
WRAP_2ARGS(int, -1, mkdir, mode_t);
WRAP_2ARGS(int, -1, access, int);
WRAP_2ARGS(int, -1, stat, struct stat *);
WRAP_2ARGS(int, -1, stat64, struct stat64 *);
WRAP_2ARGS(int, -1, lstat, struct stat *);
WRAP_2ARGS(int, -1, lstat64, struct stat64 *);

WRAP_3ARGS(ssize_t, -1, readlink, char *, size_t);

WRAP_VERSTAT(__x,);
WRAP_VERSTAT(__x, 64);
WRAP_VERSTAT(__lx,);
WRAP_VERSTAT(__lx, 64);

WRAP_OPEN(,);
WRAP_OPEN(, 64);

int
close(int fd)
{
    static int (*_close) (int);
    struct ioctl_fd_info *fdinfo;

    _close = get_libc_func("close");
    if (fd_map_get(&wrapped_sockets, fd, NULL)) {
	DBG("testbed wrapped close: closing netlink socket fd %i\n", fd);
	fd_map_remove(&wrapped_sockets, fd);
    }
    if (fd_map_get(&ioctl_wrapped_fds, fd, (const void **)&fdinfo)) {
	DBG("testbed wrapped close: closing ioctl socket fd %i\n", fd);
	fd_map_remove(&ioctl_wrapped_fds, fd);
	ioctl_tree_free(fdinfo->tree);
	free(fdinfo);
    }
    if (fd == ioctl_record_fd) {
	ioctl_record_close();
	ioctl_record_fd = -1;
    }

    return _close(fd);
}

/* vim: set sw=4 noet: */
