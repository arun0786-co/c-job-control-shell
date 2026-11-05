#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0) {
            strict_grammar = 1;
        } else {
            fprintf(stderr, "usage: %s [--strict]\n", argv[0]);
            return 2;
        }
    }

    /* line buffering even when stdout is a pipe, so the shell's output and
       its children's output come out in order */
    setvbuf(stdout, NULL, _IOLBF, 0);

    shell_pid = getpid();
    char *home = getcwd(NULL, 0);
    if (home == NULL) {
        perror("getcwd");
        return 1;
    }
    builtins_init(home);
    history_init(home);
    free(home);
    prompt_init();
    jobs_init();

    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        jobs_check();
        print_prompt();
        errno = 0;
        if (getline(&line, &cap, stdin) < 0) {
            if (errno == EINTR && !feof(stdin)) {
                clearerr(stdin);
                continue;
            }
            break; /* Ctrl-D */
        }
        jobs_check(); /* the spec asks for a check after input, before parsing */
        run_line(line, 1);
    }
    printf("logout\n");
    jobs_kill_all();
    free(line);
    return 0;
}
