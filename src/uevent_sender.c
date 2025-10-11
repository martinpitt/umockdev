/*
 * Copyright (C) 2012 Canonical Ltd.
 * Author: Martin Pitt <martin.pitt@ubuntu.com>
 *
 * umockdev is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * umockdev is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <glob.h>
#include <errno.h>
#include <err.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/un.h>
#include <unistd.h>

#include <libudev.h>

#include "utils.h"
#include "uevent_sender.h"

#define UEVENT_BUFSIZE 16384

struct _uevent_sender {
    char *rootpath;
    char socket_glob[PATH_MAX];
    struct udev *udev;
};

uevent_sender *
uevent_sender_open(const char *rootpath)
{
    uevent_sender *s;

    assert(rootpath != NULL);
    s = calloc(1, sizeof(uevent_sender));
    if (!s)
	err(EXIT_FAILURE, "uevent_sender_open: cannot allocate struct");
    s->rootpath = strdupx(rootpath);
    s->udev = udev_new();
    snprintf(s->socket_glob, sizeof(s->socket_glob), "%s/event[0-9]*", rootpath);

    return s;
}

void
uevent_sender_close(uevent_sender * sender)
{
    udev_unref(sender->udev);
    free(sender->rootpath);
    free(sender);
}

static void
sendmsg_one(struct iovec *iov, size_t iov_len, const char *path)
{
    struct sockaddr_un event_addr;
    int fd;
    int ret;

    /* create uevent socket address */
    strncpy(event_addr.sun_path, path, sizeof(event_addr.sun_path) - 1);
    event_addr.sun_family = AF_UNIX;

    /* create uevent socket */
    fd = socket(AF_UNIX, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
	err(EXIT_FAILURE, "sendmsg_one: cannot create socket");

    ret = connect(fd, (struct sockaddr *)&event_addr, sizeof(event_addr));
    if (ret < 0) {
	if (errno == ECONNREFUSED) {
	    /* client side closed its monitor underneath us, so clean up and ignore */
	    unlink(event_addr.sun_path);
	    close(fd);
	    return;
	}
	err(EXIT_FAILURE, "sendmsg_one: cannot connect to client's event socket");
    }

    const struct msghdr msg = { .msg_name = &event_addr, .msg_iov = iov, .msg_iovlen = iov_len };
    ssize_t count = sendmsg(fd, &msg, 0);
    if (count < 0) {
	if (errno == ECONNREFUSED) {
	    /* client side closed its monitor underneath us, so clean up and ignore */
	    unlink(event_addr.sun_path);
	    close(fd);
	    return;
	}
	err(EXIT_FAILURE, "uevent_sender sendmsg_one: sendmsg failed");
    }
    /* printf("passed %zi bytes to event socket %s\n", count, path); */
    close(fd);
}

static void
sendmsg_all(uevent_sender * sender, struct iovec *iov, size_t iov_len)
{
    glob_t gl;
    int res;

    /* find current listeners */
    res = glob(sender->socket_glob, GLOB_NOSORT, NULL, &gl);
    if (res == 0) {
	size_t i;
	for (i = 0; i < gl.gl_pathc; ++i)
	    sendmsg_one(iov, iov_len, gl.gl_pathv[i]);
    } else {
	/* ensure that we only fail due to that, not due to bad globs */
	if (res != GLOB_NOMATCH)
	    errx(EXIT_FAILURE, "sendmsg_all: %s glob failed with %i", sender->socket_glob, res);
    }

    globfree(&gl);
}

#define UDEV_MONITOR_MAGIC                0xfeedcafe
struct udev_monitor_netlink_header {
    /* "libudev" prefix to distinguish libudev and kernel messages */
    char prefix[8];
    /*
     * magic to protect against daemon <-> library message format mismatch
     * used in the kernel from socket filter rules; needs to be stored in network order
     */
    unsigned int magic;
    /* total length of header structure known to the sender */
    unsigned int header_size;
    /* properties string buffer */
    unsigned int properties_off;
    unsigned int properties_len;
    /*
     * hashes of primary device properties strings, to let libudev subscribers
     * use in-kernel socket filters; values need to be stored in network order
     */
    unsigned int filter_subsystem_hash;
    unsigned int filter_devtype_hash;
    unsigned int filter_tag_bloom_hi;
    unsigned int filter_tag_bloom_lo;
};

/* taken from systemd/src/basic/MurmurHash2.c */
static uint32_t
string_hash32(const char *str)
{
    /*
     *  'm' and 'r' are mixing constants generated offline.
     *  They're not really 'magic', they just happen to work well.
     */
    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    int len = strlen(str);

    /* initialize the hash to a 'random' value */
    uint32_t h = len;

    /* mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)str;

    while (len >= 4) {
	uint32_t k = 0;
        memcpy(&k, data, sizeof(uint32_t));
	k *= m;
	k ^= k >> r;
	k *= m;
	h *= m;
	h ^= k;

	data += 4;
	len -= 4;
    }

    /* handle the last few bytes of the input array */
    switch (len) {
    case 3:
	h ^= data[2] << 16;
    case 2:
	h ^= data[1] << 8;
    case 1:
	h ^= data[0];
	h *= m;
    };

    /* do a few final mixes of the hash to ensure the last few bytes are well-incorporated */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return h;
}

static size_t
append_property(char *array, size_t size, size_t offset, const char *name, const char *value)
{
    int r;
    assert(offset < size);
    r = snprintf(array + offset, size - offset, "%s%s", name, value);
    if (r < 0)
        errx(EXIT_FAILURE, "snprintf failed");
    if (r + offset >= size)
        errx(EXIT_FAILURE, "uevent_sender_send: Property buffer overflow");

    /* include the NUL terminator in the string length, as we need to keep it as a separator between keys */
    return r + 1;
}

/* this mirrors the code from systemd/src/libsystemd/sd-device/device-monitor.c,
 * device_monitor_send_device() */
void
uevent_sender_send(uevent_sender * sender, const char *devpath, const char *action, const char *properties)
{
    char buffer[UEVENT_BUFSIZE];
    size_t buffer_len = 0;
    struct iovec iov[2];
    const char *subsystem;
    const char *devtype;
    char seqnumstr[20];
    struct udev_device *device;
    struct udev_monitor_netlink_header nlh;
    static unsigned long long seqnum = 1;

    device = udev_device_new_from_syspath(sender->udev, devpath);
    if (device == NULL) {
        fprintf(stderr, "ERROR: uevent_sender_send: Failed udev_device_new_from_syspath(%s): %m\n", devpath);
        /* HACK: there's a race condition where libudev might not see the device immediately when multiple
         * threads are accessing /sys, due to TRAP_PATH_LOCK contention. Retry once. */
        usleep(100);
        device = udev_device_new_from_syspath(sender->udev, devpath);
        if (device == NULL) {
            fprintf(stderr, "ERROR: uevent_sender_send: Failed udev_device_new_from_syspath(%s) after retry: %m\n", devpath);
            return;
        }
    }

    subsystem = udev_device_get_subsystem(device);
    assert(subsystem != NULL);

    devtype = udev_device_get_devtype(device);

    /* build NUL-terminated property array */
    buffer_len += append_property(buffer, sizeof buffer, buffer_len, "ACTION=", action);
    buffer_len += append_property(buffer, sizeof buffer, buffer_len, "DEVPATH=", udev_device_get_devpath(device));
    buffer_len += append_property(buffer, sizeof buffer, buffer_len, "SUBSYSTEM=", subsystem);
    snprintf(seqnumstr, sizeof(seqnumstr), "%llu", seqnum++);
    buffer_len += append_property(buffer, sizeof buffer, buffer_len, "SEQNUM=", seqnumstr);

    /* append udevd (userland) properties, replace \n with \0 */
    /* FIXME: more sensible API */
    size_t properties_len = properties ? strlen(properties) : 0;
    if (properties_len > 0) {
        size_t prop_ofs = buffer_len;
        buffer_len += append_property(buffer, sizeof buffer, buffer_len, properties, "");
        for (size_t i = prop_ofs; i < buffer_len - 1; ++i)
            if (buffer[i] == '\n')
                buffer[i] = '\0';
        /* avoid empty property at the end from final line break */
        if (properties[strlen(properties) - 1] == '\n')
            --buffer_len;
    }

    /* add versioned header */
    memset(&nlh, 0x00, sizeof(struct udev_monitor_netlink_header));
    memcpy(nlh.prefix, "libudev", 8);
    nlh.magic = htonl(UDEV_MONITOR_MAGIC);
    nlh.header_size = sizeof(struct udev_monitor_netlink_header);

    nlh.filter_subsystem_hash = htonl(string_hash32(subsystem));
    if (devtype != NULL)
	nlh.filter_devtype_hash = htonl(string_hash32(devtype));
    iov[0].iov_base = &nlh;
    iov[0].iov_len = sizeof(struct udev_monitor_netlink_header);

    udev_device_unref(device);

    /* note, not setting nlh.filter_tag_bloom_{hi,lo} for now; if required, copy
     * from libudev */

    /* add properties list */
    nlh.properties_off = iov[0].iov_len;
    nlh.properties_len = buffer_len;
    iov[1].iov_base = buffer;
    iov[1].iov_len = buffer_len;

    /* send message */
    sendmsg_all(sender, iov, 2);
}
