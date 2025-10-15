#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/msg.h>

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

typedef struct {
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNano;
} PCB;

PCB processTable[20];

void print_help() {
    printf("How to use: oss [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i intervalInMsToLaunchChildren] [-f logfile]\n");
    printf("  -h      Show help message\n");
    printf("  -n proc Total number of children to launch\n");
    printf("  -s simul Maximum number of children to run simultaneously\n");
    printf("  -t bound of time that a child process will be launched for\n");
    printf("  -i how often you should launch a child\n");
    printf("  -f logfile name to output to\n");
}

int main(int argc, char *argv[]) { 
    int totalChildren = 0;
    int maxSimul = 0;
    float timeLimit = 0.0;
    float launchInterval = 0.0;
    char *logFileName = NULL;
    char opt;
    const char optstring[] = "hn:s:t:i:f:";
    msgbuffer buf;
    int msqid = 0;
    key_t key;

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
            case 'h':
                print_help();
                exit(0);
            case 'n':
                totalChildren = atoi(optarg);
                break;
            case 's':
                maxSimul = atoi(optarg);
                break;
            case 't':
                timeLimit = atof(optarg);
                break;
            case 'i':
                launchInterval = atof(optarg);
                break;
            case 'f':
                logFileName = optarg;
                break;
            default:
                fprintf(stderr, "Invalid option\n");
                print_help();
                exit(1);
        }
    }

     //initialize the process table
    for (int i = 0; i < 20; i++) {
        processTable[i].occupied = 0;
        processTable[i].pid = 0;
        processTable[i].startSeconds = 0;
        processTable[i].startNano = 0;
    }

    //checks for required parameters
    if (maxSimul <= 0 || timeLimit <= 0 || interval <= 0) {
        fprintf(stderr, "Invalid parameters: maxSimul, timeLimit, and interval must be positive\n");
        print_help();
        exit(1);
    }

    int shm_id = shmget(1234, sizeof(Clock), IPC_CREAT | 0666);
    if (shm_id < 0) {
        fprintf(stderr,"Parent:... Error in shmget\n");
        exit(1);
    }

    Clock *clock = (Clock *) shmat(shm_id, 0, 0);
    if (clock <= 0) {
        fprintf(stderr,"OSS:... Error in shmat\n");
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

}