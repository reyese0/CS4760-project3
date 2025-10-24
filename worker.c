#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <sys/shm.h>


int shm_key;
int shm_id;

typedef struct {
    int seconds;
    int nanoseconds; 
} Clock;

typedef struct msgbuffer {
    long mtype;     // message type (1 for oss->worker, 2 for worker->oss)
    char strData[100];
    pid_t pid;           // process ID
    int done;            // 1 if worker is done, 0 otherwise
    int messagesReceived; // count of messages received by worker
} msgbuffer;

int main(int argc, char *argv[]) {
    int inputSec = atoi(argv[1]);
    int inputNano = atoi(argv[2]);
    pid_t pid = getpid();
    pid_t ppid = getppid();
    msgbuffer buf;
    buf.mtype = 1;
    int msqid = 0;
    key_t key;

    int shm_id = shmget(1234, sizeof(Clock), 0666);
    if (shm_id < 0) {
        fprintf(stderr,"Worker:... Error in shmget\n");
        exit(1);
    }

    Clock *clock = (Clock *) shmat(shm_id, 0, 0);
    if (clock <= 0) {
        fprintf(stderr,"Worker:... Error in shmat\n");
        exit(1);
    }

    // get a key for message queue
    if ((key = ftok("msgq.txt", 1)) == -1) {
        perror("ftok");
        exit(1);
    }

    // create message queue
    if ((msqid = msgget(key, 0644)) == -1) {
        perror("msgget in child");
        exit(1);
    }

    //get start time
    int startSec = clock->seconds;
    int startNano = clock->nanoseconds;

    //calculate termination time
    int endSec = startSec + inputSec;
    int endNano = startNano + inputNano;

    printf("WORKER PID:%d PPID:%d SysClockS: %d SysclockNano: %d\n", pid, ppid, startSec, startNano);
    printf("TermTimeS: %d TermTimeNano: %d\n", endSec, endNano);
    printf("--Just Starting\n");

    int messagesReceived = 0;
    int iterations = 0;
    int done = 0;

    // Main message loop
    do {
        msgbuffer receivedBuf;
        
        // Wait for message from OSS
        printf("WORKER PID:%d - Waiting for message from OSS...\n", pid);
        if (msgrcv(msqid, &receivedBuf, sizeof(receivedBuf) - sizeof(long), pid, 0) < 0) {
            perror("Worker: Error in msgrcv");
            break;
        }
        
        messagesReceived++;
        iterations++;
        
        // Get current system time
        int currSec = clock->seconds;
        int currNano = clock->nanoseconds;
        
        printf("WORKER PID:%d - Received message %d from OSS\n", pid, messagesReceived);
        printf("Current time: %d seconds, %d nanoseconds\n", currSec, currNano);
        
        // Check if it's time to terminate
        if ((currSec > endSec) || (currSec == endSec && currNano >= endNano)) {
            done = 1;
            printf("WORKER PID:%d - Termination time reached!\n", pid);
        }
        
        // Send response back to OSS
        buf.mtype = 2;  // Worker to OSS message type
        buf.pid = pid;
        buf.done = done;
        buf.messagesReceived = messagesReceived;
        
        if (msgsnd(msqid, &buf, sizeof(buf) - sizeof(long), 0) < 0) {
            perror("Worker: Error in msgsnd");
            break;
        }
        
        printf("WORKER PID:%d - Sent response to OSS (done=%d, messagesReceived=%d)\n", 
               pid, done, messagesReceived);
        
        // Periodic output every iteration
        printf("WORKER PID:%d PPID:%d SysClockS: %d SysclockNano: %d\n", pid, ppid, currSec, currNano);
        printf("TermTimeS: %d TermTimeNano: %d\n", endSec, endNano);
        printf("--Iteration %d completed, total messages received: %d\n", iterations, messagesReceived);
        
    } while (!done);

    // Get final system time
    int currSec = clock->seconds;
    int currNano = clock->nanoseconds;

    // Final termination message
    printf("WORKER PID:%d PPID:%d SysClockS: %d SysclockNano: %d\n", pid, ppid, currSec, currNano);
    printf("TermTimeS: %d TermTimeNano: %d\n", endSec, endNano);
    printf("--Terminating after sending message back to oss after %d received messages.\n", messagesReceived);

    shmdt(clock);
    return 0;
}