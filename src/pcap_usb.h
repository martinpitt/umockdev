/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * pcap_usb.h - Reading the 64 bit members of a DLT_USB_LINUX_MMAPPED record
 *
 * pcap_next() only aligns the records it hands out to 4 bytes, so the 64 bit
 * members of the USB pseudo-header must not be read as uint64_t: that asks for
 * an 8 byte aligned load, which is a SIGBUS on architectures like sparc64.
 *
 * libpcap 1.11 declares them as a pair of 32 bit halves for that reason. Older
 * versions still declare them as uint64_t, so for those we use the header from
 * 1.11 instead of the installed one. Drop src/pcap-usb-1.11.h and this switch
 * once we can require libpcap >= 1.11.
 */

#pragma once

#include <stdint.h>
#include <string.h>

#ifdef HAVE_PCAP_ALIGNED_USB_HEADER
#include <pcap/usb.h>
#else
#include "pcap-usb-1.11.h"
#endif

/* libpcap only grew pcap_4_byte_aligned_uint64_val() in 1.11 */
static inline uint64_t
umockdev_pcap_uint64_val(const pcap_4_byte_aligned_uint64 *value)
{
    uint64_t result;

    memcpy(&result, value, sizeof(result));
    return result;
}
