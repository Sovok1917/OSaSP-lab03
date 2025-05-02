/*
 * child.c
 *
 * Child process logic. Sets a structure to {0,0} and {1,1} repeatedly.
 * An alarm signal (SIGALRM) interrupts this non-atomic update.
 * The signal handler records the state of the structure at the time of
 * the interrupt ({0,0}, {0,1}, {1,0}, {1,1}). After a set number
 * of repetitions, it prints statistics to stdout and exits.
 */
#define _POSIX_C_SOURCE 199309L // Required for sigaction, ualarm
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>       // Not strictly needed now, kept for potential nanosleep use
#include <sys/time.h>   // For ualarm
#include <errno.h>      // For errno

// --- Constants ---
// Number of alarm cycles before printing stats
#define NUM_REPETITIONS 101
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
static volatile long long g_count00 = 0;
static volatile long long g_count01 = 0;
static volatile long long g_count10 = 0;
static volatile long long g_count11 = 0;

// Flag set by signal handler - sig_atomic_t is required for signal safety
static volatile sig_atomic_t g_alarm_flag = 0;
static volatile sig_atomic_t g_repetitions_done = 0; // Track repetitions from handler

// --- Function Prototypes ---
static void handle_alarm(int sig);
static int setup_signal_handler(void);
static int setup_timer(void);


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
        // Read volatile data into local variables for analysis
        int local_v1 = g_shared_pair.v1;
        int local_v2 = g_shared_pair.v2;

        // Update statistics (incrementing volatile long long is generally safe)
        if (local_v1 == 0 && local_v2 == 0) {
            g_count00++;
        } else if (local_v1 == 0 && local_v2 == 1) {
            g_count01++;
        } else if (local_v1 == 1 && local_v2 == 0) {
            g_count10++;
        } else { // Assume {1,1}
            g_count11++;
        }

        // Increment repetition counter and set flag for main loop
        // Incrementing volatile sig_atomic_t is safe
        g_repetitions_done++;
        g_alarm_flag = 1; // Signal the main loop
    }
    // No need to re-arm timer here; main loop calls ualarm again
}

/*
 * setup_signal_handler
 *
 * Configures the signal handler for SIGALRM using sigaction.
 *
 * Accepts: None
 * Returns:
 *   0 on success, -1 on failure.
 */
static int setup_signal_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_alarm;
    sigemptyset(&sa.sa_mask); // Don't block other signals during handling
    // CRITICAL: Do NOT use SA_RESTART. We need the signal to interrupt the assignments.
    sa.sa_flags = 0;

    if (sigaction(SIGALRM, &sa, NULL) == -1) {
        perror("CHILD: Error setting SIGALRM handler");
        return -1;
    }
    return 0;
}

/*
 * setup_timer
 *
 * Configures a one-shot timer using ualarm to send SIGALRM.
 *
 * Accepts: None
 * Returns:
 *   0 on success, -1 on failure.
 */
static int setup_timer(void) {
    // ualarm schedules a one-shot alarm after ALARM_INTERVAL_US microseconds.
    // Returns 0 on success, -1 on error.
    if (ualarm(ALARM_INTERVAL_US, 0) == (useconds_t)-1) {
        // Don't use perror if called from signal handler context, but okay here in setup
        perror("CHILD: Error setting ualarm");
        return -1;
    }
    return 0;
}

/*
 * main
 *
 * Entry point for the child process.
 * Sets up signal handling, runs the main loop performing non-atomic updates,
 * prints statistics to stdout after N repetitions, and exits.
 *
 * Accepts:
 *   argc - Argument count (expected: 1)
 *   argv - Argument vector (unused, marked explicitly)
 *
 * Returns:
 *   EXIT_SUCCESS on normal completion.
 *   EXIT_FAILURE on setup errors.
 */
int main(int argc, char *argv[] __attribute__((unused))) { // Mark argv as unused
    // Child doesn't expect specific arguments from the parent's execv call
    if (argc > 1) {
        // Output warning to stderr to avoid mixing with stats output on stdout
        fprintf(stderr, "CHILD [%d]: Warning: Received unexpected arguments.\n", getpid());
    }

    // --- Initialization ---
    pid_t my_pid = getpid();
    pid_t parent_pid = getppid();

    // Print startup message to stderr
    fprintf(stderr, "CHILD [%d]: Started. PPID=%d. Will run for %d repetitions with ~%dus interval.\n",
            my_pid, parent_pid, NUM_REPETITIONS, ALARM_INTERVAL_US);
    fflush(stderr); // Ensure startup message is visible

    if (setup_signal_handler() != 0) {
        return EXIT_FAILURE;
    }

    // Initialize the pair state
    g_shared_pair.v1 = 0;
    g_shared_pair.v2 = 0;

    // --- Main Loop ---
    int current_state = 0; // 0 for {0,0}, 1 for {1,1}

    // Start the first alarm timer
    if (setup_timer() != 0) {
        return EXIT_FAILURE;
    }

    while (g_repetitions_done < NUM_REPETITIONS) {
        // Reset alarm flag for this iteration.
        // It's safe to reset here as the signal handler only sets it to 1.
        g_alarm_flag = 0;

        // Inner loop: Rapidly alternate states. The signal handler will interrupt this.
        // This loop needs to be fast enough relative to the alarm interval
        // for interesting statistics to emerge.
        while (!g_alarm_flag) {
            // Non-atomic update: The alarm can fire between these two assignments!
            if (current_state == 0) {
                g_shared_pair.v1 = 0;
                // Potential interrupt point
                g_shared_pair.v2 = 0;
                current_state = 1;
            } else {
                g_shared_pair.v1 = 1;
                // Potential interrupt point
                g_shared_pair.v2 = 1;
                current_state = 0;
            }
            // No artificial delay needed; CPU speed provides the race condition.
        }

        // Alarm has fired (g_alarm_flag is 1), handler has collected stats.
        // The handler also incremented g_repetitions_done.

        // Re-arm the timer for the next repetition *if* we haven't finished.
        if (g_repetitions_done < NUM_REPETITIONS) {
            if (setup_timer() != 0) {
                // If timer fails mid-run, log to stderr and break
                fprintf(stderr, "CHILD [%d]: Error re-arming ualarm. Exiting loop.\n", my_pid);
                break; // Exit the loop on timer error
            }
        }
    } // End of main loop (repetitions)


    // --- Print Statistics ---
    // Final output MUST be on a single line to stdout, as specified.
    printf("CHILD [%d]: PPID=%d, PID=%d, STATS={00:%lld, 01:%lld, 10:%lld, 11:%lld}\n",
           my_pid, parent_pid, my_pid,
           g_count00, g_count01, g_count10, g_count11);
    fflush(stdout); // Ensure final stats are printed to stdout

    return EXIT_SUCCESS;
}
