#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    
    int sum = 0;
    
   
    pid_t pid = getpid();
    
    for (int i = 0; i < 100; i++) {
        sum = sum +i;
       
      
        fflush(stdout);  
        usleep(100000);  
    }
    
    printf("Final sum: %d\n", sum);
    fflush(stdout);
    return 0;
}