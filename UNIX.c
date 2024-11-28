#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 64
#define MAX_PIPES 10

// Function prototypes
void handle_signal(int sig);
void parse_input(char *input, char **args);
int execute_builtin(char **args);
void execute_command(char **args);
void setup_io_redirection(char **args);
void execute_pipeline(char ***commands, int num_commands);
void handle_background_processes(void);
char **split_command(char *command);

// Global variables for background process management
pid_t background_pid = -1;

// Global variables
typedef struct {
    pid_t pid;
    char *command;
} BackgroundProcess;

BackgroundProcess bg_processes[MAX_ARGS];
int bg_process_count = 0;

int main() {
    char input[MAX_INPUT_SIZE];
    char *args[MAX_ARGS];
    
    // Set up signal handling
    signal(SIGCHLD, handle_signal);
    
    while (1) {
        // Display prompt
        printf("myshell$ ");
        fflush(stdout);
        
        // Read input
        if (fgets(input, MAX_INPUT_SIZE, stdin) == NULL) {
            break;
        }
        
        // Remove trailing newline
        input[strcspn(input, "\n")] = 0;
        
        // Parse input into arguments
        parse_input(input, args);
        
        // Check if there are any arguments
        if (args[0] == NULL) {
            continue;
        }
        
        // Check for built-in commands
        if (execute_builtin(args) == 0) {
            continue;
        }
        
        // Execute external command
        execute_command(args);
    }
    
    return 0;
}

void handle_signal(int sig) {
    if (sig == SIGCHLD) {
        // Reap zombie processes
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }
}

void parse_input(char *input, char **args) {
    int i = 0;
    char *token = strtok(input, " \t");
    
    while (token != NULL && i < MAX_ARGS - 1) {
        args[i++] = token;
        token = strtok(NULL, " \t");
    }
    args[i] = NULL;
    
    // Handle quotes for arguments with spaces
    int in_quotes = 0;
    char quote_char = '\0';
    char *curr_pos = input;
    char *token_start = input;
    int i = 0;
    
    while (*curr_pos != '\0' && i < MAX_ARGS - 1) {
        if (*curr_pos == '"' || *curr_pos == '\'') {
            if (!in_quotes) {
                in_quotes = 1;
                quote_char = *curr_pos;
                token_start = curr_pos + 1;
            } else if (*curr_pos == quote_char) {
                in_quotes = 0;
                *curr_pos = '\0';
                args[i++] = token_start;
            }
        } else if ((*curr_pos == ' ' || *curr_pos == '\t') && !in_quotes) {
            if (curr_pos > token_start) {
                *curr_pos = '\0';
                args[i++] = token_start;
            }
            token_start = curr_pos + 1;
        }
        curr_pos++;
    }
    
    if (curr_pos > token_start && i < MAX_ARGS - 1) {
        args[i++] = token_start;
    }
    args[i] = NULL;
}

int execute_builtin(char **args) {
    if (strcmp(args[0], "exit") == 0) {
        exit(0);
    }
    
    if (strcmp(args[0], "cd") == 0) {
        if (args[1] == NULL) {
            fprintf(stderr, "cd: missing argument\n");
        } else if (chdir(args[1]) != 0) {
            perror("cd");
        }
        return 0;
    }
    
    return 1;
}

void execute_command(char **args) {
    // Check for pipes
    char ***commands = malloc(MAX_PIPES * sizeof(char **));
    int num_commands = 0;
    int pipe_pos = 0;
    
    // Split commands by pipe
    commands[0] = malloc(MAX_ARGS * sizeof(char *));
    int j = 0;
    
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            commands[num_commands][j] = NULL;
            num_commands++;
            commands[num_commands] = malloc(MAX_ARGS * sizeof(char *));
            j = 0;
        } else {
            commands[num_commands][j++] = args[i];
        }
    }
    commands[num_commands][j] = NULL;
    num_commands++;
    
    if (num_commands > 1) {
        execute_pipeline(commands, num_commands);
        // Cleanup
        for (int i = 0; i < num_commands; i++) {
            free(commands[i]);
        }
        free(commands);
        return;
    }
    
    // Handle I/O redirection and background processes
    int background = 0;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "&") == 0) {
            background = 1;
            args[i] = NULL;
            break;
        }
    }
    
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return;
    }
    
    if (pid == 0) {
        // Child process
        setup_io_redirection(args);
        execvp(args[0], args);
        perror("execvp");
        exit(1);
    } else {
        // Parent process
        if (background) {
            bg_processes[bg_process_count].pid = pid;
            bg_processes[bg_process_count].command = strdup(args[0]);
            bg_process_count++;
            printf("[%d] %s\n", pid, args[0]);
        } else {
            waitpid(pid, NULL, 0);
        }
    }
}

void setup_io_redirection(char **args) {
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0) {
            int fd = open(args[i + 1], O_RDONLY);
            if (fd < 0) {
                perror("open");
                exit(1);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
            args[i] = NULL;
            i++;
        } else if (strcmp(args[i], ">") == 0) {
            int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("open");
                exit(1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            args[i] = NULL;
            i++;
        }
    }
}

void execute_pipeline(char ***commands, int num_commands) {
    int pipes[MAX_PIPES][2];
    pid_t pids[MAX_PIPES + 1];
    
    // Create pipes
    for (int i = 0; i < num_commands - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return;
        }
    }
    
    // Create processes
    for (int i = 0; i < num_commands; i++) {
        pids[i] = fork();
        
        if (pids[i] < 0) {
            perror("fork");
            return;
        }
        
        if (pids[i] == 0) {
            // Child process
            
            // Set up input from previous pipe
            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            
            // Set up output to next pipe
            if (i < num_commands - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            
            // Close all pipe fds
            for (int j = 0; j < num_commands - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            setup_io_redirection(commands[i]);
            execvp(commands[i][0], commands[i]);
            perror("execvp");
            exit(1);
        }
    }
    
    // Parent closes all pipe fds
    for (int i = 0; i < num_commands - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    // Wait for all children
    for (int i = 0; i < num_commands; i++) {
        waitpid(pids[i], NULL, 0);
    }
}
