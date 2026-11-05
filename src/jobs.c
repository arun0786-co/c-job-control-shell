#include "shell.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/*
 * Every job runs in its own process group. The foreground job's group also
 * owns the terminal, so the terminal sends Ctrl-C and Ctrl-Z to the job and
 * not to the shell. A background job that tries to read the terminal is
 * stopped by the kernel (SIGTTIN), which is how it is kept away from
 * terminal input.
 */

int interactive;
int in_child_shell;
static pid_t shell_pgid;
static struct termios shell_modes;
static volatile sig_atomic_t fg_pgid; /* foreground job's group, 0 if none */
static job_t **jobs;
static int njobs;

/* SIGINT and SIGTSTP can also reach the shell itself (the autograder sends
   them to the shell's pid). The shell must survive both, so it passes them
   on to the foreground job. kill() is safe to call in a signal handler. */
static void forward_signal(int sig)
{
    if (fg_pgid > 0)
        kill(-fg_pgid, sig);
}

static void set_handler(int sig, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    /* restart interrupted calls (read, waitpid) instead of failing them
       with EINTR. signal() does not promise this. */
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
}

void jobs_init(void)
{
    interactive = isatty(STDIN_FILENO);
    set_handler(SIGINT, forward_signal);
    set_handler(SIGTSTP, forward_signal);
    set_handler(SIGQUIT, SIG_IGN);
    /* the shell calls tcsetpgrp while it may not own the terminal */
    set_handler(SIGTTOU, SIG_IGN);
    set_handler(SIGTTIN, SIG_IGN);
    shell_pgid = getpgrp();
    if (interactive) {
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        tcgetattr(STDIN_FILENO, &shell_modes);
    }
}

/* called in a child before exec: handlers are reset by exec anyway, but
   ignored signals would stay ignored in the new program */
void reset_child_signals(void)
{
    int sigs[] = { SIGINT, SIGTSTP, SIGQUIT, SIGTTOU, SIGTTIN };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        set_handler(sigs[i], SIG_DFL);
}

job_t *job_new(const char *text, int ncmds)
{
    job_t *job = xrealloc(NULL, sizeof *job);
    memset(job, 0, sizeof *job);
    job->pids = xrealloc(NULL, ncmds * sizeof *job->pids);
    job->text = xstrdup(text);
    return job;
}

void job_free(job_t *job)
{
    free(job->pids);
    free(job->text);
    free(job);
}

/* number a job one above the highest in use, like bash */
void job_add(job_t *job)
{
    int top = 0;
    for (int i = 0; i < njobs; i++)
        if (jobs[i]->number > top)
            top = jobs[i]->number;
    job->number = top + 1;
    jobs = xrealloc(jobs, (njobs + 1) * sizeof *jobs);
    jobs[njobs++] = job;
}

void job_remove(job_t *job)
{
    for (int i = 0; i < njobs; i++) {
        if (jobs[i] == job) {
            memmove(&jobs[i], &jobs[i + 1], (njobs - i - 1) * sizeof *jobs);
            njobs--;
            return;
        }
    }
}

job_t *job_find(int number)
{
    for (int i = 0; i < njobs; i++)
        if (jobs[i]->number == number)
            return jobs[i];
    return NULL;
}

job_t *job_latest(void)
{
    job_t *latest = NULL;
    for (int i = 0; i < njobs; i++)
        if (latest == NULL || jobs[i]->number > latest->number)
            latest = jobs[i];
    return latest;
}

/* account for one status change of a process in the job */
static void record(job_t *job, pid_t pid, int status)
{
    if (WIFSTOPPED(status)) {
        job->stopped = 1;
    } else if (WIFCONTINUED(status)) {
        job->stopped = 0;
    } else {
        job->live--;
        if (pid == job->pids[job->npids - 1])
            job->last_status = status;
    }
}

/* give the job the terminal (sending SIGCONT first if resume is set) and
   wait until all its processes exit or it stops. returns 1 if it stopped. */
int wait_foreground(job_t *job, int resume)
{
    fg_pgid = job->pgid;
    if (interactive) {
        /* a job that stopped gets back the terminal modes it had then */
        if (resume && job->has_modes)
            tcsetattr(STDIN_FILENO, TCSADRAIN, &job->modes);
        tcsetpgrp(STDIN_FILENO, job->pgid);
    }
    if (resume)
        kill(-job->pgid, SIGCONT);
    job->stopped = 0;
    while (job->live > 0 && !job->stopped) {
        int status;
        pid_t pid = waitpid(-job->pgid, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            job->live = 0; /* nothing left to wait for */
            break;
        }
        record(job, pid, status);
    }
    fg_pgid = 0;
    if (interactive) {
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        if (job->stopped) {
            /* an editor stopped with Ctrl-Z has echo off; keep its modes for
               fg, and give the shell its own back */
            tcgetattr(STDIN_FILENO, &job->modes);
            job->has_modes = 1;
            tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_modes);
        } else if (WIFSIGNALED(job->last_status)) {
            /* it could not clean up after itself */
            tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_modes);
        } else {
            /* it chose to change them: this is how stty works */
            tcgetattr(STDIN_FILENO, &shell_modes);
        }
    }
    return job->stopped;
}

/* collect status changes of background jobs without blocking, and report
   the jobs that have finished */
void jobs_check(void)
{
    /* A builtin in a pipe runs in a forked copy of the shell. The copy has the
       job list, but the jobs are its siblings: waitpid fails with ECHILD, which
       below means "all gone". Only the real shell may decide that. */
    if (in_child_shell)
        return;
    for (int i = 0; i < njobs; i++) {
        job_t *job = jobs[i];
        while (job->live > 0) {
            int status;
            pid_t pid = waitpid(-job->pgid, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (pid == 0)
                break;
            if (pid < 0) {
                if (errno == EINTR)
                    continue;
                job->live = 0;
                break;
            }
            record(job, pid, status);
        }
        if (job->live == 0) {
            int ok = WIFEXITED(job->last_status) && WEXITSTATUS(job->last_status) == 0;
            printf("%s with pid %d exited %s\n", job->text, (int)job->pgid,
                   ok ? "normally" : "abnormally");
            job_remove(job);
            job_free(job);
            i--;
        }
    }
}

void jobs_kill_all(void)
{
    for (int i = 0; i < njobs; i++) {
        kill(-jobs[i]->pgid, SIGKILL);
        job_free(jobs[i]);
    }
    free(jobs);
    jobs = NULL;
    njobs = 0;
}

static int by_text(const void *a, const void *b)
{
    return strcmp((*(job_t *const *)a)->text, (*(job_t *const *)b)->text);
}

int builtin_activities(int argc, char **argv)
{
    jobs_check(); /* drop jobs that ended since the last prompt */
    if (njobs == 0)
        return 0;
    job_t **sorted = xrealloc(NULL, njobs * sizeof *sorted);
    memcpy(sorted, jobs, njobs * sizeof *sorted);
    qsort(sorted, njobs, sizeof *sorted, by_text);
    for (int i = 0; i < njobs; i++)
        printf("[%d] : %s - %s\n", (int)sorted[i]->pgid, sorted[i]->text,
               sorted[i]->stopped ? "Stopped" : "Running");
    free(sorted);
    return 0;
}

int builtin_ping(int argc, char **argv)
{
    long pid, sig;
    if (argc != 3 || !parse_number(argv[1], &pid) || !parse_number(argv[2], &sig)) {
        fprintf(stderr, "Invalid syntax!\n");
        return 1;
    }
    int actual = (int)((sig % 32 + 32) % 32);
    /* pid 0 or below would signal whole process groups, the shell's own
       included */
    if (pid <= 0 || pid > INT_MAX || kill((pid_t)pid, actual) < 0) {
        fprintf(stderr, "No such process found\n");
        return 1;
    }
    printf("Sent signal %ld to process with pid %ld\n", sig, pid);
    return 0;
}

/* fg and bg take an optional job number; without one they use the newest */
static job_t *job_from_args(int argc, char **argv)
{
    long n;
    if (argc == 1)
        return job_latest();
    if (argc == 2 && parse_number(argv[1], &n) && n > 0 && n <= INT_MAX)
        return job_find((int)n);
    return NULL;
}

int builtin_fg(int argc, char **argv)
{
    if (in_child_shell) {
        fprintf(stderr, "fg: no job control in a pipe or background group\n");
        return 1;
    }
    job_t *job = job_from_args(argc, argv);
    if (job == NULL) {
        fprintf(stderr, "No such job\n");
        return 1;
    }
    printf("%s\n", job->text);
    fflush(stdout);
    if (wait_foreground(job, job->stopped)) {
        printf("[%d] Stopped %s\n", job->number, job->text);
        return 1;
    }
    job_remove(job);
    job_free(job);
    return 0;
}

int builtin_bg(int argc, char **argv)
{
    if (in_child_shell) {
        fprintf(stderr, "bg: no job control in a pipe or background group\n");
        return 1;
    }
    job_t *job = job_from_args(argc, argv);
    if (job == NULL) {
        fprintf(stderr, "No such job\n");
        return 1;
    }
    if (!job->stopped) {
        fprintf(stderr, "Job already running\n");
        return 1;
    }
    kill(-job->pgid, SIGCONT);
    job->stopped = 0;
    printf("[%d] %s &\n", job->number, job->text);
    return 0;
}
