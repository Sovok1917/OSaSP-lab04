#include "consumer.h"
#include "ipc_manager.h"
#include "utils.h"


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

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        print_error("Consumer", "Failed to register SIGTERM handler");
        _exit(EXIT_FAILURE);
    }


    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    char info_prefix[32];
    snprintf(info_prefix, sizeof(info_prefix), "Consumer %d", consumer_id);
    print_info(info_prefix, "Started.");

    while (!s_child_terminate_flag) {
        message_t msg;
        unsigned short original_hash;
        unsigned short calculated_hash;
        unsigned long current_extracted;



        while (semaphore_op(SEM_FULL_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during wait for full slot (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue;
            } else {
                print_error(info_prefix, "Semaphore wait (Full) failed.");
                goto cleanup_and_exit;
            }
        }

        if (s_child_terminate_flag) {
            semaphore_op(SEM_FULL_IDX, 1);
            print_info(info_prefix, "Terminating after wait for full slot.");
            break;
        }




        while (semaphore_op(SEM_MUTEX_IDX, -1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                semaphore_op(SEM_FULL_IDX, 1);
                print_info(info_prefix, "Terminating during wait for mutex (EINTR).");
                goto cleanup_and_exit;
            } else if (errno == EINTR) {
                continue;
            } else {
                semaphore_op(SEM_FULL_IDX, 1);
                print_error(info_prefix, "Semaphore wait (Mutex) failed.");
                goto cleanup_and_exit;
            }
        }

        if (s_child_terminate_flag) {
            semaphore_op(SEM_MUTEX_IDX, 1);
            semaphore_op(SEM_FULL_IDX, 1);
            print_info(info_prefix, "Terminating after wait for mutex.");
            break;
        }



        memcpy(&msg, &queue->messages[queue->head_idx], sizeof(message_t));
        queue->head_idx = (queue->head_idx + 1) % QUEUE_CAPACITY;
        queue->free_slots++;
        queue->extracted_count++;
        current_extracted = queue->extracted_count;



        if (semaphore_op(SEM_MUTEX_IDX, 1) == -1) {
            print_error(info_prefix, "CRITICAL: Failed to release mutex!");
            goto cleanup_and_exit;
        }



        if (semaphore_op(SEM_EMPTY_IDX, 1) == -1) {
            if (errno == EINTR && s_child_terminate_flag) {
                print_info(info_prefix, "Terminating during signal empty (EINTR).");
                break;
            } else if (errno != EINTR) {
                print_error(info_prefix, "Semaphore signal (Empty) failed.");
                break;
            }

        }


        original_hash = msg.hash;
        msg.hash = 0;
        calculated_hash = calculate_message_hash(&msg);
        bool hash_ok = (original_hash == calculated_hash);


        printf("[%s] Extracted msg (Type:%u Size:%u Hash:%u -> %s). Total Extracted: %lu\r\n",
               info_prefix, msg.type, msg.size, original_hash,
               hash_ok ? "OK" : "FAIL", current_extracted);
        fflush(stdout);
        if (!hash_ok) {
            fprintf(stderr, "WARNING: [%s] Hash mismatch! Expected %u, Calculated %u\r\n",
                    info_prefix, original_hash, calculated_hash);
            fflush(stderr);
        }


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


    }

    cleanup_and_exit:
    print_info(info_prefix, "Terminating gracefully...");
    _exit(EXIT_SUCCESS);
}
