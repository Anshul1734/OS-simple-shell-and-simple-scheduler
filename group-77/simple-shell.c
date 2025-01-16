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
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <pthread.h>

#define INPUT_MAX 1024
#define ARGS_MAX 100
#define HISTORY_MAX 100

#define DEFAULT_PRIORITY 1
#define MAX_PRIORITY 4

typedef struct {
    char *command;
    pid_t pid;
    time_t start_time;
    time_t end_time;
    int is_background;
} CommandLog;

typedef struct {
    pid_t job_pid;
    char name[256];
    int priority;
    int is_new;
    int completed;
    time_t start_time;
    time_t end_time;
} SharedJob;

typedef struct {
    SharedJob jobs[HISTORY_MAX];
    int job_count;
    int scheduler_ready;
} SharedMemory;


key_t key;
int shmid;
SharedMemory *shared_mem;


typedef struct {
    char name[256];
    pid_t pid;
    time_t start_time;
    time_t end_time;
    int completed;
    int slices_run;
    int priority;  
} SchedulerJob;

CommandLog command_history[HISTORY_MAX];
SchedulerJob scheduler_jobs[HISTORY_MAX];
int history_count = 0;
int job_count = 0;
pid_t scheduler_pid;
int global_tslice; 

volatile sig_atomic_t received_sigint = 0;

void sigchld_handler(int sig) {
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < job_count; i++) {
            if (scheduler_jobs[i].pid == pid && !scheduler_jobs[i].completed) {
                scheduler_jobs[i].end_time = time(NULL);
                scheduler_jobs[i].completed = 1;
                scheduler_jobs[i].slices_run = 1; 
                printf("Job %s (PID: %d) completed\n", scheduler_jobs[i].name, pid);
            }
        }
    }
}
void sigint_handler(int sig) {
    received_sigint = 1;
}

void init_shared_memory() {
   
    key = ftok(".", 's');
    if (key == -1) {
        perror("ftok failed");
        exit(1);
    }

  
    shmid = shmget(key, sizeof(SharedMemory), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget failed");
        exit(1);
    }

  
    shared_mem = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shared_mem == (void *)-1) {
        perror("shmat failed");
        exit(1);
    }

    
    memset(shared_mem, 0, sizeof(SharedMemory));
}

void cleanup_shared_memory() {
   
    if (shmdt(shared_mem) == -1) {
        perror("shmdt failed");
    }

  
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl failed");
    }
}

void launch_scheduler(int ncpu, int tslice) {
    scheduler_pid = fork();
    if (scheduler_pid == 0) {
        char ncpu_str[10], tslice_str[10], shmid_str[20];
        sprintf(ncpu_str, "%d", ncpu);
        sprintf(tslice_str, "%d", tslice);
        sprintf(shmid_str, "%d", shmid);
        execl("./s", "simple-scheduler", ncpu_str, tslice_str, shmid_str, NULL);
        perror("Failed to launch scheduler");
        exit(1);
    }
}

int is_executable(const char *path) {
    if (access(path, F_OK) == -1) {
        printf("Error: File '%s' does not exist\n", path);
        return 0;
    }
    if (access(path, X_OK) == -1) {
        printf("Error: File '%s' is not executable\n", path);
        return 0;
    }
    return 1;
}

void *output_handler(void *arg) {
    int pipe_fd = *(int *)arg;
    free(arg);

    char buffer[4096];
    ssize_t bytes_read;

    while ((bytes_read = read(pipe_fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        printf("%s", buffer);
        fflush(stdout);
    }

    close(pipe_fd);
    return NULL;
}

void handle_submit(char *program, char *priority_str) {
    if (program == NULL || strlen(program) == 0) {
        printf("Usage: submit <program/command> [priority]\n");
        return;
    }

    int priority = DEFAULT_PRIORITY;
    if (priority_str != NULL) {
        priority = atoi(priority_str);
        if (priority < 1 || priority > MAX_PRIORITY) {
            printf("Invalid priority value. Using default priority %d\n", DEFAULT_PRIORITY);
            priority = DEFAULT_PRIORITY;
        }
    }

    
    char *path = NULL;
    char *path_env = getenv("PATH");
    char *path_copy = strdup(path_env);
    char *dir = strtok(path_copy, ":");
    
    while (dir != NULL) {
        char full_path[INPUT_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, program);
        if (access(full_path, X_OK) == 0) {
            path = strdup(full_path);
            break;
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);

   
    if (path == NULL) {
        char program_path[INPUT_MAX];
        if (program[0] != '.' && program[0] != '/') {
            snprintf(program_path, sizeof(program_path), "./%s", program);
        } else {
            strncpy(program_path, program, sizeof(program_path) - 1);
        }
        
        if (access(program_path, X_OK) == 0) {
            path = strdup(program_path);
        }
    }

    if (path == NULL) {
        printf("Error: Command/Program '%s' not found or not executable\n", program);
        return;
    }

   
    int output_pipe[2];
    if (pipe(output_pipe) == -1) {
        perror("Pipe creation failed");
        free(path);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
       
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        sigprocmask(SIG_BLOCK, &mask, NULL);

       
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[0]);
        close(output_pipe[1]);
        
        execl(path, program, NULL);
        perror("Failed to execute program");
        exit(1);
    } else if (pid > 0) {
       
        close(output_pipe[1]); 

   
        int idx = shared_mem->job_count;
        shared_mem->jobs[idx].job_pid = pid;
        strncpy(shared_mem->jobs[idx].name, program, sizeof(shared_mem->jobs[idx].name) - 1);
        shared_mem->jobs[idx].priority = priority;
        shared_mem->jobs[idx].is_new = 1;
        shared_mem->jobs[idx].completed = 0;
        shared_mem->jobs[idx].start_time = time(NULL);
        shared_mem->job_count++;

        printf("Submitted job: %s with PID: %d, Priority: %d\n", program, pid, priority);

        
        pthread_t output_thread;
        int *pipe_fd = malloc(sizeof(int));
        *pipe_fd = output_pipe[0];
        pthread_create(&output_thread, NULL, output_handler, pipe_fd);
        pthread_detach(output_thread);

       
        scheduler_jobs[job_count].pid = pid;
        strncpy(scheduler_jobs[job_count].name, program, sizeof(scheduler_jobs[job_count].name) - 1);
        scheduler_jobs[job_count].priority = priority;
        scheduler_jobs[job_count].start_time = time(NULL);
        scheduler_jobs[job_count].completed = 0;
        scheduler_jobs[job_count].slices_run = 0;
        job_count++;
    } else {
        perror("Fork failed");
        close(output_pipe[0]);
        close(output_pipe[1]);
    }

    free(path);
}



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
    

    if (strncmp(input, "submit", 6) == 0) {
        printf("Error: Pipes are not allowed with submit command\n");
        return;
    }

    int max_pipes = 10;  
    int pipes[10][2];  
    char *commands[11];  
    int command_count = 0;
    int is_background = 0;

    char *bg_check = strrchr(input, '&');
    if (bg_check != NULL && (bg_check == input + strlen(input) - 1 || *(bg_check + 1) == '\0')) {
        is_background = 1;
        *bg_check = '\0'; 
    }

    char *token = strtok(input, "|");
    while (token != NULL && command_count < max_pipes + 1) {
        commands[command_count++] = token;
        token = strtok(NULL, "|");
    }

    pid_t pid = fork();

    if (pid == 0) {
        setsid();

        for (int i = 0; i < command_count - 1; i++) {
            if (pipe(pipes[i]) == -1) {
                perror("Pipe creation failed");
                exit(EXIT_FAILURE);
            }
        }

        for (int i = 0; i < command_count; i++) {
            pid_t child_pid = fork();

            if (child_pid == 0) {
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
            }
        }

        for (int i = 0; i < command_count - 1; i++) {
            close(pipes[i][0]);
            close(pipes[i][1]);
        }

        for (int i = 0; i < command_count; i++) {
            wait(NULL);
        }

        exit(EXIT_SUCCESS);
    } else if (pid < 0) {
        perror("Fork failed");
        return;
    } else {
        if (is_background) {
            printf("[%d] %s\n", pid, input);
            recordCommand(input, pid, is_background);
        } else {
            waitpid(pid, NULL, 0);
        }
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
void print_scheduler_statistics() {
    
    static int printed = 0;
    if (printed) return;
    printed = 1;

    printf("\nScheduler Job Statistics:\n");
    printf("%-20s %-10s %-10s %-20s %-20s\n", 
           "Name", "PID", "Priority", "Completion Time", "Wait Time");
    
   
    for (int i = 0; i < job_count; i++) {
        if (scheduler_jobs[i].completed) {

            double slice_time = global_tslice ; 
            int p = 5 - scheduler_jobs[i].priority;
             // double completion_time = (MAX_PRIORITY - scheduler_jobs[i].priority + 1) * slice_time * 5;
           double completion_time = scheduler_jobs[i].slices_run * slice_time * p;
           double wait_time = (MAX_PRIORITY - scheduler_jobs[i].priority) * scheduler_jobs[i].slices_run * slice_time;


            printf("%-20s %-10d %-10d %-20.2f %-20.2f\n", 
                   scheduler_jobs[i].name, 
                   scheduler_jobs[i].pid,
                   scheduler_jobs[i].priority,
                   completion_time,
                   wait_time);
        }
    }
}

void printExecutionSummary() {

    printf("\nCommand Execution Summary:\n");
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

    print_scheduler_statistics();

}

void cleanupBackgroundProcesses() {
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("Process %d completed\n", pid); 
        
     
        for (int i = 0; i < job_count; i++) {
            if (scheduler_jobs[i].pid == pid && !scheduler_jobs[i].completed) {
                scheduler_jobs[i].end_time = time(NULL);
                scheduler_jobs[i].completed = 1;
                scheduler_jobs[i].slices_run = 1;  
                printf("Job %s (PID %d) completed\n", scheduler_jobs[i].name, pid);  
                break;
            }
        }
        
       
        for (int i = 0; i < history_count; i++) {
            if (command_history[i].pid == pid && command_history[i].is_background) {
                printf("[%d] Done    %s\n", pid, command_history[i].command);
                markCommandAsFinished(pid);
                break;
            }
        }
    }
}

void cleanup() {
   
    if (scheduler_pid > 0) {
        kill(scheduler_pid, SIGTERM);
        waitpid(scheduler_pid, NULL, 0);
    }
    
   
    for (int i = 0; i < job_count; i++) {
        if (!scheduler_jobs[i].completed) {
            kill(scheduler_jobs[i].pid, SIGTERM);
            waitpid(scheduler_jobs[i].pid, NULL, 0);
            scheduler_jobs[i].completed = 1;
        }
    }

    
    printExecutionSummary();
}



int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <NCPU> <TSLICE>\n", argv[0]);
        return 1;
    }
    struct sigaction sa_chld;
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("Error setting up SIGCHLD handler");
        exit(1);
    }
    int ncpu = atoi(argv[1]);
    global_tslice = atoi(argv[2]);
    init_shared_memory();

   
    launch_scheduler(ncpu, global_tslice);
    
    char input[INPUT_MAX];
    char *args[ARGS_MAX];

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error setting up SIGINT handler");
        exit(1);
    }

    while (1) {
        if (received_sigint) {
            cleanup();
            exit(0);
        }

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
            
             cleanup();
            
            break;
        }

        cleanupBackgroundProcesses();

        
        char command[INPUT_MAX];
        char program[INPUT_MAX];
        if (sscanf(input, "%s %s", command, program) == 2 && strcmp(command, "submit") == 0) {
            char priority_str[32];
            
            if (sscanf(input, "%s %s %s", command, program, priority_str) == 3) {
                handle_submit(program, priority_str);
            } else {
                handle_submit(program, NULL);
            }
        }
        else if (strchr(input, '|') != NULL) {
            handlePipedCommands(input);
        } 
        else if (strchr(input, '<') != NULL) {
            handleInputOutputRedirection(input, 0);  
        } 
        else if (strchr(input, '>') != NULL) {
            handleInputOutputRedirection(input, 1);  
        } 
        else {
            int is_background = splitCommandIntoArgs(input, args);
            if (args[0] != NULL) {
                if (strcmp(args[0], "history") == 0) {
                    showCommandHistory();
                } else {
                    executeCommand(args, is_background);
                }
            }
        }
    }

   
    if (scheduler_pid > 0) {
        kill(scheduler_pid, SIGTERM);
        waitpid(scheduler_pid, NULL, 0);
    }

    cleanup_shared_memory();
    return 0;
}