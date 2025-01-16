#ifndef DUMMY_MAIN_H
#define DUMMY_MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <time.h>


volatile sig_atomic_t can_run = 0;

void signal_handler(int signo) {
    if (signo == SIGUSR1) {
        can_run = 1;
    } else if (signo == SIGUSR2) {
        can_run = 0;
    }
}

int dummy_main(int argc, char **argv);

int main(int argc, char **argv) {
    struct sigaction sa;
    sigset_t mask;
    
   
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    
   
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    
    
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    
    
    while (!can_run) {
        usleep(1); 
    }
    
    return dummy_main(argc, argv);
}

#define main dummy_main

#endif 