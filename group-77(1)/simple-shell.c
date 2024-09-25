#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

#define INPUT_MAX 1024
#define ARGS_MAX 100
#define HISTORY_MAX 100

typedef struct {
    char *command;
    pid_t pid;
    time_t start_time;
    time_t end_time;
    int is_background;
} CommandLog;

CommandLog command_history[HISTORY_MAX];
int history_count = 0;

void removeLeadingTrailingSpaces(char *str) {
    char *end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
}

int splitCommandIntoArgs(char *input, char **args) {
    char *token;
    int index = 0;
    int is_background = 0;

    token = strtok(input, " \n");
    while (token != NULL && index < ARGS_MAX - 1) {
        if (strcmp(token, "&") == 0) {
            is_background = 1;
            break;
        }
        args[index++] = token;
        token = strtok(NULL, " \n");
    }
    args[index] = NULL;
    return is_background;
}

void recordCommand(char *cmd, pid_t pid, int is_background) {
    if (history_count < HISTORY_MAX) {
        command_history[history_count].command = strdup(cmd);
        command_history[history_count].pid = pid;
        command_history[history_count].start_time = time(NULL);
        command_history[history_count].end_time = 0;
        command_history[history_count].is_background = is_background;
        history_count++;
    }
}

void markCommandAsFinished(pid_t pid) {
    for (int i = history_count - 1; i >= 0; i--) {
        if (command_history[i].pid == pid && command_history[i].end_time == 0) {
            command_history[i].end_time = time(NULL);
            break;
        }
    }
}

int executeCommand(char **args, int is_background) {
    pid_t pid;
    int status;

    pid = fork();
    if (pid == 0) {
        if (execvp(args[0], args) == -1) {
            perror("SimpleShell");
        }
        exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("SimpleShell");
    } else {
        recordCommand(args[0], pid, is_background);
        if (!is_background) {
            do {
                waitpid(pid, &status, WUNTRACED);
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
            markCommandAsFinished(pid);
        } else {
            printf("[%d] %s\n", pid, args[0]);
        }
    }

    return 1;
}

void handlePipedCommands(char *input) {
    int max_pipes = 10;  
    int pipes[10][2];  
    char *commands[11];  
    int command_count = 0;

    char *token = strtok(input, "|");
    while (token != NULL && command_count < max_pipes + 1) {
        commands[command_count++] = token;
        token = strtok(NULL, "|");
    }

    for (int i = 0; i < command_count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("Pipe creation failed");
            return;
        }
    }

    for (int i = 0; i < command_count; i++) {
        pid_t pid = fork();

        if (pid == 0) {  

            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            }

         
            if (i < command_count - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            for (int j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

          
            char *args[ARGS_MAX];
            splitCommandIntoArgs(commands[i], args);
            execvp(args[0], args);
            perror("Command execution failed");
            exit(EXIT_FAILURE);
        } else if (pid < 0) {
            perror("Fork failed");
            return;
        }
    }

   
    for (int i = 0; i < command_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (int i = 0; i < command_count; i++) {
        wait(NULL);
    }
}

void handleInputOutputRedirection(char *input, int direction) {
    char *filename;
    int fd;
    char *args[ARGS_MAX];

    if (direction == 0) {
        filename = strchr(input, '<') + 1;
        *strchr(input, '<') = '\0';
    } else {
        filename = strchr(input, '>') + 1;
        *strchr(input, '>') = '\0';
    }

    removeLeadingTrailingSpaces(filename);
    splitCommandIntoArgs(input, args);

    fd = (direction == 0) ? open(filename, O_RDONLY) : open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("File operation failed");
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd, direction == 0 ? STDIN_FILENO : STDOUT_FILENO);
        close(fd);
        execvp(args[0], args);
        perror("Command execution failed");
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        close(fd);
        waitpid(pid, NULL, 0);
    } else {
        perror("Fork failed");
    }
}

void showCommandHistory() {
    for (int i = 0; i < history_count; i++) {
        printf("%d: %s\n", i + 1, command_history[i].command);
    }
}

void printExecutionSummary() {
    printf("\nExecution Summary:\n");
    for (int i = 0; i < history_count; i++) {
        printf("Command: %s\n", command_history[i].command);
        printf("  PID: %d\n", command_history[i].pid);
        printf("  Start Time: %s", ctime(&command_history[i].start_time));
        if (command_history[i].end_time) {
            printf("  End Time: %s", ctime(&command_history[i].end_time));
            printf("  Duration: %.2f seconds\n", 
                   difftime(command_history[i].end_time, command_history[i].start_time));
        } else {
            printf("  (Background process or terminated)\n");
        }
        printf("  Background: %s\n", command_history[i].is_background ? "Yes" : "No");
        printf("\n");
    }
}

void cleanupBackgroundProcesses() {
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < history_count; i++) {
            if (command_history[i].pid == pid && command_history[i].is_background) {
                printf("[%d] Done    %s\n", pid, command_history[i].command);
                markCommandAsFinished(pid);
                break;
            }
        }
    }
}


int main() {
    char input[INPUT_MAX];
    char *args[ARGS_MAX];

    while (1) {
        printf("SimpleShell> ");
        
        if (fgets(input, INPUT_MAX, stdin) == NULL) {
            if (feof(stdin)) {
                printf("\nExiting shell.\n");
                break;
            }
            perror("Input error");
            continue;
        }

        input[strcspn(input, "\n")] = 0;

        if (strlen(input) == 0) continue;

        if (strcmp(input, "exit") == 0) {
            break;
        }

        cleanupBackgroundProcesses();

        // Check for pipe character anywhere in the input
        if (strchr(input, '|') != NULL) {
            handlePipedCommands(input);
        } else if (strchr(input, '<')) {
            handleInputOutputRedirection(input, 0);
        } else if (strchr(input, '>')) {
            handleInputOutputRedirection(input, 1);
        } else {
            int is_background = splitCommandIntoArgs(input, args);
            if (strcmp(args[0], "history") == 0) {
                showCommandHistory();
            } else {
                executeCommand(args, is_background);
            }
        }
    }

    printExecutionSummary();

    for (int i = 0; i < history_count; i++) {
        free(command_history[i].command);
    }

    return 0;
}