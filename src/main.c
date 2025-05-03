#include "common.h"
#include "ipc_manager.h"
#include "utils.h"

int main(void) {

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    print_info("Main", "Initializing system...");


    setup_terminal_noecho_nonblock();


    if (initialize_ipc() == -1) {
        restore_terminal();
        return EXIT_FAILURE;
    }


    register_parent_signal_handlers();


    if (atexit(cleanup_resources) != 0) {
        print_error("Main", "Failed to register atexit cleanup function");
        cleanup_resources();
        restore_terminal();
        return EXIT_FAILURE;
    }

    printf("\r\n--- Producer/Consumer Control ---\r\n");
    printf("  p: Add Producer   c: Add Consumer\r\n");
    printf("  P: Remove Producer C: Remove Consumer\r\n");
    printf("  s: Show Status    q: Quit\r\n");
    printf("---------------------------------\r\n");
    printf("Enter command (p,c,P,C,s,q): ");
    fflush(stdout);

    char command = 0;





    while (!g_terminate_flag) {
        if (kbhit()) {
            command = (char)getchar();
            printf("\r\n");

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
                printf("Enter command (p,c,P,C,s,q): ");
                fflush(stdout);
            }
        }


        /*
         *
         *       unsigned long current_added = get_added_count();
         *       unsigned long current_extracted = get_extracted_count();
         *       if (current_added > last_added || current_extracted > last_extracted) {
         *           activity_occurred = true;
         *           last_added = current_added;
         *           last_extracted = current_extracted;
    }
    */




        struct timespec loop_delay = {0, 100000000L};
        nanosleep(&loop_delay, NULL);

    }




    print_info("Main", "Exiting.");


    return EXIT_SUCCESS;
}
