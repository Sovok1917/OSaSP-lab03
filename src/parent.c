/*
 * parent.c
 *
 * Parent process for managing child processes based on keyboard input.
 * Spawns children ('+'), deletes the last one ('-'), lists all ('l'),
 * kills all ('k'), enables child output ('1'), disables child output ('2'),
 * or quits ('q').
 * Children execute the 'child' program found via the CHILD_PATH env variable.
 */
#define _POSIX_C_SOURCE 200809L // Use POSIX.1-2008 for sigaction, etc.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h> // For terminal raw mode
#include <string.h>  // For memset, strerror, memmove
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h> // For errno
#include <stdint.h> // For SIZE_MAX

// --- Constants ---
#define CHILD_PROG_NAME "child" // Name of the child executable
#define INITIAL_CHILD_CAPACITY 8 // Initial capacity for child PID array
#define MAX_PATH_LEN 1024       // Max length for executable path buffer

// --- Globals ---
static pid_t *g_child_pids = NULL; // Array of child PIDs
static size_t g_child_count = 0;   // Number of active children
static size_t g_child_capacity = 0; // Allocated capacity of g_child_pids
static struct termios g_orig_termios; // To restore terminal settings
static char g_child_exec_path[MAX_PATH_LEN]; // Full path to the child executable
static volatile sig_atomic_t g_terminate_flag = 0; // Flag for graceful shutdown

// --- Type Definitions ---
typedef struct termios termios_t;

// --- Function Prototypes ---
static void enable_raw_mode(void);
static void disable_raw_mode(void);
static void cleanup_resources(void);
static void handle_signal(int sig);
static void register_signal_handlers(void);
static int add_child_pid(pid_t pid);
static void remove_child_pid_at_index(size_t index); // Added helper
static void kill_all_children(const char *reason);
static void signal_all_children(int sig);
static void spawn_child(void);
static void kill_last_child(void);
static void list_children(void);
static void initialize_globals(void);
static ssize_t safe_write(int fd, const void *buf, size_t count);


/*
 * main
 *
 * Entry point of the parent process.
 * Sets up terminal, signal handlers, and the main command loop.
 *
 * Accepts:
 *   argc - Argument count (expected: 1)
 *   argv - Argument vector (unused)
 *
 * Returns:
 *   EXIT_SUCCESS on normal exit.
 *   EXIT_FAILURE on critical errors (e.g., setup failure).
 */
int main(int argc, char *argv[]) {
    (void)argv; // Explicitly mark argv as unused

    // --- Initialize Globals FIRST ---
    initialize_globals(); // Initialize static globals

    // Check for parameters - This program expects none directly.
    if (argc > 1) {
        if (fprintf(stderr, "Info: This program doesn't expect command-line arguments.\n") < 0) { /* Handle error? */ }
        if (fprintf(stderr, "      It uses the CHILD_PATH environment variable to find the child executable.\n") < 0) { /* Handle error? */ }
    }

    // --- Get Child Executable Path ---
    const char *child_path_dir = getenv("CHILD_PATH");
    if (child_path_dir == NULL) {
        if (fprintf(stderr, "Error: CHILD_PATH environment variable not set.\n") < 0) { /* Handle error? */ }
        if (fprintf(stderr, "       Please set CHILD_PATH to the directory containing the '%s' executable.\n", CHILD_PROG_NAME) < 0) { /* Handle error? */ }
        return EXIT_FAILURE;
    }
    // Construct full path, check buffer size
    int path_len = snprintf(g_child_exec_path, sizeof(g_child_exec_path), "%s/%s", child_path_dir, CHILD_PROG_NAME);
    if (path_len < 0 || (size_t)path_len >= sizeof(g_child_exec_path)) {
        if (fprintf(stderr, "Error: Child executable path is too long or formatting error.\n") < 0) { /* Handle error? */ }
        return EXIT_FAILURE;
    }
    // Check if the child executable exists and is executable
    if (access(g_child_exec_path, X_OK) != 0) {
        // Use strerror for better error reporting
        if (fprintf(stderr, "Error: Child executable '%s' not found or not executable (errno %d: %s).\n",
            g_child_exec_path, errno, strerror(errno)) < 0) { /* Handle error? */ }
            return EXIT_FAILURE;
    }

    // --- Initialization (Continued) ---
    // Register cleanup function *before* potential failures that need cleanup
    if (atexit(cleanup_resources) != 0) {
        perror("Error: Failed to register atexit cleanup function");
        // Proceed cautiously
    }
    enable_raw_mode(); // May exit on failure
    register_signal_handlers(); // May exit on failure

    g_child_pids = malloc(INITIAL_CHILD_CAPACITY * sizeof(pid_t));
    if (g_child_pids == NULL) {
        disable_raw_mode(); // Disable before perror
        perror("Error: Failed to allocate memory for child PIDs");
        // atexit handler will free if needed, but exit directly here
        exit(EXIT_FAILURE);
    }
    g_child_capacity = INITIAL_CHILD_CAPACITY;
    g_child_count = 0;

    // Initial messages including new commands
    if (printf("Parent process started (PID: %d).\r\n", getpid()) < 0) { /* Handle error? */ }
    if (printf("Commands: '+' spawn, '-' kill last, 'l' list, 'k' kill all,\r\n") < 0) { /* Handle error? */ }
    if (printf("          '1' enable child output (SIGUSR1), '2' disable child output (SIGUSR2),\r\n") < 0) { /* Handle error? */ }
    if (printf("          'q' quit.\r\n") < 0) { /* Handle error? */ }
    if (printf("Using child executable: %s\r\n", g_child_exec_path) < 0) { /* Handle error? */ } // Should be correct now
    if (fflush(stdout) == EOF) {
        disable_raw_mode();
        perror("PARENT: Error flushing initial messages");
        exit(EXIT_FAILURE);
    }

    // --- Main Command Loop ---
    char c;
    while (!g_terminate_flag) { // Check termination flag set by signal handler
        ssize_t read_result = read(STDIN_FILENO, &c, 1);

        if (read_result == 1) {
            switch (c) {
                case '+':
                    safe_write(STDOUT_FILENO, "\r\n", 2); // Move to new line before output
                    spawn_child();
                    break;
                case '-':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    kill_last_child();
                    break;
                case 'l':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    list_children();
                    break;
                case 'k':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    kill_all_children("Received 'k' command.");
                    // Message about completion now inside kill_all_children
                    break;
                case '1': // Enable child output (SIGUSR1)
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    signal_all_children(SIGUSR1);
                    break;
                case '2': // Disable child output (SIGUSR2)
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    signal_all_children(SIGUSR2);
                    break;
                case 'q':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    // Use stderr for exit message
                    if (fprintf(stderr, "PARENT [%d]: Received 'q' command. Initiating shutdown.\r\n", getpid()) < 0) { /* Handle error? */ }
                    g_terminate_flag = 1; // Set flag to exit loop gracefully
                    break;
                default:
                    // Ignore other characters silently in raw mode
                    break;
            }
            if (fflush(stdout) == EOF) {
                fprintf(stderr, "Warning: fflush(stdout) failed in command loop.\n");
            }
            if (fflush(stderr) == EOF) {
                fprintf(stderr, "Warning: fflush(stderr) failed in command loop.\n");
            }
        } else if (read_result == 0) {
            // EOF on stdin - treat as quit
            safe_write(STDOUT_FILENO, "\r\n", 2);
            if (fprintf(stderr, "PARENT [%d]: EOF detected on stdin. Initiating shutdown.\r\n", getpid()) < 0) { /* Handle error? */ }
            g_terminate_flag = 1; // Set flag to exit loop gracefully
        } else if (errno == EINTR) {
            // Interrupted by signal (e.g., SIGCHLD or termination signal), loop will check g_terminate_flag
            continue;
        } else {
            // Read error
            disable_raw_mode(); // Disable raw mode to see perror output correctly
            perror("PARENT: Error reading from stdin");
            exit(EXIT_FAILURE); // atexit handler will run
        }
    }

    // Normal exit after loop termination (e.g., 'q' or EOF or signal)
    // atexit handler (cleanup_resources) will run automatically.
    return EXIT_SUCCESS;
}

/*
 * initialize_globals
 *
 * Explicitly initializes static global variables at runtime.
 *
 * Accepts: None
 * Returns: None
 */
static void initialize_globals(void) {
    g_child_pids = NULL;
    g_child_count = 0;
    g_child_capacity = 0;
    memset(&g_orig_termios, 0, sizeof(g_orig_termios));
    g_child_exec_path[0] = '\0'; // Initialize path string
    g_terminate_flag = 0;
}

/*
 * safe_write
 *
 * Wrapper around write() to handle EINTR and partial writes.
 * Suitable for use outside signal handlers. For signal handlers,
 * direct write() with loop might be needed if partial writes are possible.
 *
 * Accepts:
 *   fd - File descriptor
 *   buf - Buffer to write
 *   count - Number of bytes to write
 *
 * Returns:
 *   Total bytes written, or -1 on error (sets errno).
 */
static ssize_t safe_write(int fd, const void *buf, size_t count) {
    size_t total_written = 0;
    const char *ptr = (const char *)buf;

    while (total_written < count) {
        ssize_t written = write(fd, ptr + total_written, count - total_written);
        if (written == -1) {
            if (errno == EINTR) {
                continue; // Interrupted, try again
            }
            return -1; // Other error
        }
        if (written == 0) {
            // Should not happen for regular files/terminals unless fd is closed?
            errno = EIO; // Indicate an unusual I/O error
            return -1;
        }
        total_written += (size_t)written;
    }
    return (ssize_t)total_written;
}


/*
 * enable_raw_mode
 *
 * Switches the terminal controlling stdin to raw mode.
 * Disables echo and canonical processing. Saves original settings.
 * Exits on failure.
 *
 * Accepts: None
 * Returns: None
 */
static void enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) {
        if (fprintf(stderr, "Error: Standard input is not a terminal. Raw mode not applicable.\n") < 0) { /* Handle error? */ }
        exit(EXIT_FAILURE);
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        perror("Error: tcgetattr failed");
        exit(EXIT_FAILURE);
    }

    termios_t raw = g_orig_termios;
    // Apply raw mode flags using bitwise operations per POSIX
    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (unsigned long)(CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1; // Read 1 byte at a time
    raw.c_cc[VTIME] = 0; // Blocking read

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("Error: tcsetattr failed to enable raw mode");
        // Attempt to restore original settings before exiting
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios); // Ignore error here
        exit(EXIT_FAILURE);
    }
}

/*
 * disable_raw_mode
 *
 * Restores the terminal settings for stdin to their original state.
 * Ensures cursor is on a new line. Uses safe_write for output.
 *
 * Accepts: None
 * Returns: None
 */
static void disable_raw_mode(void) {
    // Check if STDIN is actually a tty before attempting restore
    if (!isatty(STDIN_FILENO)) {
        return; // Nothing to restore if not a tty
    }
    // Only restore if tcgetattr succeeded initially (basic check)
    // Check if any flags were actually changed (more robust check needed if complex)
    if (g_orig_termios.c_lflag != 0 || g_orig_termios.c_iflag != 0 || g_orig_termios.c_oflag != 0) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios) == -1) {
            // Use safe_write for error message in case called during cleanup
            const char msg[] = "Warning: Failed to restore terminal attributes.\n";
            safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
            // perror("Warning: Failed to restore terminal attributes"); // Not safe if called from signal context via exit
        }
    }
    // Ensure the cursor is at the beginning of a new line after potentially messy raw output
    safe_write(STDOUT_FILENO, "\r\n", 2);
    // No fflush needed after write() for unbuffered/line-buffered stderr/stdout usually
}

/*
 * cleanup_resources
 *
 * Frees allocated memory, kills remaining children, and restores terminal mode.
 * Intended to be registered with atexit(). Uses stderr for messages via safe_write.
 *
 * Accepts: None
 * Returns: None
 */
static void cleanup_resources(void) {
    // Restore terminal FIRST, before printing final messages
    disable_raw_mode();

    // Use safe_write for cleanup messages to keep stdout clean and be safer
    pid_t pid = getpid();
    char msg_buf[128];
    int len = snprintf(msg_buf, sizeof(msg_buf), "PARENT [%d]: Cleaning up...\r\n", pid);
    if (len > 0) {
        safe_write(STDERR_FILENO, msg_buf, (size_t)len);
    }

    kill_all_children("Parent exiting."); // This function now uses stderr/safe_write

    if (g_child_pids != NULL) {
        free(g_child_pids);
        g_child_pids = NULL; // Avoid double free if called multiple times
        g_child_count = 0;
        g_child_capacity = 0;
    }

    len = snprintf(msg_buf, sizeof(msg_buf), "PARENT [%d]: Cleanup complete.\r\n", pid);
    if (len > 0) {
        safe_write(STDERR_FILENO, msg_buf, (size_t)len);
    }
    // No fflush needed for stderr usually
}

/*
 * handle_signal
 *
 * Signal handler for SIGINT, SIGTERM, SIGQUIT (sets termination flag)
 * and SIGCHLD (reaps zombies). Async-signal-safe.
 * IMPORTANT: This handler only reaps zombies; it does NOT update g_child_pids.
 *
 * Accepts:
 *   sig - The signal number received.
 *
 * Returns: None
 */
static void handle_signal(int sig) {
    int saved_errno = errno; // Preserve errno across signal handler

    if (sig == SIGCHLD) {
        pid_t pid;
        // Reap all available zombies non-blockingly
        while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
            // Child reaped. We don't modify g_child_pids here for signal safety.
            // The list will be cleaned up when kill/signal functions encounter ESRCH.
            // Optionally log reaped child using safe_write for debugging:
            // char msg[64];
            // int len = snprintf(msg, sizeof(msg),"PARENT: Reaped child %d via SIGCHLD\n", pid);
            // if (len > 0) safe_write(STDERR_FILENO, msg, (size_t)len);
        }
        // If waitpid returned -1 and errno is ECHILD, it means no children left (expected)
        // Otherwise, log unexpected errors if desired (using safe_write)
        if (pid == -1 && errno != ECHILD) {
            const char msg[] = "PARENT: Error in waitpid (SIGCHLD handler).\n";
            safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
    } else if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT) {
        // Received a termination signal
        // Set the flag for the main loop to handle termination gracefully
        g_terminate_flag = 1;
        // Optionally write a message (async-signal safe)
        const char msg[] = "\r\nPARENT: Termination signal received, initiating shutdown...\r\n";
        safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
        // DO NOT call exit() or other non-async-signal-safe functions here.
        // Let the main loop detect the flag and exit cleanly.
    }

    errno = saved_errno; // Restore errno
}


/*
 * register_signal_handlers
 *
 * Sets up signal handlers for SIGINT, SIGTERM, SIGQUIT, and SIGCHLD.
 * Exits on failure.
 *
 * Accepts: None
 * Returns: None
 */
static void register_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    if (sigemptyset(&sa.sa_mask) == -1) {
        disable_raw_mode(); // Ensure terminal is usable for perror
        perror("Error: sigemptyset failed");
        exit(EXIT_FAILURE);
    }

    // --- Termination Signals (SIGINT, SIGTERM, SIGQUIT) ---
    // DO NOT restart syscalls like read() for termination signals.
    // We want read() to return EINTR so the main loop checks g_terminate_flag.
    sa.sa_flags = 0; // Ensure SA_RESTART is OFF
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGQUIT, &sa, NULL) == -1) {
        disable_raw_mode();
    perror("Error: Failed to register termination signal handlers");
    exit(EXIT_FAILURE);
        }

        // --- SIGCHLD handler ---
        // Restarting syscalls is acceptable for SIGCHLD, also use NOCLDSTOP.
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; // SET flags specifically for SIGCHLD
        if (sigaction(SIGCHLD, &sa, NULL) == -1) {
            disable_raw_mode();
            perror("Error: Failed to register SIGCHLD handler");
            exit(EXIT_FAILURE);
        }

        // --- Ignore SIGUSR1/SIGUSR2 in the parent itself ---
        memset(&sa, 0, sizeof(sa)); // Clear struct again
        sa.sa_handler = SIG_IGN;
        sa.sa_flags = 0; // No flags needed for SIG_IGN
        if (sigaction(SIGUSR1, &sa, NULL) == -1 || sigaction(SIGUSR2, &sa, NULL) == -1) {
            // Non-critical, maybe just warn
            fprintf(stderr, "Warning: Failed to ignore SIGUSR1/SIGUSR2 in parent.\n");
        }
}


/*
 * add_child_pid
 *
 * Adds a child PID to the dynamic array, resizing if necessary.
 * Aborts on memory allocation failure.
 *
 * Accepts:
 *   pid - The PID of the child process to add.
 *
 * Returns:
 *   0 on success. Aborts on failure.
 */
static int add_child_pid(pid_t pid) {
    if (g_child_count >= g_child_capacity) {
        size_t new_capacity = (g_child_capacity == 0) ? INITIAL_CHILD_CAPACITY : g_child_capacity * 2;
        // Check for potential overflow with large capacities
        if (new_capacity < g_child_capacity || (new_capacity > (SIZE_MAX / sizeof(pid_t)))) {
            disable_raw_mode();
            if (fprintf(stderr, "Error: Child PID array capacity overflow.\r\n") < 0) { /* Handle error? */ }
            abort(); // Cannot recover from potential overflow
        }
        pid_t *new_pids = realloc(g_child_pids, new_capacity * sizeof(pid_t));
        if (new_pids == NULL) {
            // Realloc failure is critical for tracking children
            disable_raw_mode();
            perror("Error: Failed to reallocate memory for child PIDs");
            // Kill the untracked child before aborting
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0); // Try to reap immediately
            abort(); // Abort as state is inconsistent
        }
        g_child_pids = new_pids;
        g_child_capacity = new_capacity;
    }
    g_child_pids[g_child_count++] = pid;
    return 0;
}

/*
 * remove_child_pid_at_index
 *
 * Removes a child PID from the array at the specified index by shifting
 * subsequent elements down. Assumes index is valid relative to current count.
 *
 * Accepts:
 *   index - The index of the PID to remove.
 *
 * Returns: None
 */
static void remove_child_pid_at_index(size_t index) {
    pid_t parent_pid = getpid(); // For potential error messages
    if (index >= g_child_count) {
        // Should not happen if called correctly, but add a safeguard
        if (fprintf(stderr, "PARENT [%d]: Error: Invalid index %zu in remove_child_pid_at_index (count=%zu).\r\n",
            parent_pid, index, g_child_count) < 0) { /* Handle error? */ }
            return;
    }

    // Shift elements down using memmove (safer for overlapping regions, though not needed here)
    size_t elements_to_move = g_child_count - index - 1;
    if (elements_to_move > 0) {
        memmove(&g_child_pids[index], &g_child_pids[index + 1], elements_to_move * sizeof(pid_t));
    }

    g_child_count--; // Decrement the count *after* shifting

    // Optional: Consider shrinking the array with realloc if g_child_count becomes
    // much smaller than g_child_capacity (e.g., less than half). Not implemented here.
    // Example:
    // if (g_child_capacity > INITIAL_CHILD_CAPACITY && g_child_count < g_child_capacity / 2) {
    //     size_t new_capacity = g_child_capacity / 2;
    //     if (new_capacity < INITIAL_CHILD_CAPACITY) new_capacity = INITIAL_CHILD_CAPACITY;
    //     pid_t *new_pids = realloc(g_child_pids, new_capacity * sizeof(pid_t));
    //     if (new_pids != NULL) { // Only update if realloc succeeds
    //         g_child_pids = new_pids;
    //         g_child_capacity = new_capacity;
    //     } // Ignore realloc failure when shrinking
    // }
}


/*
 * kill_all_children
 *
 * Sends SIGKILL to all currently tracked child processes.
 * Removes successfully killed or already exited (ESRCH) children from the list.
 * Prints actions to stderr.
 *
 * Accepts:
 *   reason - A string indicating why children are being killed (for logging).
 *
 * Returns: None
 */
static void kill_all_children(const char *reason) {
    pid_t parent_pid = getpid();

    if (g_child_count == 0) {
        // No message needed if called during cleanup with no children
        // Only print if explicitly called via 'k' command etc. and not exiting
        if (strcmp(reason, "Parent exiting.") != 0) {
            if (printf("PARENT [%d]: No children to kill.\r\n", parent_pid) < 0) { /* Handle error? */ }
        }
        return;
    }

    // Use stderr for the action message itself
    if (fprintf(stderr, "PARENT [%d]: Killing all %zu children (%s).\r\n", parent_pid, g_child_count, reason) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    // Iterate backwards because we might remove elements
    for (size_t i = g_child_count; i > 0; --i) {
        size_t current_index = i - 1; // Index in the array
        pid_t pid_to_kill = g_child_pids[current_index];

        // Use stderr for individual kill attempts/warnings
        if (fprintf(stderr, "PARENT [%d]: Sending SIGKILL to child PID %d...\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
        if (fflush(stderr) == EOF) { /* Handle error? */ }

        int kill_result = kill(pid_to_kill, SIGKILL);

        if (kill_result == -1) {
            if (errno == ESRCH) { // Process already dead
                if (fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
                // Child is gone, remove it from the list
                remove_child_pid_at_index(current_index);
            } else { // Other error (permissions?)
                if (fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno %d: %s).\r\n",
                    pid_to_kill, errno, strerror(errno)) < 0) { /* Handle error? */ }
                    // Don't remove on other errors, maybe log? Child might still be running.
            }
        } else {
            // Kill sent successfully. Remove from tracking list.
            // SIGCHLD handler will do the reaping.
            remove_child_pid_at_index(current_index);
        }
        // NOTE: Because we remove the element at current_index, the loop
        // condition (i > 0) and the next iteration's index calculation
        // (i - 1) remain correct relative to the now-shorter list.
    }

    // Report final status based on remaining count
    if (g_child_count == 0) {
        if (fprintf(stderr, "PARENT [%d]: All tracked children processed for killing.\r\n", parent_pid) < 0) { /* Handle error? */ }
    } else {
        if (fprintf(stderr, "PARENT [%d]: Processed children for killing. %zu children remain tracked due to kill errors.\r\n", parent_pid, g_child_count) < 0) { /* Handle error? */ }
    }
    if (fflush(stderr) == EOF) { /* Handle error? */ }
}

/*
 * signal_all_children
 *
 * Sends the specified signal (SIGUSR1 or SIGUSR2) to all tracked children.
 * Reports actions to stdout/stderr. Does NOT remove children on ESRCH here.
 *
 * Accepts:
 *   sig - The signal number to send (SIGUSR1 or SIGUSR2).
 *
 * Returns: None
 */
static void signal_all_children(int sig) {
    pid_t parent_pid = getpid();
    const char *sig_name = (sig == SIGUSR1) ? "SIGUSR1 (enable output)" : "SIGUSR2 (disable output)";

    if (g_child_count == 0) {
        if (printf("PARENT [%d]: No children to send %s to.\r\n", parent_pid, sig_name) < 0) { /* Handle error? */ }
        return;
    }

    // Use stderr for the action message itself for clarity
    if (fprintf(stderr, "PARENT [%d]: Sending %s to all %zu children.\r\n", parent_pid, sig_name, g_child_count) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    size_t signaled_count = 0;
    size_t esrch_count = 0; // Count children already gone
    for (size_t i = 0; i < g_child_count; ++i) {
        pid_t child_pid = g_child_pids[i];
        if (kill(child_pid, sig) == 0) {
            signaled_count++;
        } else {
            // ESRCH means child likely exited between command and signal dispatch
            if (errno == ESRCH) {
                esrch_count++;
                // Optionally log that the child was already gone (can be noisy)
                // fprintf(stderr, "PARENT [%d]: Child PID %d already exited before %s.\n", parent_pid, child_pid, sig_name);
            } else {
                if (fprintf(stderr, "Warning: Failed to send %s to PID %d (errno %d: %s).\r\n",
                    sig_name, child_pid, errno, strerror(errno)) < 0) { /* Handle error? */ }
            }
            // We do NOT remove the PID from the list here. Let kill_all/kill_last handle cleanup.
        }
    }

    // Report result to stdout
    if (printf("PARENT [%d]: Attempted to send %s to %zu children. Success: %zu, Already Exited: %zu.\r\n",
        parent_pid, sig_name, g_child_count, signaled_count, esrch_count) < 0) { /* Handle error? */ }
}


/*
 * spawn_child
 *
 * Forks and execs a new child process using the path in g_child_exec_path.
 * Adds the new child's PID to the list. Reports success (stdout) or failure (stderr).
 * Aborts parent on critical failure (add_child_pid).
 *
 * Accepts: None
 * Returns: None
 */
static void spawn_child(void) {
    pid_t pid = fork();

    if (pid == -1) {
        // Fork failed in parent, report to stderr
        if (fprintf(stderr, "Error: Failed to fork child process (errno %d: %s)\r\n", errno, strerror(errno)) < 0) { /* Handle error? */ }
        return; // Continue parent operation
    } else if (pid == 0) {
        // --- Child Process ---

        /* FIX: REMOVED this block to prevent child from resetting terminal mode */
        /*
         *       // Attempt to restore terminal settings for the child if it was inherited
         *       if (isatty(STDIN_FILENO)) {
         *           // Ignore error here, child might not need terminal control
         *           tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    }
    */

        // Reset signal handlers to defaults for the child
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL; // Default action
        sigaction(SIGINT, &sa, NULL); // Ignore errors, best effort
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);
        // SIGALRM, SIGUSR1, SIGUSR2 will be set by the child itself via its register_signal_handlers.

        // Prepare arguments for execv
        // argv[0] should be the program name itself by convention
        char *const child_argv[] = { g_child_exec_path, NULL };
        execv(g_child_exec_path, child_argv);

        // If execv returns, an error occurred - print to stderr
        // Use write for safety, although less likely needed here than signal handler
        int exec_errno = errno;
        char err_buf[256];
        int len = snprintf(err_buf, sizeof(err_buf), "CHILD: Error: Failed to execute '%s' (errno %d: %s)\n",
                           g_child_exec_path, exec_errno, strerror(exec_errno));
        if (len > 0) {
            safe_write(STDERR_FILENO, err_buf, (size_t)len);
        }
        exit(EXIT_FAILURE); // Child exits if exec fails

    } else {
        // --- Parent Process ---
        // add_child_pid now aborts on failure, so no need to check return value here
        add_child_pid(pid);
        // Report success to stdout
        if (printf("PARENT [%d]: Spawned child process with PID %d. Total children: %zu\r\n",
            getpid(), pid, g_child_count) < 0) { /* Handle error? */ }
    }
}

/*
 * kill_last_child
 *
 * Sends SIGKILL to the most recently spawned child process.
 * Removes the child from the list if kill succeeds or fails with ESRCH.
 * Reports the action (stdout/stderr).
 *
 * Accepts: None
 * Returns: None
 */
static void kill_last_child(void) {
    pid_t parent_pid = getpid();

    if (g_child_count == 0) {
        if (printf("PARENT [%d]: No children to kill.\r\n", parent_pid) < 0) { /* Handle error? */ }
        return;
    }

    // Target the last child added
    size_t last_index = g_child_count - 1;
    pid_t pid_to_kill = g_child_pids[last_index];

    // Use stderr for action message
    if (fprintf(stderr, "PARENT [%d]: Sending SIGKILL to last child (PID %d).\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    int kill_result = kill(pid_to_kill, SIGKILL);
    int removed = 0; // Flag to track if removed

    if (kill_result == -1) {
        if (errno == ESRCH) { // Process already dead
            if (fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
            // Child is gone, remove it from the list
            remove_child_pid_at_index(last_index);
            removed = 1;
        } else { // Other error
            if (fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno %d: %s).\r\n",
                pid_to_kill, errno, strerror(errno)) < 0) { /* Handle error? */ }
                // Do not remove from list on other errors.
        }
    } else {
        // Kill sent successfully. Remove from tracking list.
        // SIGCHLD handler will eventually reap the process.
        remove_child_pid_at_index(last_index);
        removed = 1;
    }

    // Report result to stdout
    if (removed) {
        if (printf("PARENT [%d]: Removed tracking for child %d. Remaining children: %zu\r\n",
            parent_pid, pid_to_kill, g_child_count) < 0) { /* Handle error? */ }
    } else {
        if (printf("PARENT [%d]: Did not remove tracking for child %d due to kill error. Remaining children: %zu\r\n",
            parent_pid, pid_to_kill, g_child_count) < 0) { /* Handle error? */ }
    }
}

/*
 * list_children
 *
 * Prints the PID of the parent and the PIDs of all currently tracked children to stdout.
 * Uses \r\n for raw mode compatibility. Checks printf return values.
 *
 * Accepts: None
 * Returns: None
 */
static void list_children(void) {
    pid_t parent_pid = getpid();
    // Use temporary buffer for potentially long output to minimize interleaved writes
    char list_buf[4096]; // Adjust size if many children expected
    int current_pos = 0;
    int remaining_buf = sizeof(list_buf);
    int ret;

    ret = snprintf(list_buf + current_pos, remaining_buf, "PARENT [%d]: Listing processes:\r\n", parent_pid);
    if (ret < 0 || ret >= remaining_buf) goto print_error;
    current_pos += ret; remaining_buf -= ret;

    ret = snprintf(list_buf + current_pos, remaining_buf, "  Parent: %d\r\n", parent_pid);
    if (ret < 0 || ret >= remaining_buf) goto print_error;
    current_pos += ret; remaining_buf -= ret;

    if (g_child_count == 0) {
        ret = snprintf(list_buf + current_pos, remaining_buf, "  No tracked children.\r\n");
        if (ret < 0 || ret >= remaining_buf) goto print_error;
        current_pos += ret; remaining_buf -= ret;
    } else {
        ret = snprintf(list_buf + current_pos, remaining_buf, "  Tracked Children (%zu):\r\n", g_child_count);
        if (ret < 0 || ret >= remaining_buf) goto print_error;
        current_pos += ret; remaining_buf -= ret;

        for (size_t i = 0; i < g_child_count; ++i) {
            // Indicate status is based on tracking, not real-time check
            ret = snprintf(list_buf + current_pos, remaining_buf, "    - PID %d (tracked)\r\n", g_child_pids[i]);
            if (ret < 0 || ret >= remaining_buf) goto print_error;
            current_pos += ret; remaining_buf -= ret;
        }
    }

    // Write the whole buffer at once
    if (safe_write(STDOUT_FILENO, list_buf, (size_t)current_pos) == -1) {
        perror("PARENT: Error writing child list");
    }
    return;

    print_error:
    fprintf(stderr, "PARENT [%d]: Error: Buffer overflow while formatting child list.\r\n", parent_pid);
    // Optionally print what was formatted so far
    safe_write(STDOUT_FILENO, list_buf, (size_t)current_pos);
}
