// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "uapi/edu_uapi.h"

_Static_assert(sizeof(struct edu_factorial_req) == 32,
               "unexpected factorial ABI layout");

static void wait_enter(const char *message)
{
    int ch;

    puts(message);
    fflush(stdout);

    do {
        ch = getchar();
    } while (ch != '\n' && ch != EOF);
}

static int run_ioctl(int fd, unsigned int input)
{
    struct edu_factorial_req req = {
        .input = input,
        .result = 0,
    };

    errno = 0;

    if (ioctl(fd, EDU_IOCTL_FACTORIAL, &req) == -1) {
        int saved = errno;

        printf("fd=%d input=%u errno=%d (%s)\n",
               fd, input, saved, strerror(saved));
        fflush(stdout);
        return saved;
    }

    printf("fd=%d input=%u result=%u\n",
           fd, input, req.result);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    int fd1, fd2, fd3, duplicate;
    int errors = 0;

    if (argc != 2 ||
        (strcmp(argv[1], "stale") &&
         strcmp(argv[1], "active") &&
         strcmp(argv[1], "timeout"))) {
        fprintf(stderr,
                "usage: %s stale|active|timeout\n", argv[0]);
        return 2;
    }

    fd1 = open("/dev/edu0", O_RDWR);
    if (fd1 == -1) {
        perror("open");
        return 1;
    }

    if (!strcmp(argv[1], "active")) {
        int err = run_ioctl(fd1, 12);

        wait_enter("ioctl finished; fd remains open. "
                   "Press Enter to close.");
        close(fd1);
        return err == ENODEV ? 0 : 1;
    }

    if (!strcmp(argv[1], "timeout")) {
        int first = run_ioctl(fd1, 12);
        int second = run_ioctl(fd1, 5);

        close(fd1);
        return first == ETIMEDOUT && second == EIO ? 0 : 1;
    }

    fd2 = open("/dev/edu0", O_RDWR);
    fd3 = open("/dev/edu0", O_RDWR);
    duplicate = dup(fd1);

    if (fd2 == -1 || fd3 == -1 || duplicate == -1) {
        perror("open/dup");
        if (duplicate != -1)
            close(duplicate);
        if (fd3 != -1)
            close(fd3);
        if (fd2 != -1)
            close(fd2);
        close(fd1);
        return 1;
    }

    wait_enter("Three opens and one dup are ready. "
               "Unbind in another terminal, then press Enter.");

    errors += run_ioctl(fd1, 5) != ENODEV;
    errors += run_ioctl(fd2, 5) != ENODEV;
    errors += run_ioctl(fd3, 5) != ENODEV;
    errors += run_ioctl(duplicate, 5) != ENODEV;

    close(fd1);
    close(fd2);
    close(fd3);

    wait_enter("Only dup(fd1) remains. "
               "Object must still be alive. "
               "Press Enter to close the last description.");

    close(duplicate);
    return errors ? 1 : 0;
}
