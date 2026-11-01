#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the last 15 commands, oldest first, mirrored to ~/.shell_history after
   every change so they survive a restart */

#define HISTORY_MAX 15

static char *entries[HISTORY_MAX];
static int count;
static char *path;

static void push(const char *line)
{
    if (count > 0 && strcmp(entries[count - 1], line) == 0)
        return;
    if (count == HISTORY_MAX) {
        free(entries[0]);
        memmove(entries, entries + 1, (HISTORY_MAX - 1) * sizeof *entries);
        count--;
    }
    entries[count++] = xstrdup(line);
}

static void save(void)
{
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return;
    for (int i = 0; i < count; i++)
        fprintf(f, "%s\n", entries[i]);
    fclose(f);
}

void history_init(const char *fallback_dir)
{
    const char *dir = getenv("HOME");
    if (dir == NULL || dir[0] == '\0')
        dir = fallback_dir;
    const char *name = "/.shell_history";
    path = xrealloc(NULL, strlen(dir) + strlen(name) + 1);
    strcpy(path, dir);
    strcat(path, name);

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n')
            line[--len] = '\0';
        if (len > 0)
            push(line);
    }
    free(line);
    fclose(f);
}

void history_add(const char *line)
{
    push(line);
    save();
}

int history_count(void)
{
    return count;
}

const char *history_get(int i)
{
    return entries[i];
}

void history_purge(void)
{
    for (int i = 0; i < count; i++)
        free(entries[i]);
    count = 0;
    save();
}
