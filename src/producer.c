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
        const char msg[] = "[Producer] SIGTERM received\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
}

/*
 * producer_run (Implementation)
 */
void producer_run(int producer_id, queue_t *queue) {
    // Register signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        print_error("Producer", "Failed to register SIGTERM handler");
        _exit(EXIT_FAILURE);
    }

    // Seed random
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
        // FIX: Handle EINTR from semaphore_op
        while (semaphore_op(SEM_EMPTY_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for empty slot (EINTR).");
                goto cleanup_and_exit; // Use goto for cleaner exit from nested loops/checks
            } else if (errno == EINTR) {
                continue; // Interrupted but not terminating, retry semop
            } else {
                // Other semaphore error
                print_error(info_prefix, "Semaphore wait (Empty) failed.");
                goto cleanup_and_exit; // Exit on other errors
            }
        }
        // Check flag *after* successful wait (or if loop exited cleanly)
        if (s_child_terminate_flag) {
            semaphore_op(SEM_EMPTY_IDX, 1); // Release acquired slot if terminating now
            print_info(info_prefix, "Terminating after wait for empty slot.");
            break;
        }


        // 3. Wait for mutex access to queue
        // FIX: Handle EINTR from semaphore_op
        while (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                semaphore_op(SEM_EMPTY_IDX, 1); // Release acquired empty slot
                print_info(info_prefix, "Terminating during wait for mutex (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue; // Interrupted but not terminating, retry semop
            } else {
                semaphore_op(SEM_EMPTY_IDX, 1); // Release acquired empty slot
                print_error(info_prefix, "Semaphore wait (Mutex) failed.");
                goto cleanup_and_exit;
            }
        }
        // Check flag *after* successful wait
        if (s_child_terminate_flag) {
            semaphore_op(SEM_MUTEX_IDX, 1); // Release acquired mutex
            semaphore_op(SEM_EMPTY_IDX, 1); // Release acquired empty slot
            print_info(info_prefix, "Terminating after wait for mutex.");
            break;
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
            // If mutex release fails, state is critical. Don't try to signal Full.
            print_error(info_prefix, "CRITICAL: Failed to release mutex!");
            goto cleanup_and_exit; // Use goto for consistency
        }

        // 6. Signal that a slot is now full
        // FIX: Handle EINTR (less critical, but good practice)
        if (semaphore_op(SEM_FULL_IDX, 1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during signal full (EINTR).");
                break; // Allow loop to terminate naturally
            } else if (errno != EINTR) { // Ignore EINTR if not terminating
                print_error(info_prefix, "Semaphore signal (Full) failed.");
                // Decide if this is fatal? Maybe continue? Let's break loop.
                break;
            }
            // If EINTR and not terminating, just continue
        }

        // 7. Print status
        printf("[%s] Added msg (Type:%u Size:%u Hash:%u). Total Added: %lu\r\n",
               info_prefix, msg.type, msg.size, msg.hash, current_added);
        fflush(stdout);

        // 8. Delay (Using nanosleep)
        struct timespec delay_req, delay_rem;
        long delay_us = (rand() % 400000L) + 100000L;
        delay_req.tv_sec = delay_us / 1000000L;
        delay_req.tv_nsec = (delay_us % 1000000L) * 1000L;
        while (nanosleep(&delay_req, &delay_rem) == -1) {
            if (errno == EINTR) {
                if (s_child_terminate_flag) break;
                delay_req = delay_rem;
            } else {
                print_error(info_prefix, "nanosleep failed"); break;
            }
        }
        // No need to check flag again here, loop condition handles it

    } // end while

    cleanup_and_exit: // Label for cleanup before exiting
    print_info(info_prefix, "Terminating gracefully...");
    _exit(EXIT_SUCCESS);
}
