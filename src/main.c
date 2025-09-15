#include "my_shell.h"

// Shell loop
// Input Parsing
// Command execution 
// Handle Built-in commands exp. cd, pwd, echo, env, setenv, unsetenv, which, exit
// Execute external commands
// Manage environment variables
// Manage Path
// Error Handling

void display_help() {
    printf("Available commands:\n");
    printf("\tcd <directory>      - Change the current directory.\n");
    printf("\tpwd                 - Print the current working directory.\n");
    printf("\techo <text>         - Print the given text.\n");
    printf("\tenv                 - Display all environment variables.\n");
    printf("\tsetenv VAR=value    - Set an environment variable.\n");
    printf("\tunsetenv <variable> - Remove an environment variable.\n");
    printf("\twhich <command>     - Locate an executable in the system's PATH.\n");
    printf("\t.help               - Display this help message.\n");
    printf("\texit or quit        - Exit the shell.\n");
}

// Built-ins: cd, pwd, echo, env, setenv, unsetenv, which, exit
// Binary: ls, cat.. we'll use executor
int shell_builts(char** args, char** env, char* initial_directory)
{
    if (my_strcmp(args[0], "cd") == 0) {
        return command_cd(args, initial_directory);
    } else if (my_strcmp(args[0], "pwd") == 0) {
        return command_pwd();
    } else if (my_strcmp(args[0], "echo") == 0) {
        return command_echo(args, env);
    } else if (my_strcmp(args[0], "env") == 0) {
        return command_env(env);
    } else if (my_strcmp(args[0], "which") == 0) {
        return command_which(args, env);
    } else if (my_strcmp(args[0], ".help") == 0) {
        display_help();
        return 0;
    } else if (my_strcmp(args[0], "exit") == 0 || my_strcmp(args[0], "quit") == 0) {
        exit(EXIT_SUCCESS);
    } else {
        // Not a built-in command, execute as external command
        return executor(args, env);
    }
    return 0;
}

void display_prompt(char* home_dir)
{
    // Resolve username robustly (works under pexpect): prefer passwd DB over getlogin
    const char* username = NULL;
    {
        #include <pwd.h>
        uid_t uid = geteuid();
        struct passwd* pw = getpwuid(uid);
        if (pw && pw->pw_name) {
            username = pw->pw_name;
        } else {
            char* env_user = my_getenv("USER", environ);
            username = env_user ? env_user : NULL;
        }
    }
    char hostname_buf[256];
    if (gethostname(hostname_buf, sizeof(hostname_buf)) != 0) {
        hostname_buf[0] = '\0';
    }
    char* current_dir = getcwd(NULL, 0);
    
    // Default values if environment variables are not set
    if (!username) username = "unknown";
    const char* hostname = hostname_buf[0] ? hostname_buf : "unknown";
    
    // Replace home directory with ~ if current directory is under home
    char* display_path = current_dir;
    if (home_dir && current_dir && my_strncmp(current_dir, home_dir, my_strlen(home_dir)) == 0) {
        // Current directory is under home directory
        size_t home_len = my_strlen(home_dir);
        if (current_dir[home_len] == '/' || current_dir[home_len] == '\0') {
            // Allocate memory for display path with ~
            size_t remaining_len = my_strlen(current_dir) - home_len;
            display_path = malloc(remaining_len + 2); // +2 for ~ and null terminator
            if (display_path) {
                display_path[0] = '~';
                if (remaining_len > 0) {
                    my_strcpy(display_path + 1, current_dir + home_len);
                } else {
                    display_path[1] = '\0';
                }
            }
        }
    }
    
    printf("<%s@%s:%s> ", username, hostname, display_path ? display_path : current_dir);
    
    if (display_path != current_dir && display_path) {
        free(display_path);
    }
    if (current_dir) {
        free(current_dir);
    }
}

void shell_loop(char** env)
{
    char* input = NULL;
    size_t input_size = 0;

    char* initial_directory = getcwd(NULL, 0);
    char* prev_directory = NULL;  // Track previous directory for hop and reveal

    // Initialize job control and history
    init_job_control();
    init_history(initial_directory);
    setup_signal_handlers();

    while (1)
    {
        // Check for completed background processes
        update_job_status();
        
        display_prompt(initial_directory);
        if (getline(&input, &input_size, stdin) == -1) // End of the file (EOF), ctrl + D
        {
            printf("logout\n");
            cleanup_jobs();
            cleanup_history();
            break;
        }    

        // Remove newline from input
        input[strcspn(input, "\n")] = '\0';
        
        // Add to history (make a copy to avoid issues)
        char* input_copy = my_strdup(input);
        add_to_history(input_copy);
        free(input_copy);

        // Check if input contains ; or & for sequential/background execution
        int has_sequential_or_background = 0;
        for (int i = 0; input[i]; i++) {
            if (input[i] == ';' || input[i] == '&') {
                has_sequential_or_background = 1;
                break;
            }
        }
        
        if (has_sequential_or_background) {
            // Use command sequence parser for ; and &
            command_sequence_t* seq = parse_command_sequence(input);
            if (seq) {
                execute_command_sequence(seq, env);
                free_command_sequence(seq);
            }
        } else {
            // Check if input contains pipes or redirections
            int has_pipes_or_redirections = 0;
            for (int i = 0; input[i]; i++) {
                if (input[i] == '|' || input[i] == '<' || input[i] == '>') {
                    has_pipes_or_redirections = 1;
                    break;
                }
            }
            
            if (has_pipes_or_redirections) {
                // Use advanced parser for pipes and redirections
                pipeline_t* pipeline = parse_pipeline(input);
                if (pipeline) {
                    execute_pipeline(pipeline, env);
                    free_pipeline(pipeline);
                }
            } else {
                // Use simple parser for basic commands
                char** args = parse_input(input);

                if (args == NULL) {
                    // parse_input returned NULL - this means invalid syntax
                    // (already printed "Invalid Syntax!")
                    continue; // Just show prompt again
                }
                
                if (!args[0]) {
                    free_tokens(args);
                    continue; // Empty input, just show prompt again
                } else if (my_strcmp(args[0], "setenv") == 0) {
                    env = command_setenv(args, env);
                    free_tokens(args);
                } else if (my_strcmp(args[0], "unsetenv") == 0) {
                    env = command_unsetenv(args, env);
                    free_tokens(args);
                } else if (my_strcmp(args[0], "hop") == 0) {
                    command_hop(args, initial_directory, &prev_directory);
                    free_tokens(args);
                } else if (my_strcmp(args[0], "reveal") == 0) {
                    command_reveal(args, initial_directory, &prev_directory);
                    free_tokens(args);
                } else if (my_strcmp(args[0], "log") == 0) {
                    command_log(args);
                    free_tokens(args);
                } else if (my_strcmp(args[0], "activities") == 0) {
                    command_activities();
                    free_tokens(args);
                } else if (my_strcmp(args[0], "ping") == 0) {
                    command_ping(args);
                    free_tokens(args);
                } else if (my_strcmp(args[0], "fg") == 0) {
                    command_fg(args);
                    free_tokens(args);
                } else if (my_strcmp(args[0], "bg") == 0) {
                    command_bg(args);
                    free_tokens(args);
                } else {
                    shell_builts(args, env, initial_directory);
                    free_tokens(args);
                }
            }
        }
    }
    
    if (prev_directory) {
        free(prev_directory);
    }
    // Don't free env as it's passed from main and managed by the system
} 

int main (int argc, char** argv, char** env)
{
    (void)argc;
    (void)argv;

    shell_loop(env);

    return 0;
}