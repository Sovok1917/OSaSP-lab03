// /home/sovok/My Programs/Code Studies/OSaSP/Везенков М.Ю./lab03/src/parent.c
/*
 * parent.c
 *
 * Parent process for managing child processes based on keyboard input.
 * Spawns children ('+'), deletes the last one ('-'), lists all ('l'),
 * kills all ('k'), enables child output ('1'), disables child output ('2'),
 * or quits ('q').
 * Children execute the 'child' program found via the CHILD_PATH env variable.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdint.h> // For SIZE_MAX


#define CHILD_PROG_NAME "child"
#define INITIAL_CHILD_CAPACITY 8
#define MAX_PATH_LEN 1024


static pid_t *g_child_pids = NULL;
static size_t g_child_count = 0;
static size_t g_child_capacity = 0;
static struct termios g_orig_termios; // Typedef'd to termios_t later, but struct termios is the actual type
static char g_child_exec_path[MAX_PATH_LEN];
static volatile sig_atomic_t g_terminate_flag = 0;


typedef struct termios termios_t; // This is fine, but g_orig_termios is already declared


static void enable_raw_mode(void);
static void disable_raw_mode(void);
static void cleanup_resources(void);
static void handle_signal(int sig);
static void register_signal_handlers(void);
static int add_child_pid(pid_t pid);
static void remove_child_pid_at_index(size_t index);
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
    (void)argv; // Mark as unused


    initialize_globals();


    if (argc > 1) {
        if (fprintf(stderr, "Info: This program doesn't expect command-line arguments.\r\n") < 0) { /* Handle error? */ }
        if (fprintf(stderr, "      It uses the CHILD_PATH environment variable to find the child executable.\r\n") < 0) { /* Handle error? */ }
    }


    const char *child_path_dir = getenv("CHILD_PATH");
    if (child_path_dir == NULL) {
        if (fprintf(stderr, "Error: CHILD_PATH environment variable not set.\r\n") < 0) { /* Handle error? */ }
        if (fprintf(stderr, "       Please set CHILD_PATH to the directory containing the '%s' executable.\r\n", CHILD_PROG_NAME) < 0) { /* Handle error? */ }
        return EXIT_FAILURE;
    }

    int path_len = snprintf(g_child_exec_path, sizeof(g_child_exec_path), "%s/%s", child_path_dir, CHILD_PROG_NAME);
    if (path_len < 0 || (size_t)path_len >= sizeof(g_child_exec_path)) {
        if (fprintf(stderr, "Error: Child executable path is too long or formatting error.\r\n") < 0) { /* Handle error? */ }
        return EXIT_FAILURE;
    }

    if (access(g_child_exec_path, X_OK) != 0) {
        // Use \r\n for consistency in raw mode messages
        if (fprintf(stderr, "Error: Child executable '%s' not found or not executable (errno %d: %s).\r\n",
            g_child_exec_path, errno, strerror(errno)) < 0) { /* Handle error? */ }
            return EXIT_FAILURE;
    }


    // Register atexit first, so it's called even if enable_raw_mode or other setup fails
    if (atexit(cleanup_resources) != 0) {
        // Use \r\n for consistency
        perror("Error: Failed to register atexit cleanup function"); // perror adds its own newline
        // No need to call disable_raw_mode() here as it wasn't enabled yet.
        exit(EXIT_FAILURE); // Exit directly if atexit registration fails
    }

    enable_raw_mode(); // Now enable raw mode
    register_signal_handlers();

    g_child_pids = malloc(INITIAL_CHILD_CAPACITY * sizeof(pid_t));
    if (g_child_pids == NULL) {
        // disable_raw_mode(); // Already handled by atexit
        perror("Error: Failed to allocate memory for child PIDs"); // perror adds its own newline
        exit(EXIT_FAILURE); // This will trigger atexit
    }
    g_child_capacity = INITIAL_CHILD_CAPACITY;
    g_child_count = 0;

    // Use \r\n for all multi-line or full-line outputs when terminal is raw
    if (printf("Parent process started (PID: %d).\r\n", getpid()) < 0) { /* Handle error? */ }
    if (printf("Commands: '+' spawn, '-' kill last, 'l' list, 'k' kill all,\r\n") < 0) { /* Handle error? */ }
    if (printf("          '1' enable child output (SIGUSR1), '2' disable child output (SIGUSR2),\r\n") < 0) { /* Handle error? */ }
    if (printf("          'q' quit.\r\n") < 0) { /* Handle error? */ }
    if (printf("Using child executable: %s\r\n", g_child_exec_path) < 0) { /* Handle error? */ }
    if (fflush(stdout) == EOF) {
        // disable_raw_mode(); // Already handled by atexit
        perror("PARENT: Error flushing initial messages");
        exit(EXIT_FAILURE); // This will trigger atexit
    }


    char c;
    while (!g_terminate_flag) {
        ssize_t read_result = read(STDIN_FILENO, &c, 1);

        if (read_result == 1) {
            // Echoing commands or providing feedback can be done here if desired
            // For example, to ensure the command prompt is clean after each command:
            // safe_write(STDOUT_FILENO, "\r\n", 2); // Move to new line before processing/printing output

            switch (c) {
                case '+':
                    safe_write(STDOUT_FILENO, "\r\n", 2); // Ensure command output starts on a new line
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
                    break;
                case '1':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    signal_all_children(SIGUSR1);
                    break;
                case '2':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    signal_all_children(SIGUSR2);
                    break;
                case 'q':
                    safe_write(STDOUT_FILENO, "\r\n", 2);
                    if (fprintf(stderr, "PARENT [%d]: Received 'q' command. Initiating shutdown.\r\n", getpid()) < 0) { /* Handle error? */ }
                    g_terminate_flag = 1; // Set flag to terminate
                    break;
                default:
                    // Optionally, provide feedback for unknown characters or ignore
                    // safe_write(STDOUT_FILENO, "\a", 1); // Bell for unknown command
                    break;
            }
            // It's good practice to flush output streams, especially in raw mode
            if (fflush(stdout) == EOF) {
                fprintf(stderr, "Warning: fflush(stdout) failed in command loop.\r\n");
            }
            if (fflush(stderr) == EOF) {
                fprintf(stderr, "Warning: fflush(stderr) failed in command loop.\r\n");
            }
        } else if (read_result == 0) { // EOF
            safe_write(STDOUT_FILENO, "\r\n", 2); // Ensure cursor is on a new line
            if (fprintf(stderr, "PARENT [%d]: EOF detected on stdin. Initiating shutdown.\r\n", getpid()) < 0) { /* Handle error? */ }
            g_terminate_flag = 1;
        } else if (errno == EINTR) { // Interrupted by a signal (e.g., SIGINT if ISIG is on)
            // If g_terminate_flag was set by the signal handler, the loop condition will catch it
            continue;
        } else { // Other read error
            // disable_raw_mode(); // Handled by atexit
            perror("PARENT: Error reading from stdin");
            exit(EXIT_FAILURE); // This will trigger atexit
        }
    }

    // The loop has exited, meaning g_terminate_flag is set.
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
    memset(&g_orig_termios, 0, sizeof(g_orig_termios)); // Initialize original termios settings
    g_child_exec_path[0] = '\0';
    g_terminate_flag = 0;
}

/*
 * safe_write
 *
 * Wrapper around write() to handle EINTR and partial writes.
 * Suitable for use outside signal handlers. For signal handlers,
 * direct write() with loop might be needed if partial writes are possible.
 * This version is fine for its current uses.
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
                continue; // Retry on interrupt
            }
            return -1; // Other error
        }
        if (written == 0) {
            // According to POSIX write(2), a return of 0 means nothing was written
            // and this should not happen if count > 0.
            // However, some interpretations treat it as an error or EOF on certain devices.
            // For robustness, treat as an error if count > 0 and written is 0.
            errno = EIO; // Indicate an I/O error
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
 * **MODIFIED: Keeps ISIG enabled so Ctrl+C generates SIGINT.**
 * Exits on failure.
 *
 * Accepts: None
 * Returns: None
 */
static void enable_raw_mode(void) {
    if (!isatty(STDIN_FILENO)) {
        if (fprintf(stderr, "Error: Standard input is not a terminal. Raw mode not applicable.\r\n") < 0) { /* Handle error? */ }
        // No raw mode to disable, so exit directly. atexit will handle g_child_pids if allocated.
        exit(EXIT_FAILURE);
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        perror("Error: tcgetattr failed");
        // No raw mode to disable. atexit will handle g_child_pids if allocated.
        exit(EXIT_FAILURE);
    }

    termios_t raw = g_orig_termios;

    // Configure input flags
    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Configure output flags: OPOST is often disabled in raw mode for full control.
    // If OPOST is disabled, the application must manually send \r\n for newlines.
    // The current code uses \r\n for its outputs, so disabling OPOST is consistent.
    raw.c_oflag &= ~(unsigned long)(OPOST);

    // Configure control flags
    raw.c_cflag |= (unsigned long)(CS8); // 8 bits per byte

    // Configure local flags:
    // Disable ECHO (characters not echoed back)
    // Disable ICANON (canonical mode, no line buffering)
    // Disable IEXTEN (implementation-defined input processing)
    // **KEEP ISIG ENABLED** so Ctrl+C, Ctrl+\, etc., generate signals.
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN);
    // If you wanted to disable ISIG, it would be:
    // raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);

    // Control characters for non-canonical mode
    raw.c_cc[VMIN] = 1;  // read() blocks until at least 1 byte is available
    raw.c_cc[VTIME] = 0; // No timeout for read()

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("Error: tcsetattr failed to enable raw mode");
        // Attempt to restore original settings if possible, though unlikely to succeed if setattr failed.
        // tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios); // This might also fail.
        // atexit handler will attempt to restore, but it's best effort.
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
    // Only attempt to restore if stdin is a TTY and g_orig_termios has been populated
    if (!isatty(STDIN_FILENO)) {
        return;
    }

    // Check if g_orig_termios seems initialized (not all zeros, which is its state after memset)
    // This is a basic check; a more robust way would be a flag set after successful tcgetattr.
    int is_orig_termios_set = 0;
    for (size_t i = 0; i < sizeof(g_orig_termios.c_cc); ++i) {
        if (g_orig_termios.c_cc[i] != 0) { is_orig_termios_set = 1; break; }
    }
    if (!is_orig_termios_set && g_orig_termios.c_iflag == 0 &&
        g_orig_termios.c_oflag == 0 && g_orig_termios.c_cflag == 0 &&
        g_orig_termios.c_lflag == 0) {
        // g_orig_termios appears to be uninitialized or all zeros, possibly tcgetattr failed
        // or was never called. Avoid restoring with potentially invalid settings.
        const char msg[] = "Warning: Original terminal attributes not available for restoration or were all zero.\r\n";
    safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
        } else {
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios) == -1) {
                // Use a simple message for async-signal safety if this were called from a signal handler
                // (though atexit is not a signal handler context).
                const char msg[] = "Warning: Failed to restore terminal attributes.\r\n";
                safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
            }
        }

        // Ensure the cursor is at the beginning of a new line after exiting raw mode.
        // This is important because the application might have left the cursor anywhere.
        safe_write(STDOUT_FILENO, "\r\n", 2);
        fflush(stdout); // Try to flush any buffered output
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
    // disable_raw_mode must be called first to ensure subsequent messages are seen correctly.
    disable_raw_mode();

    pid_t pid = getpid(); // Safe to call getpid()
    char msg_buf[128];
    int len;

    // Using snprintf and safe_write for messages from atexit handler
    len = snprintf(msg_buf, sizeof(msg_buf), "PARENT [%d]: Cleaning up...\r\n", pid);
    if (len > 0 && (size_t)len < sizeof(msg_buf)) {
        safe_write(STDERR_FILENO, msg_buf, (size_t)len);
    }

    kill_all_children("Parent exiting.");

    if (g_child_pids != NULL) {
        free(g_child_pids);
        g_child_pids = NULL; // Important to prevent double-free if cleanup is somehow called again
        g_child_count = 0;
        g_child_capacity = 0;
    }

    len = snprintf(msg_buf, sizeof(msg_buf), "PARENT [%d]: Cleanup complete.\r\n", pid);
    if (len > 0 && (size_t)len < sizeof(msg_buf)) {
        safe_write(STDERR_FILENO, msg_buf, (size_t)len);
    }
    fflush(stderr); // Try to flush stderr
}

/*
 * handle_signal
 *
 * Signal handler for SIGINT, SIGTERM, SIGQUIT (sets termination flag)
 * and SIGCHLD (reaps zombies). Async-signal-safe.
 * IMPORTANT: This handler only reaps zombies; it does NOT update g_child_pids list here
 * as modifying complex data structures in signal handlers is unsafe.
 *
 * Accepts:
 *   sig - The signal number received.
 *
 * Returns: None
 */
static void handle_signal(int sig) {
    int saved_errno = errno; // Preserve errno

    if (sig == SIGCHLD) {
        pid_t child_pid;
        // Reap all available zombie children non-blockingly
        while ((child_pid = waitpid(-1, NULL, WNOHANG)) > 0) {
            // Optionally, log that a child was reaped.
            // For this lab, the main purpose is to prevent zombies.
            // The g_child_pids list is not updated here for signal safety.
            // It will be updated during operations like kill_last_child or kill_all_children
            // when they encounter ESRCH.
        }
        // ECHILD means no more children to wait for, which is not an error in this loop.
        // Other errors from waitpid in a SIGCHLD handler are unusual but could be logged.
        if (child_pid == -1 && errno != ECHILD) {
            const char msg[] = "PARENT: Error in waitpid (SIGCHLD handler).\r\n";
            safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
        }
    } else if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT) {
        // Set the flag that the main loop will check.
        g_terminate_flag = 1;

        // Output a message. This must be async-signal-safe.
        // Using safe_write directly.
        const char msg[] = "\r\nPARENT: Termination signal received, initiating shutdown...\r\n";
        safe_write(STDERR_FILENO, msg, sizeof(msg) - 1);
        // Note: The main loop will detect g_terminate_flag and exit,
        // then atexit handler (cleanup_resources) will run.
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

    // Block common signals within the handler to prevent reentrancy issues
    // for these specific signals, though handle_signal is designed to be safe.
    if (sigfillset(&sa.sa_mask) == -1) { // Block all signals during handler execution
        // disable_raw_mode(); // Handled by atexit if main exits
        perror("Error: sigfillset failed");
        exit(EXIT_FAILURE);
    }
    // Alternatively, more targeted:
    // if (sigemptyset(&sa.sa_mask) == -1) { perror("Error: sigemptyset failed"); exit(EXIT_FAILURE); }
    // if (sigaddset(&sa.sa_mask, SIGINT) == -1) { /* ... */ }
    // if (sigaddset(&sa.sa_mask, SIGTERM) == -1) { /* ... */ }
    // if (sigaddset(&sa.sa_mask, SIGQUIT) == -1) { /* ... */ }
    // if (sigaddset(&sa.sa_mask, SIGCHLD) == -1) { /* ... */ }


    // For SIGINT, SIGTERM, SIGQUIT:
    // sa.sa_flags = 0; // Default: signal handler is not re-entered, syscalls not restarted
    // If syscalls (like read) should be interrupted and return EINTR:
    sa.sa_flags = 0; // Let them be interrupted. The main loop handles EINTR from read.

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGQUIT, &sa, NULL) == -1) {
        // disable_raw_mode(); // Handled by atexit
        perror("Error: Failed to register termination signal handlers");
    exit(EXIT_FAILURE);
        }

        // For SIGCHLD:
        // SA_RESTART can be useful for SIGCHLD if you want interrupted syscalls to restart.
        // SA_NOCLDSTOP prevents SIGCHLD when a child is stopped (e.g., by SIGSTOP/SIGTSTP).
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        if (sigaction(SIGCHLD, &sa, NULL) == -1) {
            // disable_raw_mode(); // Handled by atexit
            perror("Error: Failed to register SIGCHLD handler");
            exit(EXIT_FAILURE);
        }

        // Parent should ignore SIGUSR1 and SIGUSR2 if it's not meant to act on them.
        // This prevents accidental termination if a child (or other process) sends them to the parent.
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN; // Ignore the signal
        sa.sa_flags = 0;
        if (sigaction(SIGUSR1, &sa, NULL) == -1 || sigaction(SIGUSR2, &sa, NULL) == -1) {
            // This is a warning because the program can still function.
            fprintf(stderr, "Warning: Failed to ignore SIGUSR1/SIGUSR2 in parent.\r\n");
        }
}


/*
 * add_child_pid
 *
 * Adds a child PID to the dynamic array, resizing if necessary.
 * Aborts on memory allocation failure (as this is a critical part of tracking).
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
        // Check for overflow before multiplication
        if (g_child_capacity > SIZE_MAX / 2 && g_child_capacity != 0) { // Potential overflow with * 2
            // disable_raw_mode(); // Handled by atexit via abort()
            if (fprintf(stderr, "Error: Child PID array capacity overflow (requested %zu).\r\n", new_capacity) < 0) { /* Handle error? */ }
            abort(); // Critical error
        }
        // Check if new_capacity * sizeof(pid_t) would overflow size_t
        if (new_capacity > SIZE_MAX / sizeof(pid_t)) {
            // disable_raw_mode(); // Handled by atexit via abort()
            if (fprintf(stderr, "Error: Child PID array memory size overflow (requested %zu elements).\r\n", new_capacity) < 0) { /* Handle error? */ }
            abort(); // Critical error
        }

        pid_t *new_pids = realloc(g_child_pids, new_capacity * sizeof(pid_t));
        if (new_pids == NULL) {
            // disable_raw_mode(); // Handled by atexit via abort()
            perror("Error: Failed to reallocate memory for child PIDs");
            // Try to kill the newly created child if we can't track it.
            // This is best effort as we are about to abort.
            kill(pid, SIGKILL); // Send SIGKILL to the child we just forked but can't track
            waitpid(pid, NULL, 0); // Wait for it to ensure it's gone before aborting parent
            abort(); // Critical error
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
 * Does NOT shrink the allocated memory (g_child_capacity remains).
 *
 * Accepts:
 *   index - The index of the PID to remove.
 *
 * Returns: None
 */
static void remove_child_pid_at_index(size_t index) {
    pid_t parent_pid = getpid();
    if (index >= g_child_count) {
        // This should ideally not happen if logic is correct elsewhere.
        if (fprintf(stderr, "PARENT [%d]: Error: Invalid index %zu in remove_child_pid_at_index (count=%zu).\r\n",
            parent_pid, index, g_child_count) < 0) { /* Handle error? */ }
            return;
    }

    // Number of elements to move is g_child_count - 1 (new count) - index
    size_t elements_to_move = g_child_count - 1 - index;
    if (elements_to_move > 0) {
        // memmove is safe for overlapping regions
        memmove(&g_child_pids[index], &g_child_pids[index + 1], elements_to_move * sizeof(pid_t));
    }
    // else: removing the last element, no move needed.

    g_child_count--;

    // Optional: Shrink g_child_pids array if count is much smaller than capacity.
    // For this lab, not shrinking is simpler and likely fine.
    // Example shrink condition (e.g., if count is 1/4 of capacity and capacity > initial):
    /*
     *   if (g_child_count > 0 && g_child_capacity > INITIAL_CHILD_CAPACITY && g_child_count <= g_child_capacity / 4) {
     *       size_t new_capacity = g_child_capacity / 2;
     *       if (new_capacity < INITIAL_CHILD_CAPACITY) new_capacity = INITIAL_CHILD_CAPACITY;
     *       if (new_capacity < g_child_count) new_capacity = g_child_count; // Should not happen if logic is right
     *
     *       pid_t *new_pids = realloc(g_child_pids, new_capacity * sizeof(pid_t));
     *       if (new_pids != NULL) { // Only update if realloc (shrinking) succeeds
     *           g_child_pids = new_pids;
     *           g_child_capacity = new_capacity;
} else {
    // Failed to shrink, not critical, continue with larger array.
    // fprintf(stderr, "PARENT [%d]: Warning: realloc failed to shrink PID array.\r\n", parent_pid);
}
} else if (g_child_count == 0 && g_child_capacity > INITIAL_CHILD_CAPACITY) {
    // Optionally shrink to initial or free completely if count is 0
    // For simplicity, current code keeps it allocated until program exit.
}
*/
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
        // Only print "No children to kill" if not part of the standard exit cleanup.
        if (strcmp(reason, "Parent exiting.") != 0) {
            if (printf("PARENT [%d]: No children to kill.\r\n", parent_pid) < 0) { /* Handle error? */ }
        }
        return;
    }

    // Use stderr for operational messages like killing children
    if (fprintf(stderr, "PARENT [%d]: Killing all %zu children (%s).\r\n", parent_pid, g_child_count, reason) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    // Iterate backwards because remove_child_pid_at_index shifts elements
    for (size_t i = g_child_count; i > 0; --i) {
        size_t current_index = i - 1;
        pid_t pid_to_kill = g_child_pids[current_index];

        if (fprintf(stderr, "PARENT [%d]: Sending SIGKILL to child PID %d...\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
        if (fflush(stderr) == EOF) { /* Handle error? */ }

        int kill_result = kill(pid_to_kill, SIGKILL);

        if (kill_result == -1) {
            if (errno == ESRCH) { // Child already exited
                if (fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
                remove_child_pid_at_index(current_index); // Remove from our list
            } else { // Other error sending signal
                if (fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno %d: %s).\r\n",
                    pid_to_kill, errno, strerror(errno)) < 0) { /* Handle error? */ }
                    // Do not remove from list if kill failed for reasons other than ESRCH,
                    // as it might still be alive and we might want to retry or log it.
                    // However, for SIGKILL, if it fails with something other than ESRCH,
                    // it's an unusual situation (e.g. permission denied, which shouldn't happen for own child).
            }
        } else { // kill succeeded
            // Child will be reaped by SIGCHLD handler eventually.
            // We remove it from our active tracking list.
            if (fprintf(stderr, "PARENT [%d]: SIGKILL sent to PID %d. It will be reaped.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
            remove_child_pid_at_index(current_index);
        }
        // No explicit waitpid here; SIGCHLD handler is responsible for reaping.
        // If we waitpid here, it could interfere with the SIGCHLD handler or block.
    }

    if (g_child_count == 0) {
        if (fprintf(stderr, "PARENT [%d]: All tracked children processed for killing.\r\n", parent_pid) < 0) { /* Handle error? */ }
    } else {
        if (fprintf(stderr, "PARENT [%d]: Processed children for killing. %zu children may remain tracked due to kill errors (should be rare).\r\n", parent_pid, g_child_count) < 0) { /* Handle error? */ }
    }
    if (fflush(stderr) == EOF) { /* Handle error? */ }
}

/*
 * signal_all_children
 *
 * Sends the specified signal (SIGUSR1 or SIGUSR2) to all tracked children.
 * Reports actions to stdout/stderr. Does NOT remove children on ESRCH here,
 * as SIGCHLD handler should deal with exited children.
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

    // Use stderr for operational messages
    if (fprintf(stderr, "PARENT [%d]: Sending %s to all %zu children.\r\n", parent_pid, sig_name, g_child_count) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    size_t signaled_count = 0;
    size_t esrch_count = 0; // Count children that were already gone
    for (size_t i = 0; i < g_child_count; ++i) {
        pid_t child_pid = g_child_pids[i];
        if (kill(child_pid, sig) == 0) {
            signaled_count++;
        } else {
            if (errno == ESRCH) { // Process does not exist
                esrch_count++;
                // Don't remove from g_child_pids here; SIGCHLD handler should have/will handle it.
                // If we remove here, and SIGCHLD handler also tries, it could be problematic.
                // The list will get cleaned up eventually by kill_all_children or kill_last_child
                // if they encounter ESRCH.
                if (fprintf(stderr, "PARENT [%d]: Child PID %d for %s already exited (ESRCH).\r\n", parent_pid, child_pid, sig_name) < 0) { /* Handle error? */ }

            } else { // Other error
                if (fprintf(stderr, "Warning: Failed to send %s to PID %d (errno %d: %s).\r\n",
                    sig_name, child_pid, errno, strerror(errno)) < 0) { /* Handle error? */ }
            }
        }
    }

    // Use printf for user-facing summary
    if (printf("PARENT [%d]: Attempted to send %s to %zu children. Success: %zu, Already Exited (ESRCH): %zu.\r\n",
        parent_pid, sig_name, g_child_count, signaled_count, esrch_count) < 0) { /* Handle error? */ }
}


/*
 * spawn_child
 *
 * Forks and execs a new child process using the path in g_child_exec_path.
 * Adds the new child's PID to the list. Reports success (stdout) or failure (stderr).
 * Aborts parent on critical failure in add_child_pid.
 *
 * Accepts: None
 * Returns: None
 */
static void spawn_child(void) {
    pid_t pid = fork();

    if (pid == -1) { // Fork failed
        if (fprintf(stderr, "Error: Failed to fork child process (errno %d: %s)\r\n", errno, strerror(errno)) < 0) { /* Handle error? */ }
        return;
    } else if (pid == 0) { // Child process
        // Child-specific setup:
        // 1. Restore default signal handlers for signals parent might ignore or handle differently.
        //    The parent's raw mode setup (termios) is NOT inherited across execv.
        //    The child will have default terminal settings unless it changes them.
        struct sigaction sa_dfl;
        memset(&sa_dfl, 0, sizeof(sa_dfl));
        sa_dfl.sa_handler = SIG_DFL; // Default action
        sigaction(SIGINT, &sa_dfl, NULL);  // Child should terminate on SIGINT by default
        sigaction(SIGTERM, &sa_dfl, NULL); // Child should terminate on SIGTERM by default
        sigaction(SIGQUIT, &sa_dfl, NULL); // Child should terminate and dump core on SIGQUIT
        // SIGCHLD is irrelevant for the child itself to handle this way.
        // SIGUSR1, SIGUSR2 will be set up by the child_main.

        // 2. The child does not need to (and should not) try to restore parent's g_orig_termios.
        //    The terminal settings are per-process (more accurately, per controlling terminal session,
        //    but execv resets many process attributes).

        char *const child_argv[] = { g_child_exec_path, NULL };
        execv(g_child_exec_path, child_argv);

        // execv only returns on error
        int exec_errno = errno;
        // Use safe_write for this critical error message from child before exit
        char err_buf[256];
        int len = snprintf(err_buf, sizeof(err_buf), "CHILD_EXEC_FAIL: Failed to execute '%s' (errno %d: %s)\r\n",
                           g_child_exec_path, exec_errno, strerror(exec_errno));
        if (len > 0 && (size_t)len < sizeof(err_buf)) {
            safe_write(STDERR_FILENO, err_buf, (size_t)len);
        }
        _exit(EXIT_FAILURE); // Use _exit in child after fork to avoid flushing parent's stdio buffers

    } else { // Parent process
        if (add_child_pid(pid) != 0) {
            // add_child_pid aborts on failure, so this part might not be reached
            // if it does, it means add_child_pid had a non-aborting error (not current design)
            kill(pid, SIGKILL); // Kill the child we can't track
            waitpid(pid, NULL, 0); // Reap it
            return; // Or handle error more gracefully if add_child_pid didn't abort
        }
        // Report success to user
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

    size_t last_index = g_child_count - 1;
    pid_t pid_to_kill = g_child_pids[last_index];

    // Use stderr for operational messages
    if (fprintf(stderr, "PARENT [%d]: Sending SIGKILL to last child (PID %d).\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
    if (fflush(stderr) == EOF) { /* Handle error? */ }

    int kill_result = kill(pid_to_kill, SIGKILL);
    int removed_from_tracking = 0;

    if (kill_result == -1) {
        if (errno == ESRCH) { // Child already exited
            if (fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
            remove_child_pid_at_index(last_index);
            removed_from_tracking = 1;
        } else { // Other error sending signal
            if (fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno %d: %s).\r\n",
                pid_to_kill, errno, strerror(errno)) < 0) { /* Handle error? */ }
                // Do not remove if kill failed for other reasons, it might still be around.
        }
    } else { // kill succeeded
        if (fprintf(stderr, "PARENT [%d]: SIGKILL sent to PID %d. It will be reaped.\r\n", parent_pid, pid_to_kill) < 0) { /* Handle error? */ }
        remove_child_pid_at_index(last_index);
        removed_from_tracking = 1;
    }

    // Report outcome to user
    if (removed_from_tracking) {
        if (printf("PARENT [%d]: Processed kill for child %d. Remaining children tracked: %zu\r\n",
            parent_pid, pid_to_kill, g_child_count) < 0) { /* Handle error? */ }
    } else {
        if (printf("PARENT [%d]: Did not remove tracking for child %d due to kill error. Children tracked: %zu\r\n",
            parent_pid, pid_to_kill, g_child_count) < 0) { /* Handle error? */ }
    }
}

/*
 * list_children
 *
 * Prints the PID of the parent and the PIDs of all currently tracked children to stdout.
 * Uses \r\n for raw mode compatibility. Checks printf return values.
 * Uses a local buffer to build the string to minimize interleaved output if children also print.
 *
 * Accepts: None
 * Returns: None
 */
static void list_children(void) {
    pid_t parent_pid = getpid();
    char list_buf[4096]; // Reasonably sized buffer for the list
    int current_pos = 0;
    int remaining_buf = sizeof(list_buf);
    int ret;

    // snprintf returns number of chars written (excluding null) or <0 on error
    ret = snprintf(list_buf + current_pos, (size_t)remaining_buf, "PARENT [%d]: Listing processes:\r\n", parent_pid);
    if (ret < 0 || ret >= remaining_buf) goto buffer_error;
    current_pos += ret; remaining_buf -= ret;

    ret = snprintf(list_buf + current_pos, (size_t)remaining_buf, "  Parent: %d\r\n", parent_pid);
    if (ret < 0 || ret >= remaining_buf) goto buffer_error;
    current_pos += ret; remaining_buf -= ret;

    if (g_child_count == 0) {
        ret = snprintf(list_buf + current_pos, (size_t)remaining_buf, "  No tracked children.\r\n");
        if (ret < 0 || ret >= remaining_buf) goto buffer_error;
        current_pos += ret; remaining_buf -= ret;
    } else {
        ret = snprintf(list_buf + current_pos, (size_t)remaining_buf, "  Tracked Children (%zu):\r\n", g_child_count);
        if (ret < 0 || ret >= remaining_buf) goto buffer_error;
        current_pos += ret; remaining_buf -= ret;

        for (size_t i = 0; i < g_child_count; ++i) {
            ret = snprintf(list_buf + current_pos, (size_t)remaining_buf, "    - PID %d (tracked)\r\n", g_child_pids[i]);
            if (ret < 0 || ret >= remaining_buf) goto buffer_error;
            current_pos += ret; remaining_buf -= ret;
        }
    }

    // Write the fully formed string once
    if (safe_write(STDOUT_FILENO, list_buf, (size_t)current_pos) == -1) {
        // If safe_write fails, print error to stderr
        perror("PARENT: Error writing child list to stdout");
    }
    return;

    buffer_error:
    // Handle buffer overflow during snprintf (should be rare with 4K buffer for this data)
    fprintf(stderr, "PARENT [%d]: Error: Buffer overflow while formatting child list. Output may be truncated.\r\n", parent_pid);
    // Try to write what was formatted
    if (current_pos > 0 && safe_write(STDOUT_FILENO, list_buf, (size_t)current_pos) == -1) {
        perror("PARENT: Error writing truncated child list to stdout");
    }
}
