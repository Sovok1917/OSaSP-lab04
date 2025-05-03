#include "consumer.h"
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
        const char msg[] = "[Consumer] SIGTERM received\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
}

/*
 * consumer_run (Implementation)
 */
void consumer_run(int consumer_id, queue_t *queue) {
    // Register signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        print_error("Consumer", "Failed to register SIGTERM handler");
        _exit(EXIT_FAILURE);
    }

    // Seed random
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    char info_prefix[32];
    snprintf(info_prefix, sizeof(info_prefix), "Consumer %d", consumer_id);
    print_info(info_prefix, "Started.");

    while (!s_child_terminate_flag) {
        message_t msg;
        unsigned short original_hash;
        unsigned short calculated_hash;
        unsigned long current_extracted;

        // 1. Wait for a full slot
        // FIX: Handle EINTR from semaphore_op
        while (semaphore_op(SEM_FULL_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for full slot (EINTR).");
                goto cleanup_and_exit; // Use goto for cleaner exit
            } else if (errno == EINTR) {
                continue; // Interrupted but not terminating, retry semop
            } else {
                print_error(info_prefix, "Semaphore wait (Full) failed.");
                goto cleanup_and_exit; // Exit on other errors
            }
        }
        // Check flag *after* successful wait
        if (s_child_terminate_flag) {
            semaphore_op(SEM_FULL_IDX, 1); // Release acquired slot if terminating now
            print_info(info_prefix, "Terminating after wait for full slot.");
            break;
        }


        // 2. Wait for mutex access to queue
        // FIX: Handle EINTR from semaphore_op
        while (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                semaphore_op(SEM_FULL_IDX, 1); // Release acquired full slot
                print_info(info_prefix, "Terminating during wait for mutex (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue; // Interrupted but not terminating, retry semop
            } else {
                semaphore_op(SEM_FULL_IDX, 1); // Release acquired full slot
                print_error(info_prefix, "Semaphore wait (Mutex) failed.");
                goto cleanup_and_exit;
            }
        }
        // Check flag *after* successful wait
        if (s_child_terminate_flag) {
            semaphore_op(SEM_MUTEX_IDX, 1); // Release acquired mutex
            semaphore_op(SEM_FULL_IDX, 1); // Release acquired full slot
            print_info(info_prefix, "Terminating after wait for mutex.");
            break;
        }


        // --- Critical Section ---
        memcpy(&msg, &queue->messages[queue->head_idx], sizeof(message_t));
        queue->head_idx = (queue->head_idx + 1) % QUEUE_CAPACITY;
        queue->free_slots++;
        queue->extracted_count++;
        current_extracted = queue->extracted_count;
        // --- End Critical Section ---

        // 4. Release mutex
        if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {
            print_error(info_prefix, "CRITICAL: Failed to release mutex!");
            goto cleanup_and_exit; // Use goto for consistency
        }

        // 5. Signal that a slot is now empty
        // FIX: Handle EINTR (less critical, but good practice)
        if (semaphore_op(SEM_EMPTY_IDX, 1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during signal empty (EINTR).");
                break; // Allow loop to terminate naturally
            } else if (errno != EINTR) { // Ignore EINTR if not terminating
                print_error(info_prefix, "Semaphore signal (Empty) failed.");
                break; // Exit loop on other errors
            }
            // If EINTR and not terminating, just continue
        }

        // 6. Process Message (Verify Hash)
        original_hash = msg.hash;
        msg.hash = 0;
        calculated_hash = calculate_message_hash(&msg);
        bool hash_ok = (original_hash == calculated_hash);

        // 7. Print status
        printf("[%s] Extracted msg (Type:%u Size:%u Hash:%u -> %s). Total Extracted: %lu\r\n",
               info_prefix, msg.type, msg.size, original_hash,
               hash_ok ? "OK" : "FAIL", current_extracted);
        fflush(stdout);
        if (!hash_ok) {
            fprintf(stderr, "WARNING: [%s] Hash mismatch! Expected %u, Calculated %u\r\n",
                    info_prefix, original_hash, calculated_hash);
            fflush(stderr);
        }

        // 8. Delay (Using nanosleep)
        struct timespec delay_req, delay_rem;
        long delay_us = (rand() % 400000L) + 200000L;
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
