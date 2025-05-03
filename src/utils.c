#include "common.h"

// For non-blocking terminal input
static struct termios original_termios;
static int terminal_modified = 0;
static int original_fcntl_flags;

/*
 * print_error
 * Prints an error message to stderr, prefixed with "ERROR: ".
 */
void print_error(const char *prefix, const char *msg) {
    fprintf(stderr, "ERROR: [%s] %s (errno %d: %s)\r\n", prefix, msg, errno, strerror(errno));
    fflush(stderr);
}

/*
 * print_info
 * Prints an informational message to stdout, prefixed.
 */
void print_info(const char *prefix, const char *msg) {
    printf("[%s] %s\r\n", prefix, msg);
    fflush(stdout);
}


/*
 * setup_terminal_noecho_nonblock
 * Configures the terminal for non-blocking, no-echo input.
 * Saves original settings for restoration.
 */
void setup_terminal_noecho_nonblock(void) {
    if (!isatty(STDIN_FILENO)) {
        print_error("Terminal", "Standard input is not a terminal.");
        // Decide if this is fatal or just prevents kbhit
        return; // Allow running without tty for non-interactive testing?
    }
    if (tcgetattr(STDIN_FILENO, &original_termios) == -1) {
        print_error("Terminal", "tcgetattr failed");
        exit(EXIT_FAILURE); // Cannot proceed without saving state
    }

    struct termios new_termios = original_termios;
    // Disable canonical mode (line buffering) and echo
    new_termios.c_lflag &= ~(unsigned long)(ICANON | ECHO);
    // Set non-blocking read (VMIN=0, VTIME=0)
    new_termios.c_cc[VMIN] = 0;
    new_termios.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &new_termios) == -1) {
        print_error("Terminal", "tcsetattr failed");
        exit(EXIT_FAILURE); // Cannot proceed
    }

    // Set non-blocking flag for file descriptor
    original_fcntl_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (original_fcntl_flags == -1) {
        print_error("Terminal", "fcntl F_GETFL failed");
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios); // Attempt restore
        exit(EXIT_FAILURE);
    }
    if (fcntl(STDIN_FILENO, F_SETFL, original_fcntl_flags | O_NONBLOCK) == -1) {
        print_error("Terminal", "fcntl F_SETFL O_NONBLOCK failed");
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios); // Attempt restore
        exit(EXIT_FAILURE);
    }

    terminal_modified = 1;
}

/*
 * restore_terminal
 * Restores terminal settings to their original state if they were modified.
 */
void restore_terminal(void) {
    if (terminal_modified && isatty(STDIN_FILENO)) {
        // Restore file descriptor flags first
        if (fcntl(STDIN_FILENO, F_SETFL, original_fcntl_flags) == -1) {
            // Non-fatal, print warning
            fprintf(stderr, "Warning: Failed to restore fcntl flags for stdin.\r\n");
        }
        // Restore termios settings
        if (tcsetattr(STDIN_FILENO, TCSANOW, &original_termios) == -1) {
            // Non-fatal, print warning
            fprintf(stderr, "Warning: Failed to restore terminal attributes.\r\n");
        }
        // Ensure cursor is at start of new line
        printf("\r\n");
        fflush(stdout);
        terminal_modified = 0;
    }
}

/*
 * kbhit
 * Checks if a key has been pressed without blocking.
 * Assumes setup_terminal_noecho_nonblock has been called.
 * Returns 1 if a key is available, 0 otherwise.
 */
int kbhit(void) {
    if (!terminal_modified) return 0; // Don't try if terminal wasn't set up

    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);

    if (n == 1) {
        // Put the character back so getchar() can read it
        if (ungetc(c, stdin) == EOF) {
            // Should not happen with stdin, but handle defensively
            print_error("kbhit", "ungetc failed");
            return 0; // Treat as no key hit
        }
        return 1;
    } else if (n == 0) {
        // No data available (non-blocking)
        return 0;
    } else {
        // Error during read
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Expected errors for non-blocking read with no data
            return 0;
        } else {
            // Unexpected error
            print_error("kbhit", "read failed");
            // Consider how to handle this - maybe trigger termination?
            return 0; // Treat as no key hit for now
        }
    }
}

/*
 * calculate_message_hash
 * Calculates a simple hash for the message content (type, size, data).
 * Assumes msg->hash is temporarily zeroed by the caller if needed for verification.
 */
unsigned short calculate_message_hash(const message_t *msg) {
    unsigned short hash = 0;
    const unsigned char *byte_ptr;
    size_t i;

    // Include type
    hash = (hash << 5) + hash + msg->type;

    // Include size
    hash = (hash << 5) + hash + msg->size;

    // Include data bytes up to msg->size
    byte_ptr = msg->data;
    for (i = 0; i < msg->size; ++i) {
        hash = (hash << 5) + hash + byte_ptr[i];
    }

    return hash;
}
