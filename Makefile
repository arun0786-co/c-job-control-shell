CC ?= gcc
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror \
	-Wno-unused-parameter -fno-asm -Iinclude
SRC = src/main.c src/prompt.c src/parser.c src/executor.c src/builtins.c src/jobs.c \
	src/history.c src/util.c src/expand.c

all: shell.out

shell.out: $(SRC) include/shell.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

# every test also runs against this build
shell-asan: $(SRC) include/shell.h
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $(SRC)

test: shell.out shell-asan
	python3 tests/run.py
	ASAN_OPTIONS=detect_leaks=0 SHELL_UNDER_TEST=$(CURDIR)/shell-asan python3 tests/run.py

clean:
	rm -f shell.out shell-asan

.PHONY: all test clean
