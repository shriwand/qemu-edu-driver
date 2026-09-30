CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -std=c11

all: edu_ctl

edu_ctl: edu_ctl.c ../kernel/edu_uapi.h
	$(CC) $(CFLAGS) -o $@ edu_ctl.c

clean:
	rm -f edu_ctl

.PHONY: all clean
