// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <sys/ioctl.h>

#include "uapi/edu_uapi.h"

_Static_assert(sizeof(struct edu_factorial_req) == 32,
               "factorial ABI size changed");

_Static_assert(sizeof(struct edu_dma_selftest_req) == 16,
               "DMA ABI size changed");

static void show_command(const char *name,
                         unsigned long command)
{
    printf("%s: command=%#lx type=%#x nr=%#x "
           "size=%u dir=%u\n",
           name,
           command,
           (unsigned int)_IOC_TYPE(command),
           (unsigned int)_IOC_NR(command),
           (unsigned int)_IOC_SIZE(command),
           (unsigned int)_IOC_DIR(command));
}

int main(void)
{
    show_command("factorial", EDU_IOCTL_FACTORIAL);
    show_command("coherent", EDU_IOCTL_DMA_SELFTEST);
    show_command("streaming", EDU_IOCTL_DMA_STREAM_SELFTEST);

    return 0;
}
