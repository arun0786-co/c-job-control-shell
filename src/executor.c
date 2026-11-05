#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int strict_grammar;
int last_status;
pid_t shell_pid;
pid_t last_background_pid;

/* a forked copy of the shell puts what it runs in this group, not in new ones */
static pid_t subshell_pgid;

/* this process is now a forked copy of the shell, doing work for the real one.
   what it runs goes into the group pgid. */
void enter_subshell(pid_t pgid)
{
    reset_child_signals();
    interactive = 0;
    in_child_shell = 1;
    subshell_pgid = pgid;
}

static int run_expanded_group(const group_t *g);

/* lines that run log anywhere are not stored in the history */
static int mentions_log(const shell_cmd_t *sc)
{
    for (int i = 0; i < sc->ngroups; i++)
        for (int j = 0; j < sc->groups[i].ncmds; j++)
            if (strcmp(sc->groups[i].cmds[j].argv[0], "log") == 0)
                return 1;
    return 0;
}

static char *trim(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    return xstrndup(s, n);
}

/* open each redirection in the order typed and point fd 0 or 1 at it, so
   the last one of each kind wins. stops at the first file that fails. */
int apply_redirections(const command_t *cmd)
{
    for (int i = 0; i < cmd->nredirs; i++) {
        const redir_t *r = &cmd->redirs[i];
        int fd, target;
        if (r->type == REDIR_IN) {
            fd = open(r->file, O_RDONLY);
            target = STDIN_FILENO;
            if (fd < 0) {
                fprintf(stderr, "No such file or directory\n");
                return -1;
            }
        } else {
            int flags = O_WRONLY | O_CREAT | (r->type == REDIR_APPEND ? O_APPEND : O_TRUNC);
            fd = open(r->file, flags, 0644);
            target = STDOUT_FILENO;
            if (fd < 0) {
                fprintf(stderr, "Unable to create file for writing\n");
                return -1;
            }
        }
        if (fd != target) {
            dup2(fd, target);
            close(fd);
        }
    }
    return 0;
}

/* a builtin alone in a foreground group runs inside the shell, so that
   hop can change the shell's own directory. its redirections are undone
   afterwards. */
static int run_builtin_here(const command_t *cmd, builtin_fn fn)
{
    int status = 1;
    fflush(stdout);
    int saved_in = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    if (apply_redirections(cmd) == 0) {
        status = fn != NULL ? fn(cmd->argc, cmd->argv) : 0;
        fflush(stdout);
    }
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
    return status;
}

/* runs in the child after fork and never returns */
static void exec_command(const command_t *cmd, int in_fd, int out_fd, pid_t pgid, int background)
{
    /* the whole pipeline shares one process group, led by its first
       process. a foreground group also gets the terminal. */
    setpgid(0, pgid);
    if (interactive && !background)
        tcsetpgrp(STDIN_FILENO, getpgrp());
    reset_child_signals();

    if (in_fd != STDIN_FILENO) {
        dup2(in_fd, STDIN_FILENO);
        close(in_fd);
    } else if (background && !interactive) {
        /* with no terminal to stop it, a background job would read the
           shell's own input */
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
    }
    if (out_fd != STDOUT_FILENO) {
        dup2(out_fd, STDOUT_FILENO);
        close(out_fd);
    }
    if (apply_redirections(cmd) < 0)
        _exit(1);
    for (int i = 0; i < cmd->nassigns; i++)
        putenv(cmd->assigns[i]);
    if (cmd->argc == 0)
        _exit(0); /* a word that expanded to nothing, or only settings */

    builtin_fn fn = find_builtin(cmd->argv[0]);
    if (fn != NULL) {
        interactive = 0; /* this copy of the shell does not own the terminal */
        in_child_shell = 1;
        int status = fn(cmd->argc, cmd->argv);
        fflush(stdout);
        _exit(status);
    }

    /* execvp searches PATH, and runs names containing a / as they are */
    execvp(cmd->argv[0], cmd->argv);
    if (errno == ENOENT || errno == ENOTDIR)
        fprintf(stderr, "Command not found!\n");
    else
        fprintf(stderr, "%s: %s\n", cmd->argv[0], strerror(errno));
    _exit(127);
}

/* fork one process per command, connected by pipes */
static job_t *launch(const group_t *g)
{
    job_t *job = job_new(g->text, g->ncmds);
    job->pgid = subshell_pgid;
    int in_fd = STDIN_FILENO;
    for (int i = 0; i < g->ncmds; i++) {
        int pfd[2] = { -1, -1 };
        int out_fd = STDOUT_FILENO;
        if (i < g->ncmds - 1) {
            if (pipe(pfd) < 0) {
                perror("pipe");
                break;
            }
            out_fd = pfd[1];
        }
        fflush(stdout); /* or the child inherits unwritten output */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (pfd[0] >= 0) {
                close(pfd[0]);
                close(pfd[1]);
            }
            break;
        }
        if (pid == 0) {
            if (pfd[0] >= 0)
                close(pfd[0]); /* the next command's end */
            exec_command(&g->cmds[i], in_fd, out_fd, job->pgid, g->background);
        }
        /* setpgid in both parent and child: whichever runs first wins, so
           the group exists before the shell waits on it */
        if (job->pgid == 0)
            job->pgid = pid;
        setpgid(pid, job->pgid);
        job->pids[job->npids++] = pid;
        job->live++;
        /* the parent must close its copies, or the reader never sees EOF */
        if (in_fd != STDIN_FILENO)
            close(in_fd);
        if (out_fd != STDOUT_FILENO)
            close(out_fd);
        in_fd = pfd[0];
    }
    if (in_fd != STDIN_FILENO)
        close(in_fd);
    return job;
}

/* NAME=value on its own, with no command: applies to the shell from now on */
static int run_settings_here(const command_t *cmd)
{
    for (int i = 0; i < cmd->nassigns; i++) {
        char *eq = strchr(cmd->assigns[i], '=');
        char *name = xstrndup(cmd->assigns[i], (size_t)(eq - cmd->assigns[i]));
        setenv(name, eq + 1, 1);
        free(name);
    }
    /* x=$(false) gives the status of what ran inside it, and x=1 gives 0 */
    return substitution_ran ? last_status : 0;
}

/* runs a builtin in this shell with settings that last for that command only */
static int run_builtin_with_settings(const command_t *cmd, builtin_fn fn)
{
    char **old = NULL;
    if (cmd->nassigns > 0) {
        old = xrealloc(NULL, (size_t)cmd->nassigns * sizeof *old);
        for (int i = 0; i < cmd->nassigns; i++) {
            char *eq = strchr(cmd->assigns[i], '=');
            char *name = xstrndup(cmd->assigns[i], (size_t)(eq - cmd->assigns[i]));
            const char *value = getenv(name);
            old[i] = value ? xstrdup(value) : NULL;
            setenv(name, eq + 1, 1);
            free(name);
        }
    }
    int status = run_builtin_here(cmd, fn);
    for (int i = 0; i < cmd->nassigns; i++) {
        char *eq = strchr(cmd->assigns[i], '=');
        char *name = xstrndup(cmd->assigns[i], (size_t)(eq - cmd->assigns[i]));
        if (old[i] != NULL)
            setenv(name, old[i], 1);
        else
            unsetenv(name);
        free(old[i]);
        free(name);
    }
    free(old);
    return status;
}

/* words are expanded here, just before running, and in the shell itself, so
   that a $(...) in any command of a pipeline is finished before it starts */
static int run_group(const group_t *raw)
{
    group_t expanded = *raw;
    group_t *g = &expanded;
    g->cmds = xrealloc(NULL, (size_t)raw->ncmds * sizeof *g->cmds);
    substitution_ran = 0;
    for (int i = 0; i < raw->ncmds; i++)
        expand_command(&raw->cmds[i], &g->cmds[i]);
    int status = run_expanded_group(g);
    for (int i = 0; i < g->ncmds; i++)
        free_command(&g->cmds[i]);
    free(g->cmds);
    return status;
}

static int run_expanded_group(const group_t *g)
{
    if (g->ncmds == 1 && !g->background && g->cmds[0].argc == 0) {
        /* no command is left, but redirections still create their files */
        int status = run_builtin_here(&g->cmds[0], NULL);
        return status != 0 ? status : run_settings_here(&g->cmds[0]);
    }
    builtin_fn fn = g->ncmds == 1 && g->cmds[0].argc > 0 ? find_builtin(g->cmds[0].argv[0]) : NULL;
    if (fn != NULL && !g->background)
        return run_builtin_with_settings(&g->cmds[0], fn);
    job_t *job = launch(g);
    if (job->npids == 0) {
        job_free(job);
        return 1;
    }
    if (g->background) {
        job_add(job);
        last_background_pid = job->pgid;
        printf("[%d] %d\n", job->number, (int)job->pgid);
        return 0;
    }
    int status;
    if (wait_foreground(job, 0)) {
        status = job_exit_status(job);
        job_add(job);
        printf("[%d] Stopped %s\n", job->number, job->text);
    } else {
        status = job_exit_status(job);
        job_free(job);
    }
    return status;
}

/* run groups first to last of one and_or, each only if the status so far
   allows it. a skipped group leaves the status alone, so that in
   false && a || b, b runs. */
static void run_and_or(const shell_cmd_t *sc, int first, int last)
{
    for (int i = first; i <= last; i++) {
        const group_t *g = &sc->groups[i];
        if (g->run_if == RUN_IF_OK && last_status != 0)
            continue;
        if (g->run_if == RUN_IF_FAILED && last_status == 0)
            continue;
        last_status = run_group(g);
    }
}

/* a && b & puts the whole of a && b in the background. No one group can stand
   for it, so a forked copy of the shell runs it and is the job. */
static void run_and_or_background(const shell_cmd_t *sc, int first, int last)
{
    size_t start = sc->groups[first].start, end = sc->groups[last].end;
    char *text = xstrndup(sc->line + start, end - start);
    job_t *job = job_new(text, 1);
    free(text);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        job_free(job);
        last_status = 1;
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (!interactive) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd >= 0) {
                dup2(fd, STDIN_FILENO);
                close(fd);
            }
        }
        enter_subshell(getpid());
        /* the last group's background flag is what brought us here */
        shell_cmd_t copy = *sc;
        group_t *groups = xrealloc(NULL, (size_t)sc->ngroups * sizeof *groups);
        memcpy(groups, sc->groups, (size_t)sc->ngroups * sizeof *groups);
        groups[last].background = 0;
        copy.groups = groups;
        run_and_or(&copy, first, last);
        fflush(stdout);
        _exit(last_status);
    }
    setpgid(pid, pid);
    job_start_background(job, pid);
    last_background_pid = pid;
    printf("[%d] %d\n", job->number, (int)job->pgid);
    last_status = 0;
}

void run_line(const char *line, int store_in_history)
{
    shell_cmd_t *sc;
    int r = parse_line(line, &sc);
    if (r < 0) {
        fprintf(stderr, "Invalid Syntax!\n");
        last_status = 2;
        return;
    }
    if (r == 0)
        return;
    if (store_in_history && !mentions_log(sc)) {
        char *t = trim(line);
        history_add(t);
        free(t);
    }
    int first = 0;
    while (first < sc->ngroups) {
        int last = first;
        while (last + 1 < sc->ngroups && sc->groups[last + 1].run_if != RUN_ALWAYS)
            last++;
        if (last > first && sc->groups[last].background)
            run_and_or_background(sc, first, last);
        else
            run_and_or(sc, first, last);
        first = last + 1;
    }
    free_shell_cmd(sc);
}
