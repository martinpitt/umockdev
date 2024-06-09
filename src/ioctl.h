#ifndef __IOCTL_H
#define __IOCTL_H

#include <stdint.h>

struct cros_ec_command_v2 {
    uint32_t version;
    uint32_t command;
    uint32_t outsize;
    uint32_t insize;
    uint32_t result;
    uint8_t data[0];
};

#define CROS_EC_DEV_IOC_V2 0xEC
#define CROS_EC_DEV_IOCXCMD_V2 \
    _IOWR(CROS_EC_DEV_IOC_V2, 0, struct cros_ec_command_v2)

#endif
