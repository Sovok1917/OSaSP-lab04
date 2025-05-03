#include "common.h"
#include "ipc_manager.h"
#include "utils.h" // For kbhit, terminal setup/restore

int main(void) {
    // Seed random for main process
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    print_info("Main", "Initializing system...");

    // Setup non-blocking terminal input
    setup_terminal_noecho_nonblock();

    // Initialize IPC resources
    if (initialize_ipc() == -1) {
        restore_terminal();
        return EXIT_FAILURE;
    }

    // Register signal handlers for parent termination
    register_parent_signal_handlers();

    // Register cleanup function to run on exit
    if (atexit(cleanup_resources) != 0) {
        print_error("Main", "Failed to register atexit cleanup function");
        cleanup_resources(); // Attempt manual cleanup
        restore_terminal();
        return EXIT_FAILURE;
    }

    printf("\r\n--- Producer/Consumer Control ---\r\n");
    printf("  p: Add Producer   c: Add Consumer\r\n");
    printf("  P: Remove Producer C: Remove Consumer\r\n");
    printf("  s: Show Status    q: Quit\r\n");
    printf("---------------------------------\r\n");
    printf("Enter command (p,c,P,C,s,q): "); // Initial prompt
    fflush(stdout);

    char command = 0;
    // FIX: Removed unused variables
    // unsigned long last_added = 0;
    // unsigned long last_extracted = 0;
    // bool activity_occurred = false;

    while (!g_terminate_flag) {
        if (kbhit()) {
            command = (char)getchar();
            printf("\r\n"); // Move to new line after command input

            switch (command) {
                case 'p':
                    create_new_producer();
                    break;
                case 'c':
                    create_new_consumer();
                    break;
                case 'P':
                    stop_last_producer();
                    break;
                case 'C':
                    stop_last_consumer();
                    break;
                case 's':
                    display_status();
                    break;
                case 'q':
                    print_info("Main", "Quit command received. Initiating shutdown...");
                    g_terminate_flag = 1;
                    break;
                default:
                    printf("[Main] Unknown command: '%c'\r\n", command);
                    break;
            }
            if (!g_terminate_flag) {
                printf("Enter command (p,c,P,C,s,q): "); // Reprint prompt
                fflush(stdout);
            }
        }

        // FIX: Removed unused activity tracking block
        /*
         *       // Update activity flag (optional, might be removed if not needed elsewhere)
         *       unsigned long current_added = get_added_count();
         *       unsigned long current_extracted = get_extracted_count();
         *       if (current_added > last_added || current_extracted > last_extracted) {
         *           activity_occurred = true;
         *           last_added = current_added;
         *           last_extracted = current_extracted;
    }
    */

        // --- REMOVED DEADLOCK PREVENTION BLOCK ---

        // Pause briefly using nanosleep
        struct timespec loop_delay = {0, 100000000L}; // 100ms
        nanosleep(&loop_delay, NULL); // Ignore interruptions in main loop delay

    } // end while

    // --- Termination Sequence ---
    // atexit handler (cleanup_resources) will be called automatically.

    print_info("Main", "Exiting.");
    // restore_terminal(); // atexit handler calls this

    return EXIT_SUCCESS;
}
