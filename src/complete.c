#include "shell.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

/* Tab finds what could finish the word under the cursor. The first word of a
   command is a program or builtin, a word starting with $ is a variable, and
   anything else is a file name. */

/* the word as it means itself: a\ b is a b */
char *complete_unescape(const char *s, size_t n)
{
    char *out = xrealloc(NULL, n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n)
            i++;
        out[k++] = s[i];
    }
    out[k] = '\0';
    return out;
}

static int ends_word(const char *line, size_t i)
{
    char c = line[i];
    if (i > 0 && line[i - 1] == '\\')
        return 0; /* an escaped space does not */
    return strchr(" \t|&;<>", c) != NULL;
}

/* is the word at start in command position? */
static int is_command_word(const char *line, size_t start)
{
    size_t i = start;
    while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t'))
        i--;
    return i == 0 || strchr("|&;", line[i - 1]) != NULL;
}

static int by_text(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void add_unique_sorted(strlist_t *out)
{
    if (out->n < 2)
        return;
    qsort(out->items, (size_t)out->n, sizeof *out->items, by_text);
    int k = 1;
    for (int i = 1; i < out->n; i++) {
        if (strcmp(out->items[i], out->items[k - 1]) == 0)
            free(out->items[i]);
        else
            out->items[k++] = out->items[i];
    }
    out->n = k;
    out->items[k] = NULL;
}

static void complete_files(const char *word, strlist_t *out)
{
    const char *slash = strrchr(word, '/');
    const char *base = slash ? slash + 1 : word;
    size_t dirlen = slash ? (size_t)(slash - word) + 1 : 0;
    char *shown_dir = xstrndup(word, dirlen);
    char *dir;
    if (dirlen == 0) {
        dir = xstrdup(".");
    } else if (word[0] == '~' && word[1] == '/') {
        /* look in the real place, but keep the ~ the person typed */
        const char *home = shell_home();
        dir = xrealloc(NULL, strlen(home) + dirlen);
        strcpy(dir, home);
        strcat(dir, shown_dir + 1);
    } else {
        dir = xstrdup(shown_dir);
    }
    size_t blen = strlen(base);
    DIR *d = opendir(dir);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            if (e->d_name[0] == '.' && base[0] != '.')
                continue;
            if (strncmp(e->d_name, base, blen) != 0)
                continue;
            char *full = xrealloc(NULL, strlen(dir) + strlen(e->d_name) + 2);
            strcpy(full, dir);
            strcat(full, "/");
            strcat(full, e->d_name);
            struct stat st;
            int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            free(full);
            char *item = xrealloc(NULL, dirlen + strlen(e->d_name) + 2);
            strcpy(item, shown_dir);
            strcat(item, e->d_name);
            if (isdir)
                strcat(item, "/");
            strlist_push(out, item);
            free(item);
        }
        closedir(d);
    }
    free(dir);
    free(shown_dir);
}

static void complete_commands(const char *word, strlist_t *out)
{
    size_t wlen = strlen(word);
    const char *name;
    for (int i = 0; (name = builtin_name(i)) != NULL; i++)
        if (strncmp(name, word, wlen) == 0)
            strlist_push(out, name);
    const char *path = getenv("PATH");
    if (path == NULL)
        return;
    char *copy = xstrdup(path);
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
        DIR *d = opendir(dir);
        if (d == NULL)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.' || strncmp(e->d_name, word, wlen) != 0)
                continue;
            char *full = xrealloc(NULL, strlen(dir) + strlen(e->d_name) + 2);
            strcpy(full, dir);
            strcat(full, "/");
            strcat(full, e->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0)
                strlist_push(out, e->d_name);
            free(full);
        }
        closedir(d);
    }
    free(copy);
}

static void complete_variables(const char *word, strlist_t *out)
{
    size_t wlen = strlen(word + 1);
    for (char **e = environ; *e != NULL; e++) {
        const char *eq = strchr(*e, '=');
        if (eq == NULL || strncmp(*e, word + 1, wlen) != 0)
            continue;
        size_t n = (size_t)(eq - *e);
        char *item = xrealloc(NULL, n + 2);
        item[0] = '$';
        memcpy(item + 1, *e, n);
        item[n + 1] = '\0';
        strlist_push(out, item);
        free(item);
    }
}

/* fills out with what could finish the word ending at pos, and sets *start to
   where that word begins */
void complete_word(const char *line, size_t pos, size_t *start, strlist_t *out)
{
    size_t i = pos;
    while (i > 0 && !ends_word(line, i - 1))
        i--;
    *start = i;
    char *word = complete_unescape(line + i, pos - i);
    if (word[0] == '$')
        complete_variables(word, out);
    else if (is_command_word(line, i) && strchr(word, '/') == NULL)
        complete_commands(word, out);
    else
        complete_files(word, out);
    free(word);
    add_unique_sorted(out);
}
