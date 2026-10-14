// SPDX-License-Identifier: GPL-2.0-only
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

struct edu_dma_selftest_req {
	__u32 length;
	__u32 reserved[3];
};

#define EDU_IOCTL_FACTORIAL \
	_IOWR(EDU_IOCTL_MAGIC,0x1,struct edu_factorial_req)

#define EDU_IOCTL_DMA_SELFTEST \
    _IOW(EDU_IOCTL_MAGIC, 0x2, struct edu_dma_selftest_req)

#define EDU_IOCTL_DMA_STREAM_SELFTEST \
    _IOW(EDU_IOCTL_MAGIC, 0x3, struct edu_dma_selftest_req)

#define EDU_FACTORIAL_MAX_INPUT    12

#define EDU_DMA_MAX_LENGTH 4096U

#endif /* _EDU_UAPI_H */
