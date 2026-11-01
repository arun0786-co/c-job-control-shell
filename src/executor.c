#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static void run_builtin_here(const command_t *cmd, builtin_fn fn)
{
    fflush(stdout);
    int saved_in = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    if (apply_redirections(cmd) == 0) {
        fn(cmd->argc, cmd->argv);
        fflush(stdout);
    }
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    close(saved_in);
    close(saved_out);
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

    builtin_fn fn = find_builtin(cmd->argv[0]);
    if (fn != NULL) {
        interactive = 0; /* this copy of the shell does not own the terminal */
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

static void run_group(const group_t *g)
{
    builtin_fn fn = g->ncmds == 1 ? find_builtin(g->cmds[0].argv[0]) : NULL;
    if (fn != NULL && !g->background) {
        run_builtin_here(&g->cmds[0], fn);
        return;
    }
    job_t *job = launch(g);
    if (job->npids == 0) {
        job_free(job);
        return;
    }
    if (g->background) {
        job_add(job);
        printf("[%d] %d\n", job->number, (int)job->pgid);
        return;
    }
    if (wait_foreground(job, 0)) {
        job_add(job);
        printf("[%d] Stopped %s\n", job->number, job->text);
    } else {
        job_free(job);
    }
}

void run_line(const char *line, int store_in_history)
{
    shell_cmd_t *sc;
    int r = parse_line(line, &sc);
    if (r < 0) {
        fprintf(stderr, "Invalid Syntax!\n");
        return;
    }
    if (r == 0)
        return;
    if (store_in_history && !mentions_log(sc)) {
        char *t = trim(line);
        history_add(t);
        free(t);
    }
    for (int i = 0; i < sc->ngroups; i++)
        run_group(&sc->groups[i]);
    free_shell_cmd(sc);
}
