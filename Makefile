CC ?= gcc
# _DEFAULT_SOURCE and _DARWIN_C_SOURCE are for one thing: TIOCGWINSZ, the
# terminal's width, which is not in POSIX
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
	-Wall -Wextra -Werror -Wno-unused-parameter -fno-asm -Iinclude
SRC = src/main.c src/prompt.c src/parser.c src/executor.c src/builtins.c src/jobs.c \
	src/history.c src/util.c src/expand.c src/edit.c src/complete.c

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
