#include "shell.h"

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The parser keeps each word exactly as typed, quotes included. It is expanded
 * here just before the command runs, because $? and $(...) must see what
 * earlier commands on the same line did.
 *
 * A word can become any number of arguments: $X may split into several, and
 * *.c into many or none. So each character is built up along with a flag
 * saying whether it was quoted. Only unquoted text is split, and only unquoted
 * * ? [ are wildcards, so "*" and \* stay as they are while * does not.
 */

int substitution_ran;
strlist_t script_args;

typedef struct {
    char *text;
    char *quoted; /* one flag per character */
    size_t len, cap;
    int started; /* "" must give one empty argument, not none */
} field_t;

static void put(field_t *f, char c, int quoted)
{
    if (f->len + 1 >= f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->text = xrealloc(f->text, f->cap);
        f->quoted = xrealloc(f->quoted, f->cap);
    }
    f->text[f->len] = c;
    f->quoted[f->len] = (char)quoted;
    f->len++;
    f->started = 1;
}

void strlist_push(strlist_t *list, const char *s)
{
    list->items = xrealloc(list->items, (size_t)(list->n + 2) * sizeof *list->items);
    list->items[list->n++] = xstrdup(s);
    list->items[list->n] = NULL;
}

void strlist_free(strlist_t *list)
{
    for (int i = 0; i < list->n; i++)
        free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->n = 0;
}

static int has_wildcard(const field_t *f)
{
    for (size_t i = 0; i < f->len; i++)
        if (!f->quoted[i] && strchr("*?[", f->text[i]) != NULL)
            return 1;
    return 0;
}

/* finish the current field: it becomes one argument, or the files it matches */
static void end_field(field_t *f, strlist_t *out, int use_glob)
{
    if (!f->started)
        return;
    if (f->cap == 0)
        put(f, '\0', 1), f->len = 0;
    f->text[f->len] = '\0';
    if (use_glob && has_wildcard(f)) {
        /* quoted characters must reach glob as plain text */
        char *pattern = xrealloc(NULL, f->len * 2 + 1);
        size_t n = 0;
        for (size_t i = 0; i < f->len; i++) {
            if (f->quoted[i] && strchr("*?[]\\", f->text[i]) != NULL)
                pattern[n++] = '\\';
            pattern[n++] = f->text[i];
        }
        pattern[n] = '\0';
        glob_t g;
        memset(&g, 0, sizeof g);
        int r = glob(pattern, 0, NULL, &g);
        free(pattern);
        if (r == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++)
                strlist_push(out, g.gl_pathv[i]);
            globfree(&g);
            f->len = 0;
            f->started = 0;
            return;
        }
        globfree(&g);
        /* nothing matched: keep the word, as other shells do */
    }
    strlist_push(out, f->text);
    f->len = 0;
    f->started = 0;
}

/* add the value of an expansion. unquoted, whitespace in it separates fields */
static void put_value(field_t *f, strlist_t *out, const char *v, int quoted, int split, int use_glob)
{
    if (quoted || !split) {
        for (; *v != '\0'; v++)
            put(f, *v, quoted);
        return;
    }
    for (; *v != '\0'; v++) {
        if (*v == ' ' || *v == '\t' || *v == '\n')
            end_field(f, out, use_glob);
        else
            put(f, *v, 0);
    }
}

/* run a command with its output in a pipe, and return that output */
static char *capture_output(const char *cmd)
{
    int pfd[2];
    if (pipe(pfd) < 0) {
        perror("pipe");
        return xstrdup("");
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(pfd[0]);
        close(pfd[1]);
        return xstrdup("");
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        enter_subshell(getpgrp());
        run_line(cmd, 0);
        fflush(stdout);
        _exit(last_status);
    }
    close(pfd[1]);
    char *buf = xrealloc(NULL, 4096);
    size_t cap = 4096, n = 0;
    for (;;) {
        ssize_t r = read(pfd[0], buf + n, cap - n - 1);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n + 1 == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    close(pfd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        ;
    last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    substitution_ran = 1;
    /* trailing newlines are dropped, so `x=$(pwd)` is what you want */
    while (n > 0 && buf[n - 1] == '\n')
        n--;
    buf[n] = '\0';
    return buf;
}

static int name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int name_char(char c)
{
    return name_start(c) || (c >= '0' && c <= '9');
}

/* w[i] is a $. expands what follows and returns the position after it */
static size_t expand_dollar(const char *w, size_t i, field_t *f, strlist_t *out,
                            int quoted, int split, int use_glob)
{
    char num[32];
    size_t j = i + 1;
    if (w[j] == '?') {
        snprintf(num, sizeof num, "%d", last_status);
        put_value(f, out, num, 1, 0, use_glob);
        return j + 1;
    }
    if (w[j] >= '0' && w[j] <= '9') {
        int n = w[j] - '0';
        if (n < script_args.n)
            put_value(f, out, script_args.items[n], quoted, split, use_glob);
        return j + 1;
    }
    if (w[j] == '#') {
        snprintf(num, sizeof num, "%d", script_args.n > 0 ? script_args.n - 1 : 0);
        put_value(f, out, num, 1, 0, use_glob);
        return j + 1;
    }
    if (w[j] == '@') {
        /* "$@" keeps each argument whole; it is the one quoted thing that makes
           several fields */
        for (int n = 1; n < script_args.n; n++) {
            if (n > 1 && (quoted || split))
                end_field(f, out, use_glob);
            if (quoted)
                f->started = 1;
            put_value(f, out, script_args.items[n], quoted, split, use_glob);
        }
        return j + 1;
    }
    if (w[j] == '$') {
        snprintf(num, sizeof num, "%d", (int)shell_pid);
        put_value(f, out, num, 1, 0, use_glob);
        return j + 1;
    }
    if (w[j] == '!') {
        if (last_background_pid > 0) {
            snprintf(num, sizeof num, "%d", (int)last_background_pid);
            put_value(f, out, num, 1, 0, use_glob);
        }
        return j + 1;
    }
    if (w[j] == '(') {
        size_t end = skip_quoted(w, i);
        /* the parser has checked it is closed, so end is past the ) */
        char *cmd = xstrndup(w + j + 1, end - j - 2);
        char *value = capture_output(cmd);
        put_value(f, out, value, quoted, split, use_glob);
        free(value);
        free(cmd);
        return end;
    }
    size_t start = j, stop;
    int braces = w[j] == '{';
    if (braces) {
        start = j + 1;
        stop = start;
        while (w[stop] != '\0' && w[stop] != '}')
            stop++;
        j = w[stop] == '}' ? stop + 1 : stop;
    } else if (name_start(w[j])) {
        stop = j;
        while (name_char(w[stop]))
            stop++;
        j = stop;
    } else {
        put(f, '$', 1); /* a lone $ is just a $ */
        return j;
    }
    char *name = xstrndup(w + start, stop - start);
    const char *value = getenv(name);
    free(name);
    if (value != NULL)
        put_value(f, out, value, quoted, split, use_glob);
    return j;
}

static void expand_word(const char *w, strlist_t *out, int split_and_glob)
{
    field_t f;
    memset(&f, 0, sizeof f);
    size_t i = 0;
    /* ~ is a word's first character, and is the shell's home like hop's */
    if (w[0] == '~' && (w[1] == '\0' || w[1] == '/')) {
        for (const char *h = shell_home(); *h != '\0'; h++)
            put(&f, *h, 1);
        i = 1;
    }
    while (w[i] != '\0') {
        char c = w[i];
        if (c == '\\') {
            if (w[i + 1] != '\0') {
                put(&f, w[i + 1], 1);
                i += 2;
            } else {
                i++;
            }
        } else if (c == '\'') {
            f.started = 1;
            for (i++; w[i] != '\0' && w[i] != '\''; i++)
                put(&f, w[i], 1);
            if (w[i] != '\0')
                i++;
        } else if (c == '"') {
            f.started = 1;
            i++;
            /* the tokenizer has checked the quotes, but ${ can swallow a closing
               one, so the end of the string has to be checked here as well */
            while (w[i] != '\0' && w[i] != '"') {
                if (w[i] == '\\' && w[i + 1] != '\0' && strchr("\"\\$`", w[i + 1]) != NULL) {
                    put(&f, w[i + 1], 1);
                    i += 2;
                } else if (w[i] == '$') {
                    i = expand_dollar(w, i, &f, out, 1, split_and_glob, 0);
                } else {
                    put(&f, w[i++], 1);
                }
            }
            if (w[i] != '\0')
                i++;
        } else if (c == '$') {
            i = expand_dollar(w, i, &f, out, 0, split_and_glob, split_and_glob);
        } else {
            put(&f, c, 0);
            i++;
        }
    }
    end_field(&f, out, split_and_glob);
    free(f.text);
    free(f.quoted);
}

/* the value of an assignment, or a redirection's file, is one word whatever is in it */
static char *expand_one(const char *w)
{
    strlist_t list = { NULL, 0 };
    expand_word(w, &list, 0);
    char *s = xstrdup(list.n > 0 ? list.items[0] : "");
    strlist_free(&list);
    return s;
}

/* NAME=value, with the value expanded as one word */
static void push_setting(strlist_t *list, const char *word)
{
    const char *eq = strchr(word, '=');
    char *value = expand_one(eq + 1);
    size_t n = (size_t)(eq - word) + 1;
    char *pair = xrealloc(NULL, n + strlen(value) + 1);
    memcpy(pair, word, n);
    strcpy(pair + n, value);
    strlist_push(list, pair);
    free(pair);
    free(value);
}

/* NAME=... with an unquoted name */
static int is_assignment(const char *w)
{
    if (!name_start(w[0]))
        return 0;
    size_t i = 1;
    while (name_char(w[i]))
        i++;
    return w[i] == '=';
}

/* fills out with cmd's arguments and redirections after expansion. strict mode
   has no expansion at all, and this only copies. */
void expand_command(const command_t *cmd, command_t *out)
{
    memset(out, 0, sizeof *out);
    strlist_t args = { NULL, 0 }, assigns = { NULL, 0 };
    int i = 0;
    if (!strict_grammar) {
        /* leading NAME=value words are settings for the command, not its name */
        for (; i < cmd->argc && is_assignment(cmd->argv[i]); i++)
            push_setting(&assigns, cmd->argv[i]);
    }
    int is_export = i < cmd->argc && strcmp(cmd->argv[i], "export") == 0;
    for (; i < cmd->argc; i++) {
        if (strict_grammar) {
            strlist_push(&args, cmd->argv[i]);
        } else if (is_export && is_assignment(cmd->argv[i])) {
            /* export A=$B must not split, whatever $B holds */
            push_setting(&args, cmd->argv[i]);
        } else {
            expand_word(cmd->argv[i], &args, 1);
        }
    }
    out->argv = args.items;
    out->argc = args.n;
    if (out->argv == NULL) {
        out->argv = xrealloc(NULL, sizeof *out->argv);
        out->argv[0] = NULL;
    }
    out->assigns = assigns.items;
    out->nassigns = assigns.n;
    out->nredirs = cmd->nredirs;
    out->redirs = cmd->nredirs ? xrealloc(NULL, (size_t)cmd->nredirs * sizeof *out->redirs) : NULL;
    for (int k = 0; k < cmd->nredirs; k++) {
        out->redirs[k] = cmd->redirs[k];
        const char *file = cmd->redirs[k].file;
        out->redirs[k].file = file == NULL ? NULL : strict_grammar ? xstrdup(file) : expand_one(file);
    }
}

void free_command(command_t *cmd)
{
    for (int k = 0; k < cmd->argc; k++)
        free(cmd->argv[k]);
    free(cmd->argv);
    for (int k = 0; k < cmd->nassigns; k++)
        free(cmd->assigns[k]);
    free(cmd->assigns);
    for (int k = 0; k < cmd->nredirs; k++)
        free(cmd->redirs[k].file);
    free(cmd->redirs);
}
