CC ?= cc
CPPFLAGS += -I../include
CFLAGS ?= -Wall -Wextra -Werror -O2 -std=c11

all: edu_lifetime

edu_lifetime: edu_lifetime.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $<

clean:
	rm -f edu_lifetime

.PHONY: all clean
