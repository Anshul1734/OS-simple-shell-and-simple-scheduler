#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include "dummy_main.h"

#define MAX_PROCESSES 100

typedef struct Process {
    pid_t pid;
    char name[256];
    time_t start_time;
    time_t end_time;
    int priority;
    int is_running;
    int slices_run;
} Process;

typedef struct {
    Process processes[MAX_PROCESSES];
    int front;
    int rear;
    int size;
} ProcessQueue;

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
    SharedJob jobs[100];
    int job_count;
    int scheduler_ready;
} SharedMemory;

// Global variables
ProcessQueue ready_queue;
Process *running_processes;
int ncpu;
int tslice;
volatile sig_atomic_t timer_expired = 0;
volatile sig_atomic_t should_exit = 0;
SharedMemory *shared_mem;

void initQueue() {
    ready_queue.front = 0;
    ready_queue.rear = -1;
    ready_queue.size = 0;
}

void enqueue(Process p) {
    if (ready_queue.size >= MAX_PROCESSES) return;
    ready_queue.rear = (ready_queue.rear + 1) % MAX_PROCESSES;
    ready_queue.processes[ready_queue.rear] = p;
    ready_queue.size++;
}

Process dequeue() {
    Process empty = {0};
    if (ready_queue.size == 0) return empty;
    Process p = ready_queue.processes[ready_queue.front];
    ready_queue.front = (ready_queue.front + 1) % MAX_PROCESSES;
    ready_queue.size--;
    return p;
}

void timer_handler(int signo) {
    timer_expired = 1;
}

void term_handler(int signo) {
    should_exit = 1;
}

void stop_running_processes() {
    for (int i = 0; i < ncpu; i++) {
        if (running_processes[i].pid != 0) {
            kill(running_processes[i].pid, SIGUSR2);
            if (!should_exit) {
                enqueue(running_processes[i]);
            }
            running_processes[i].pid = 0;
        }
    }
}

#include <sys/time.h>  // For precise timing with gettimeofday

void check_completed_processes() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        
        for (int i = 0; i < shared_mem->job_count; i++) {
            if (shared_mem->jobs[i].job_pid == pid) {
                shared_mem->jobs[i].completed = 1;
                shared_mem->jobs[i].end_time = time(NULL);
                printf("Job %s with PID %d completed.\n", shared_mem->jobs[i].name, pid);
                break;
            }
        }
        
        for (int i = 0; i < ncpu; i++) {
            if (running_processes[i].pid == pid) {
                running_processes[i].pid = 0;
            }
        }
    }
}

void schedule_processes() {
    // Check and update completed processes
    check_completed_processes();
    
    // Pause running processes and log exact timeslices
    struct timeval start, end;
    for (int i = 0; i < ncpu; i++) {
        if (running_processes[i].pid != 0) {
            // Capture the start time for each TSLICE
            gettimeofday(&start, NULL);

            // Pause the process after it uses a TSLICE
            kill(running_processes[i].pid, SIGUSR2);
            running_processes[i].slices_run++;

            // Calculate how long the process actually ran
            gettimeofday(&end, NULL);
            long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_usec - start.tv_usec) / 1000;
            printf("Paused job %s with PID %d after %d slices; ran for %ld ms.\n",
                   running_processes[i].name, running_processes[i].pid,
                   running_processes[i].slices_run, elapsed_ms);

            if (!should_exit && !shared_mem->jobs[i].completed) {
                enqueue(running_processes[i]);
            }
            running_processes[i].pid = 0;
        }
    }
    
    // Add new jobs to the ready queue
    for (int i = 0; i < shared_mem->job_count; i++) {
        if (shared_mem->jobs[i].is_new && !shared_mem->jobs[i].completed) {
            Process new_process = {
                .pid = shared_mem->jobs[i].job_pid,
                .start_time = shared_mem->jobs[i].start_time,
                .priority = shared_mem->jobs[i].priority,
                .slices_run = 0
            };
            strncpy(new_process.name, shared_mem->jobs[i].name, sizeof(new_process.name) - 1);
            enqueue(new_process);
            shared_mem->jobs[i].is_new = 0;
            printf("Added new job %s with PID %d to the queue.\n", new_process.name, new_process.pid);
        }
    }
    
    // Schedule processes based on round-robin order
    for (int i = 0; i < ncpu && ready_queue.size > 0; i++) {
        if (running_processes[i].pid == 0) {
            Process selected = dequeue();
            
            running_processes[i] = selected;
            if (running_processes[i].pid != 0) {
                printf("Starting job %s with PID %d at slice %d.\n",
                       running_processes[i].name, running_processes[i].pid,
                       running_processes[i].slices_run);
                kill(running_processes[i].pid, SIGUSR1);
            }
        }
    }
}




int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <NCPU> <TSLICE> <SHMID>\n", argv[0]);
        return 1;
    }
    
    // Set up signal handlers
    struct sigaction sa;
    sa.sa_handler = timer_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    
    sa.sa_handler = term_handler;
    sigaction(SIGTERM, &sa, NULL);
    
    ncpu = atoi(argv[1]);
    tslice = atoi(argv[2]);
    int shmid = atoi(argv[3]);
    
    // Attach to shared memory
    shared_mem = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shared_mem == (void *)-1) {
        perror("shmat failed");
        exit(1);
    }
    
    // Initialize
    initQueue();
    running_processes = calloc(ncpu, sizeof(Process));
    
    // Set up timer
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = tslice;
    timer.it_interval = timer.it_value;
    
    if (setitimer(ITIMER_REAL, &timer, NULL) < 0) {
        perror("setitimer failed");
        exit(1);
    }
    
    // Main scheduling loop
    while (!should_exit) {
        if (timer_expired) {
            schedule_processes();
            timer_expired = 0;
            
            // Check if all processes are completed
            int active_processes = 0;
            for (int i = 0; i < shared_mem->job_count; i++) {
                if (!shared_mem->jobs[i].completed) {
                    active_processes++;
                }
            }
            
            if (active_processes == 0 && ready_queue.size == 0) {
                // Double check no processes are running
                int running = 0;
                for (int i = 0; i < ncpu; i++) {
                    if (running_processes[i].pid != 0) {
                        running = 1;
                        break;
                    }
                }
                if (!running) break;
            }
        }
        usleep(100);
    }
    
    // Cleanup
    stop_running_processes();
    free(running_processes);
    shmdt(shared_mem);
    
    return 0;
}
