#include "producer.h"
#include "ipc_manager.h" // For semaphore_op
#include "utils.h"       // For print_info, print_error, calculate_message_hash

// Child-local termination flag
static volatile sig_atomic_t s_child_terminate_flag = 0;

/*
 * child_signal_handler
 * Sets the child-local termination flag.
 */
static void child_signal_handler(int sig) {
    if (sig == SIGTERM) {
        s_child_terminate_flag = 1;
        // Avoid printf/fprintf in signal handlers
        const char msg[] = "[Producer] SIGTERM received\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
}

/*
 * producer_run (Implementation)
 */
// Removed sem_id parameter
void producer_run(int producer_id, queue_t *queue) {
    // Register signal handler for graceful termination
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART needed here
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        print_error("Producer", "Failed to register SIGTERM handler");
        exit(EXIT_FAILURE);
    }

    // Seed random number generator per process
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    char info_prefix[32];
    snprintf(info_prefix, sizeof(info_prefix), "Producer %d", producer_id);

    print_info(info_prefix, "Started.");

    while (!s_child_terminate_flag) {
        message_t msg;

        // 1. Create Message
        msg.type = (unsigned char)(rand() % 256);
        msg.size = (unsigned char)(rand() % MAX_DATA_SIZE);
        for (int i = 0; i < msg.size; ++i) {
            msg.data[i] = (unsigned char)(rand() % 256);
        }
        msg.hash = 0;
        msg.hash = calculate_message_hash(&msg);

        // 2. Wait for an empty slot
        if (semaphore_op(SEM_EMPTY_IDX, -1) == -1) {
            if (errno == ECANCELED || s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for empty slot.");
                break; // Terminating
            }
            print_error(info_prefix, "Semaphore wait (Empty) failed.");
            break; // Exit on other semaphore errors
        }

        // Check flag *after* potentially blocking call
        if (s_child_terminate_flag) {
            // Release the slot we acquired if terminating now
            semaphore_op(SEM_EMPTY_IDX, 1);
            print_info(info_prefix, "Terminating after wait for empty slot.");
            break;
        }

        // 3. Wait for mutex access to queue
        if (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
            // Release the empty slot semaphore before breaking.
            semaphore_op(SEM_EMPTY_IDX, 1); // Best effort release
            if (errno == ECANCELED || s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for mutex.");
                break; // Terminating
            }
            print_error(info_prefix, "Semaphore wait (Mutex) failed.");
            break; // Exit on other semaphore errors
        }

        // --- Critical Section ---
        memcpy(&queue->messages[queue->tail_idx], &msg, sizeof(message_t));
        queue->tail_idx = (queue->tail_idx + 1) % QUEUE_CAPACITY;
        queue->free_slots--;
        queue->added_count++;
        unsigned long current_added = queue->added_count;
        // --- End Critical Section ---

        // 5. Release mutex
        if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {
            print_error(info_prefix, "CRITICAL: Failed to release mutex!");
            exit(EXIT_FAILURE); // Exit immediately, state is broken
        }

        // 6. Signal that a slot is now full
        if (semaphore_op(SEM_FULL_IDX, 1) == -1) {
            if (errno == ECANCELED || s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during signal full.");
                break; // Terminating
            }
            print_error(info_prefix, "Semaphore signal (Full) failed.");
            break; // Exit on other semaphore errors
        }

        // 7. Print status
        printf("[%s] Added msg (Type:%u Size:%u Hash:%u). Total Added: %lu\r\n",
               info_prefix, msg.type, msg.size, msg.hash, current_added);
        fflush(stdout);

        // 8. Delay (Using nanosleep)
        struct timespec delay_req, delay_rem;
        long delay_us = (rand() % 400000L) + 100000L; // 100ms to 500ms
        delay_req.tv_sec = delay_us / 1000000L;
        delay_req.tv_nsec = (delay_us % 1000000L) * 1000L;

        while (nanosleep(&delay_req, &delay_rem) == -1) {
            if (errno == EINTR) {
                // Interrupted, check termination flag
                if (s_child_terminate_flag) break; // Exit delay loop if terminating
                // Otherwise, continue sleep with remaining time
                delay_req = delay_rem;
            } else {
                // Other error
                print_error(info_prefix, "nanosleep failed");
                break; // Exit delay loop
            }
        }
        if (s_child_terminate_flag) break; // Check flag again after delay

    } // end while

    print_info(info_prefix, "Terminating gracefully...");
    exit(EXIT_SUCCESS);
}
