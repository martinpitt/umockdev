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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/un.h>

#include <libudev.h>

#include "uevent_sender.h"

struct _uevent_sender {
  char*  rootpath;
  int    event_fd; 
  struct sockaddr_un event_addr;
  struct udev *udev;
};

uevent_sender*
uevent_sender_open (const char* rootpath)
{
  uevent_sender *s;

  assert (rootpath != NULL);
  s = calloc (1, sizeof (uevent_sender));
  s->rootpath = strdup (rootpath);
  s->event_fd = -1;
  s->udev = udev_new ();

  return s;
}

void
uevent_sender_close (uevent_sender* sender)
{
  udev_unref (sender->udev);
  free (sender->rootpath);
  free (sender);
}

static int
_get_fd (uevent_sender *sender)
{
  int ret;

  if (sender->event_fd > 0)
    return sender->event_fd;

  /* create uevent socket address */
  ret = snprintf (sender->event_addr.sun_path, sizeof (sender->event_addr.sun_path),
                  "%s/event", sender->rootpath);
  assert (ret < sizeof (sender->event_addr.sun_path));
  sender->event_addr.sun_family = AF_UNIX;

  /* create uevent socket */
  sender->event_fd = socket (AF_UNIX, SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
  if (sender->event_fd < 0)
    return -1;

  ret = connect (sender->event_fd, (struct sockaddr*) &sender->event_addr,
                 sizeof (sender->event_addr));
  if (ret < 0)
    {
      perror ("ERROR: cannot connect to client's event socket");
      return -1;
    }

  return sender->event_fd;
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

/* taken from systemd/src/libudev/libudev-util.c */
static unsigned int
string_hash32(const char *str)
{
  /*
   *  'm' and 'r' are mixing constants generated offline.
   *  They're not really 'magic', they just happen to work well.
   */
  const unsigned int m = 0x5bd1e995;
  const int r = 24;
  int len = strlen (str);

  /* initialize the hash to a 'random' value */
  unsigned int h = len;

  /* mix 4 bytes at a time into the hash */
  const unsigned char * data = (const unsigned char *)str;

  while(len >= 4) {
          unsigned int k = *(unsigned int *)data;

          k *= m;
          k ^= k >> r;
          k *= m;
          h *= m;
          h ^= k;

          data += 4;
          len -= 4;
  }

  /* handle the last few bytes of the input array */
  switch(len) {
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

/* this mirrors the code from systemd/src/libudev/libudev-monitor.c,
 * udev_monitor_send_device() */
void
uevent_sender_send (uevent_sender* sender,
                    const char*    devpath,
                    const char*    action)
{
  char props[1024];
  ssize_t count;
  struct msghdr smsg;
  struct iovec iov[2];
  const char *subsystem;
  const char *devtype;
  struct udev_device *device;
  struct udev_monitor_netlink_header nlh;

  assert (_get_fd (sender) >= 0);

  device = udev_device_new_from_syspath (sender->udev, devpath);
  if (device == NULL)
    {
      fprintf (stderr, "ERROR: uevent_sender_send: No such device %s\n", devpath);
      return;
    }

  subsystem = udev_device_get_subsystem (device);
  assert (subsystem != NULL);

  /* build NUL-terminated property array */
  count = 0;
  strcpy (props, "ACTION=");
  strcat (props, action);
  count += strlen (props) + 1;
  strcpy (props + count, "DEVPATH=");
  strcat (props + count, udev_device_get_devpath (device));
  count += strlen (props + count) + 1;
  strcpy (props + count, "SUBSYSTEM=");
  strcat (props + count, subsystem);

  /* add versioned header */
  memset (&nlh, 0x00, sizeof (struct udev_monitor_netlink_header));
  memcpy (nlh.prefix, "libudev", 8);
  nlh.magic = htonl (UDEV_MONITOR_MAGIC);
  nlh.header_size = sizeof (struct udev_monitor_netlink_header);

  nlh.filter_subsystem_hash = htonl (string_hash32 (subsystem));
  devtype = udev_device_get_devtype (device);
  if (devtype != NULL)
    nlh.filter_devtype_hash = htonl (string_hash32 (devtype));
  iov[0].iov_base = &nlh;
  iov[0].iov_len = sizeof (struct udev_monitor_netlink_header);

  /* note, not setting nlh.filter_tag_bloom_{hi,lo} for now; if required, copy
   * from libudev */

  /* add properties list */
  nlh.properties_off = iov[0].iov_len;
  nlh.properties_len = sizeof (props);
  iov[1].iov_base = props;
  iov[1].iov_len = sizeof (props);

  /* send message */
  memset (&smsg, 0x00, sizeof (struct msghdr));
  smsg.msg_iov = iov;
  smsg.msg_iovlen = 2;
  smsg.msg_name = &sender->event_addr;
  smsg.msg_namelen = sizeof (sender->event_addr);
  count = sendmsg (sender->event_fd, &smsg, 0);
  /* printf ("passed %zi bytes to event socket\n", count); */
}

