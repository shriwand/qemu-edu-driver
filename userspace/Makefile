CC ?= cc
CPPFLAGS += -I../include
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c11

all: edu_ctl

edu_ctl: edu_ctl.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $<

clean:
	rm -f edu_ctl

.PHONY: all clean
