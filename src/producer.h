#ifndef PRODUCER_H
#define PRODUCER_H

#include "common.h"

// --- Function Declarations ---

/*
 * producer_run
 *
 * Main function for a producer process.
 * Generates messages, adds them to the queue, and prints status.
 * Exits gracefully when s_child_terminate_flag is set.
 *
 * Accepts:
 *   producer_id - An identifier for this producer instance.
 *   queue       - Pointer to the shared memory queue.
 *
 * Returns:
 *   Does not return; calls exit().
 */
// Removed sem_id parameter
void producer_run(int producer_id, queue_t *queue);

#endif // PRODUCER_H
