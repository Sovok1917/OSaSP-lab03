/*
 * child.c
 *
 * Child process logic. Sets a structure to {0,0} and {1,1} repeatedly.
 * A timer signal (SIGALRM) interrupts this non-atomic update.
 * The signal handler records the state of the structure at the time of
 * the interrupt ({0,0}, {0,1}, {1,0}, {1,1}). After a set number
 * of repetitions, it prints statistics to stdout (if enabled via SIGUSR1)
 * and exits. Output can be suppressed via SIGUSR2.
 */
#define _POSIX_C_SOURCE 200809L // Use POSIX.1-2008 for setitimer, sigaction
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>   // For setitimer
#include <errno.h>      // For errno

// --- Constants ---
// Increase runtime significantly for easier testing (~5 seconds with 500us interval)
#define NUM_REPETITIONS 10001
// Alarm interval in microseconds (adjust for meaningful stats)
#define ALARM_INTERVAL_US 500 // 0.5 milliseconds

// --- Type Definitions ---
// Structure being updated non-atomically
typedef struct pair_s {
    int v1;
    int v2;
} pair_t;

// --- Globals ---
// The shared data structure - volatile is crucial!
static volatile pair_t g_shared_pair;

// Statistics counters - volatile because updated in signal handler
static volatile long long g_count00;
static volatile long long g_count01;
static volatile long long g_count10;
static volatile long long g_count11;

// Flags set by signal handlers - sig_atomic_t is required for signal safety
static volatile sig_atomic_t g_alarm_flag;
static volatile sig_atomic_t g_repetitions_done;
static volatile sig_atomic_t g_output_enabled; // Controlled by SIGUSR1/SIGUSR2

// --- Function Prototypes ---
static void handle_alarm(int sig);
static void handle_usr_signals(int sig);
static int register_signal_handlers(void);
static int setup_timer(void);
static void initialize_globals(void);

/*
 * main
 *
 * Entry point for the child process.
 * Sets up signal handling, runs the main loop performing non-atomic updates,
 * prints statistics to stdout after N repetitions (if enabled), and exits.
 *
 * Accepts:
 *   argc - Argument count (expected: 1)
 *   argv - Argument vector (unused)
 *
 * Returns:
 *   EXIT_SUCCESS on normal completion.
 *   EXIT_FAILURE on setup errors.
 */
int main(int argc, char *argv[]) {
    (void)argv; // Explicitly mark argv as unused

    pid_t my_pid = getpid(); // Get PID early for messages

    // Child doesn't expect specific arguments from the parent's execv call
    if (argc > 1) {
        // Use \r\n for stderr messages for better raw mode display
        if (fprintf(stderr, "CHILD [%d]: Warning: Received unexpected arguments.\r\n", my_pid) < 0) { /* Handle error? */ }
    }

    // --- Initialization ---
    initialize_globals(); // Explicitly initialize static globals at runtime

    pid_t parent_pid = getppid();

    // Use \r\n for stderr messages
    if (fprintf(stderr, "CHILD [%d]: Started. PPID=%d. Output initially %s. Will run %d reps.\r\n",
        my_pid, parent_pid, g_output_enabled ? "ENABLED" : "DISABLED", NUM_REPETITIONS) < 0) { /* Handle error? */ }
        if (fflush(stderr) == EOF) {
            // Use \r\n for stderr messages
            fprintf(stderr, "CHILD [%d]: Error flushing stderr on start: %s\r\n", my_pid, strerror(errno));
            // Continue if possible
        }

        if (register_signal_handlers() != 0) {
            return EXIT_FAILURE;
        }

        // --- Main Loop ---
        int current_state = 0; // 0 for {0,0}, 1 for {1,1}

        // Start the first alarm timer
        if (setup_timer() != 0) {
            return EXIT_FAILURE;
        }

        while (g_repetitions_done < NUM_REPETITIONS) {
            g_alarm_flag = 0; // Reset alarm flag for this iteration.

            // Inner loop: Rapidly alternate states.
            while (!g_alarm_flag) {
                if (current_state == 0) {
                    g_shared_pair.v1 = 0;
                    g_shared_pair.v2 = 0; // Potential interrupt point
                    current_state = 1;
                } else {
                    g_shared_pair.v1 = 1;
                    g_shared_pair.v2 = 1; // Potential interrupt point
                    current_state = 0;
                }
            }

            // Alarm has fired. Re-arm the timer if needed.
            if (g_repetitions_done < NUM_REPETITIONS) {
                if (setup_timer() != 0) {
                    // Use \r\n for stderr messages
                    if (fprintf(stderr, "CHILD [%d]: Error re-arming timer. Exiting loop.\r\n", my_pid) < 0) { /* Handle error? */ }
                    break; // Exit the loop on timer error
                }
            }
        } // End of main loop (repetitions)


        // --- Print Statistics (Conditionally) ---
        if (g_output_enabled) {
            // Final stats output to stdout uses \n as required by spec (single line)
            if (printf("PPID=%d, PID=%d, STATS={00:%lld, 01:%lld, 10:%lld, 11:%lld}\n",
                parent_pid, my_pid,
                g_count00, g_count01, g_count10, g_count11) < 0) {
                // Use \r\n for stderr messages
                fprintf(stderr, "CHILD [%d]: Error writing final stats to stdout: %s\r\n", my_pid, strerror(errno));
                }
                if (fflush(stdout) == EOF) {
                    // Use \r\n for stderr messages
                    fprintf(stderr, "CHILD [%d]: Error flushing stdout for stats: %s\r\n", my_pid, strerror(errno));
                }
        } else {
            // Output suppressed, notify via stderr using \r\n
            if (fprintf(stderr, "CHILD [%d]: Final statistics output suppressed by signal.\r\n", my_pid) < 0) { /* Handle error? */ }
            if (fflush(stderr) == EOF) { /* Handle error? */ } // Error already printed if needed
        }

        // Use \r\n for final stderr message
        if (fprintf(stderr, "CHILD [%d]: Exiting normally.\r\n", my_pid) < 0) { /* Handle error? */ }
        if (fflush(stderr) == EOF) { /* Handle error? */ }

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
    g_shared_pair.v1 = 0;
    g_shared_pair.v2 = 0;
    g_count00 = 0;
    g_count01 = 0;
    g_count10 = 0;
    g_count11 = 0;
    g_alarm_flag = 0;
    g_repetitions_done = 0;
    g_output_enabled = 1; // Default: Output is ENABLED
}


/*
 * handle_alarm
 *
 * Signal handler for SIGALRM. Reads the state of the volatile g_shared_pair,
 * increments the corresponding counter, updates repetition count, and sets the g_alarm_flag.
 * This function must be async-signal-safe.
 *
 * Accepts:
 *   sig - The signal number (expected: SIGALRM)
 *
 * Returns: None
 */
static void handle_alarm(int sig) {
    // Async-signal-safe operations only!
    if (sig == SIGALRM) {
        int local_v1 = g_shared_pair.v1;
        int local_v2 = g_shared_pair.v2;

        if (local_v1 == 0 && local_v2 == 0) g_count00++;
        else if (local_v1 == 0 && local_v2 == 1) g_count01++;
        else if (local_v1 == 1 && local_v2 == 0) g_count10++;
        else g_count11++; // Assume {1,1}

        if (g_repetitions_done < NUM_REPETITIONS) {
            g_repetitions_done++;
        }
        g_alarm_flag = 1; // Signal the main loop
    }
}

/*
 * handle_usr_signals
 *
 * Signal handler for SIGUSR1 and SIGUSR2. Toggles the g_output_enabled flag.
 * This function must be async-signal-safe.
 *
 * Accepts:
 *   sig - The signal number (expected: SIGUSR1 or SIGUSR2)
 *
 * Returns: None
 */
static void handle_usr_signals(int sig) {
    // Async-signal-safe: Assigning to volatile sig_atomic_t
    if (sig == SIGUSR1) {
        g_output_enabled = 1; // Enable output
    } else if (sig == SIGUSR2) {
        g_output_enabled = 0; // Disable output
    }
}


/*
 * register_signal_handlers
 *
 * Configures the signal handlers for SIGALRM, SIGUSR1, and SIGUSR2 using sigaction.
 *
 * Accepts: None
 * Returns:
 *   0 on success, -1 on failure (prints error message).
 */
static int register_signal_handlers(void) {
    struct sigaction sa_alarm, sa_usr;
    pid_t my_pid = getpid(); // For error messages

    // --- Configure SIGALRM handler ---
    memset(&sa_alarm, 0, sizeof(sa_alarm));
    sa_alarm.sa_handler = handle_alarm;
    if (sigemptyset(&sa_alarm.sa_mask) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error initializing alarm signal mask: %s\r\n", my_pid, strerror(errno));
        return -1;
    }
    if (sigaddset(&sa_alarm.sa_mask, SIGALRM) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error adding SIGALRM to alarm signal mask: %s\r\n", my_pid, strerror(errno));
        return -1;
    }
    sa_alarm.sa_flags = 0; // No SA_RESTART for SIGALRM

    if (sigaction(SIGALRM, &sa_alarm, NULL) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error setting SIGALRM handler: %s\r\n", my_pid, strerror(errno));
        return -1;
    }

    // --- Configure SIGUSR1/SIGUSR2 handlers ---
    memset(&sa_usr, 0, sizeof(sa_usr));
    sa_usr.sa_handler = handle_usr_signals;
    if (sigemptyset(&sa_usr.sa_mask) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error initializing usr signal mask: %s\r\n", my_pid, strerror(errno));
        return -1;
    }
    if (sigaddset(&sa_usr.sa_mask, SIGUSR1) == -1 || sigaddset(&sa_usr.sa_mask, SIGUSR2) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error adding SIGUSR1/2 to usr signal mask: %s\r\n", my_pid, strerror(errno));
        return -1;
    }
    sa_usr.sa_flags = SA_RESTART; // SA_RESTART is okay here

    if (sigaction(SIGUSR1, &sa_usr, NULL) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error setting SIGUSR1 handler: %s\r\n", my_pid, strerror(errno));
        return -1;
    }
    if (sigaction(SIGUSR2, &sa_usr, NULL) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error setting SIGUSR2 handler: %s\r\n", my_pid, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * setup_timer
 *
 * Configures a one-shot timer using setitimer to send SIGALRM after
 * ALARM_INTERVAL_US microseconds.
 *
 * Accepts: None
 * Returns:
 *   0 on success, -1 on failure (prints error message).
 */
static int setup_timer(void) {
    struct itimerval timer;

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = ALARM_INTERVAL_US;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0; // One-shot

    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
        // Use \r\n for stderr messages
        fprintf(stderr, "CHILD [%d]: Error setting timer with setitimer: %s\r\n", getpid(), strerror(errno));
        return -1;
    }
    return 0;
}
