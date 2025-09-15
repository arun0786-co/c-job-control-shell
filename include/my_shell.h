#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>

#define MAX_INPUT 1024
#define MAX_ARGS 64
#define MAX_JOBS 100
#define MAX_HISTORY 15

extern char **environ;

// Command structure for parsing
typedef struct {
    char** args;           // Command arguments
    char* input_file;      // Input redirection file
    char* output_file;     // Output redirection file
    int append_output;     // 1 for >>, 0 for >
} command_t;

typedef struct {
    command_t* commands;   // Array of commands
    int count;            // Number of commands
} pipeline_t;

// Job structure for background processes
typedef struct {
    pid_t pid;            // Process ID
    char* command;        // Command string
    int job_number;       // Job number
    int status;           // 0: running, 1: stopped
} job_t;

// History structure for log command
typedef struct {
    char* commands[MAX_HISTORY];
    int count;
    int current;
} history_t;

// Input Parser
char** parse_input      (char* input);
void free_tokens        (char** tokens);

// Advanced Parser for Part C and D
pipeline_t* parse_pipeline(char* input);
pipeline_t* parse_pipeline_from_tokens(char** tokens);
void free_pipeline(pipeline_t* pipeline);
int execute_pipeline(pipeline_t* pipeline, char** env);
int execute_single_command(command_t* cmd, char** env);
char* get_command_string(pipeline_t* pipeline);

// Sequential and Background Execution (Part D)
typedef struct {
    pipeline_t* pipelines;  // Array of pipelines
    int count;             // Number of pipelines
    int* bg_flags;         // Parallel array: 1 if this pipeline runs in background
} command_sequence_t;

command_sequence_t* parse_command_sequence(char* input);
void free_command_sequence(command_sequence_t* seq);
int execute_command_sequence(command_sequence_t* seq, char** env);

// Shell functions
void display_prompt     (char* home_dir);
void shell_loop         (char** env);

// Built-in function implementations
int command_cd          (char** args, char* initial_directory);
int command_pwd         ();
int command_echo        (char** args, char** env);
int command_env         (char** env);
int command_which       (char** args, char** env);
char** command_setenv   (char** args, char** env);
char** command_unsetenv (char** args, char** env);

// Part B commands
int command_hop         (char** args, char* home_dir, char** prev_dir);
int command_reveal      (char** args, char* home_dir, char** prev_dir);
int command_log         (char** args);

// Part E commands
int command_activities  ();
int command_ping        (char** args);
int command_fg          (char** args);
int command_bg          (char** args);

// Executor
int executor            (char** args, char** env);
int child_process       (char** args, char** env);

// Path functions
char* get_path          (char** env);
char** split_paths      (char* paths, int* count);

// Helpers
int my_strcmp           (const char* str1, const char* str2);
size_t my_strlen        (const char* str);
int my_strncmp          (const char* str1, const char* str2, size_t n);
char* my_getenv         (const char *name, char**env);
char* my_strdup         (const char* str);
char* my_strcpy         (char* dest, const char* src);
char* my_strchr         (const char* str, char c);
char* my_strtok         (char* input_string, const char* delimiter);
char* my_strncpy        (char* dest, const char* src, size_t n);

// Global variables for job control and history
extern job_t jobs[MAX_JOBS];
extern int job_count;
extern int next_job_number;
extern history_t history;

// Job control functions
void init_job_control();
void add_job(pid_t pid, char* command, int background);
void remove_job(pid_t pid);
void update_job_status();
void cleanup_jobs();
void print_job_status(pid_t pid, int status);

// History functions
void init_history(const char* home_dir);
void add_to_history(char* command);
void save_history();
void load_history();
void cleanup_history();

// Signal handlers
void sigint_handler(int sig);
void sigtstp_handler(int sig);
void setup_signal_handlers();