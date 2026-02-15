/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ioctl_termios.c - Helper functions for termios ioctl handling
 *
 * We can't use the Linux.Termios.TCGETS2 constants here: It is defined in sys/ioctl.h using _IOR(struct termios2),
 * and Vala tries to apply sizeof() to the incomplete termios2 struct. Also, the TC*2 variants are not available
 * on ppc64le, and Vala does not have conditional compilation.
 */

#include "ioctl_termios.h"
#include <sys/ioctl.h>
#include <asm/termbits.h>
#include <asm/ioctls.h>

bool is_termios_ioctl(unsigned long request)
{
    switch (request) {
        case TCGETS:
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
        case TCGETA:
        case TCSETA:
        case TCSETAW:
        case TCSETAF:
        case TIOCGWINSZ:
        case TIOCSWINSZ:
#ifdef TCGETS2
        case TCGETS2:
        case TCSETS2:
        case TCSETSW2:
        case TCSETSF2:
#endif
            return true;
        default:
            return false;
    }
}

unsigned long get_tcgets_ioctl(void)
{
    return TCGETS;
}
