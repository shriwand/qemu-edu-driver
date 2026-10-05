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
    fprintf(stderr, "Usage: %s factorial <0..12>\n", prog);
}

int main(int argc, char **argv)
{
    struct edu_factorial_req req = { 0 };
    char *end;
    unsigned long value;
    int fd;
    int ret;

    if (argc != 3 || strcmp(argv[1], "factorial")) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    errno = 0;
    value = strtoul(argv[2], &end, 10);
    if (errno || *end != '\0' || value > EDU_FACTORIAL_MAX_INPUT) {
        fprintf(stderr, "input must be an integer in range 0..12\n");
        return EXIT_FAILURE;
    }

    req.input = (unsigned int)value;

    fd = open("/dev/edu0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/edu0");
        return EXIT_FAILURE;
    }

    ret = ioctl(fd, EDU_IOCTL_FACTORIAL, &req);
    if (ret < 0) {
        perror("EDU_IOCTL_FACTORIAL");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("%u! = %u\n", req.input, req.result);

    close(fd);
    return EXIT_SUCCESS;
}
