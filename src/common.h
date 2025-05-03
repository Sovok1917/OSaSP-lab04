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
#include <stdint.h> // For SIZE_MAX

// --- Constants ---
#define QUEUE_CAPACITY 10       // Size of the ring buffer
#define MAX_DATA_SIZE 256       // Max data bytes in message (0-255 specified, buffer holds 256)
#define MAX_PRODUCERS 10        // Max concurrent producers
#define MAX_CONSUMERS 10        // Max concurrent consumers

// Semaphore indices
#define SEM_MUTEX_IDX 0         // Index for mutex semaphore
#define SEM_EMPTY_IDX 1         // Index for empty slots semaphore
#define SEM_FULL_IDX 2          // Index for full slots semaphore
#define NUM_SEMAPHORES 3        // Total number of semaphores

// --- Message Structure ---
typedef struct message_s {
    unsigned char type;         // Message type
    unsigned short hash;        // Checksum/hash
    unsigned char size;         // Actual data length (0-255)
    unsigned char data[MAX_DATA_SIZE]; // Data buffer
} message_t;

// --- Shared Queue Structure ---
typedef struct queue_s {
    message_t messages[QUEUE_CAPACITY]; // Ring buffer holding messages
    int head_idx;               // Index of the next message to consume
    int tail_idx;               // Index of the next empty slot to produce into
    int free_slots;             // Count of available slots (used by producer)
    // Note: full_slots count is implicitly tracked by SEM_FULL semaphore
    unsigned long added_count;  // Total messages ever added
    unsigned long extracted_count; // Total messages ever extracted
} queue_t;

// --- Global Termination Flag ---
// Declared here, defined in ipc_manager.c
// Set by signal handler, checked by main loop.
extern volatile sig_atomic_t g_terminate_flag;

// --- Utility Function Declarations (from utils.c) ---
int kbhit(void);
void restore_terminal(void);
void setup_terminal_noecho_nonblock(void);
unsigned short calculate_message_hash(const message_t *msg);
void print_error(const char *prefix, const char *msg);
void print_info(const char *prefix, const char *msg);

#endif // COMMON_H
