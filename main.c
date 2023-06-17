#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#define MAX_ARGS 512
#define MAX_LENGTH 2048

int foreground_only = 0; // Flag to track foreground-only mode
int last_exit_status = 0; // Variable to store the exit status of the last foreground process
pid_t background_pid = -1; // Variable to store the background process ID

void execute_command(char** args);
void cd_command(char* dir);
void status_command();
void exit_shell();
void handle_sigint(int signal);
void handle_sigtstp(int signal);
void expand_variable(char* arg);

int main() {
    char* args[MAX_ARGS];
    char command[MAX_LENGTH];
    int exit_status = 0;

    // Set up signal handling for SIGINT
    struct sigaction sa_int = {0};
    sa_int.sa_handler = handle_sigint;
    sigfillset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_int, NULL);

    // Set up signal handling for SIGTSTP
    struct sigaction sa_tstp = {0};
    sa_tstp.sa_handler = handle_sigtstp;
    sigfillset(&sa_tstp.sa_mask);
    sa_tstp.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &sa_tstp, NULL);

    while (1) {
        // Print termination message for background processes
        pid_t terminated_pid;
        int status;
        terminated_pid = waitpid(-1, &status, WNOHANG);
        while (terminated_pid > 0) {
            printf("background pid %d is done: ", terminated_pid);
            if (WIFEXITED(status)) {
                printf("exit value %d\n", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                printf("terminated by signal %d\n", WTERMSIG(status));
            }
            fflush(stdout);
            terminated_pid = waitpid(-1, &status, WNOHANG);
        }

        // Prompt for command
        printf(": ");
        fflush(stdout);

        // Read command from user
        if (fgets(command, MAX_LENGTH, stdin) == NULL) {
            clearerr(stdin);
            continue;
        }

        // Remove trailing newline character
        command[strcspn(command, "\n")] = '\0';

        // Check for comment (ignore lines starting with '#')
        if (command[0] == '#') {
            continue;
        }

        // Tokenize command into arguments
        char* token = strtok(command, " ");
        int i = 0;
        while (token != NULL) {
            args[i] = token;
            token = strtok(NULL, " ");
            i++;
        }
        args[i] = NULL;

        // Check for built-in commands or empty command
        if (args[0] == NULL) {
            continue;
        } else if (strcmp(args[0], "exit") == 0) {
            exit_shell();
        } else if (strcmp(args[0], "cd") == 0) {
            cd_command(args[1]);
            continue;
        } else if (strcmp(args[0], "status") == 0) {
            status_command();
            continue;
        }

        // Execute the command
        execute_command(args);
    }

    return 0;
}

void execute_command(char** args) {
    pid_t spawn_pid;
    int exit_status = 0;
    int bg = 0;
    char* input_file = NULL;
    char* output_file = NULL;

    // Check if the command should be run in the background
    int background = 0;
    int command_size = 0;

    // Calculate the command size
    while (args[command_size] != NULL) {
        command_size++;
    }

    if (command_size > 0 && strcmp(args[command_size - 1], "&") == 0) {
        args[command_size - 1] = NULL;
        background = 1;
    }

    // Check for input and output file redirection
    int i = 0;
    while (args[i] != NULL) {
        if (strcmp(args[i], "<") == 0) {
            input_file = args[i + 1];
            args[i] = NULL;
        } else if (strcmp(args[i], ">") == 0) {
            output_file = args[i + 1];
            args[i] = NULL;
        }
        i++;
    }

    // Expand the $$ variable in each argument
    i = 0;
    while (args[i] != NULL) {
        expand_variable(args[i]);
        i++;
    }

    // Fork a new process
    spawn_pid = fork();

    switch (spawn_pid) {
        case -1:
            perror("fork() failed");
            exit(1);
        case 0:
            // Child process

            // Set signal handling for child process
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);

            // Input file redirection
            if (input_file != NULL) {
                int fd_in = open(input_file, O_RDONLY);
                if (fd_in == -1) {
                    perror("open() failed");
                    exit(1);
                }
                if (dup2(fd_in, 0) == -1) {
                    perror("dup2() failed");
                    exit(1);
                }
                close(fd_in);
            }

            // Output file redirection
            if (output_file != NULL) {
                // Check if output file is a directory
                struct stat path_stat;
                stat(output_file, &path_stat);
                if (S_ISDIR(path_stat.st_mode)) {
                    fprintf(stderr, "Output file is a directory\n");
                    exit(1);
                }

                int fd_out = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd_out == -1) {
                    perror("open() failed");
                    exit(1);
                }
                if (dup2(fd_out, 1) == -1) {
                    perror("dup2() failed");
                    exit(1);
                }
                close(fd_out);
            }

            // Execute the command
            if (execvp(args[0], args) == -1) {
                perror("execvp() failed");
                exit(1);
            }
            break;
        default:
            // Parent process

            // Run the command in the foreground or background
            if (background && !foreground_only) {
                printf("background pid is %d\n", spawn_pid);
                fflush(stdout);
                background_pid = spawn_pid; // Store the background process ID
            } else {
                background_pid = -1; // Reset the background process ID
                waitpid(spawn_pid, &exit_status, 0);
                last_exit_status = exit_status;
            }
            break;
    }
}

void cd_command(char* dir) {
    if (dir == NULL) {
        dir = getenv("HOME");
    }

    // Expand the $$ variable in the directory path
    char expanded_dir[MAX_LENGTH];
    char pid_str[16];
    sprintf(pid_str, "%d", getpid());

    char* ptr = strstr(dir, "$$");
    if (ptr != NULL) {
        strncpy(expanded_dir, dir, ptr - dir);
        expanded_dir[ptr - dir] = '\0';
        strcat(expanded_dir, pid_str);
        strcat(expanded_dir, ptr + 2);

        dir = expanded_dir;
    }

    int result = chdir(dir);
    if (result == -1) {
        perror("chdir() failed");
    } else {
        char cwd[MAX_LENGTH];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("Current directory: %s\n", cwd);
        } else {
            perror("getcwd() failed");
        }
    }
}

void status_command() {
    if (WIFEXITED(last_exit_status)) {
        printf("exit value %d\n", WEXITSTATUS(last_exit_status));
    } else if (WIFSIGNALED(last_exit_status)) {
        printf("terminated by signal %d\n", WTERMSIG(last_exit_status));
    }
    fflush(stdout);
}

void exit_shell() {
    // Terminate any running background processes
    if (background_pid != -1) {
        kill(background_pid, SIGKILL);
    }

    exit(0);
}

void handle_sigint(int signal) {
    // Ignore SIGINT signal in the shell program
    struct sigaction sa_int = {0};
    sa_int.sa_handler = SIG_IGN;
    sigfillset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_int, NULL);
}

void handle_sigtstp(int signal) {
    // Toggle foreground-only mode on SIGTSTP signal
    foreground_only = !foreground_only;

    // Print message indicating the mode change
    if (foreground_only) {
        printf("\nEntering foreground-only mode (& is now ignored)\n");
    } else {
        printf("\nExiting foreground-only mode\n");
    }
    fflush(stdout);
}

void expand_variable(char* arg) {
    char expanded[MAX_LENGTH];
    char pid_str[16];
    sprintf(pid_str, "%d", getpid());

    char* ptr = strstr(arg, "$$");
    if (ptr == NULL) {
        return;
    }

    strncpy(expanded, arg, ptr - arg);
    expanded[ptr - arg] = '\0';
    strcat(expanded, pid_str);
    strcat(expanded, ptr + 2);

    strcpy(arg, expanded);
}