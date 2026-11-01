CC = gcc
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror \
	-Wno-unused-parameter -fno-asm -Iinclude
SRC = src/main.c src/prompt.c src/parser.c src/executor.c src/builtins.c src/jobs.c \
	src/history.c src/util.c

all: shell.out

shell.out: $(SRC) include/shell.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -f shell.out

.PHONY: all clean
