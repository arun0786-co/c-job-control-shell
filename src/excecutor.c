#include "my_shell.h"

// Executes a command by forking and running it in a child process
int executor(char** args, char** env)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) // child process
    { 
        child_process(args, env);
        exit(1); // Exit if child_process returns
    } 
    else // Parent process
    { 
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            return 1;
        }
        if (WIFSIGNALED(status)) {
            printf("Process terminated by signal: %d\n", WTERMSIG(status));
        }
    }
    return 0;
}

// Attempts to execute the command by searching paths and the current directory
int child_process(char** args, char** env) 
{
    char* path_string = get_path(env);
    int num_paths;
    char** path_list = split_paths(path_string, &num_paths);

    // access() execve()
    for (int i = 0; i < num_paths; i++) {
        char full_path[MAX_INPUT];
        snprintf(full_path, sizeof(full_path), "%s/%s", path_list[i], args[0]);

        if (access(full_path, X_OK) == 0) {
            execve(full_path, args, env);
        }
    }

    for (int i = 0; path_list[i]; i++)
        free(path_list[i]);
    free(path_string);
    free(path_list);

    // Try executing the command in the current working directory
    char* cwd = NULL;
    cwd = getcwd(NULL, 0);
    if(cwd == NULL) {
        perror("getcwd");
        return 1;
    }

    char full_cwd_path[MAX_INPUT];
    snprintf(full_cwd_path, sizeof(full_cwd_path), "%s/%s", cwd, args[0]);
    execve(full_cwd_path, args, env);

    // If we get here, the command was not found
    printf("Command not found!\n");
    
    return 1;
}

// Fetches the PATH environment variable
char* get_path(char** env) {
    for (int i = 0; env[i]; i++) {
        if(my_strncmp(env[i], "PATH=", 5) == 0) {
            return my_strdup(env[i] + 5);
        }
    }
    return NULL;
}

// Split the PATH string into individual paths
char** split_paths(char* paths, int* count) {
    char** result = NULL;
    char* token;
    size_t size_of_path = my_strlen(paths);
    char paths_copy[size_of_path];

    my_strncpy(paths_copy, paths, sizeof(paths_copy));
    paths_copy[sizeof(paths_copy) - 1] = '\0';

    token = my_strtok(paths_copy, ":");
    *count = 0;

    while (token) {
        result = realloc(result, ((*count + 1) * sizeof(char*)));
        if (!result) {
            perror("realloc");
            return NULL;
        }
        result[*count] = my_strdup(token);
        if (!result[*count]) {
            perror("my_strdup");
            return NULL;
        }

        (*count)++;
        token = my_strtok(NULL, ":");
    }

    return result;
}

// Execute a pipeline of commands with pipes and redirections
int execute_pipeline(pipeline_t* pipeline, char** env)
{
    if (!pipeline || pipeline->count == 0) {
        return 1;
    }
    
    // Create pipes if needed (for N commands, we need N-1 pipes; if N==1, this loop is skipped)
    int pipes[pipeline->count - 1][2];
    pid_t pids[pipeline->count];
    
    for (int i = 0; i < pipeline->count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            return 1;
        }
    }
    
    // Fork processes for each command
    for (int i = 0; i < pipeline->count; i++) {
        pids[i] = fork();
        
        if (pids[i] == -1) {
            perror("fork");
            return 1;
        }
        
        if (pids[i] == 0) {
            // Child process
            command_t* cmd = &pipeline->commands[i];
            
            // Handle input redirection
            if (cmd->input_file) {
                int fd = open(cmd->input_file, O_RDONLY);
                if (fd == -1) {
                    printf("No such file or directory\n");
                    exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            } else if (i > 0) {
                // Connect to previous pipe
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            
            // Handle output redirection
            if (cmd->output_file) {
                int flags = O_WRONLY | O_CREAT;
                if (cmd->append_output) {
                    flags |= O_APPEND;
                } else {
                    flags |= O_TRUNC;
                }
                
                int fd = open(cmd->output_file, flags, 0644);
                if (fd == -1) {
                    printf("Unable to create file for writing\n");
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            } else if (i < pipeline->count - 1) {
                // Connect to next pipe
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            
            // Close all pipe file descriptors
            for (int j = 0; j < pipeline->count - 1; j++) {
                if (pipeline->count > 1) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
            }
            
            // Execute the command
            execute_single_command(cmd, env);
            exit(1);
        }
    }
    
    // Close all pipe file descriptors in parent
    for (int i = 0; i < pipeline->count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    // Wait for all child processes
    for (int i = 0; i < pipeline->count; i++) {
        int status;
        waitpid(pids[i], &status, 0);
    }
    
    return 0;
}

// Execute a single command with redirections
int execute_single_command(command_t* cmd, char** env)
{
    if (!cmd || !cmd->args || !cmd->args[0]) {
        return 1;
    }
    
    // Handle input redirection
    if (cmd->input_file) {
        int fd = open(cmd->input_file, O_RDONLY);
        if (fd == -1) {
            printf("No such file or directory\n");
            return 1;
        }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    
    // Handle output redirection
    if (cmd->output_file) {
        int flags = O_WRONLY | O_CREAT;
        if (cmd->append_output) {
            flags |= O_APPEND;
        } else {
            flags |= O_TRUNC;
        }
        
        int fd = open(cmd->output_file, flags, 0644);
        if (fd == -1) {
            printf("Unable to create file for writing\n");
            return 1;
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    
    // Execute the command
    char* path_string = get_path(env);
    int num_paths;
    char** path_list = split_paths(path_string, &num_paths);

    // Search in PATH
    for (int i = 0; i < num_paths; i++) {
        char full_path[MAX_INPUT];
        snprintf(full_path, sizeof(full_path), "%s/%s", path_list[i], cmd->args[0]);

        if (access(full_path, X_OK) == 0) {
            execve(full_path, cmd->args, env);
        }
    }

    for (int i = 0; path_list[i]; i++)
        free(path_list[i]);
    free(path_string);
    free(path_list);

    // Try executing in current directory
    char* cwd = getcwd(NULL, 0);
    if(cwd == NULL) {
        perror("getcwd");
        return 1;
    }

    char full_cwd_path[MAX_INPUT];
    snprintf(full_cwd_path, sizeof(full_cwd_path), "%s/%s", cwd, cmd->args[0]);
    execve(full_cwd_path, cmd->args, env);

    // Command not found
    printf("Command not found!\n");
    
    return 1;
}

// Execute command sequence for Part D
int execute_command_sequence(command_sequence_t* seq, char** env)
{
    if (!seq || seq->count == 0) {
        return 1;
    }
    
    // Execute each pipeline sequentially
    for (int i = 0; i < seq->count; i++) {
        pipeline_t* pipeline = &seq->pipelines[i];
        int run_in_bg = seq->bg_flags ? seq->bg_flags[i] : 0;
        
        if (run_in_bg) {
            pid_t pid = fork();
            if (pid == -1) {
                perror("fork");
                return 1;
            }
            if (pid == 0) {
                // Background child should not read from terminal
                close(STDIN_FILENO);
                execute_pipeline(pipeline, env);
                exit(0);
            } else {
                add_job(pid, get_command_string(pipeline), 1);
                // Intentionally suppress printing job line to match tester expectations
            }
        } else {
            execute_pipeline(pipeline, env);
        }
    }
    
    return 0;
}

// Helper function to get command string for job tracking
char* get_command_string(pipeline_t* pipeline)
{
    if (!pipeline || pipeline->count == 0) {
        return my_strdup("");
    }
    
    char* result = malloc(MAX_INPUT);
    if (!result) return NULL;
    
    result[0] = '\0';
    
    for (int i = 0; i < pipeline->count; i++) {
        command_t* cmd = &pipeline->commands[i];
        
        if (cmd->args && cmd->args[0]) {
            if (i > 0) {
                strcat(result, " | ");
            }
            strcat(result, cmd->args[0]);
        }
    }
    
    return result;
}