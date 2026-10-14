// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "uapi/edu_uapi.h"

static void usage(const char *prog)
{
   fprintf(stderr,
            "Usage:\n"
            "  %s factorial <0..12>\n"
            "  %s dma-selftest <1..4096>\n"
            "  %s dma-stream <1..4096>\n",
            prog, prog,prog);
}

static int parse_value(const char *text, unsigned long *value)
{
    char *end;

    if (!text[0] || text[0] == '-')
        return -1;

    errno = 0;
    *value = strtoul(text, &end, 10);

    if (errno || end == text || *end != '\0')
        return -1;

    return 0;
}


int main(int argc, char **argv)
{
    unsigned long value;
    int fd;
    int ret;
    int saved_errno;

    if (argc != 3 || (strcmp(argv[1], "factorial")
	&& strcmp(argv[1], "dma-selftest")
	&& strcmp(argv[1], "dma-stream"))) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (parse_value(argv[2], &value)) {
        fprintf(stderr, "invalid integer\n");
        return EXIT_FAILURE;
    }

    if (!strcmp(argv[1], "factorial")) {
        if (value > EDU_FACTORIAL_MAX_INPUT) {
            fprintf(stderr, "factorial input must be 0..12\n");
            return EXIT_FAILURE;
        }
    } else {
        if (!value || value > EDU_DMA_MAX_LENGTH) {
            fprintf(stderr, "DMA length must be 1..4096\n");
            return EXIT_FAILURE;
        }
    }

    fd = open("/dev/edu0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/edu0");
        return EXIT_FAILURE;
    }

    if (!strcmp(argv[1], "factorial")) {
        struct edu_factorial_req req = { 0 };

        req.input = (__u32)value;
        ret = ioctl(fd, EDU_IOCTL_FACTORIAL, &req);
        saved_errno = errno;

        if (!ret)
            printf("%u! = %u\n", req.input, req.result);
    } else {
        struct edu_dma_selftest_req req = { 0 };

        req.length = (__u32)value;
	unsigned long command = !strcmp(argv[1], "dma-stream")
	    ? EDU_IOCTL_DMA_SELFTEST
	    : EDU_IOCTL_DMA_STREAM_SELFTEST;
        ret = ioctl(fd, command, &req);
        saved_errno = errno;

        if (!ret)
            printf("DMA self-test PASS: %lu bytes\n", value);
    }

    close(fd);

    if (ret < 0) {
        errno = saved_errno;
        perror("ioctl");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
