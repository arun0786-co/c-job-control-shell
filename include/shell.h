#ifndef SHELL_H
#define SHELL_H

#include <sys/types.h>

/*
 * Parse tree for one line of input, following the grammar in the spec:
 *
 *   shell_cmd -> cmd_group ((& | ;) cmd_group)* &?
 *   cmd_group -> atomic (| atomic)*
 *   atomic    -> name (name | input | output)*
 */

typedef enum { REDIR_IN, REDIR_OUT, REDIR_APPEND } redir_type_t;

typedef struct {
    redir_type_t type;
    char *file;
} redir_t;

/* one atomic command: argv plus its redirections, in the order typed */
typedef struct {
    char **argv; /* NULL-terminated */
    int argc;
    redir_t *redirs;
    int nredirs;
} command_t;

/* commands joined by pipes */
typedef struct {
    command_t *cmds;
    int ncmds;
    int background; /* followed by & */
    char *text;     /* the group as typed, used as the job's name */
} group_t;

typedef struct {
    group_t *groups;
    int ngroups;
} shell_cmd_t;

/* util.c */
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int parse_number(const char *s, long *out);

/* parser.c */
/* returns 1 and sets *out on success, 0 for a blank line, -1 on a syntax
   error */
int parse_line(const char *line, shell_cmd_t **out);
void free_shell_cmd(shell_cmd_t *sc);

/* executor.c */
void run_line(const char *line, int store_in_history);
int apply_redirections(const command_t *cmd);

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
    char *text;
} job_t;

extern int interactive;
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
