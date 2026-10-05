// SPDX-License-Identifier: GPL-2.0
/* edu_uapi.h — ABI SKELETON. Leave empty until LAB5. */
#ifndef _EDU_UAPI_H
#define _EDU_UAPI_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define EDU_IOCTL_MAGIC 'E'

struct edu_factorial_req {
	__u32 input;
	__u32 result;
	__u32 reserved[6];
};

#define EDU_IOCTL_FACTORIAL \
	_IOWR(EDU_IOCTL_MAGIC,0x1,struct edu_factorial_req)

#define EDU_FACTORIAL_MAX_INPUT    12
#define EDU_FACTORIAL_TIMEOUT_US   1000000
#define EDU_FACTORIAL_POLL_US      10

#endif /* _EDU_UAPI_H */
