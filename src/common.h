#ifndef COMMON_H
#define COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>


#define QUEUE_CAPACITY 10
#define MAX_DATA_SIZE 256
#define MAX_PRODUCERS 10
#define MAX_CONSUMERS 10


#define SEM_MUTEX_IDX 0
#define SEM_EMPTY_IDX 1
#define SEM_FULL_IDX 2
#define NUM_SEMAPHORES 3


typedef struct message_s {
    unsigned char type;
    unsigned short hash;
    unsigned char size;
    unsigned char data[MAX_DATA_SIZE];
} message_t;


typedef struct queue_s {
    message_t messages[QUEUE_CAPACITY];
    int head_idx;
    int tail_idx;
    int free_slots;

    unsigned long added_count;
    unsigned long extracted_count;
} queue_t;




extern volatile sig_atomic_t g_terminate_flag;


int kbhit(void);
void restore_terminal(void);
void setup_terminal_noecho_nonblock(void);
unsigned short calculate_message_hash(const message_t *msg);
void print_error(const char *prefix, const char *msg);
void print_info(const char *prefix, const char *msg);

#endif
