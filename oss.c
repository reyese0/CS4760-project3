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
#include <fcntl.h>

int shm_key;
int shm_id;

typedef struct {
    int seconds;
    int nanoseconds; 
} Clock;

typedef struct msgbuffer {
    long mtype;     //message type (1 for oss->worker, 2 for worker->oss)
    char strData[100];
    pid_t pid;           
    int done;            //1 if worker is done, 0 otherwise
    int messagesReceived; 
} msgbuffer;

typedef struct {
    int occupied;
    pid_t pid;
    int startSeconds;
    int startNano;
    int messagesSent;
    int slot; 
} PCB;

PCB processTable[20];
int nextChildIndex = 0;

void print_help() {
    printf("How to use: oss [-h] [-n proc] [-s simul] [-t timelimitForChildren] [-i intervalInMsToLaunchChildren] [-f logfile]\n");
    printf("  -h      Show help message\n");
    printf("  -n proc Total number of children to launch\n");
    printf("  -s simul Maximum number of children to run simultaneously\n");
    printf("  -t bound of time that a child process will be launched for\n");
    printf("  -i how often you should launch a child\n");
    printf("  -f logfile name to output to\n");
}

int getActiveChildrenCount() {
    int count = 0;
    for (int i = 0; i < 20; i++) {
        if (processTable[i].occupied) {
            count++;
        }
    }
    return count;
}

int findNextChildToMessage() {
    for (int i = 0; i < 20; i++) {
        int index = (nextChildIndex + i) % 20;
        if (processTable[index].occupied) {
            nextChildIndex = (index + 1) % 20;
            return index;
        }
    }
    return -1;
}

int main(int argc, char *argv[]) { 
    int totalChildren = 0;
    int maxSimul = 0;
    float timeBound = 0.0;
    float launchInterval = 0.0;
    char logfile[100] = "oss.log";
    FILE *log_fp;

    char opt;
    const char optstring[] = "hn:s:t:i:f:";

    msgbuffer buf;
    int msqid = 0;
    key_t key;

    int childrenLaunched = 0;
    int childrenTerminated = 0;
    int total_messages_sent = 0;

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
                timeBound = atof(optarg);
                break;
            case 'i':
                launchInterval = atof(optarg);
                break;
            case 'f':
                strncpy(logfile, optarg, sizeof(logfile) - 1);
                break;
            default:
                fprintf(stderr, "Invalid option\n");
                print_help();
                exit(1);
        }
    }

    // Open log file
    log_fp = fopen(logfile, "w");
    if (log_fp == NULL) {
        perror("Failed to open log file");
        log_fp = stdout;
    }

     //initialize the process table
    for (int i = 0; i < 20; i++) {
        processTable[i].occupied = 0;
        processTable[i].pid = 0;
        processTable[i].startSeconds = 0;
        processTable[i].startNano = 0;
        processTable[i].messagesSent = 0;
        processTable[i].slot = i;
    }

    //checks for required parameters
    if (maxSimul <= 0 || timeBound <= 0 || interval <= 0) {
        fprintf(stderr, "Invalid parameters: maxSimul, timeBound, and interval must be positive\n");
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

    // Initialize system clock
    clock->seconds = 0;
    clock->nanoseconds = 0;

    //add log message about oss starting
    log_message("OSS: Starting OSS with PID %d\n", getpid());

    int lastPrintTime = 0;
    int lastLaunchSec = 0;
    int lastLaunchNano = 0;

    // Main loop
    while (childrenLaunched < totalChildren || childrenTerminated < totalChildren) {
        //increment system clock
        int activeChildren = getActiveChildrenCount();
        if (activeChildren == 0) {
            activeChildren = 1; // Avoid division by zero
        }
        
        // Increment by 250ms divided by number of active children
        int incrementNano = 250000000 / activeChildren; // 250ms in nanoseconds
        clock->nanoseconds += incrementNano;
        
        if (clock->nanoseconds >= 1000000000) {
            clock->seconds++;
            clock->nanoseconds -= 1000000000;
        }

        // Print process table every half second
        if (clock->seconds >= lastPrintTime + 0.5) {
            log_message("OSS PID:%d SysClockS: %d SysClockNano: %d\n", 
                getpid(), clock->seconds, clock->nanoseconds);
            log_message("Process Table:\n");
            log_message("Entry\tOccupied\tPID\tStartS\tStartN\tMessagesSent\n");
            
            for (int i = 0; i < 20; i++) {
                log_message("%d\t%d\t\t%d\t%d\t%d\t%d\n",
                        processTable[i].slot,
                        processTable[i].occupied,
                        processTable[i].pid,
                        processTable[i].startSeconds,
                        processTable[i].startNano,
                        processTable[i].messagesSent);
            }
            log_message("\n");

            lastPrintTime = clock->seconds;
        }

        // Find next child to send message to
        int childIndex = findNextChildToMessage();
        if (childIndex != -1) {
            pid_t child_pid = processTable[childIndex].pid;
            
            // Send message to child
            msgbuffer snd_buf;
            snd_buf.mtype = child_pid;
            snd_buf.running = 1;
            snd_buf.messagesReceived = processTable[childIndex].messagesSent;
            
            log_message("OSS: Sending message to worker %d PID %d at time %d:%d\n",
                       childIndex, child_pid, clock->seconds, clock->nanoseconds);
            
            if (msgsnd(msqid, &snd_buf, sizeof(snd_buf) - sizeof(long), 0) < 0) {
                perror("OSS: Error sending message");
            } else {
                processTable[childIndex].messagesSent++;
                total_messages_sent++;
            }
            
            // Receive message from child
            msgbuffer rcv_buf;
            log_message("OSS: Receiving message from worker %d PID %d at time %d:%d\n",
                       childIndex, child_pid, clock->seconds, clock->nanoseconds);
            
            if (msgrcv(msqid, &rcv_buf, sizeof(rcv_buf) - sizeof(long), child_pid, 0) < 0) {
                perror("OSS: Error receiving message");
            } else {
                log_message("OSS: Received message from worker %d PID %d (running=%d, messagesReceived=%d)\n",
                           childIndex, child_pid, rcv_buf.running, rcv_buf.messagesReceived);
                
                if (!rcv_buf.running) {
                    log_message("OSS: Worker %d PID %d is planning to terminate.\n", childIndex, child_pid);
                    
                    // Wait for child to terminate
                    int status;
                    waitpid(child_pid, &status, 0);
                    
                    // Update process table
                    processTable[childIndex].occupied = 0;
                    processTable[childIndex].pid = 0;
                    childrenTerminated++;
                    
                    log_message("OSS: Worker %d PID %d has terminated.\n", childIndex, child_pid);
                }
            }
        }

        // Launch new children if conditions are met
        if (childrenLaunched < totalChildren) {
            int currentChildren = getActiveChildrenCount();
            long timeSinceLastLaunch = (long)(clock->seconds - lastLaunchSec) * 1000000000L + 
                                      (clock->nanoseconds - lastLaunchNano);
            long intervalNano = (long)(launchInterval * 1000000000L);
            
            if (currentChildren < maxSimultaneous && timeSinceLastLaunch >= intervalNano) {
                //launchWorker
                // Find empty slot in process table
                int slot = -1;
                for (int i = 0; i < 20; i++) {
                    if (!processTable[i].occupied) {
                        slot = i;
                        break;
                    }
                }
                
                if (slot == -1) {
                    log_message("OSS: No available slots in process table!\n");
                    return;
                }
                
                // Generate random duration for worker (1 to timeBound seconds)
                int durationSec = (rand() % (int)timeBound) + 1;
                int durationNano = rand() % 1000000000;
                
                // If timeBound has fractional part, include it
                if (timeBound > (int)timeBound) {
                    float fractional = timeBound - (int)timeBound;
                    durationNano += (int)(fractional * 1000000000);
                    if (durationNano >= 1000000000) {
                        durationSec++;
                        durationNano -= 1000000000;
                    }
                }
                
                pid_t child_pid = fork();
                if (child_pid == 0) {
                    // Child process
                    char secStr[20];
                    char nanoStr[20];
                    snprintf(secStr, sizeof(secStr), "%d", durationSec);
                    snprintf(nanoStr, sizeof(nanoStr), "%d", durationNano);
                    execl("./worker", "worker", secStr, nanoStr, NULL);
                    perror("execl failed");
                    exit(1);
                } else if (child_pid > 0) {
                    // Parent process
                    processTable[slot].occupied = 1;
                    processTable[slot].pid = child_pid;
                    processTable[slot].startSeconds = clock->seconds;
                    processTable[slot].startNano = clock->nanoseconds;
                    processTable[slot].messagesSent = 0;
                    childrenLaunched++;
                    
                    log_message("OSS: Launched worker %d PID %d at time %d:%d with duration %d:%d\n",
                            slot, child_pid, clock->seconds, clock->nanoseconds,
                            durationSec, durationNano);
                    
                    // Print process table after launching new child
                    log_message("OSS PID:%d SysClockS: %d SysClockNano: %d\n", 
                getpid(), clock->seconds, clock->nanoseconds);
                    log_message("Process Table:\n");
                    log_message("Entry\tOccupied\tPID\tStartS\tStartN\tMessagesSent\n");
                    
                    for (int i = 0; i < 20; i++) {
                        log_message("%d\t%d\t\t%d\t%d\t%d\t%d\n",
                                processTable[i].slot,
                                processTable[i].occupied,
                                processTable[i].pid,
                                processTable[i].startSeconds,
                                processTable[i].startNano,
                                processTable[i].messagesSent);
                    }
                    log_message("\n");
                } else {
                    perror("OSS: Fork failed");
                }
                lastLaunchSec = clock->seconds;
                lastLaunchNano = clock->nanoseconds;
            }
        }
    }

    // Generate final report
    printf("\nOSS TERMINATION REPORT\n");
    printf("OSS PID: %d terminating\n", getpid());
    printf("Total workers launched: %d\n", childrenLaunched);
    printf("Total workers terminated: %d\n", childrenTerminated);
    printf("Total messages sent from OSS: %d\n", total_messages_sent);
    printf("Final system time: %d seconds, %d nanoseconds\n", clock->seconds, clock->nanoseconds);
    
    
    shmdt(clock);
    shmctl(shm_id, IPC_RMID, NULL);
    msgctl(msqid, IPC_RMID, NULL);
    
    if (log_fp != NULL && log_fp != stdout) {
        fclose(log_fp);
    }
    
    return 0;
}
