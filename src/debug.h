#ifndef __UMOCKDEV_DEBUG_H
#define __UMOCKDEV_DEBUG_H

/********************************
 *
 * Debug logging
 *
 ********************************/

#define DBG_PATH    (1 << 0)
#define DBG_NETLINK (1 << 1)
#define DBG_SCRIPT  (1 << 2)
#define DBG_IOCTL   (1 << 3)
#define DBG_IOCTL_TREE (1 << 4)

unsigned debug_categories;

void init_debug(void) __attribute__((constructor));

#define DBG(cat, ...) if (cat & debug_categories) fprintf(stderr, __VA_ARGS__)

#endif
