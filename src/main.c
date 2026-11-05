#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s [--strict] [-c command | script [args...]]\n", name);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *command = NULL, *script = NULL;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0) {
            strict_grammar = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (++i == argc)
                usage(argv[0]);
            command = argv[i];
            i++;
            break;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            usage(argv[0]);
        } else {
            script = argv[i];
            break;
        }
    }
    /* $0 and up: the script's name and its arguments */
    for (int k = script ? i : (command ? i : argc); k < argc; k++)
        strlist_push(&script_args, argv[k]);

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

    if (command != NULL || script != NULL) {
        enter_batch_mode();
        if (command != NULL)
            run_line(command, 0);
        else if (access(script, R_OK) != 0) {
            perror(script);
            return 127;
        } else {
            run_file(script);
        }
        fflush(stdout);
        return last_status;
    }

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
