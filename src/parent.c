/*
 * parent.c
 *
 * Parent process for managing child processes based on keyboard input.
 * Spawns children ('+'), deletes the last one ('-'), lists all ('l'),
 * kills all ('k'), or quits ('q').
 * Children execute the 'child' program found via the CHILD_PATH env variable.
 */

// Feature test macros (_GNU_SOURCE, _POSIX_C_SOURCE) are defined via CFLAGS in Makefile

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h> // For terminal raw mode
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h> // For errno

// --- Constants ---
#define CHILD_PROG_NAME "child" // Name of the child executable
#define INITIAL_CHILD_CAPACITY 8 // Initial capacity for child PID array

// --- Globals ---
static pid_t *g_child_pids = NULL; // Array of child PIDs
static size_t g_child_count = 0;   // Number of active children
static size_t g_child_capacity = 0; // Allocated capacity of g_child_pids
static struct termios g_orig_termios; // To restore terminal settings
static char g_child_exec_path[1024]; // Full path to the child executable

// --- Type Definitions ---
typedef struct termios termios_t;

// --- Function Prototypes ---
static void enable_raw_mode(void);
static void disable_raw_mode(void);
static void cleanup_resources(void);
static void handle_signal(int sig);
static void register_signal_handlers(void);
static int add_child_pid(pid_t pid);
static void kill_all_children(const char *reason);
static void spawn_child(void);
static void kill_last_child(void);
static void list_children(void);
// print_usage prototype removed

/*
 * main
 *
 * Entry point of the parent process.
 * Sets up terminal, signal handlers, and the main command loop.
 *
 * Accepts:
 *   argc - Argument count (expected: 1)
 *   argv - Argument vector (unused, marked explicitly)
 *
 * Returns:
 *   EXIT_SUCCESS on normal exit.
 *   EXIT_FAILURE on critical errors (e.g., setup failure).
 */
int main(int argc, char *argv[] __attribute__((unused))) { // Mark argv as unused
    // Check for parameters - This program expects none directly.
    // The child path is passed via environment variable CHILD_PATH.
    if (argc > 1) {
        // Note: Makefile 'run' target doesn't pass args.
        // This check remains just in case it's run manually with args.
        fprintf(stderr, "Info: This program doesn't expect command-line arguments.\n");
        fprintf(stderr, "      It uses the CHILD_PATH environment variable to find the child executable.\n");
        // Continue execution
    }

    // --- Get Child Executable Path ---
    const char *child_path_dir = getenv("CHILD_PATH");
    if (child_path_dir == NULL) {
        fprintf(stderr, "Error: CHILD_PATH environment variable not set.\n");
        fprintf(stderr, "       Please set CHILD_PATH to the directory containing the '%s' executable.\n", CHILD_PROG_NAME);
        fprintf(stderr, "       Example: export CHILD_PATH=/path/to/build/debug\n");
        fprintf(stderr, "       (The Makefile 'run' target should set this automatically).\n");
        return EXIT_FAILURE;
    }
    // Construct full path, check buffer size
    int path_len = snprintf(g_child_exec_path, sizeof(g_child_exec_path), "%s/%s", child_path_dir, CHILD_PROG_NAME);
    if (path_len < 0 || (size_t)path_len >= sizeof(g_child_exec_path)) {
        fprintf(stderr, "Error: Child executable path is too long or formatting error.\n");
        return EXIT_FAILURE;
    }

    // Check if the child executable exists and is executable
    if (access(g_child_exec_path, X_OK) != 0) {
        fprintf(stderr, "Error: Child executable '%s' not found or not executable (errno: %d - %s).\n",
                g_child_exec_path, errno, strerror(errno));
        return EXIT_FAILURE;
    }

    // --- Initialization ---
    enable_raw_mode();
    atexit(cleanup_resources); // Ensure cleanup happens on exit
    register_signal_handlers();

    g_child_pids = malloc(INITIAL_CHILD_CAPACITY * sizeof(pid_t));
    if (g_child_pids == NULL) {
        // Raw mode is active, disable before printing error for clarity
        disable_raw_mode();
        perror("Error: Failed to allocate memory for child PIDs");
        // cleanup_resources called by atexit handler
        return EXIT_FAILURE;
    }
    g_child_capacity = INITIAL_CHILD_CAPACITY;
    g_child_count = 0;

    // Initial messages before entering raw mode loop
    printf("Parent process started (PID: %d). Commands: '+' spawn, '-' kill last, 'l' list, 'k' kill all, 'q' quit.\r\n", getpid());
    printf("Using child executable: %s\r\n", g_child_exec_path);
    fflush(stdout); // Ensure messages are visible before potential raw input blocking

    // --- Main Command Loop ---
    char c;
    while (1) {
        // Read one character in raw mode
        ssize_t read_result = read(STDIN_FILENO, &c, 1);

        if (read_result == 1) {
            // Optional: Echo character back for feedback - can be noisy
            // write(STDOUT_FILENO, &c, 1);

            // Process the command, ensuring output starts on a new line (\r\n)
            switch (c) {
                case '+':
                    write(STDOUT_FILENO, "\r\n", 2); // Move to new line before output
                    spawn_child();
                    break;
                case '-':
                    write(STDOUT_FILENO, "\r\n", 2);
                    kill_last_child();
                    break;
                case 'l':
                    write(STDOUT_FILENO, "\r\n", 2);
                    list_children();
                    break;
                case 'k':
                    write(STDOUT_FILENO, "\r\n", 2);
                    kill_all_children("Received 'k' command.");
                    // Confirmation message after kill_all_children prints its own lines
                    printf("PARENT [%d]: All children kill command processed.\r\n", getpid());
                    break;
                case 'q':
                    write(STDOUT_FILENO, "\r\n", 2);
                    // Use stderr for exit message to avoid interfering with potential output redirection
                    fprintf(stderr, "PARENT [%d]: Received 'q' command. Cleaning up and exiting.\r\n", getpid());
                    fflush(stderr); // Make sure message is seen before exit
                    exit(EXIT_SUCCESS); // atexit handler will run for cleanup
                default:
                    // Ignore other characters silently in raw mode
                    // Optionally beep or print a small error message
                    // write(STDOUT_FILENO, "\a", 1); // Bell sound
                    break;
            }
            fflush(stdout); // Ensure output is visible immediately
        } else if (read_result == 0) {
            // EOF on stdin - treat as quit
            write(STDOUT_FILENO, "\r\n", 2);
            fprintf(stderr, "PARENT [%d]: EOF detected on stdin. Cleaning up and exiting.\r\n", getpid());
            fflush(stderr);
            exit(EXIT_SUCCESS); // atexit handler will run
        } else if (errno == EINTR) {
            // Interrupted by signal (e.g., SIGCHLD), just continue loop
            continue;
        } else {
            // Read error
            // Need to disable raw mode before printing error
            disable_raw_mode(); // Disable raw mode to see perror output correctly
            perror("PARENT: Error reading from stdin");
            // cleanup_resources will be called by atexit
            exit(EXIT_FAILURE); // atexit handler will run
        }
    }

    // Should not be reached
    return EXIT_SUCCESS;
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
        fprintf(stderr, "Error: Standard input is not a terminal. Raw mode not applicable.\n");
        exit(EXIT_FAILURE);
    }
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
        perror("Error: tcgetattr failed");
        exit(EXIT_FAILURE);
    }

    termios_t raw = g_orig_termios;
    raw.c_iflag &= ~(unsigned long)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_cflag |= (unsigned long)(CS8);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1; // Read 1 byte at a time
    raw.c_cc[VTIME] = 0; // Blocking read

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("Error: tcsetattr failed");
        exit(EXIT_FAILURE);
    }
}

/*
 * disable_raw_mode
 *
 * Restores the terminal settings for stdin to their original state.
 * Ensures cursor is on a new line.
 *
 * Accepts: None
 * Returns: None
 */
static void disable_raw_mode(void) {
    // Check if STDIN is actually a tty before attempting restore
    if (!isatty(STDIN_FILENO)) {
        return; // Nothing to restore if not a tty
    }
    // Only restore if tcgetattr succeeded initially
    if (g_orig_termios.c_lflag != 0 || g_orig_termios.c_iflag != 0 ) { // Basic check
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios) == -1) {
            // Don't exit here, as we might be in a cleanup path already
            perror("Warning: Failed to restore terminal attributes");
        }
    }
    // Ensure the cursor is at the beginning of a new line after potentially messy raw output
    write(STDOUT_FILENO, "\r\n", 2);
    fflush(stdout);
}

/*
 * cleanup_resources
 *
 * Frees allocated memory, kills remaining children, and restores terminal mode.
 * Intended to be registered with atexit(). Uses stderr for messages.
 *
 * Accepts: None
 * Returns: None
 */
static void cleanup_resources(void) {
    // Restore terminal FIRST, before printing final messages
    disable_raw_mode();
    // Use stderr for cleanup messages to keep stdout clean
    fprintf(stderr, "PARENT [%d]: Cleaning up...\r\n", getpid());
    fflush(stderr); // Ensure message is visible

    kill_all_children("Parent exiting."); // This function now prints its own messages too

    if (g_child_pids != NULL) {
        free(g_child_pids);
        g_child_pids = NULL; // Avoid double free if called multiple times
        g_child_count = 0;
        g_child_capacity = 0;
    }
    fprintf(stderr, "PARENT [%d]: Cleanup complete.\r\n", getpid());
    fflush(stderr);
}

/*
 * handle_signal
 *
 * Signal handler for SIGINT, SIGTERM, SIGQUIT. Initiates cleanup.
 * Also handles SIGCHLD to reap zombie processes non-blockingly.
 *
 * Accepts:
 *   sig - The signal number received.
 *
 * Returns: None
 */
static void handle_signal(int sig) {
    if (sig == SIGCHLD) {
        int saved_errno = errno;
        pid_t pid;
        int status;
        // Reap all available zombies non-blockingly
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            // Optionally log reaped child (use stderr or a log file)
            // fprintf(stderr, "PARENT [%d]: Reaped child %d with status %d.\r\n", getpid(), pid, status);
        }
        // If waitpid returned -1 and errno is ECHILD, it means no children left (expected)
        // Otherwise, log unexpected errors if desired
        if (pid == -1 && errno != ECHILD) {
            // Cannot use perror directly in handler, use write or flags
            // fprintf(stderr, "PARENT [%d]: Error in waitpid (SIGCHLD handler).\r\n", getpid());
        }
        errno = saved_errno; // Restore errno
    } else {
        // Received a termination signal (SIGINT, SIGTERM, SIGQUIT)
        // Use write for async-signal safety for the message
        const char msg[] = "\r\nPARENT: Received termination signal, exiting via cleanup...\r\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1); // Write to stderr
        // Trigger atexit handler for proper cleanup by exiting
        exit(EXIT_FAILURE); // Use failure code as it's abnormal termination
    }
}


/*
 * register_signal_handlers
 *
 * Sets up signal handlers for SIGINT, SIGTERM, SIGQUIT, and SIGCHLD.
 *
 * Accepts: None
 * Returns: None
 */
static void register_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // Restart interrupted system calls if possible (like read)

    // Register for termination signals
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGQUIT, &sa, NULL) == -1) {
        // Must disable raw mode before calling perror
        disable_raw_mode();
    perror("Error: Failed to register termination signal handlers");
    exit(EXIT_FAILURE);
        }

        // Register for SIGCHLD to reap zombies
        // Reset flags slightly for SIGCHLD: don't necessarily restart syscalls,
        // and only care about termination (NOCLDSTOP).
        sa.sa_flags = SA_NOCLDSTOP; // Only trigger on termination/exit
        if (sigaction(SIGCHLD, &sa, NULL) == -1) {
            disable_raw_mode();
            perror("Error: Failed to register SIGCHLD handler");
            exit(EXIT_FAILURE);
        }

        // Explicitly ignore SIGUSR1 and SIGUSR2 as they are not used by the parent logic
        memset(&sa, 0, sizeof(sa)); // Clear struct again
        sa.sa_handler = SIG_IGN;
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGUSR2, &sa, NULL);
}


/*
 * add_child_pid
 *
 * Adds a child PID to the dynamic array, resizing if necessary.
 *
 * Accepts:
 *   pid - The PID of the child process to add.
 *
 * Returns:
 *   0 on success, -1 on failure (memory allocation).
 */
static int add_child_pid(pid_t pid) {
    if (g_child_count >= g_child_capacity) {
        size_t new_capacity = (g_child_capacity == 0) ? INITIAL_CHILD_CAPACITY : g_child_capacity * 2;
        // Check for potential overflow with large capacities (unlikely here)
        if (new_capacity < g_child_capacity) {
            fprintf(stderr, "Error: Child PID array capacity overflow.\r\n");
            return -1;
        }
        pid_t *new_pids = realloc(g_child_pids, new_capacity * sizeof(pid_t));
        if (new_pids == NULL) {
            // Print error to stderr
            fprintf(stderr, "Error: Failed to reallocate memory for child PIDs (PID: %d).\r\n", pid);
            return -1;
        }
        g_child_pids = new_pids;
        g_child_capacity = new_capacity;
    }
    g_child_pids[g_child_count++] = pid;
    return 0;
}

/*
 * kill_all_children
 *
 * Sends SIGKILL to all currently tracked child processes.
 * Clears the internal list of children. Prints actions to stdout/stderr.
 *
 * Accepts:
 *   reason - A string indicating why children are being killed (for logging).
 *
 * Returns: None
 */
static void kill_all_children(const char *reason) {
    if (g_child_count == 0) {
        // No message needed if called during cleanup with no children
        if (strcmp(reason, "Parent exiting.") != 0) {
            printf("PARENT [%d]: No children to kill.\r\n", getpid());
        }
        return;
    }

    // Use stderr for the action message itself
    fprintf(stderr, "PARENT [%d]: Killing all %zu children (%s).\r\n", getpid(), g_child_count, reason);
    fflush(stderr); // Ensure message visibility

    for (size_t i = g_child_count; i > 0; --i) {
        pid_t pid_to_kill = g_child_pids[i - 1];
        // Use stderr for individual kill attempts/warnings
        fprintf(stderr, "PARENT [%d]: Sending SIGKILL to child PID %d...\r\n", getpid(), pid_to_kill);
        fflush(stderr);
        if (kill(pid_to_kill, SIGKILL) == -1) {
            if (errno == ESRCH) { // Process already dead
                fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", getpid(), pid_to_kill);
            } else { // Other error (permissions?)
                fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno: %d - %s).\r\n",
                        pid_to_kill, errno, strerror(errno));
            }
        }
        // SIGCHLD handler will eventually reap the process after SIGKILL
    }

    // Clear the list
    g_child_count = 0;
    // Optionally shrink the array back if memory is critical, but usually not needed
    // if (g_child_capacity > INITIAL_CHILD_CAPACITY * 2) { // Example shrink condition
    //     size_t new_capacity = g_child_capacity / 2;
    //     pid_t *new_pids = realloc(g_child_pids, new_capacity * sizeof(pid_t));
    //     if (new_pids != NULL) { // Only shrink if realloc succeeds
    //          g_child_pids = new_pids;
    //          g_child_capacity = new_capacity;
    //      }
    // }
}

/*
 * spawn_child
 *
 * Forks and execs a new child process using the path in g_child_exec_path.
 * Adds the new child's PID to the list. Reports success (stdout) or failure (stderr).
 *
 * Accepts: None
 * Returns: None
 */
static void spawn_child(void) {
    pid_t pid = fork();

    if (pid == -1) {
        // Fork failed in parent, report to stderr
        fprintf(stderr, "Error: Failed to fork child process (errno: %d - %s)\r\n", errno, strerror(errno));
        return; // Continue parent operation
    } else if (pid == 0) {
        // --- Child Process ---
        // Attempt to restore terminal settings for the child if it was inherited
        if (isatty(STDIN_FILENO)) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios); // Ignore error here
        }

        // Reset signal handlers to defaults for the child
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGCHLD, &sa, NULL);
        // Let child set its own SIGALRM handler etc.

        // Prepare arguments for execv
        char *child_argv[] = {g_child_exec_path, NULL}; // argv[0] is program name
        execv(g_child_exec_path, child_argv);

        // If execv returns, an error occurred - print to stderr
        fprintf(stderr, "CHILD: Error: Failed to execute '%s' (errno: %d - %s)\n",
                g_child_exec_path, errno, strerror(errno));
        exit(EXIT_FAILURE); // Child exits if exec fails

    } else {
        // --- Parent Process ---
        if (add_child_pid(pid) == 0) {
            // Report success to stdout
            printf("PARENT [%d]: Spawned child process with PID %d. Total children: %zu\r\n",
                   getpid(), pid, g_child_count);
        } else {
            // Failed to add PID (memory allocation reported by add_child_pid to stderr)
            // Kill the spawned child as we can't track it
            fprintf(stderr, "PARENT [%d]: Killing untracked child PID %d due to tracking failure.\r\n", getpid(), pid);
            kill(pid, SIGKILL); // Force kill
            waitpid(pid, NULL, 0); // Wait for it immediately to prevent zombie
        }
    }
}

/*
 * kill_last_child
 *
 * Sends SIGKILL to the most recently spawned child process.
 * Removes the child from the list. Reports the action (stdout/stderr).
 *
 * Accepts: None
 * Returns: None
 */
static void kill_last_child(void) {
    if (g_child_count == 0) {
        printf("PARENT [%d]: No children to kill.\r\n", getpid());
        return;
    }

    // Target the last child added
    g_child_count--; // Decrement count first
    pid_t pid_to_kill = g_child_pids[g_child_count]; // Get PID at new count index

    // Use stderr for action message
    fprintf(stderr, "PARENT [%d]: Sending SIGKILL to last child (PID %d).\r\n", getpid(), pid_to_kill);
    fflush(stderr);

    if (kill(pid_to_kill, SIGKILL) == -1) {
        if (errno == ESRCH) { // Process already dead
            fprintf(stderr, "PARENT [%d]: Child PID %d already exited.\r\n", getpid(), pid_to_kill);
        } else { // Other error
            fprintf(stderr, "Warning: Failed to send SIGKILL to PID %d (errno: %d - %s).\r\n",
                    pid_to_kill, errno, strerror(errno));
        }
        // Proceed with removal from list even if kill failed (it might be dead or error)
    }
    // SIGCHLD handler will eventually reap the process

    // Report result to stdout
    printf("PARENT [%d]: Removed tracking for child %d. Remaining children: %zu\r\n",
           getpid(), pid_to_kill, g_child_count);
}

/*
 * list_children
 *
 * Prints the PID of the parent and the PIDs of all currently tracked children to stdout.
 * Uses \r\n for raw mode compatibility.
 *
 * Accepts: None
 * Returns: None
 */
static void list_children(void) {
    printf("PARENT [%d]: Listing processes:\r\n", getpid());
    printf("  Parent: %d\r\n", getpid());
    if (g_child_count == 0) {
        printf("  No tracked children.\r\n");
    } else {
        printf("  Tracked Children (%zu):\r\n", g_child_count);
        for (size_t i = 0; i < g_child_count; ++i) {
            printf("    - PID %d\r\n", g_child_pids[i]);
        }
    }
}
