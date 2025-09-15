#include "my_shell.h"

// Global variables for job control
job_t jobs[MAX_JOBS];
int job_count = 0;
int next_job_number = 1;
history_t history;
static char history_path[1024];

// Initialize job control
void init_job_control()
{
    job_count = 0;
    next_job_number = 1;
    for (int i = 0; i < MAX_JOBS; i++) {
        jobs[i].pid = 0;
        jobs[i].command = NULL;
        jobs[i].job_number = 0;
        jobs[i].status = 0;
    }
}

// Add a job to the job list
void add_job(pid_t pid, char* command, int background)
{
    if (job_count >= MAX_JOBS) {
        return; // No space for more jobs
    }
    
    jobs[job_count].pid = pid;
    jobs[job_count].command = my_strdup(command);
    jobs[job_count].job_number = next_job_number++;
    jobs[job_count].status = background ? 0 : 1; // 0: running, 1: stopped
    
    job_count++;
}

// Remove a job from the job list
void remove_job(pid_t pid)
{
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].pid == pid) {
            if (jobs[i].command) {
                free(jobs[i].command);
            }
            
            // Shift remaining jobs
            for (int j = i; j < job_count - 1; j++) {
                jobs[j] = jobs[j + 1];
            }
            
            job_count--;
            break;
        }
    }
}

// Update job status by checking which processes have terminated
void update_job_status()
{
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].pid > 0) {
            int status;
            pid_t result = waitpid(jobs[i].pid, &status, WNOHANG);
            
            if (result > 0) {
                // Process has terminated
                print_job_status(jobs[i].pid, status);
                remove_job(jobs[i].pid);
                i--; // Adjust index since we removed an element
            }
        }
    }
}

// Print job status when a process terminates
void print_job_status(pid_t pid, int status)
{
    // Find the job
    char* command = NULL;
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].pid == pid) {
            command = jobs[i].command;
            break;
        }
    }
    
    if (!command) {
        command = "unknown";
    }
    
    if (WIFEXITED(status)) {
        printf("%s with pid %d exited normally\n", command, pid);
    } else if (WIFSIGNALED(status)) {
        printf("%s with pid %d exited abnormally\n", command, pid);
    }
}

// Clean up all jobs
void cleanup_jobs()
{
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].pid > 0) {
            kill(jobs[i].pid, SIGKILL);
        }
        if (jobs[i].command) {
            free(jobs[i].command);
            jobs[i].command = NULL;
        }
    }
    job_count = 0;
}

// Initialize history
void init_history(const char* home_dir)
{
    history.count = 0;
    history.current = 0;
    for (int i = 0; i < MAX_HISTORY; i++) {
        history.commands[i] = NULL;
    }
    // Prefer the user's actual home directory for history storage
    const char* user_home = my_getenv("HOME", environ);
    if (!user_home || !user_home[0]) {
        #include <pwd.h>
        uid_t uid = geteuid();
        struct passwd* pw = getpwuid(uid);
        if (pw && pw->pw_dir) {
            user_home = pw->pw_dir;
        }
    }
    const char* base_dir = (user_home && user_home[0]) ? user_home : home_dir;
    if (base_dir && base_dir[0]) {
        size_t len = my_strlen(base_dir);
        if (len + 16 < sizeof(history_path)) {
            snprintf(history_path, sizeof(history_path), "%s/%s", base_dir, ".shell_history");
        } else {
            snprintf(history_path, sizeof(history_path), ".shell_history");
        }
    } else {
        snprintf(history_path, sizeof(history_path), ".shell_history");
    }
    load_history();
}

// Add command to history
void add_to_history(char* command)
{
    if (!command || my_strlen(command) == 0) {
        return;
    }
    
    // Don't add if it's the same as the last command
    if (history.count > 0 && my_strcmp(history.commands[history.count - 1], command) == 0) {
        return;
    }
    
    // Don't add log commands
    if (my_strncmp(command, "log", 3) == 0) {
        return;
    }
    
    // If history is full, shift commands
    if (history.count >= MAX_HISTORY) {
        free(history.commands[0]);
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            history.commands[i] = history.commands[i + 1];
        }
        history.count = MAX_HISTORY - 1;
    }
    
    history.commands[history.count] = my_strdup(command);
    history.count++;
    
    save_history();
}

// Save history to file
void save_history()
{
    FILE* file = fopen(history_path[0] ? history_path : ".shell_history", "w");
    if (!file) return;
    
    for (int i = 0; i < history.count; i++) {
        fprintf(file, "%s\n", history.commands[i]);
    }
    
    fclose(file);
}

// Load history from file
void load_history()
{
    FILE* file = fopen(history_path[0] ? history_path : ".shell_history", "r");
    if (!file) return;
    
    char line[MAX_INPUT];
    while (fgets(line, sizeof(line), file) && history.count < MAX_HISTORY) {
        // Remove newline
        line[strcspn(line, "\n")] = '\0';
        if (my_strlen(line) > 0) {
            history.commands[history.count] = my_strdup(line);
            history.count++;
        }
    }
    
    fclose(file);
}

// Clean up history
void cleanup_history()
{
    for (int i = 0; i < history.count; i++) {
        if (history.commands[i]) {
            free(history.commands[i]);
            history.commands[i] = NULL;
        }
    }
    history.count = 0;
}

// Signal handlers
void sigint_handler(int sig)
{
    (void)sig;
    // Send SIGINT to foreground process group
    // For now, just ignore it in the shell
}

void sigtstp_handler(int sig)
{
    (void)sig;
    // Send SIGTSTP to foreground process group
    // For now, just ignore it in the shell
}

// Setup signal handlers
void setup_signal_handlers()
{
    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, sigtstp_handler);
    // Do not ignore SIGCHLD; foreground waits rely on waitpid.
}
