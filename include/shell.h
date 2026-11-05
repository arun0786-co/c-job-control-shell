#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>
#include <termios.h>

/*
 * Parse tree for one line of input:
 *
 *   shell_cmd -> and_or ((& | ;) and_or)* (& | ;)?
 *   and_or    -> cmd_group ((&& | "||") cmd_group)*
 *   cmd_group -> atomic (| atomic)*
 *   atomic    -> name (name | input | output)*
 *
 * With --strict the grammar is the course's: no && or ||, and only & may end
 * the line.
 */

typedef enum { REDIR_IN, REDIR_OUT, REDIR_APPEND, REDIR_DUP } redir_type_t;

typedef struct {
    redir_type_t type;
    int fd;     /* the descriptor being redirected */
    int dup_to; /* for 2>&1 */
    char *file; /* NULL for REDIR_DUP */
} redir_t;

/* one atomic command: argv plus its redirections, in the order typed */
typedef struct {
    char **argv; /* NULL-terminated */
    int argc;
    redir_t *redirs;
    int nredirs;
    char **assigns; /* NAME=value settings in front of the command */
    int nassigns;
} command_t;

typedef enum { RUN_ALWAYS, RUN_IF_OK, RUN_IF_FAILED } run_if_t;

/* commands joined by pipes */
typedef struct {
    command_t *cmds;
    int ncmds;
    run_if_t run_if; /* came after nothing, && or || */
    int background;  /* followed by & */
    size_t start, end; /* where it is in the line */
    char *text;      /* the group as typed, used as the job's name */
} group_t;

typedef struct {
    group_t *groups;
    int ngroups;
    char *line;
} shell_cmd_t;

extern int strict_grammar; /* --strict: only what the course's grammar allows */
extern int last_status;    /* of the last command; $? */

/* util.c */
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int parse_number(const char *s, long *out);

/* a NULL-terminated list of strings */
typedef struct {
    char **items;
    int n;
} strlist_t;

/* expand.c */
extern int substitution_ran; /* a $(...) ran since this was last cleared */
void strlist_push(strlist_t *list, const char *s);
void strlist_free(strlist_t *list);
void expand_command(const command_t *cmd, command_t *out);
void free_command(command_t *cmd);

/* parser.c */
/* from a quote, a backslash or $( at s[i], returns the position just past its
   end, or (size_t)-1 if it never ends. any other character is just itself. */
size_t skip_quoted(const char *s, size_t i);
/* returns 1 and sets *out on success, 0 for a blank line, -1 on a syntax
   error */
int parse_line(const char *line, shell_cmd_t **out);
void free_shell_cmd(shell_cmd_t *sc);

/* executor.c */
extern pid_t shell_pid;           /* $$, the same in every forked copy */
extern pid_t last_background_pid; /* $! */
void run_line(const char *line, int store_in_history);
int apply_redirections(const command_t *cmd);
void enter_subshell(pid_t pgid);

/* builtins.c */
typedef int (*builtin_fn)(int argc, char **argv);
void builtins_init(const char *home);
builtin_fn find_builtin(const char *name);
const char *shell_home(void);

/* jobs.c */
typedef struct {
    int number;
    pid_t pgid;
    pid_t *pids; /* one per command in the pipeline */
    int npids;
    int live;        /* processes not reaped yet */
    int last_status; /* wait status of the last command in the pipeline */
    int stopped;
    struct termios modes; /* terminal modes when it last stopped */
    int has_modes;
    char *text;
} job_t;

extern int interactive;
extern int in_child_shell; /* this process is a forked copy of the shell */
void jobs_init(void);
job_t *job_new(const char *text, int ncmds);
void job_free(job_t *job);
void job_add(job_t *job);
void job_remove(job_t *job);
job_t *job_find(int number);
job_t *job_latest(void);
int wait_foreground(job_t *job, int resume);
void reset_child_signals(void);
void jobs_check(void);
void jobs_kill_all(void);
int job_exit_status(const job_t *job);
void job_start_background(job_t *job, pid_t pid);
int builtin_activities(int argc, char **argv);
int builtin_ping(int argc, char **argv);
int builtin_fg(int argc, char **argv);
int builtin_bg(int argc, char **argv);

/* history.c */
void history_init(const char *fallback_dir);
void history_add(const char *line);
int history_count(void);
const char *history_get(int i); /* 0 is the oldest */
void history_purge(void);

/* prompt.c */
void prompt_init(void);
void print_prompt(void);

#endif
