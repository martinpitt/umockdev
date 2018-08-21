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
#include <time.h>
#include <signal.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/xattr.h>
#include <linux/ioctl.h>
#include <linux/un.h>
#include <linux/netlink.h>
#include <linux/input.h>
#include <unistd.h>
#include <pthread.h>

#include "config.h"
#include "debug.h"
#include "ioctl_tree.h"


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

#define libc_func(name, rettype, ...)			\
    static rettype (*_ ## name) (__VA_ARGS__) = NULL;	\
    if (_ ## name == NULL)				\
        _ ## name = get_libc_func(#name);

/* return rdev of a file descriptor */
static dev_t
dev_of_fd(int fd)
{
    struct stat st;
    int ret, orig_errno;

    orig_errno = errno;
    ret = fstat(fd, &st);
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

#define TRAP_PATH_LOCK pthread_mutex_lock (&trap_path_lock)
#define TRAP_PATH_UNLOCK pthread_mutex_unlock (&trap_path_lock)

static size_t trap_path_prefix_len = 0;

static const char *
trap_path(const char *path)
{
    static char buf[PATH_MAX * 2];
    const char *prefix;
    size_t path_len;
    int check_exist = 0;

    /* do we need to trap this path? */
    if (path == NULL)
	return path;

    prefix = getenv("UMOCKDEV_DIR");
    if (prefix == NULL)
	return path;

    if (strncmp(path, "/dev/", 5) == 0 || strcmp(path, "/dev") == 0 || strncmp(path, "/proc/", 5) == 0)
	check_exist = 1;
    else if (strncmp(path, "/sys/", 5) != 0 && strcmp(path, "/sys") != 0)
	return path;

    path_len = strlen(path);
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

    strcpy(buf + trap_path_prefix_len, path);

    if (check_exist && path_exists(buf) < 0)
	return path;

    return buf;
}

static dev_t
get_rdev(const char *nodename)
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
	DBG(DBG_PATH, "get_rdev %s: cannot read link %s: %m\n", nodename, buf);
	errno = orig_errno;
	return (dev_t) 0;
    }
    errno = orig_errno;
    if (sscanf(link, "%i:%i", &major, &minor) != 2) {
	DBG(DBG_PATH, "get_rdev %s: cannot decode major/minor from '%s'\n", nodename, link);
	return (dev_t) 0;
    }
    DBG(DBG_PATH, "get_rdev %s: got major/minor %i:%i\n", nodename, major, minor);
    return makedev(major, minor);
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
netlink_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
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
netlink_recvmsg(int sockfd, struct msghdr * msg, int flags, ssize_t ret)
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
	    struct ucred *cred = (struct ucred *)CMSG_DATA(cmsg);
	    cred->uid = 0;
	}
    }
}


/********************************
 *
 * ioctl recording
 *
 ********************************/

/* global state for recording ioctls */
int ioctl_record_fd = -1;
FILE *ioctl_record_log;
ioctl_tree *ioctl_record;
struct sigaction orig_actint;

static void ioctl_record_sigint_handler(int signum);

static void
ioctl_record_open(int fd)
{
    libc_func(fopen, FILE*, const char *, const char*);
    static dev_t record_rdev = (dev_t) - 1;

    if (fd < 0)
	return;

    /* lazily initialize record_rdev */
    if (record_rdev == (dev_t) - 1) {
	const char *dev = getenv("UMOCKDEV_IOCTL_RECORD_DEV");

	if (dev != NULL)
	    record_rdev = parse_dev_t(dev, "$UMOCKDEV_IOCTL_RECORD_DEV", 1);
	else {
	    /* not recording */
	    record_rdev = 0;
	}
    }

    if (record_rdev == 0)
	return;

    /* check if the opened device is the one we want to record */
    if (dev_of_fd(fd) != record_rdev)
	return;

    /* recording is already in progress? */
    if (ioctl_record_fd >= 0) {
	/* libmtp opens the device multiple times, we can't do that */
	/*
       fprintf(stderr, "umockdev: recording for this device is already ongoing, stopping recording of previous open()\n");
       ioctl_record_close();
       */
       fprintf(stderr, "umockdev: WARNING: ioctl recording for this device is already ongoing on fd %i, but application opened it a second time on fd %i without closing\n", ioctl_record_fd, fd);
    }

    ioctl_record_fd = fd;

    /* lazily open the record file */
    if (ioctl_record_log == NULL) {
	const char *path = getenv("UMOCKDEV_IOCTL_RECORD_FILE");
	const char *device_path = getenv("UMOCKDEV_IOCTL_RECORD_DEVICE_PATH");
	struct sigaction act_int;

	if (path == NULL) {
	    fprintf(stderr, "umockdev: $UMOCKDEV_IOCTL_RECORD_FILE not set\n");
	    exit(1);
	}
	if (device_path == NULL) {
	    fprintf(stderr, "umockdev: $UMOCKDEV_IOCTL_RECORD_DEVICE_PATH not set\n");
	    exit(1);
	}
	if (getenv("UMOCKDEV_DIR") != NULL) {
	    fprintf(stderr, "umockdev: $UMOCKDEV_DIR cannot be used while recording\n");
	    exit(1);
	}
	ioctl_record_log = _fopen(path, "a+");
	if (ioctl_record_log == NULL) {
	    perror("umockdev: failed to open ioctl record file");
	    exit(1);
	}
	/* We record the device node for later loading without specifying
	 * the devpath in umockdev_testbed_load_ioctl.
	 */
	fseek(ioctl_record_log, 0, SEEK_END);
	if (ftell(ioctl_record_log) > 0) {
	    /* We're updating a previous log; don't write the devnode header again,
	     * but check that we're recording the same device as the previous log.
	     */
	    char *existing_device_path;
	    char c;
	    DBG(DBG_IOCTL, "ioctl_record_open: Updating existing record for path %s\n", path);
	    fseek(ioctl_record_log, 0, SEEK_SET);

	    /* Start by skipping any leading comments */
	    while ((c = fgetc(ioctl_record_log)) == '#')
		while (fgetc(ioctl_record_log) != '\n')
		    ;
	    ungetc(c, ioctl_record_log);

	    if (fscanf(ioctl_record_log, "@DEV %ms\n", &existing_device_path) == 1)
	    {
		/* We have an existing "@DEV /dev/something" directive, check it matches */
		DBG(DBG_IOCTL, "ioctl_record_open: recording %s, existing device spec in record %s\n", device_path, existing_device_path);
		if (strcmp(device_path, existing_device_path) != 0) {
		    fprintf(stderr, "umockdev: attempt to record two different devices to the same ioctl recording\n");
		    exit(1);
		}
		free(existing_device_path);
	    }

	    /* load an already existing log */
	    fseek(ioctl_record_log, 0, SEEK_SET);
	    ioctl_record = ioctl_tree_read(ioctl_record_log);
	} else {
	    /* New log, add devnode header */
	    DBG(DBG_IOCTL, "ioctl_record_open: Starting new record %s\n", path);
	    fprintf(ioctl_record_log, "@DEV %s\n", device_path);
	}

	/* ensure that we write the file also on Control-C */
	act_int.sa_handler = ioctl_record_sigint_handler;
	assert(sigemptyset(&act_int.sa_mask) == 0);
	act_int.sa_flags = 0;
	assert(sigaction(SIGINT, &act_int, &orig_actint) == 0);

	DBG(DBG_IOCTL, "ioctl_record_open: starting ioctl recording of fd %i into %s\n", fd, path);
    } else {
	DBG(DBG_IOCTL, "ioctl_record_open: ioctl recording is already ongoing, continuing on new fd %i\n", fd);
    }
}

static void
ioctl_record_close(int fd)
{
    if (fd < 0 || fd != ioctl_record_fd)
	return;

    DBG(DBG_IOCTL, "ioctl_record_close: stopping ioctl recording on fd %i\n", fd);
    ioctl_record_fd = -1;

    /* recorded anything? */
    if (ioctl_record != NULL) {
	rewind(ioctl_record_log);
	assert(ftruncate(fileno(ioctl_record_log), 0) == 0);
	fprintf(ioctl_record_log, "@DEV %s\n", getenv("UMOCKDEV_IOCTL_RECORD_DEVICE_PATH"));
	ioctl_tree_write(ioctl_record_log, ioctl_record);
	fflush(ioctl_record_log);
    }
}

static void ioctl_record_sigint_handler(int signum)
{
    DBG(DBG_IOCTL, "ioctl_record_sigint_handler: got signal %i, flushing record\n", signum);
    ioctl_record_close(ioctl_record_fd);
    assert(sigaction(SIGINT, &orig_actint, NULL) == 0);
    raise(signum);
}

static void
record_ioctl(IOCTL_REQUEST_TYPE request, void *arg, int result)
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
ioctl_emulate_open(int fd, const char *dev_path)
{
    libc_func(fopen, FILE*, const char *, const char*);
    libc_func(fclose, int, FILE *);
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

    f = _fopen(ioctl_path, "r");
    if (f == NULL)
	return;

    fdinfo->tree = ioctl_tree_read(f);
    _fclose(f);
    if (fdinfo->tree == NULL) {
	fprintf(stderr, "ERROR: libumockdev-preload: failed to load ioctl record file for %s: empty or invalid format?",
		dev_path);
	exit(1);
    }
    DBG(DBG_IOCTL, "ioctl_emulate_open fd %i (%s): loaded ioctl tree\n", fd, dev_path);
}

static void
ioctl_emulate_close(int fd)
{
    struct ioctl_fd_info *fdinfo;

    if (fd_map_get(&ioctl_wrapped_fds, fd, (const void **)&fdinfo)) {
	DBG(DBG_IOCTL, "ioctl_emulate_close: closing ioctl socket fd %i\n", fd);
	fd_map_remove(&ioctl_wrapped_fds, fd);
	ioctl_tree_free(fdinfo->tree);
	free(fdinfo);
    }
}

static int
ioctl_emulate(int fd, IOCTL_REQUEST_TYPE request, void *arg)
{
    ioctl_tree *ret;
    int ioctl_result = -1;
    int orig_errno;
    struct ioctl_fd_info *fdinfo;

    if (fd_map_get(&ioctl_wrapped_fds, fd, (const void **)&fdinfo)) {
	/* we default to erroring and an appropriate error code before
	 * tree_execute, as handlers might change errno; if they succeed, we
	 * reset errno */
	orig_errno = errno;
	/* evdev ioctls default to ENOENT; FIXME: record that instead of
	 * hardcoding, and handle in ioctl_tree */
	if (_IOC_TYPE(request) == 'E')
	    errno = ENOENT;
	else
	    errno = ENOTTY;

	/* check our ioctl tree */
	ret = ioctl_tree_execute(fdinfo->tree, fdinfo->last, request, arg, &ioctl_result);
	DBG(DBG_IOCTL, "ioctl_emulate: tree execute ret %p, result %i, errno %i (%m); orig errno: %i\n", ret, ioctl_result, errno, orig_errno);
	if (ret != NULL)
	    fdinfo->last = ret;
	if (ioctl_result != -1 && errno != 0)
	    errno = orig_errno;
    } else {
	ioctl_result = UNHANDLED;
    }

    return ioctl_result;
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
	DBG(DBG_SCRIPT, "script_start_record: Appending to existing record of format %i for path %s\n", fmt, recording_path);
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

    srinfo = malloc(sizeof(struct script_record_info));
    srinfo->log = log;
    assert(clock_gettime(CLOCK_MONOTONIC, &srinfo->time) == 0);
    srinfo->op = 0;
    srinfo->fmt = fmt;
    fd_map_add(&script_recorded_fds, fd, srinfo);
}

static void
script_record_open(int fd)
{
    dev_t fd_dev;
    const char *logname, *recording_path;
    const void* data;
    enum script_record_format fmt;

    if (!script_dev_logfile_map_inited)
	init_script_dev_logfile_map();

    /* check if the opened device is one we want to record */
    fd_dev = dev_of_fd(fd);
    if (!fd_map_get(&script_dev_logfile_map, fd_dev, (const void **)&logname)) {
	DBG(DBG_SCRIPT, "script_record_open: fd %i on device %i:%i is not recorded\n", fd, major(fd_dev), minor(fd_dev));
	return;
    }
    assert (fd_map_get(&script_dev_devpath_map, fd_dev, (const void **)&recording_path));
    assert (fd_map_get(&script_dev_format_map, fd_dev, &data));
    fmt = (enum script_record_format) data;

    DBG(DBG_SCRIPT, "script_record_open: start recording fd %i on device %i:%i into %s (format %i)\n",
	fd, major(fd_dev), minor(fd_dev), logname, fmt);
    script_start_record(fd, logname, recording_path, fmt);
}

static void
script_record_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen, int res)
{
    const char *path = getenv("UMOCKDEV_DIR");
    size_t i;

    if (addr->sa_family == AF_UNIX && res == 0 && path == NULL) {
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
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
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
    const unsigned char *cur;
    int i;

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
		assert(_fwrite(header, strlen(header), 1, srinfo->log) == 1);
	    }

	    /* escape ASCII control chars */
	    for (i = 0, cur = buf; i < size; ++i, ++cur) {
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
			(long) e->time.tv_sec, (long) e->time.tv_usec, e->type, e->code, e->value);
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
    else				    \
	r = (*_ ## name)(p);		    \
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

/* wrapper template for stat family; note that we abuse the sticky bit in
 * the emulated /dev to indicate a block device (the sticky bit has no
 * real functionality for device nodes) */
#define WRAP_STAT(prefix, suffix) \
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
    if (ret == 0 && p != path && strncmp(path, "/dev/", 5) == 0			\
	&& is_emulated_device(p, st->st_mode)) {				\
	st->st_mode &= ~S_IFREG;						\
	if (st->st_mode &  S_ISVTX) {						\
	    st->st_mode &= ~S_ISVTX; st->st_mode |= S_IFBLK;			\
	    DBG(DBG_PATH, "  %s is an emulated block device\n", path);		\
	} else {								\
	    st->st_mode |= S_IFCHR;						\
	    DBG(DBG_PATH, "  %s is an emulated char device\n", path);		\
	}									\
	st->st_rdev = get_rdev(path + 5);					\
    }										\
    return ret;									\
}

/* wrapper template for __xstat family; note that we abuse the sticky bit in
 * the emulated /dev to indicate a block device (the sticky bit has no
 * real functionality for device nodes) */
#define WRAP_VERSTAT(prefix, suffix) \
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
    if (ret == 0 && p != path && strncmp(path, "/dev/", 5) == 0			\
	&& is_emulated_device(p, st->st_mode)) {				\
	st->st_mode &= ~S_IFREG;						\
	if (st->st_mode &  S_ISVTX) {						\
	    st->st_mode &= ~S_ISVTX; st->st_mode |= S_IFBLK;			\
	    DBG(DBG_PATH, "  %s is an emulated block device\n", path);		\
	} else {								\
	    st->st_mode |= S_IFCHR;						\
	    DBG(DBG_PATH, "  %s is an emulated char device\n", path);		\
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
    if (path != p)						    \
	ioctl_emulate_open(ret, path);				    \
    else {							    \
	ioctl_record_open(ret);					    \
	script_record_open(ret);				    \
    }								    \
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
    if (path != p)						    \
	ioctl_emulate_open(ret, path);				    \
    else {							    \
	ioctl_record_open(ret);					    \
	script_record_open(ret);				    \
    }								    \
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
	if (path != p)						    \
	    ioctl_emulate_open(fd, path);			    \
	else {							    \
	    ioctl_record_open(fd);				    \
	    script_record_open(fd);				    \
	}							    \
    }								    \
    return ret;							    \
}

WRAP_1ARG(DIR *, NULL, opendir);

WRAP_FOPEN(,);
WRAP_2ARGS(int, -1, mkdir, mode_t);
WRAP_2ARGS(int, -1, chmod, mode_t);
WRAP_2ARGS(int, -1, access, int);
WRAP_STAT(,);
WRAP_STAT(l,);

#ifdef __GLIBC__
WRAP_STAT(,64);
WRAP_STAT(l,64);
WRAP_FOPEN(,64);
#endif

WRAP_3ARGS(ssize_t, -1, readlink, char *, size_t);

WRAP_4ARGS(ssize_t, -1, getxattr, const char*, void*, size_t);
WRAP_4ARGS(ssize_t, -1, lgetxattr, const char*, void*, size_t);

#ifdef __GLIBC__
WRAP_VERSTAT(__x,);
WRAP_VERSTAT(__x, 64);
WRAP_VERSTAT(__lx,);
WRAP_VERSTAT(__lx, 64);
#endif

int __open_2(const char *path, int flags);
int __open64_2(const char *path, int flags);

WRAP_OPEN(,);
WRAP_OPEN2(__,_2);

/* wrapper template for openat family; intercept opening /sys from the root dir */
#define WRAP_OPENAT(prefix, suffix) \
int prefix ## openat ## suffix (int dirfd, const char *pathname, int flags, ...)		\
{ \
    const char *p = NULL;									\
    libc_func(prefix ## openat ## suffix, int, int, const char *, int, ...);			\
    libc_func(readlink, ssize_t, const char*, char *, size_t);					\
    int trapped = 0, ret;									\
    TRAP_PATH_LOCK;										\
  \
    if (strncmp(pathname, "sys", 3) == 0 && (pathname[3] == '/' || pathname[3] == '\0')) {	\
        static char buf[PATH_MAX],link[PATH_MAX];						\
        snprintf(buf, sizeof(buf), "/proc/self/fd/%d", dirfd);					\
        if (_readlink(buf, link, sizeof(link)) == 1 && link[0] == '/') {			\
            trapped = 1;									\
            strncpy(link + 1, pathname, sizeof(link) - 2);					\
            buf[sizeof(link) - 1] = 0;								\
            p = trap_path(link);								\
        } \
    } \
  \
    if (!trapped)										\
        p = trap_path(pathname);								\
    DBG(DBG_PATH, "testbed wrapped " #prefix "openat" #suffix "(%s) -> %s\n", pathname, p);	\
    if (p == NULL) { TRAP_PATH_UNLOCK; return -1; }						\
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
    DBG(DBG_PATH, "testbed wrapped readlinkat (%s) -> %s\n", pathname, p);
    if (p == NULL)
	r = -1;
    else
	r = _readlinkat(dirfd, p, buf, bufsiz);
    TRAP_PATH_UNLOCK;
    return r;
}

WRAP_2ARGS_PATHRET(char *, NULL, realpath, char *);

char *__realpath_chk(const char *path, char *resolved, size_t size);
WRAP_3ARGS_PATHRET(char *, NULL, __realpath_chk, char *, size_t);

#ifdef __GLIBC__
WRAP_1ARG_PATHRET(char *, NULL, canonicalize_file_name);
#endif

ssize_t
read(int fd, void *buf, size_t count)
{
    libc_func(read, ssize_t, int, void *, size_t);
    ssize_t res;

    res = _read(fd, buf, count);
    script_record_op('r', fd, buf, res);
    return res;
}

ssize_t
write(int fd, const void *buf, size_t count)
{
    libc_func(write, ssize_t, int, const void *, size_t);
    ssize_t res;

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
    script_record_op('r', fileno(stream), ptr, (res == 0 && ferror(stream)) ? -1 : res * size);
    return res;
}

size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE * stream)
{
    libc_func(fwrite, size_t, const void *, size_t, size_t, FILE *);
    size_t res;

    res = _fwrite(ptr, size, nmemb, stream);
    script_record_op('w', fileno(stream), ptr, (res == 0 && ferror(stream)) ? -1 : res * size);
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

    netlink_recvmsg(sockfd, msg, flags, ret);

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

    res = netlink_bind(sockfd, addr, addrlen);
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
    script_record_connect(sockfd, addr, addrlen, res);

    return res;
}

int
close(int fd)
{
    libc_func(close, int, int);

    netlink_close(fd);
    ioctl_emulate_close(fd);
    ioctl_record_close(fd);
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
	ioctl_record_close(fd);
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

    result = ioctl_emulate(d, request, arg);
    if (result != UNHANDLED) {
	DBG(DBG_IOCTL, "ioctl fd %i request %X: emulated, result %i\n", d, (unsigned) request, result);
	return result;
    }

    /* call original ioctl */
    result = _ioctl(d, request, arg);
    DBG(DBG_IOCTL, "ioctl fd %i request %X: original, result %i\n", d, (unsigned) request, result);

    if (result != -1 && ioctl_record_fd == d)
	record_ioctl(request, arg, result);

    return result;
}

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
