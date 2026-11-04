CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2

minidb: main.c
	$(CC) $(CFLAGS) -o minidb main.c

run: minidb
	./minidb

clean:
	rm -f minidb

.PHONY: run clean
