/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ioctl_termios.h - Helper functions for termios ioctl handling
 */

#pragma once

#include <stdbool.h>

bool is_termios_ioctl(unsigned long request);
unsigned long get_tcgets_ioctl(void);
