#include "shell.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *home;     /* the directory the shell started in */
static char *prev_dir; /* for "-", NULL until the first directory change */

void builtins_init(const char *dir)
{
    home = xstrdup(dir);
}

const char *shell_home(void)
{
    return home;
}

/* map a hop/reveal argument to a path. returns NULL for "-" when there is
   no previous directory yet. */
static char *resolve(const char *arg)
{
    if (strcmp(arg, "-") == 0)
        return prev_dir ? xstrdup(prev_dir) : NULL;
    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
        size_t n = strlen(home);
        char *p = xrealloc(NULL, n + strlen(arg));
        memcpy(p, home, n);
        strcpy(p + n, arg + 1);
        return p;
    }
    return xstrdup(arg);
}

static int change_dir(const char *path)
{
    char *old = getcwd(NULL, 0);
    if (chdir(path) < 0) {
        free(old);
        fprintf(stderr, "No such directory!\n");
        return -1;
    }
    free(prev_dir);
    prev_dir = old;
    return 0;
}

/* hop: every argument is applied in turn, and a bad one does not stop the
   rest */
static int builtin_hop(int argc, char **argv)
{
    if (argc == 1)
        return change_dir(home) < 0;
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ".") == 0)
            continue;
        char *path = resolve(argv[i]);
        if (path == NULL)
            continue; /* "-" with nowhere to go back to */
        if (change_dir(path) < 0)
            status = 1;
        free(path);
    }
    return status;
}

static int by_name(const void *a, const void *b)
{
    /* strcmp compares byte values, which is the ASCII order the spec asks for */
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int builtin_reveal(int argc, char **argv)
{
    int all = 0, one_per_line = 0;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            for (int j = 1; a[j] != '\0'; j++) {
                if (a[j] == 'a') {
                    all = 1;
                } else if (a[j] == 'l') {
                    one_per_line = 1;
                } else {
                    fprintf(stderr, "reveal: Invalid Syntax!\n");
                    return 1;
                }
            }
        } else if (target != NULL) {
            fprintf(stderr, "reveal: Invalid Syntax!\n");
            return 1;
        } else {
            target = a; /* a lone "-" lands here: the previous directory */
        }
    }

    char *path = target ? resolve(target) : xstrdup(".");
    DIR *dir = path ? opendir(path) : NULL;
    free(path);
    if (dir == NULL) {
        fprintf(stderr, "No such directory!\n");
        return 1;
    }
    char **names = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (!all && e->d_name[0] == '.')
            continue;
        names = xrealloc(names, (n + 1) * sizeof *names);
        names[n++] = xstrdup(e->d_name);
    }
    closedir(dir);
    if (n > 1)
        qsort(names, n, sizeof *names, by_name);
    for (int i = 0; i < n; i++) {
        printf("%s%s", names[i], one_per_line || i == n - 1 ? "\n" : " ");
        free(names[i]);
    }
    free(names);
    return 0;
}

static int builtin_log(int argc, char **argv)
{
    long n;
    if (argc == 1) {
        for (int i = 0; i < history_count(); i++)
            printf("%s\n", history_get(i));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "purge") == 0) {
        history_purge();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "execute") == 0 && parse_number(argv[2], &n)
        && n >= 1 && n <= history_count()) {
        /* 1 is the newest entry. copy it: the line runs through the same
           path as typed input, but is not stored again */
        char *line = xstrdup(history_get(history_count() - (int)n));
        run_line(line, 0);
        free(line);
        return 0;
    }
    fprintf(stderr, "log: Invalid Syntax!\n");
    return 1;
}

static const struct {
    const char *name;
    builtin_fn fn;
} builtins[] = {
    { "hop", builtin_hop },
    { "reveal", builtin_reveal },
    { "log", builtin_log },
    { "activities", builtin_activities },
    { "ping", builtin_ping },
    { "fg", builtin_fg },
    { "bg", builtin_bg },
};

builtin_fn find_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strcmp(builtins[i].name, name) == 0)
            return builtins[i].fn;
    return NULL;
}
