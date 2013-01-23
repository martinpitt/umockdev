/*
 * Copyright (C) 2012  ProFUSION embedded systems
 * Copyright (C) 2012  Canonical Ltd.
 * Authors:
 * Lucas De Marchi <lucas.demarchi@profusion.mobi>
 * Martin Pitt <martin.pitt@ubuntu.com>
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

/********************************
 *
 * Wrappers for accessing /sys
 *
 ********************************/

static void *nextlib;

static const char *trap_path(const char *path)
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

	if (strncmp(path, "/dev/", 5) == 0)
		check_exist = 1;
	else if (strncmp(path, "/sys/", 5) != 0)
		return path;

	path_len = strlen(path);
	prefix_len = strlen(prefix);
	if (path_len + prefix_len >= sizeof(buf)) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	strcpy(buf, prefix);
	strcpy(buf + prefix_len, path);

	if (check_exist && access(buf, F_OK) < 0)
		return path;

	return buf;
}

static void *get_libc_func(const char *f)
{
	void *fp;

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

/* wrapper template for a function with one "const char* path" argument */
#define WRAP_1ARG(rettype, failret, name) \
rettype name(const char *path) \
{ \
	const char *p;				\
	static rettype (*_fn)(const char*);	\
	_fn = get_libc_func(#name);		\
	p = trap_path(path);			\
	if (p == NULL)				\
		return failret;			\
	return (*_fn)(p);			\
}

/* wrapper template for a function with "const char* path" and another argument */
#define WRAP_2ARGS(rettype, failret, name, arg2t) \
rettype name(const char *path, arg2t arg2) \
{ \
	const char *p;					\
	static rettype (*_fn)(const char*, arg2t arg2);	\
	_fn = get_libc_func(#name);			\
	p = trap_path(path);				\
	if (p == NULL)					\
		return failret;				\
	return (*_fn)(p, arg2);				\
}

/* wrapper template for a function with "const char* path" and two other arguments */
#define WRAP_3ARGS(rettype, failret, name, arg2t, arg3t) \
rettype name(const char *path, arg2t arg2, arg3t arg3) \
{ \
	const char *p;						    \
	static rettype (*_fn)(const char*, arg2t arg2, arg3t arg3); \
	_fn = get_libc_func(#name);				    \
	p = trap_path(path);					    \
	if (p == NULL)						    \
		return failret;					    \
	return (*_fn)(p, arg2, arg3);				    \
}

/* wrapper template for __xstat family */
#define WRAP_VERSTAT(prefix, suffix) \
int prefix ## stat ## suffix (int ver, const char *path, struct stat ## suffix *st) \
{ \
	const char *p;								    \
	static int (*_fn)(int ver, const char *path, struct stat ## suffix *buf);   \
	_fn = get_libc_func(#prefix "stat" #suffix);				    \
	p = trap_path(path);							    \
        /* printf("testbed wrapped " #prefix "stat" #suffix "(%s) -> %s\n", path, p);*/	\
	if (p == NULL)								    \
		return -1;							    \
	return _fn(ver, p, st);							    \
}

/* wrapper template for open family */
#define WRAP_OPEN(prefix, suffix) \
int prefix ## open ## suffix (const char *path, int flags, ...)	    \
{ \
	const char *p;						    \
	static int (*_fn)(const char *path, int flags, ...);	    \
	_fn = get_libc_func(#prefix "open" #suffix);		    \
	p = trap_path(path);					    \
	if (p == NULL)						    \
		return -1;					    \
	if (flags & O_CREAT) {					    \
		mode_t mode;					    \
		va_list ap;					    \
		va_start(ap, flags);				    \
		mode = va_arg(ap, mode_t);			    \
		va_end(ap);					    \
		return _fn(p, flags, mode);			    \
	}							    \
	return _fn(p, flags);					    \
}

WRAP_1ARG(DIR*, NULL, opendir);

WRAP_2ARGS(FILE*, NULL, fopen, const char*);
WRAP_2ARGS(int, -1, mkdir, mode_t);
WRAP_2ARGS(int, -1, access, int);
WRAP_2ARGS(int, -1, stat, struct stat*);
WRAP_2ARGS(int, -1, stat64, struct stat64*);
WRAP_2ARGS(int, -1, lstat, struct stat*);
WRAP_2ARGS(int, -1, lstat64, struct stat64*);

WRAP_3ARGS(ssize_t, -1, readlink, char*, size_t);

WRAP_VERSTAT(__x,);
WRAP_VERSTAT(__x,64);
WRAP_VERSTAT(__lx,);
WRAP_VERSTAT(__lx,64);

WRAP_OPEN(,);
WRAP_OPEN(,64);

/********************************
 *
 * Wrappers for accessing netlink socket
 *
 ********************************/

/* keep track of the last socket fds wrapped by socket(), so that we can
 * identify them in the other functions */
static int wrapped_sockets[] = {-1, -1, -1, -1, -1, -1, -1};

/* mark fd as wrapped */
static void mark_wrapped_socket (int fd)
{
	unsigned i;
	for (i = 0; i < sizeof(wrapped_sockets); ++i) {
		if (wrapped_sockets[i] < 0) {
			wrapped_sockets[i] = fd;
			return;
		}
	}

	printf("testbed mark_wrapped_socket overflow");
	abort();
}

static void unmark_wrapped_socket (int fd)
{
	unsigned i;
	for (i = 0; i < sizeof(wrapped_sockets); ++i) {
		if (wrapped_sockets[i] == fd) {
			wrapped_sockets[i] = -1;
			return;
		}
	}

	printf("testbed unmark_wrapped_socket %i not found", fd);
	abort();
}

static int is_wrapped_socket (int fd)
{
	unsigned i;
	if (fd < 0)
		return 0;

	for (i = 0; i < sizeof(wrapped_sockets); ++i)
		if (wrapped_sockets[i] == fd)
			return 1;
	return 0;
}

int socket(int domain, int type, int protocol)
{
	static int (*_socket)(int, int, int);
	int fd;
	_socket = get_libc_func("socket");

	if (domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT) {
		fd = _socket(AF_UNIX, type, 0);
		mark_wrapped_socket(fd);
		/* printf("testbed wrapped socket: intercepting netlink, fd %i\n", fd); */
		return fd;
	}

	return _socket(domain, type, protocol);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	static int (*_bind)(int, const struct sockaddr*, socklen_t);
	struct sockaddr_un sa;
	const char *path = getenv("UMOCKDEV_DIR");
	size_t path_len;

	_bind = get_libc_func("bind");
	if (is_wrapped_socket(sockfd) && path != NULL) {
		/* printf("testbed wrapped bind: intercepting netlink socket fd %i\n", sockfd); */
		sa.sun_family = AF_UNIX;

		path_len = strlen(path);
		assert (path_len < UNIX_PATH_MAX - 7);
		strcpy(sa.sun_path, path);
		strcat(sa.sun_path, "/event");

		return _bind(sockfd, (struct sockaddr*) &sa, sizeof(sa));
	}

	return _bind(sockfd, addr, addrlen);
}

int close(int fd)
{
	static int (*_close)(int);
	_close = get_libc_func("close");
	if (is_wrapped_socket(fd)) {
		/* printf("testbed wrapped close: closing netlink socket fd %i\n", fd); */
		unmark_wrapped_socket(fd);
	}

	return _close(fd);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
	static int (*_recvmsg)(int, struct msghdr *, int);
	ssize_t ret;
	struct cmsghdr *cmsg;
        struct sockaddr_nl *sender;

	_recvmsg = get_libc_func("recvmsg");
	ret = _recvmsg(sockfd, msg, flags);

	if (is_wrapped_socket (sockfd) && ret > 0) {
		/*printf("testbed wrapped recvmsg: netlink socket fd %i, got %zi bytes\n", sockfd, ret); */

                /* fake sender to be netlink */
                sender = (struct sockaddr_nl*) msg->msg_name;
                sender->nl_family = AF_NETLINK;
                sender->nl_pid = 0;
                sender->nl_groups = 2; /* UDEV_MONITOR_UDEV */
                msg->msg_namelen = sizeof(sender);

		/* fake sender credentials to be uid 0 */
		cmsg = CMSG_FIRSTHDR(msg);
		if (cmsg != NULL) {
                        struct ucred *cred = (struct ucred *) CMSG_DATA(cmsg);
                        cred->uid = 0;
                }
	}
	return ret;
}
