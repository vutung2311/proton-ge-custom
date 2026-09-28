#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define TOTAL_STRESS_ITEMS 200000
#define NUM_PRODUCERS 8
#define NUM_CONSUMERS 8
#define ITEMS_PER_PRODUCER (TOTAL_STRESS_ITEMS / NUM_PRODUCERS)

static FILE *log_file = NULL;

/* The log file only: under `proton run` the console stdout is not drained, and a flush to it
 * blocks forever (run_iocp_suite.sh prints the log file afterwards). */
#define LOG_PRINT(fmt, ...) do { \
    if (log_file) { fprintf(log_file, fmt, ##__VA_ARGS__); fflush(log_file); } \
} while(0)

static volatile LONG total_dequeued = 0;
static volatile LONG producer_done = 0;

DWORD WINAPI producer_thread(LPVOID param) {
    HANDLE port = (HANDLE)param;
    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        if (!PostQueuedCompletionStatus(port, 42, (ULONG_PTR)(i + 1), (LPOVERLAPPED)(uintptr_t)0x12345678)) {
            LOG_PRINT("[FAIL] PostQueuedCompletionStatus failed in producer!\n");
            return 1;
        }
    }
    InterlockedIncrement(&producer_done);
    return 0;
}

DWORD WINAPI consumer_thread(LPVOID param) {
    HANDLE port = (HANDLE)param;
    OVERLAPPED_ENTRY entries[32];
    ULONG removed = 0;

    while (InterlockedCompareExchange(&total_dequeued, 0, 0) < TOTAL_STRESS_ITEMS) {
        if (GetQueuedCompletionStatusEx(port, entries, 32, &removed, 100, FALSE)) {
            for (ULONG i = 0; i < removed; i++) {
                if (entries[i].dwNumberOfBytesTransferred != 42 ||
                    entries[i].lpOverlapped != (LPOVERLAPPED)(uintptr_t)0x12345678) {
                    LOG_PRINT("[FAIL] Data corruption detected in batch dequeue: bytes=%lu, ov=%p, key=%p\n",
                              entries[i].dwNumberOfBytesTransferred, entries[i].lpOverlapped, (void*)entries[i].lpCompletionKey);
                }
            }
            InterlockedAdd(&total_dequeued, removed);
        } else {
            DWORD err = GetLastError();
            if (err != WAIT_TIMEOUT && err != ERROR_ABANDONED_WAIT_0) {
                if (InterlockedCompareExchange(&total_dequeued, 0, 0) >= TOTAL_STRESS_ITEMS) break;
            }
        }
    }
    return 0;
}

struct close_worker_arg {
    HANDLE port;
    volatile LONG exited;
    volatile DWORD last_err;
};

DWORD WINAPI close_worker_thread(LPVOID param) {
    struct close_worker_arg *arg = (struct close_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;
    BOOL res = GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, INFINITE);
    arg->last_err = GetLastError();
    InterlockedIncrement(&arg->exited);
    return res ? 0 : 1;
}

struct cascade_worker_arg {
    HANDLE port;
    volatile LONG received;
    ULONG_PTR received_key;
};

DWORD WINAPI cascade_worker_thread(LPVOID param) {
    struct cascade_worker_arg *arg = (struct cascade_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;
    if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 3000)) {
        arg->received_key = key;
        InterlockedIncrement(&arg->received);
        return 0;
    }
    return 1;
}

struct mixed_worker_arg {
    HANDLE port;
    volatile LONG items_consumed;
    volatile LONG exit_flag;
};

DWORD WINAPI mixed_single_consumer(LPVOID param) {
    struct mixed_worker_arg *arg = (struct mixed_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;
    while (!InterlockedCompareExchange(&arg->exit_flag, 0, 0)) {
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 50)) {
            if (key == 0xDEADBEEF) break;
            InterlockedIncrement(&arg->items_consumed);
        }
    }
    return 0;
}

DWORD WINAPI mixed_batch_consumer_3(LPVOID param) {
    struct mixed_worker_arg *arg = (struct mixed_worker_arg *)param;
    OVERLAPPED_ENTRY entries[3];
    ULONG removed = 0;
    while (!InterlockedCompareExchange(&arg->exit_flag, 0, 0)) {
        if (GetQueuedCompletionStatusEx(arg->port, entries, 3, &removed, 50, FALSE)) {
            for (ULONG i = 0; i < removed; i++) {
                if (entries[i].lpCompletionKey == 0xDEADBEEF) {
                    for (ULONG k = i + 1; k < removed; k++) {
                        if (entries[k].lpCompletionKey == 0xDEADBEEF) {
                            PostQueuedCompletionStatus(arg->port, entries[k].dwNumberOfBytesTransferred,
                                                       entries[k].lpCompletionKey, entries[k].lpOverlapped);
                        }
                    }
                    return 0;
                }
                InterlockedIncrement(&arg->items_consumed);
            }
        }
    }
    return 0;
}

DWORD WINAPI mixed_batch_consumer(LPVOID param) {
    struct mixed_worker_arg *arg = (struct mixed_worker_arg *)param;
    OVERLAPPED_ENTRY entries[16];
    ULONG removed = 0;
    while (!InterlockedCompareExchange(&arg->exit_flag, 0, 0)) {
        if (GetQueuedCompletionStatusEx(arg->port, entries, 16, &removed, 50, FALSE)) {
            for (ULONG i = 0; i < removed; i++) {
                if (entries[i].lpCompletionKey == 0xDEADBEEF) {
                    for (ULONG k = i + 1; k < removed; k++) {
                        if (entries[k].lpCompletionKey == 0xDEADBEEF) {
                            PostQueuedCompletionStatus(arg->port, entries[k].dwNumberOfBytesTransferred,
                                                       entries[k].lpCompletionKey, entries[k].lpOverlapped);
                        }
                    }
                    return 0;
                }
                InterlockedIncrement(&arg->items_consumed);
            }
        }
    }
    return 0;
}

struct yield_worker_arg {
    int iterations;
    volatile LONG completed;
};

static DWORD WINAPI yield_worker(LPVOID param) {
    struct yield_worker_arg *arg = (struct yield_worker_arg *)param;
    for (int i = 0; i < arg->iterations; i++) {
        if (i % 2 == 0) SwitchToThread();
        else Sleep(0);
    }
    InterlockedIncrement(&arg->completed);
    return 0;
}

struct blocked_file_io_worker_arg {
    HANDLE port;
    volatile LONG ready;
    volatile LONG completed;
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED ov;
    DWORD err;
};

static DWORD WINAPI blocked_file_io_worker(LPVOID param) {
    struct blocked_file_io_worker_arg *arg = (struct blocked_file_io_worker_arg *)param;
    InterlockedIncrement(&arg->ready);
    BOOL res = GetQueuedCompletionStatus(arg->port, &arg->bytes, &arg->key, &arg->ov, 5000);
    if (!res) arg->err = GetLastError();
    InterlockedIncrement(&arg->completed);
    return res ? 0 : 1;
}

static DWORD WINAPI blocked_infinite_worker(LPVOID param) {
    struct blocked_file_io_worker_arg *arg = (struct blocked_file_io_worker_arg *)param;
    InterlockedIncrement(&arg->ready);
    BOOL res = GetQueuedCompletionStatus(arg->port, &arg->bytes, &arg->key, &arg->ov, INFINITE);
    if (!res) arg->err = GetLastError();
    InterlockedIncrement(&arg->completed);
    return res ? 0 : 1;
}

struct alertable_worker_arg {
    HANDLE port;
    volatile LONG ready;
    volatile LONG completed;
    OVERLAPPED_ENTRY entry;
    ULONG removed;
    BOOL res;
    DWORD err;
};

static DWORD WINAPI alertable_worker_thread(LPVOID param) {
    struct alertable_worker_arg *arg = (struct alertable_worker_arg *)param;
    InterlockedIncrement(&arg->ready);
    arg->res = GetQueuedCompletionStatusEx(arg->port, &arg->entry, 1, &arg->removed, 2000, TRUE);
    if (!arg->res) arg->err = GetLastError();
    InterlockedIncrement(&arg->completed);
    return arg->res ? 0 : 1;
}

#define NUM_SCALING_WORKERS 8
#define NUM_COORDINATOR_TASKS 64

struct scaling_worker_arg {
    HANDLE port;
    int worker_id;
    volatile LONG *task_counts;
    volatile LONG *ready_count;
    volatile LONG *completed_count;
};

static DWORD WINAPI scaling_worker_thread(LPVOID param) {
    struct scaling_worker_arg *arg = (struct scaling_worker_arg *)param;
    InterlockedIncrement(arg->ready_count);

    OVERLAPPED_ENTRY entries[16];
    ULONG removed = 0;

    while (InterlockedCompareExchange(arg->completed_count, 0, 0) < NUM_COORDINATOR_TASKS) {
        if (GetQueuedCompletionStatusEx(arg->port, entries, 16, &removed, 1000, FALSE)) {
            for (ULONG i = 0; i < removed; i++) {
                InterlockedIncrement(&arg->task_counts[arg->worker_id]);
                // Simulate compute workload (20,000 pause iterations ~ 50 microseconds)
                for (volatile int p = 0; p < 20000; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                    YieldProcessor();
#endif
                }
            }
            InterlockedAdd(arg->completed_count, removed);
        } else {
            if (InterlockedCompareExchange(arg->completed_count, 0, 0) >= NUM_COORDINATOR_TASKS) break;
        }
    }
    return 0;
}

#define NUM_DUAL_WORKERS 4
#define DUAL_INPROC_TASKS 16

struct dual_worker_arg {
    HANDLE port;
    int worker_id;
    volatile LONG *ready_count;
    volatile LONG *file_completed;
    volatile LONG *inproc_completed;
    volatile LONG *stop;
    volatile LONG *worker_task_count;
};

static DWORD WINAPI dual_worker_thread(LPVOID param) {
    struct dual_worker_arg *arg = (struct dual_worker_arg *)param;
    InterlockedIncrement(arg->ready_count);

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = NULL;
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, INFINITE)) {
            if (key == 0xFFFF) {
                break;
            } else if (key == 0xAA55) {
                InterlockedIncrement(arg->file_completed);
                InterlockedIncrement(arg->worker_task_count);
            } else if (key == 0x1234) {
                InterlockedIncrement(arg->inproc_completed);
                InterlockedIncrement(arg->worker_task_count);
                Sleep(1);
            }
        } else {
            break;
        }
    }
    return 0;
}

#define NUM_T19_WORKERS 6
#define T19_WAVES 250
#define T19_WAVE_SIZE 4
#define TOTAL_T19_TASKS (T19_WAVES * T19_WAVE_SIZE)

struct t19_worker_arg {
    HANDLE port;
    int worker_id;
    volatile LONG ready;
    volatile LONG completed;
    volatile LONG wakeups;
    volatile LONG single_wakeups;
    volatile LONG batch_wakeups;
    volatile LONG *global_completed;
    volatile LONG *stop;
};

static DWORD WINAPI t19_worker_thread(LPVOID param) {
    struct t19_worker_arg *arg = (struct t19_worker_arg *)param;
    OVERLAPPED_ENTRY entries[16];
    ULONG removed = 0;

    InterlockedIncrement(&arg->ready);

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        removed = 0;
        if (GetQueuedCompletionStatusEx(arg->port, entries, 16, &removed, 200, FALSE)) {
            if (removed == 0) continue;

            ULONG valid_count = 0;
            for (ULONG i = 0; i < removed; i++) {
                if (entries[i].lpCompletionKey == 0xDEADBEEF) {
                    InterlockedExchange(arg->stop, 1);
                    break;
                }
                valid_count++;
                for (volatile int p = 0; p < 2000; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                    YieldProcessor();
#endif
                }
            }

            if (valid_count > 0) {
                InterlockedIncrement(&arg->wakeups);
                if (valid_count == 1) InterlockedIncrement(&arg->single_wakeups);
                else InterlockedIncrement(&arg->batch_wakeups);
                InterlockedAdd(&arg->completed, valid_count);
                InterlockedAdd(arg->global_completed, valid_count);
            }
        }
    }
    return 0;
}

#define NUM_T20_WORKERS 6
#define TOTAL_T20_TASKS 50000

struct t20_worker_arg {
    HANDLE port;
    int worker_id;
    volatile LONG ready;
    volatile LONG tasks_completed;
    volatile LONG file_io_completed;
    volatile LONG *global_tasks;
    volatile LONG *global_file;
    volatile LONG *stop;
};

static DWORD WINAPI t20_worker_thread(LPVOID param) {
    struct t20_worker_arg *arg = (struct t20_worker_arg *)param;
    InterlockedIncrement(&arg->ready);

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = NULL;
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 500)) {
            if (key == 0xDEADBEEF) break;
            if (key == 0xAA55F11E) {
                InterlockedIncrement(&arg->file_io_completed);
                InterlockedIncrement(arg->global_file);
            } else {
                InterlockedIncrement(&arg->tasks_completed);
                InterlockedIncrement(arg->global_tasks);
            }
        }
    }
    return 0;
}

#define NUM_T21_WORKERS 6
#define NUM_T21_WAVES 3000
#define TASKS_PER_T21_WAVE 4

struct t21_worker_arg {
    HANDLE port;
    int worker_id;
    HANDLE wave_event;
    volatile LONG *wave_remaining;
    volatile LONG *stop;
    volatile LONG total_tasks;
    LARGE_INTEGER freq;
};

static DWORD WINAPI t21_worker_thread(LPVOID param) {
    struct t21_worker_arg *arg = (struct t21_worker_arg *)param;
    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = NULL;
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 200)) {
            if (key == 0xDEADBEEF) break;

            InterlockedIncrement(&arg->total_tasks);

            LARGE_INTEGER start, now;
            QueryPerformanceCounter(&start);
            LONGLONG target = start.QuadPart + (arg->freq.QuadPart * 20 / 1000000);
            do {
#if defined(__x86_64__) || defined(_M_X64)
                YieldProcessor();
#endif
                QueryPerformanceCounter(&now);
            } while (now.QuadPart < target);

            if (InterlockedDecrement(arg->wave_remaining) == 0) {
                SetEvent(arg->wave_event);
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 22: Sparse Single-Task Dispatch Latency & Lost-Wakeup Detection
 * ------------------------------------------------------------------------- */
#define NUM_STALL_WORKERS 8

struct sparse_worker_arg {
    HANDLE port;
    int id;
    volatile LONG stop;
    volatile LONG tasks_processed;
    volatile LONG max_latency_us;
    LARGE_INTEGER freq;
};

struct sparse_task {
    LARGE_INTEGER post_time;
    ULONG seq;
};

static DWORD WINAPI sparse_worker_thread(LPVOID param) {
    struct sparse_worker_arg *arg = (struct sparse_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;

    while (!InterlockedCompareExchange(&arg->stop, 0, 0)) {
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 500)) {
            if (key == 0xDEADBEEF) break;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            struct sparse_task *task = (struct sparse_task *)ov;
            if (task) {
                LONGLONG lat = ((now.QuadPart - task->post_time.QuadPart) * 1000000) / arg->freq.QuadPart;
                if (lat > arg->max_latency_us) {
                    InterlockedExchange(&arg->max_latency_us, (LONG)lat);
                }
                free(task);
            }
            InterlockedIncrement(&arg->tasks_processed);
        }
    }
    return 0;
}

static int run_test_sparse_dispatch(void) {
    LOG_PRINT("[TEST 22] Sparse Single-Task Dispatch Latency (500 tasks, 100us interval)... \n");
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  FAILED to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    HANDLE workers[NUM_STALL_WORKERS];
    struct sparse_worker_arg args[NUM_STALL_WORKERS];

    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].port = port;
        args[i].id = i;
        args[i].freq = freq;
        workers[i] = CreateThread(NULL, 0, sparse_worker_thread, &args[i], 0, NULL);
    }

    Sleep(50);

    const int NUM_TASKS = 500;
    LONG global_max_latency = 0;

    for (int i = 0; i < NUM_TASKS; i++) {
        struct sparse_task *task = malloc(sizeof(*task));
        task->seq = i;
        QueryPerformanceCounter(&task->post_time);

        if (!PostQueuedCompletionStatus(port, sizeof(*task), i + 1, (LPOVERLAPPED)task)) {
            LOG_PRINT("  FAILED PostQueuedCompletionStatus (err=%lu)\n", GetLastError());
            return 1;
        }

        LARGE_INTEGER start, now;
        QueryPerformanceCounter(&start);
        LONGLONG target = start.QuadPart + (freq.QuadPart * 100 / 1000000);
        do {
#if defined(__x86_64__) || defined(_M_X64)
            YieldProcessor();
#endif
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < target);
    }

    Sleep(200);

    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        InterlockedExchange(&args[i].stop, 1);
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(NUM_STALL_WORKERS, workers, TRUE, 2000);
    for (int i = 0; i < NUM_STALL_WORKERS; i++) CloseHandle(workers[i]);
    CloseHandle(port);

    int total_processed = 0;
    int participating = 0;
    int high_latency_count = 0;
    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        total_processed += args[i].tasks_processed;
        if (args[i].tasks_processed > 0) participating++;
        if (args[i].max_latency_us > global_max_latency) {
            global_max_latency = args[i].max_latency_us;
        }
        if (args[i].max_latency_us > 10000) {
            high_latency_count++;
        }
    }

    LOG_PRINT("  Processed %d/%d tasks across %d/%d workers. Max latency: %ld us (%.2f ms), stalls: %d\n",
              total_processed, NUM_TASKS, participating, NUM_STALL_WORKERS,
              global_max_latency, (double)global_max_latency / 1000.0, high_latency_count);

    if (total_processed < NUM_TASKS) {
        LOG_PRINT("  [FAIL] Lost tasks: only %d/%d completed!\n", total_processed, NUM_TASKS);
        return 1;
    }
    if (high_latency_count > 0 || global_max_latency > 15000) {
        LOG_PRINT("  [FAIL] STALL DETECTED: Max latency %ld us > 15,000 us (lost wakeup delay)!\n", global_max_latency);
        return 1;
    }
    if (participating < 1) {
        LOG_PRINT("  [FAIL] STARVATION: Only %d/%d workers participated!\n", participating, NUM_STALL_WORKERS);
        return 1;
    }

    LOG_PRINT("  [PASS] Sparse dispatch succeeded cleanly.\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 23: Chained JobSystem Fork-Join Workload (Messiah Engine Pattern)
 * ------------------------------------------------------------------------- */
struct chain_worker_arg {
    HANDLE port;
    int id;
    volatile LONG *remaining_tasks;
    volatile LONG *stop;
    volatile LONG tasks_processed;
    volatile LONG spurious_wakes;
    LARGE_INTEGER freq;
};

static DWORD WINAPI chain_worker_thread(LPVOID param) {
    struct chain_worker_arg *arg = (struct chain_worker_arg *)param;
    OVERLAPPED_ENTRY entries[8];
    ULONG removed = 0;

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        if (GetQueuedCompletionStatusEx(arg->port, entries, 8, &removed, 50, FALSE)) {
            for (ULONG i = 0; i < removed; i++) {
                ULONG_PTR key = entries[i].lpCompletionKey;
                if (key == 0xDEADBEEF) return 0;

                InterlockedIncrement(&arg->tasks_processed);
                LONG rem = InterlockedDecrement(arg->remaining_tasks);

                if (rem > 0) {
                    PostQueuedCompletionStatus(arg->port, 1, key + 1, NULL);
                }

                LARGE_INTEGER start, now;
                QueryPerformanceCounter(&start);
                LONGLONG target = start.QuadPart + (arg->freq.QuadPart * 15 / 1000000);
                do {
#if defined(__x86_64__) || defined(_M_X64)
                    YieldProcessor();
#endif
                    QueryPerformanceCounter(&now);
                } while (now.QuadPart < target);
            }
        }
    }
    return 0;
}

static int run_test_chained_jobsystem(void) {
    LOG_PRINT("[TEST 23] Chained JobSystem Fork-Join (20,000 tasks across 8 workers)... \n");
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  FAILED to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    const int TOTAL_TASKS = 20000;
    volatile LONG remaining_tasks = TOTAL_TASKS;
    volatile LONG stop = 0;

    HANDLE workers[NUM_STALL_WORKERS];
    struct chain_worker_arg args[NUM_STALL_WORKERS];

    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].port = port;
        args[i].id = i;
        args[i].remaining_tasks = &remaining_tasks;
        args[i].stop = &stop;
        args[i].freq = freq;
        workers[i] = CreateThread(NULL, 0, chain_worker_thread, &args[i], 0, NULL);
    }

    Sleep(50);

    LARGE_INTEGER t_start, t_end;
    QueryPerformanceCounter(&t_start);

    for (int i = 0; i < 16; i++) {
        PostQueuedCompletionStatus(port, 1, i + 1, NULL);
    }

    DWORD wait_start = GetTickCount();
    while (InterlockedCompareExchange(&remaining_tasks, 0, 0) > 0) {
        if (GetTickCount() - wait_start > 5000) break;
        Sleep(5);
    }
    QueryPerformanceCounter(&t_end);

    InterlockedExchange(&stop, 1);
    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(NUM_STALL_WORKERS, workers, TRUE, 2000);
    for (int i = 0; i < NUM_STALL_WORKERS; i++) CloseHandle(workers[i]);
    CloseHandle(port);

    double elapsed_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / freq.QuadPart;
    int total_processed = 0;
    int participating = 0;
    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        total_processed += args[i].tasks_processed;
        if (args[i].tasks_processed > 0) participating++;
        LOG_PRINT("    Worker %d: %ld tasks\n", i, args[i].tasks_processed);
    }

    LOG_PRINT("  Completed %d/%d tasks in %.2f ms (%d/%d workers active)\n",
              total_processed, TOTAL_TASKS, elapsed_ms, participating, NUM_STALL_WORKERS);

    if (total_processed < TOTAL_TASKS) {
        LOG_PRINT("  [FAIL] STALL: Chained tasks failed to complete within 5s (remaining: %ld)!\n",
                  remaining_tasks);
        return 1;
    }
    if (participating < 6) {
        LOG_PRINT("  [FAIL] STARVATION: Only %d/%d workers participated!\n", participating, NUM_STALL_WORKERS);
        return 1;
    }
    if (elapsed_ms > 1500.0) {
        LOG_PRINT("  [FAIL] THRASHING: Pipeline took %.2f ms > 1500 ms (thundering herd stall)!\n", elapsed_ms);
        return 1;
    }

    LOG_PRINT("  [PASS] Chained JobSystem executed smoothly.\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 24: Dual Coordinator Producer Ping-Pong (Stage 3 Messiah Engine Simulation)
 * ------------------------------------------------------------------------- */
static long get_voluntary_ctxt_switches(void) {
    FILE *f = fopen("Z:\\proc\\self\\status", "r");
    if (!f) f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long csw = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
            csw = atol(line + 24);
            break;
        }
    }
    fclose(f);
    return csw;
}

struct pingpong_task {
    HANDLE done_event;
    volatile LONG completed;
};

struct pingpong_worker_arg {
    HANDLE port;
    int id;
    volatile LONG stop;
    volatile LONG tasks_processed;
    LARGE_INTEGER freq;
};

static DWORD WINAPI pingpong_worker_thread(LPVOID param) {
    struct pingpong_worker_arg *arg = (struct pingpong_worker_arg *)param;
    OVERLAPPED_ENTRY entries[4];
    ULONG removed = 0;

    while (!InterlockedCompareExchange(&arg->stop, 0, 0)) {
        if (GetQueuedCompletionStatusEx(arg->port, entries, 4, &removed, 50, FALSE)) {
            for (ULONG i = 0; i < removed; i++) {
                ULONG_PTR key = entries[i].lpCompletionKey;
                if (key == 0xDEADBEEF) return 0;

                struct pingpong_task *task = (struct pingpong_task *)entries[i].lpOverlapped;
                if (task) {
                    LARGE_INTEGER start, now;
                    QueryPerformanceCounter(&start);
                    LONGLONG target = start.QuadPart + (arg->freq.QuadPart * 10 / 1000000);
                    do {
#if defined(__x86_64__) || defined(_M_X64)
                        YieldProcessor();
#endif
                        QueryPerformanceCounter(&now);
                    } while (now.QuadPart < target);

                    InterlockedExchange(&task->completed, 1);
                    SetEvent(task->done_event);
                }
                InterlockedIncrement(&arg->tasks_processed);
            }
        }
    }
    return 0;
}

struct pingpong_coord_arg {
    HANDLE port;
    int id;
    int iterations;
    LARGE_INTEGER freq;
    double total_ms;
};

static DWORD WINAPI pingpong_coordinator_thread(LPVOID param) {
    struct pingpong_coord_arg *arg = (struct pingpong_coord_arg *)param;
    HANDLE done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    struct pingpong_task task;
    task.done_event = done_event;

    LARGE_INTEGER t_start, t_end;
    QueryPerformanceCounter(&t_start);

    for (int i = 0; i < arg->iterations; i++) {
        task.completed = 0;
        ResetEvent(done_event);
        if (!PostQueuedCompletionStatus(arg->port, sizeof(task), arg->id + 1, (LPOVERLAPPED)&task)) {
            break;
        }
        WaitForSingleObject(done_event, INFINITE);
    }

    QueryPerformanceCounter(&t_end);
    arg->total_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / arg->freq.QuadPart;
    CloseHandle(done_event);
    return 0;
}

static int run_test_dual_producer_pingpong(void) {
    LOG_PRINT("[TEST 24] Dual Coordinator Ping-Pong (Stage 3 Simulation: 2x 3,000 tasks)... \n");
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  FAILED to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    HANDLE workers[NUM_STALL_WORKERS];
    struct pingpong_worker_arg w_args[NUM_STALL_WORKERS];

    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        memset(&w_args[i], 0, sizeof(w_args[i]));
        w_args[i].port = port;
        w_args[i].id = i;
        w_args[i].freq = freq;
        workers[i] = CreateThread(NULL, 0, pingpong_worker_thread, &w_args[i], 0, NULL);
    }

    Sleep(50);

    const int TASKS_PER_COORD = 3000;
    const int NUM_COORDS = 2;
    HANDLE coords[NUM_COORDS];
    struct pingpong_coord_arg c_args[NUM_COORDS];

    long csw_start = get_voluntary_ctxt_switches();
    LARGE_INTEGER t_start, t_end;
    QueryPerformanceCounter(&t_start);

    for (int i = 0; i < NUM_COORDS; i++) {
        c_args[i].port = port;
        c_args[i].id = i;
        c_args[i].iterations = TASKS_PER_COORD;
        c_args[i].freq = freq;
        c_args[i].total_ms = 0;
        coords[i] = CreateThread(NULL, 0, pingpong_coordinator_thread, &c_args[i], 0, NULL);
    }

    WaitForMultipleObjects(NUM_COORDS, coords, TRUE, 10000);
    QueryPerformanceCounter(&t_end);
    long csw_end = get_voluntary_ctxt_switches();

    for (int i = 0; i < NUM_COORDS; i++) CloseHandle(coords[i]);

    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        InterlockedExchange(&w_args[i].stop, 1);
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(NUM_STALL_WORKERS, workers, TRUE, 2000);
    for (int i = 0; i < NUM_STALL_WORKERS; i++) CloseHandle(workers[i]);
    CloseHandle(port);

    double total_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / freq.QuadPart;
    int total_processed = 0;
    int participating = 0;
    for (int i = 0; i < NUM_STALL_WORKERS; i++) {
        total_processed += w_args[i].tasks_processed;
        if (w_args[i].tasks_processed > 0) participating++;
        LOG_PRINT("    Worker %d: %ld tasks\n", i, w_args[i].tasks_processed);
    }

    int expected_tasks = TASKS_PER_COORD * NUM_COORDS;
    long csw_delta = (csw_start >= 0 && csw_end >= 0) ? (csw_end - csw_start) : -1;
    double csw_rate = (csw_delta >= 0 && total_ms > 0) ? ((double)csw_delta / (total_ms / 1000.0)) : 0;

    LOG_PRINT("  Completed %d/%d tasks in %.2f ms. Worker participation: %d/%d.\n",
              total_processed, expected_tasks, total_ms, participating, NUM_STALL_WORKERS);
    if (csw_delta >= 0) {
        LOG_PRINT("  Voluntary Context Switches: %ld (rate: %.0f csw/sec, %.2f csw/task)\n",
                  csw_delta, csw_rate, (double)csw_delta / expected_tasks);
    }

    if (total_processed < expected_tasks) {
        LOG_PRINT("  [FAIL] STALL: Tasks failed to complete within timeout!\n");
        return 1;
    }
    if (participating < 2) {
        LOG_PRINT("  [FAIL] STARVATION: Only %d/%d workers participated (severe thread bias & starvation due to aggressive spinner thievery)!\n",
                  participating, NUM_STALL_WORKERS);
        return 1;
    }
    if (total_ms > 1000.0) {
        LOG_PRINT("  [FAIL] THRASHING: 6,000 ping-pong tasks took %.2f ms > 1000 ms (livelock slowdown)!\n", total_ms);
        return 1;
    }

    LOG_PRINT("  [PASS] Dual coordinator ping-pong executed efficiently.\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 25: Single-Worker Kernel Sleep Boundary & Lost Wakeup Regression Test
 * Targets Dekker's store-load reordering race and kernel sleep transition.
 * Verifies that zero wakeups are lost when single workers transition between
 * user-space dequeue and kernel wait while producers post tasks.
 * ------------------------------------------------------------------------- */
struct boundary_worker_arg {
    HANDLE port;
    HANDLE done_sem;
    volatile LONG stop;
    volatile LONG timed_out;
    volatile LONG tasks_processed;
    volatile LONG in_wait;
    bool use_ex;
};

static DWORD WINAPI boundary_worker_thread(LPVOID param) {
    struct boundary_worker_arg *arg = (struct boundary_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;
    OVERLAPPED_ENTRY entry;
    ULONG removed = 0;

    while (!InterlockedCompareExchange(&arg->stop, 0, 0)) {
        InterlockedExchange(&arg->in_wait, 1);
        BOOL ok;
        if (arg->use_ex) {
            removed = 0;
            ok = GetQueuedCompletionStatusEx(arg->port, &entry, 1, &removed, 1000, FALSE);
            if (ok && removed > 0) {
                key = entry.lpCompletionKey;
            }
        } else {
            ok = GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 1000);
        }
        InterlockedExchange(&arg->in_wait, 0);

        if (!ok) {
            DWORD err = GetLastError();
            if (InterlockedCompareExchange(&arg->stop, 0, 0)) return 0;
            if (err == WAIT_TIMEOUT) {
                InterlockedExchange(&arg->timed_out, 1);
                ReleaseSemaphore(arg->done_sem, 1, NULL);
                return 0;
            }
            InterlockedExchange(&arg->timed_out, 2);
            ReleaseSemaphore(arg->done_sem, 1, NULL);
            return 0;
        }

        if (key == 0xDEADBEEF) return 0;

        InterlockedIncrement(&arg->tasks_processed);
        ReleaseSemaphore(arg->done_sem, 1, NULL);
    }
    return 0;
}

struct multi_boundary_arg {
    int id;
    int iterations;
    volatile LONG failed;
    LARGE_INTEGER freq;
    double total_ms;
};

static DWORD WINAPI multi_boundary_coord_thread(LPVOID param) {
    struct multi_boundary_arg *arg = (struct multi_boundary_arg *)param;
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    HANDLE sem = CreateSemaphoreA(NULL, 0, arg->iterations + 10, NULL);
    if (!port || !sem) {
        arg->failed = 1;
        if (port) CloseHandle(port);
        if (sem) CloseHandle(sem);
        return 1;
    }

    struct boundary_worker_arg w_arg;
    memset(&w_arg, 0, sizeof(w_arg));
    w_arg.port = port;
    w_arg.done_sem = sem;
    w_arg.use_ex = (arg->id % 2 == 1);

    HANDLE hWorker = CreateThread(NULL, 0, boundary_worker_thread, &w_arg, 0, NULL);
    if (!hWorker) {
        arg->failed = 2;
        CloseHandle(sem);
        CloseHandle(port);
        return 1;
    }

    LARGE_INTEGER t_start, t_end;
    QueryPerformanceCounter(&t_start);

    for (int i = 0; i < arg->iterations; i++) {
        if (!PostQueuedCompletionStatus(port, sizeof(DWORD), (ULONG_PTR)(i + 1), NULL)) {
            arg->failed = 3;
            break;
        }
        DWORD wr = WaitForSingleObject(sem, 1000);
        if (wr != WAIT_OBJECT_0 || w_arg.timed_out) {
            arg->failed = 4;
            break;
        }
    }

    QueryPerformanceCounter(&t_end);
    arg->total_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / arg->freq.QuadPart;

    InterlockedExchange(&w_arg.stop, 1);
    PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    WaitForSingleObject(hWorker, 2000);
    CloseHandle(hWorker);
    CloseHandle(sem);
    CloseHandle(port);

    if (w_arg.tasks_processed < arg->iterations && !arg->failed) {
        arg->failed = 5;
    }
    return 0;
}

static int run_test_boundary_wakeup_regression(void) {
    LOG_PRINT("[TEST 25] Single-Worker Kernel Sleep Boundary & Lost Wakeup Regression Test... \n");

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    HANDLE sem = CreateSemaphoreA(NULL, 0, 20000, NULL);
    if (!port || !sem) {
        LOG_PRINT("  FAILED to create IOCP or Semaphore (err=%lu)\n", GetLastError());
        if (port) CloseHandle(port);
        if (sem) CloseHandle(sem);
        return 1;
    }

    struct boundary_worker_arg w_arg;
    memset(&w_arg, 0, sizeof(w_arg));
    w_arg.port = port;
    w_arg.done_sem = sem;
    w_arg.use_ex = false;

    HANDLE hWorker = CreateThread(NULL, 0, boundary_worker_thread, &w_arg, 0, NULL);
    if (!hWorker) {
        LOG_PRINT("  FAILED to create worker thread (err=%lu)\n", GetLastError());
        CloseHandle(sem);
        CloseHandle(port);
        return 1;
    }

    Sleep(20);

    LARGE_INTEGER t_start, t_end;
    QueryPerformanceCounter(&t_start);

    // Phase 1: Tight Lockstep Ping-Pong (10,000 iterations)
    const int P1_ITERS = 10000;
    for (int i = 0; i < P1_ITERS; i++) {
        if (!PostQueuedCompletionStatus(port, sizeof(DWORD), (ULONG_PTR)(i + 1), NULL)) {
            LOG_PRINT("  [FAIL] Phase 1: PostQueuedCompletionStatus failed at iteration %d (err=%lu)\n", i, GetLastError());
            goto fail_cleanup;
        }
        DWORD wr = WaitForSingleObject(sem, 1000);
        if (wr != WAIT_OBJECT_0 || w_arg.timed_out) {
            LOG_PRINT("  [FAIL] Phase 1 STALL: Lost wakeup detected at iteration %d/%d! Worker failed to wake within 1000ms.\n",
                      i, P1_ITERS);
            goto fail_cleanup;
        }
    }

    // Phase 2: Micro-Jittered Boundary Probing (5,000 iterations)
    const int P2_ITERS = 5000;
    for (int i = 0; i < P2_ITERS; i++) {
        int mode = i % 4;
        if (mode == 1) {
            for (volatile int s = 0; s < (i % 40) + 1; s++) {
#if defined(__x86_64__) || defined(_M_X64)
                YieldProcessor();
#endif
            }
        } else if (mode == 2) {
            int spin = 0;
            while (!InterlockedCompareExchange(&w_arg.in_wait, 0, 0) && spin++ < 1000) {
#if defined(__x86_64__) || defined(_M_X64)
                YieldProcessor();
#endif
            }
        } else if (mode == 3) {
            Sleep(0);
        }

        if (!PostQueuedCompletionStatus(port, sizeof(DWORD), (ULONG_PTR)(P1_ITERS + i + 1), NULL)) {
            LOG_PRINT("  [FAIL] Phase 2: PostQueuedCompletionStatus failed at iteration %d (err=%lu)\n", i, GetLastError());
            goto fail_cleanup;
        }
        DWORD wr = WaitForSingleObject(sem, 1000);
        if (wr != WAIT_OBJECT_0 || w_arg.timed_out) {
            LOG_PRINT("  [FAIL] Phase 2 STALL: Lost wakeup under jitter at iteration %d/%d!\n", i, P2_ITERS);
            goto fail_cleanup;
        }
    }

    // Phase 3: Batch Drain & Empty-Queue Handoff (1,000 rounds = 3,000 tasks)
    const int P3_ROUNDS = 1000;
    for (int r = 0; r < P3_ROUNDS; r++) {
        BOOL p1 = PostQueuedCompletionStatus(port, sizeof(DWORD), 1, NULL);
        BOOL p2 = PostQueuedCompletionStatus(port, sizeof(DWORD), 2, NULL);

        DWORD wr = WaitForSingleObject(sem, 1000);
        if (wr != WAIT_OBJECT_0 || w_arg.timed_out) {
            struct { LONG Depth; } cbi = {0};
            ULONG retlen = 0;
            typedef NTSTATUS (WINAPI *pfnNtQueryIoCompletion)(HANDLE, ULONG, PVOID, ULONG, PULONG);
            pfnNtQueryIoCompletion pNtQuery = (pfnNtQueryIoCompletion)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryIoCompletion");
            if (pNtQuery) pNtQuery(port, 0, &cbi, sizeof(cbi), &retlen);
            LOG_PRINT("  [FAIL] Phase 3 STALL: Lost wakeup on burst at round %d (wr=%lu, timed_out=%ld, err=%lu, tasks=%ld, p1=%d, p2=%d, depth=%ld)\n",
                      r, wr, w_arg.timed_out, GetLastError(), w_arg.tasks_processed, p1, p2, cbi.Depth);
            goto fail_cleanup;
        }

        PostQueuedCompletionStatus(port, sizeof(DWORD), 3, NULL);

        wr = WaitForSingleObject(sem, 1000);
        if (wr != WAIT_OBJECT_0 || w_arg.timed_out) {
            LOG_PRINT("  [FAIL] Phase 3 STALL: Lost wakeup on 2nd packet at round %d (wr=%lu, timed_out=%ld, err=%lu, tasks=%ld)\n",
                      r, wr, w_arg.timed_out, GetLastError(), w_arg.tasks_processed);
            goto fail_cleanup;
        }
        wr = WaitForSingleObject(sem, 1000);
        if (wr != WAIT_OBJECT_0 || w_arg.timed_out) {
            LOG_PRINT("  [FAIL] Phase 3 STALL: Lost wakeup on 3rd packet at round %d (wr=%lu, timed_out=%ld, err=%lu, tasks=%ld)\n",
                      r, wr, w_arg.timed_out, GetLastError(), w_arg.tasks_processed);
            goto fail_cleanup;
        }
    }

    QueryPerformanceCounter(&t_end);
    double single_port_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / freq.QuadPart;
    int single_port_tasks = P1_ITERS + P2_ITERS + (P3_ROUNDS * 3);

    InterlockedExchange(&w_arg.stop, 1);
    PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    WaitForSingleObject(hWorker, 2000);
    CloseHandle(hWorker);
    CloseHandle(sem);
    CloseHandle(port);

    LOG_PRINT("  Single-Worker Phases 1-3: %d tasks completed in %.2f ms (%.2f us/handoff, 0 stalls)\n",
              single_port_tasks, single_port_ms, (single_port_ms * 1000.0) / single_port_tasks);

    // Phase 4: Multi-Port Concurrent Boundary Stress (4 ports x 3,000 = 12,000 tasks)
#define NUM_BOUNDARY_PORTS 4
    const int TASKS_PER_PORT = 3000;
    HANDLE coord_threads[NUM_BOUNDARY_PORTS];
    struct multi_boundary_arg coord_args[NUM_BOUNDARY_PORTS];

    LARGE_INTEGER m_start, m_end;
    QueryPerformanceCounter(&m_start);

    for (int p = 0; p < NUM_BOUNDARY_PORTS; p++) {
        memset(&coord_args[p], 0, sizeof(coord_args[p]));
        coord_args[p].id = p;
        coord_args[p].iterations = TASKS_PER_PORT;
        coord_args[p].freq = freq;
        coord_threads[p] = CreateThread(NULL, 0, multi_boundary_coord_thread, &coord_args[p], 0, NULL);
    }

    WaitForMultipleObjects(NUM_BOUNDARY_PORTS, coord_threads, TRUE, 10000);
    QueryPerformanceCounter(&m_end);

    int multi_failures = 0;
    double max_port_ms = 0;
    for (int p = 0; p < NUM_BOUNDARY_PORTS; p++) {
        if (coord_args[p].failed != 0) {
            LOG_PRINT("  [FAIL] Phase 4: Port %d coordinator failed with code %ld!\n", p, coord_args[p].failed);
            multi_failures++;
        }
        if (coord_args[p].total_ms > max_port_ms) max_port_ms = coord_args[p].total_ms;
        CloseHandle(coord_threads[p]);
    }

    if (multi_failures > 0) {
        LOG_PRINT("  [FAIL] Phase 4: %d/%d concurrent ports experienced stalls or failures!\n", multi_failures, NUM_BOUNDARY_PORTS);
        return 1;
    }

    double multi_wall_ms = ((double)(m_end.QuadPart - m_start.QuadPart) * 1000.0) / freq.QuadPart;
    LOG_PRINT("  Multi-Port Phase 4: %d ports x %d tasks = %d tasks completed concurrently in %.2f ms (0 stalls)\n",
              NUM_BOUNDARY_PORTS, TASKS_PER_PORT, NUM_BOUNDARY_PORTS * TASKS_PER_PORT, multi_wall_ms);

    LOG_PRINT("  [PASS] Boundary ping-pong regression test succeeded with 0 lost wakeups across %d handoffs.\n",
              single_port_tasks + (NUM_BOUNDARY_PORTS * TASKS_PER_PORT));
#undef NUM_BOUNDARY_PORTS
    return 0;

fail_cleanup:
    InterlockedExchange(&w_arg.stop, 1);
    PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    WaitForSingleObject(hWorker, 1000);
    CloseHandle(hWorker);
    CloseHandle(sem);
    CloseHandle(port);
    return 1;
}

/* -------------------------------------------------------------------------
 * TEST 26: WaitOnAddress Wave Barrier & Single-Coordinator Zero-Yield Conformance
 * (Where Winds Meet Post-97% Stall Reproduction & Prevention)
 * ------------------------------------------------------------------------- */
typedef NTSTATUS (WINAPI *pfnRtlWaitOnAddress)(const void *addr, const void *cmp, SIZE_T size, const LARGE_INTEGER *timeout);
typedef void (WINAPI *pfnRtlWakeAddressSingle)(const void *addr);
typedef void (WINAPI *pfnRtlWakeAddressAll)(const void *addr);
typedef NTSTATUS (WINAPI *pfnNtYieldExecution)(void);
typedef NTSTATUS (WINAPI *pfnNtDelayExecution)(BOOLEAN alertable, const LARGE_INTEGER *timeout);

#define NUM_T26_WORKERS 6
#define NUM_T26_WAVES 2500
#define TASKS_PER_T26_WAVE 4

struct t26_worker_arg {
    HANDLE port;
    int id;
    volatile LONG *wave_remaining;
    volatile LONG *stop;
    volatile LONG tasks_processed;
    pfnRtlWakeAddressSingle pRtlWakeSingle;
};

static DWORD WINAPI t26_worker_thread(LPVOID param) {
    struct t26_worker_arg *arg = (struct t26_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 100)) {
            if (key == 0xDEADBEEF) break;
            arg->tasks_processed++;

            // Simulate micro-task compute (~1.5 microsecond)
            for (volatile int p = 0; p < 400; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                YieldProcessor();
#endif
            }

            LONG rem = InterlockedDecrement(arg->wave_remaining);
            if (rem == 0 && arg->pRtlWakeSingle) {
                arg->pRtlWakeSingle((const void *)arg->wave_remaining);
            }
        }
    }
    return 0;
}

static int run_test_waitonaddress_wave_barrier(void) {
    LOG_PRINT("[TEST 26] WaitOnAddress Wave Barrier & Coordinator Zero-Yield Conformance... \n");

    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    pfnRtlWaitOnAddress pRtlWait = (pfnRtlWaitOnAddress)(void *)GetProcAddress(hNtdll, "RtlWaitOnAddress");
    pfnRtlWakeAddressSingle pRtlWakeSingle = (pfnRtlWakeAddressSingle)(void *)GetProcAddress(hNtdll, "RtlWakeAddressSingle");
    pfnNtYieldExecution pNtYield = (pfnNtYieldExecution)(void *)GetProcAddress(hNtdll, "NtYieldExecution");
    pfnNtDelayExecution pNtDelay = (pfnNtDelayExecution)(void *)GetProcAddress(hNtdll, "NtDelayExecution");

    if (!pRtlWait || !pRtlWakeSingle) {
        LOG_PRINT("  [FAIL] RtlWaitOnAddress / RtlWakeAddressSingle missing in ntdll!\n");
        return 1;
    }

    // --- Part A: Zero-Delay Yield Return Conformance ---
    int no_yield_delay_count = 0;
    int no_yield_yield_count = 0;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    for (int i = 0; i < 1000; i++) {
        if (pNtDelay && pNtDelay(FALSE, &zero) == 0x40000024 /* STATUS_NO_YIELD_PERFORMED */) {
            no_yield_delay_count++;
        }
        if (pNtYield && pNtYield() == 0x40000024) {
            no_yield_yield_count++;
        }
    }
    LOG_PRINT("  Part A: in 1,000 zero-delay calls: %d returned STATUS_NO_YIELD_PERFORMED (NtDelay), %d (NtYield)\n",
              no_yield_delay_count, no_yield_yield_count);
    int test_failed = 0;
    if (no_yield_delay_count > 0 || no_yield_yield_count > 0) {
        LOG_PRINT("  [FAIL] Zero-delay yield returned STATUS_NO_YIELD_PERFORMED (0x40000024) on %d calls! Must return STATUS_SUCCESS (0x0) to match P10-34 and prevent tight coordinator spin starvation.\n",
                  no_yield_delay_count + no_yield_yield_count);
        test_failed = 1;
    }

    // --- Part B: Micro-Task Wave Barrier Performance (2,500 waves x 4 tasks) ---
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  [FAIL] Failed to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }

    volatile LONG wave_remaining = 0;
    volatile LONG stop = 0;
    HANDLE workers[NUM_T26_WORKERS];
    struct t26_worker_arg w_args[NUM_T26_WORKERS];

    for (int i = 0; i < NUM_T26_WORKERS; i++) {
        memset(&w_args[i], 0, sizeof(w_args[i]));
        w_args[i].port = port;
        w_args[i].id = i;
        w_args[i].wave_remaining = &wave_remaining;
        w_args[i].stop = &stop;
        w_args[i].pRtlWakeSingle = pRtlWakeSingle;
        workers[i] = CreateThread(NULL, 0, t26_worker_thread, &w_args[i], 0, NULL);
    }

    Sleep(50); // allow workers to enter GetQueuedCompletionStatus

    LARGE_INTEGER freq, t_start, t_end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_start);

    for (int wave = 0; wave < NUM_T26_WAVES; wave++) {
        InterlockedExchange(&wave_remaining, TASKS_PER_T26_WAVE);
        for (int t = 0; t < TASKS_PER_T26_WAVE; t++) {
            PostQueuedCompletionStatus(port, sizeof(int), (ULONG_PTR)(wave * TASKS_PER_T26_WAVE + t + 1), NULL);
        }

        // WaitOnAddress barrier (matching Where Winds Meet coordinator pattern)
        while (InterlockedCompareExchange(&wave_remaining, 0, 0) > 0) {
            LONG cmp = InterlockedCompareExchange(&wave_remaining, 0, 0);
            if (cmp <= 0) break;
            pRtlWait((const void *)&wave_remaining, &cmp, sizeof(LONG), NULL);
        }
    }

    QueryPerformanceCounter(&t_end);
    double elapsed_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / freq.QuadPart;
    double lat_us_per_wave = (elapsed_ms * 1000.0) / NUM_T26_WAVES;
    double tasks_per_sec = (NUM_T26_WAVES * TASKS_PER_T26_WAVE) / (elapsed_ms / 1000.0);

    InterlockedExchange(&stop, 1);
    for (int i = 0; i < NUM_T26_WORKERS; i++) {
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(NUM_T26_WORKERS, workers, TRUE, 2000);
    for (int i = 0; i < NUM_T26_WORKERS; i++) CloseHandle(workers[i]);
    CloseHandle(port);

    int participating = 0;
    LONG total_tasks = 0;
    for (int i = 0; i < NUM_T26_WORKERS; i++) {
        total_tasks += w_args[i].tasks_processed;
        if (w_args[i].tasks_processed > 0) participating++;
        LOG_PRINT("    Worker %d: %ld tasks\n", i, w_args[i].tasks_processed);
    }

    LOG_PRINT("  Executed %ld/%d tasks (%d waves) across %d/%d workers in %.2f ms (%.2f us/wave, %.0f tasks/s)\n",
              total_tasks, NUM_T26_WAVES * TASKS_PER_T26_WAVE, NUM_T26_WAVES,
              participating, NUM_T26_WORKERS, elapsed_ms, lat_us_per_wave, tasks_per_sec);

    if (total_tasks < NUM_T26_WAVES * TASKS_PER_T26_WAVE) {
        LOG_PRINT("  [FAIL] STALL: Tasks failed to complete!\n");
        return 1;
    }
    if (participating < TASKS_PER_T26_WAVE) {
        LOG_PRINT("  [FAIL] STARVATION: Only %d/%d workers participated!\n", participating, NUM_T26_WORKERS);
        return 1;
    }
    if (lat_us_per_wave > 120.0) {
        LOG_PRINT("  [FAIL] WAVE BARRIER LATENCY STALL: %.2f us/wave > 120.0 us (futex wait latency collapse)!\n", lat_us_per_wave);
        return 1;
    }

    if (test_failed) {
        LOG_PRINT("  [FAIL] Test 26 failed due to Part A yield conformance errors.\n");
        return 1;
    }

    LOG_PRINT("  [PASS] WaitOnAddress wave barrier & zero-yield executed smoothly (%.2f us/wave).\n", lat_us_per_wave);
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 27: Bound File Descriptor Async I/O Wave Storm (Messiah Engine Repro)
 * Verifies inproc IOCP performance and file I/O bridge under wave storm
 * with bound file handles (iocp->has_bound_fd = 1).
 * ------------------------------------------------------------------------- */
#define NUM_T27_WORKERS 6
#define NUM_T27_WAVES 1000
#define TASKS_PER_T27_WAVE 4
#define NUM_T27_ASYNC_READS 250
#define T27_FILE_KEY 0xFA57F11E
#define T27_RING_SLOTS 16

struct t27_worker_arg {
    HANDLE port;
    int id;
    volatile LONG *wave_remaining;
    volatile LONG *stop;
    volatile LONG tasks_processed;
    volatile LONG file_reads_processed;
    pfnRtlWakeAddressSingle pRtlWakeSingle;
};

static DWORD WINAPI t27_worker_thread(LPVOID param) {
    struct t27_worker_arg *arg = (struct t27_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 100)) {
            if (key == 0xDEADBEEF) break;

            if (key == T27_FILE_KEY) {
                InterlockedIncrement(&arg->file_reads_processed);
                continue;
            }

            arg->tasks_processed++;

            // Simulate micro-task compute (~25-30 microseconds)
            for (volatile int p = 0; p < 1000; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                YieldProcessor();
#endif
            }

            LONG rem = InterlockedDecrement(arg->wave_remaining);
            if (rem == 0 && arg->pRtlWakeSingle) {
                arg->pRtlWakeSingle((const void *)arg->wave_remaining);
            }
        }
    }
    return 0;
}

static int run_test_bound_fd_wave_storm(void) {
    LOG_PRINT("[TEST 27] Bound File Descriptor Async I/O Wave Storm (Messiah Engine Repro)... \n");

    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    pfnRtlWaitOnAddress pRtlWait = (pfnRtlWaitOnAddress)(void *)GetProcAddress(hNtdll, "RtlWaitOnAddress");
    pfnRtlWakeAddressSingle pRtlWakeSingle = (pfnRtlWakeAddressSingle)(void *)GetProcAddress(hNtdll, "RtlWakeAddressSingle");

    if (!pRtlWait || !pRtlWakeSingle) {
        LOG_PRINT("  [FAIL] RtlWaitOnAddress / RtlWakeAddressSingle missing in ntdll!\n");
        return 1;
    }

    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  [FAIL] Failed to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }
    LOG_PRINT("  [DEBUG] IOCP created: %p\n", port);

    /* Create temporary file with OVERLAPPED I/O */
    char temp_path[MAX_PATH];
    char temp_file[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    GetTempFileNameA(temp_path, "iocp27", 0, temp_file);
    LOG_PRINT("  [DEBUG] Temp file: %s\n", temp_file);

    HANDLE hFile = CreateFileA(temp_file, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE,
                               NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        LOG_PRINT("  [FAIL] Failed to create temp overlapped file %s (err=%lu)\n", temp_file, GetLastError());
        CloseHandle(port);
        return 1;
    }
    LOG_PRINT("  [DEBUG] Temp file opened: %p\n", hFile);

    /* Populate file with 64KB dummy data synchronously */
    char write_buf[4096];
    memset(write_buf, 0xAB, sizeof(write_buf));
    DWORD written = 0;
    OVERLAPPED init_ov;
    for (int i = 0; i < 16; i++) {
        memset(&init_ov, 0, sizeof(init_ov));
        init_ov.Offset = i * sizeof(write_buf);
        WriteFile(hFile, write_buf, sizeof(write_buf), &written, &init_ov);
        GetOverlappedResult(hFile, &init_ov, &written, TRUE);
    }
    LOG_PRINT("  [DEBUG] File populated with 64KB dummy data\n");

    /* Bind file handle to IOCP port -> triggers mark_inproc_iocp_has_fd */
    LOG_PRINT("  [DEBUG] Binding file handle to IOCP...\n");
    HANDLE bound_port = CreateIoCompletionPort(hFile, port, T27_FILE_KEY, 0);
    if (bound_port != port) {
        LOG_PRINT("  [FAIL] CreateIoCompletionPort bind failed (err=%lu)\n", GetLastError());
        CloseHandle(hFile);
        CloseHandle(port);
        return 1;
    }
    LOG_PRINT("  [DEBUG] File handle bound successfully\n");

    volatile LONG wave_remaining = 0;
    volatile LONG stop = 0;
    HANDLE workers[NUM_T27_WORKERS];
    struct t27_worker_arg w_args[NUM_T27_WORKERS];

    for (int i = 0; i < NUM_T27_WORKERS; i++) {
        memset(&w_args[i], 0, sizeof(w_args[i]));
        w_args[i].port = port;
        w_args[i].id = i;
        w_args[i].wave_remaining = &wave_remaining;
        w_args[i].stop = &stop;
        w_args[i].pRtlWakeSingle = pRtlWakeSingle;
        workers[i] = CreateThread(NULL, 0, t27_worker_thread, &w_args[i], 0, NULL);
    }
    LOG_PRINT("  [DEBUG] 6 Workers spawned\n");

    Sleep(50); // allow workers to enter GetQueuedCompletionStatus
    LOG_PRINT("  [DEBUG] Starting wave loop with inter-wave coordinator compute & continuous streaming file I/O...\n");

    OVERLAPPED read_ov[T27_RING_SLOTS];
    char read_bufs[T27_RING_SLOTS][4096];
    memset(read_ov, 0, sizeof(read_ov));
    int reads_dispatched = 0;

    LARGE_INTEGER freq, t_start, t_end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_start);

    LONGLONG total_barrier_ticks = 0;

    for (int wave = 0; wave < NUM_T27_WAVES; wave++) {
        /* Interleave continuous async file reads across the waves (e.g. 1 read every 4 waves) */
        if ((wave % (NUM_T27_WAVES / NUM_T27_ASYNC_READS)) == 0 && reads_dispatched < NUM_T27_ASYNC_READS) {
            int slot = reads_dispatched % T27_RING_SLOTS;
            memset(&read_ov[slot], 0, sizeof(OVERLAPPED));
            read_ov[slot].Offset = (reads_dispatched % 16) * 4096;
            DWORD bytes_read = 0;
            ReadFile(hFile, read_bufs[slot], 4096, &bytes_read, &read_ov[slot]);
            reads_dispatched++;
        }

        /* 1. Simulate Messiah Engine coordinator inter-wave work (Lua VM, chunk parsing: ~50us)
         * This creates the realistic pause during which worker threads drain the queue and enter kernel wait */
        LARGE_INTEGER cw_s, cw_now;
        QueryPerformanceCounter(&cw_s);
        do {
#if defined(__x86_64__) || defined(_M_X64)
            YieldProcessor();
#endif
            QueryPerformanceCounter(&cw_now);
        } while (((double)(cw_now.QuadPart - cw_s.QuadPart) * 1000000.0) / freq.QuadPart < 50.0);

        /* 2. Dispatch the wave micro-tasks and measure pure barrier round-trip latency */
        LARGE_INTEGER w_s, w_e;
        QueryPerformanceCounter(&w_s);

        InterlockedExchange(&wave_remaining, TASKS_PER_T27_WAVE);
        for (int t = 0; t < TASKS_PER_T27_WAVE; t++) {
            PostQueuedCompletionStatus(port, sizeof(int), (ULONG_PTR)(wave * TASKS_PER_T27_WAVE + t + 1), NULL);
        }

        // WaitOnAddress barrier with 5-second safety timeout
        LARGE_INTEGER b_start, b_now;
        QueryPerformanceCounter(&b_start);
        while (InterlockedCompareExchange(&wave_remaining, 0, 0) > 0) {
            LONG cmp = InterlockedCompareExchange(&wave_remaining, 0, 0);
            if (cmp <= 0) break;
            LARGE_INTEGER to;
            to.QuadPart = -10000000; /* 1.0s wait timeout */
            pRtlWait((const void *)&wave_remaining, &cmp, sizeof(LONG), &to);
            QueryPerformanceCounter(&b_now);
            if (((double)(b_now.QuadPart - b_start.QuadPart) * 1000.0) / freq.QuadPart > 5000.0) {
                LOG_PRINT("  [FAIL] STALL: Wave %d barrier timed out after 5.0s (remaining: %ld)!\n", wave, wave_remaining);
                break;
            }
        }

        QueryPerformanceCounter(&w_e);
        total_barrier_ticks += (w_e.QuadPart - w_s.QuadPart);
    }

    QueryPerformanceCounter(&t_end);
    double elapsed_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / freq.QuadPart;
    double barrier_lat_us_per_wave = ((double)total_barrier_ticks * 1000000.0) / (freq.QuadPart * NUM_T27_WAVES);
    double total_lat_us_per_wave = (elapsed_ms * 1000.0) / NUM_T27_WAVES;
    double tasks_per_sec = (NUM_T27_WAVES * TASKS_PER_T27_WAVE) / (elapsed_ms / 1000.0);

    InterlockedExchange(&stop, 1);
    for (int i = 0; i < NUM_T27_WORKERS; i++) {
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(NUM_T27_WORKERS, workers, TRUE, 2000);
    for (int i = 0; i < NUM_T27_WORKERS; i++) CloseHandle(workers[i]);

    CloseHandle(hFile);
    CloseHandle(port);

    int participating = 0;
    LONG total_tasks = 0;
    LONG total_file_reads = 0;
    for (int i = 0; i < NUM_T27_WORKERS; i++) {
        total_tasks += w_args[i].tasks_processed;
        total_file_reads += w_args[i].file_reads_processed;
        if (w_args[i].tasks_processed > 0) participating++;
        LOG_PRINT("    Worker %d: %ld tasks, %ld file reads\n", i, w_args[i].tasks_processed, w_args[i].file_reads_processed);
    }

    LOG_PRINT("  Executed %ld/%d tasks (%d waves) and %ld/%d async file reads across %d/%d workers in %.2f ms (barrier: %.2f us/wave, total: %.2f us/wave, %.0f tasks/s)\n",
              total_tasks, NUM_T27_WAVES * TASKS_PER_T27_WAVE, NUM_T27_WAVES,
              total_file_reads, NUM_T27_ASYNC_READS,
              participating, NUM_T27_WORKERS, elapsed_ms, barrier_lat_us_per_wave, total_lat_us_per_wave, tasks_per_sec);

    if (total_tasks < NUM_T27_WAVES * TASKS_PER_T27_WAVE) {
        LOG_PRINT("  [FAIL] STALL: Tasks failed to complete!\n");
        return 1;
    }
    if (total_file_reads < NUM_T27_ASYNC_READS) {
        LOG_PRINT("  [FAIL] ASYNC FILE I/O LOSS: Only %ld/%d file completions received!\n", total_file_reads, NUM_T27_ASYNC_READS);
        return 1;
    }
    if (participating < 6) {
        LOG_PRINT("  [FAIL] WORKER MONOPOLIZATION: Only %d/%d workers participated in micro-task processing! (Server Waiter was completely starved/trapped)\n",
                  participating, NUM_T27_WORKERS);
        return 1;
    }
    if (barrier_lat_us_per_wave > 120.0) {
        LOG_PRINT("  [FAIL] WAVE BARRIER LATENCY STALL: Barrier round-trip %.2f us/wave > 120.0 us (futex sleep/wake context-switch collapse)!\n", barrier_lat_us_per_wave);
        return 1;
    }

    LOG_PRINT("  [PASS] Bound FD async I/O wave storm executed cleanly (barrier: %.2f us/wave, %ld file reads received).\n",
              barrier_lat_us_per_wave, total_file_reads);
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 31: Messiah Engine Settle Wave Pipeline Benchmark
 * Simulates Where Winds Meet Space 501 Teleportation Pipeline:
 * - 1 Coordinator thread + 6 Worker threads (matching P10-34 baseline).
 * - Coordinator does ~20 us CPU compute per wave (preparing entity/world packet).
 * - Dispatches wave of 4 tasks to IOCP port.
 * - Coordinator waits on WaitOnAddress wave barrier.
 * - Each worker dequeues via GetQueuedCompletionStatus, performs ~20 us CPU compute,
 *   decrements wave_remaining, and if last, wakes coordinator via WakeByAddressSingle.
 * - Measures:
 *     1. Wall-clock duration & wave rate (waves/sec).
 *     2. Wave turnaround latency (us/wave).
 *     3. Task throughput (tasks/sec total and per worker).
 *     4. Coordinator and Worker CPU consumption (User + Kernel time via GetThreadTimes).
 * ------------------------------------------------------------------------- */
#define MAX_SETTLE_WORKERS 32
#define DEFAULT_SETTLE_WORKERS 6
#define NUM_SETTLE_WAVES 10000
#define TASKS_PER_SETTLE_WAVE 4

static int global_settle_workers = DEFAULT_SETTLE_WORKERS;

struct settle_worker_arg {
    HANDLE port;
    int id;
    volatile LONG *wave_remaining;
    volatile LONG *stop;
    volatile LONG tasks_processed;
    pfnRtlWakeAddressSingle pRtlWakeSingle;
    LARGE_INTEGER freq;
    double target_compute_us;
    HANDLE hThread;
    double cpu_time_sec;
};

static inline void spin_compute_us(double target_us, LARGE_INTEGER freq) {
    if (target_us <= 0.0) return;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    do {
#if defined(__x86_64__) || defined(_M_X64)
        YieldProcessor();
#endif
        QueryPerformanceCounter(&now);
    } while (((double)(now.QuadPart - start.QuadPart) * 1000000.0) / (double)freq.QuadPart < target_us);
}

static double get_thread_cpu_time_sec(HANDLE hThread) {
    FILETIME creation, exit, kernel, user;
    if (GetThreadTimes(hThread, &creation, &exit, &kernel, &user)) {
        ULONGLONG kt = (((ULONGLONG)kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        ULONGLONG ut = (((ULONGLONG)user.dwHighDateTime) << 32) | user.dwLowDateTime;
        return (double)(kt + ut) / 10000000.0;
    }
    return 0.0;
}

static void get_process_context_switches(LONG *vol_cs, LONG *invol_cs) {
    FILE *f = fopen("Z:\\proc\\self\\status", "r");
    if (!f) f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
                if (vol_cs) *vol_cs = atol(line + 24);
            } else if (strncmp(line, "nonvoluntary_ctxt_switches:", 27) == 0) {
                if (invol_cs) *invol_cs = atol(line + 27);
            }
        }
        fclose(f);
    }
}

static DWORD WINAPI settle_worker_thread(LPVOID param) {
    struct settle_worker_arg *arg = (struct settle_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 100)) {
            if (key == 0xDEADBEEF) break;
            arg->tasks_processed++;

            // Simulate worker micro-task compute (~20 microseconds)
            spin_compute_us(arg->target_compute_us, arg->freq);

            LONG rem = InterlockedDecrement(arg->wave_remaining);
            if (rem == 0 && arg->pRtlWakeSingle) {
                arg->pRtlWakeSingle((const void *)arg->wave_remaining);
            }
        }
    }
    return 0;
}

static int run_test_messiah_settle_pipeline(void) {
    LOG_PRINT("[TEST 31] Messiah Engine Wave Pipeline Benchmark (Space 501 Settle Repro)... \n");

    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    pfnRtlWaitOnAddress pRtlWait = (pfnRtlWaitOnAddress)(void *)GetProcAddress(hNtdll, "RtlWaitOnAddress");
    pfnRtlWakeAddressSingle pRtlWakeSingle = (pfnRtlWakeAddressSingle)(void *)GetProcAddress(hNtdll, "RtlWakeAddressSingle");

    if (!pRtlWait || !pRtlWakeSingle) {
        LOG_PRINT("  [FAIL] RtlWaitOnAddress / RtlWakeAddressSingle missing in ntdll!\n");
        return 1;
    }

    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  [FAIL] Failed to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    int num_workers = (global_settle_workers > 0 && global_settle_workers <= MAX_SETTLE_WORKERS) ? global_settle_workers : DEFAULT_SETTLE_WORKERS;

    volatile LONG wave_remaining = 0;
    volatile LONG stop = 0;
    HANDLE workers[MAX_SETTLE_WORKERS];
    struct settle_worker_arg w_args[MAX_SETTLE_WORKERS];

    for (int i = 0; i < num_workers; i++) {
        memset(&w_args[i], 0, sizeof(w_args[i]));
        w_args[i].port = port;
        w_args[i].id = i;
        w_args[i].wave_remaining = &wave_remaining;
        w_args[i].stop = &stop;
        w_args[i].pRtlWakeSingle = pRtlWakeSingle;
        w_args[i].freq = freq;
        w_args[i].target_compute_us = 20.0; /* 20us per task matching P10-34 trace */
        workers[i] = CreateThread(NULL, 0, settle_worker_thread, &w_args[i], 0, NULL);
        w_args[i].hThread = workers[i];
    }

    Sleep(50); // allow workers to enter GetQueuedCompletionStatus

    HANDLE hCoordThread = GetCurrentThread();
    double coord_cpu_start = get_thread_cpu_time_sec(hCoordThread);
    double worker_cpu_start[MAX_SETTLE_WORKERS];
    for (int i = 0; i < num_workers; i++) {
        worker_cpu_start[i] = get_thread_cpu_time_sec(workers[i]);
    }

    LONG vol_cs_start = 0, invol_cs_start = 0;
    get_process_context_switches(&vol_cs_start, &invol_cs_start);

    LARGE_INTEGER t_start, t_end;
    QueryPerformanceCounter(&t_start);

    for (int wave = 0; wave < NUM_SETTLE_WAVES; wave++) {
        // 1. Simulate coordinator inter-wave compute (~20us matching P10-34 trace)
        spin_compute_us(20.0, freq);

        // 2. Dispatch wave micro-tasks
        InterlockedExchange(&wave_remaining, TASKS_PER_SETTLE_WAVE);
        for (int t = 0; t < TASKS_PER_SETTLE_WAVE; t++) {
            PostQueuedCompletionStatus(port, sizeof(int), (ULONG_PTR)(wave * TASKS_PER_SETTLE_WAVE + t + 1), NULL);
        }

        // 3. Coordinator wait barrier on WaitOnAddress
        while (InterlockedCompareExchange(&wave_remaining, 0, 0) > 0) {
            LONG cmp = InterlockedCompareExchange(&wave_remaining, 0, 0);
            if (cmp <= 0) break;
            pRtlWait((const void *)&wave_remaining, &cmp, sizeof(LONG), NULL);
        }
    }

    QueryPerformanceCounter(&t_end);
    double elapsed_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / (double)freq.QuadPart;
    double elapsed_sec = elapsed_ms / 1000.0;

    double coord_cpu_end = get_thread_cpu_time_sec(hCoordThread);
    double coord_cpu_sec = coord_cpu_end - coord_cpu_start;

    double total_worker_cpu_sec = 0.0;
    for (int i = 0; i < num_workers; i++) {
        double w_cpu_end = get_thread_cpu_time_sec(workers[i]);
        w_args[i].cpu_time_sec = w_cpu_end - worker_cpu_start[i];
        total_worker_cpu_sec += w_args[i].cpu_time_sec;
    }

    LONG vol_cs_end = 0, invol_cs_end = 0;
    get_process_context_switches(&vol_cs_end, &invol_cs_end);
    LONG total_vol_cs = vol_cs_end - vol_cs_start;
    LONG total_invol_cs = invol_cs_end - invol_cs_start;

    InterlockedExchange(&stop, 1);
    for (int i = 0; i < num_workers; i++) {
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(num_workers, workers, TRUE, 2000);

    LONG total_tasks = 0;
    int participating = 0;
    for (int i = 0; i < num_workers; i++) {
        total_tasks += w_args[i].tasks_processed;
        if (w_args[i].tasks_processed > 0) participating++;
        CloseHandle(workers[i]);
    }
    CloseHandle(port);

    double wave_rate = (double)NUM_SETTLE_WAVES / elapsed_sec;
    double lat_us_per_wave = (elapsed_ms * 1000.0) / (double)NUM_SETTLE_WAVES;
    double tasks_per_sec = (double)total_tasks / elapsed_sec;
    double coord_cpu_pct = (coord_cpu_sec / elapsed_sec) * 100.0;
    double avg_worker_cpu_pct = (total_worker_cpu_sec / (num_workers * elapsed_sec)) * 100.0;
    double worker_cpu_us_per_task = total_tasks > 0 ? (total_worker_cpu_sec * 1000000.0 / total_tasks) : 0.0;

    LOG_PRINT("  --- Teleport Settle Pipeline Profile (%d waves x %d tasks across %d workers) ---\n",
              NUM_SETTLE_WAVES, TASKS_PER_SETTLE_WAVE, num_workers);
    double p10_ratio = (wave_rate / 9717.0) * 100.0;
    LOG_PRINT("  Wave Rate:                %.0f waves/sec  (P10-34 actual: 9,717/s | %.1f%% of P10-34)\n", wave_rate, p10_ratio);
    LOG_PRINT("  Turnaround Latency:       %.2f us/wave    (P10-34 actual: 102.91 us/wave)\n", lat_us_per_wave);
    LOG_PRINT("  Task Throughput:          %.0f tasks/sec  (P10-34 actual: 38,867/s)\n", tasks_per_sec);
    LOG_PRINT("  Coordinator CPU:          %.1f%% of core   (P10-34 actual: 35.0%%)\n", coord_cpu_pct);
    LOG_PRINT("  Avg Worker CPU:           %.1f%% of core   (P10-34 actual: 19.9%%)\n", avg_worker_cpu_pct);
    LOG_PRINT("  Worker CPU per task:      %.2f us/task    (P10-34 actual: 30.75 us/task)\n", worker_cpu_us_per_task);
    if (total_vol_cs > 0) {
        LOG_PRINT("  Voluntary Context Sw:     %ld (%0.f csw/s, %.2f csw/task)\n",
                  total_vol_cs, (double)total_vol_cs / elapsed_sec, (double)total_vol_cs / total_tasks);
        LOG_PRINT("  Involuntary Preemptions:  %ld\n", total_invol_cs);
    }
    for (int i = 0; i < num_workers; i++) {
        LOG_PRINT("    Worker %d: %ld tasks (%.0f tasks/s), CPU=%.2fs (%.1f%%)\n",
                  i, w_args[i].tasks_processed,
                  (double)w_args[i].tasks_processed / elapsed_sec,
                  w_args[i].cpu_time_sec,
                  (w_args[i].cpu_time_sec / elapsed_sec) * 100.0);
    }

    if (total_tasks < NUM_SETTLE_WAVES * TASKS_PER_SETTLE_WAVE) {
        LOG_PRINT("  [FAIL] Incomplete tasks: %ld < %d\n", total_tasks, NUM_SETTLE_WAVES * TASKS_PER_SETTLE_WAVE);
        return 1;
    }
    if (participating < num_workers) {
        LOG_PRINT("  [FAIL] Worker starvation: only %d/%d participated\n", participating, num_workers);
        return 1;
    }

    LOG_PRINT("  [PASS] Settle pipeline benchmark completed successfully!\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 33: Sleeping Worker Wave Barrier & Concurrent Wakeup Distribution
 * Empirically tests whether worker threads wake up concurrently in parallel
 * or sequentially in a serial cascading bucket brigade.
 * ------------------------------------------------------------------------- */
#define NUM_T33_WAVES 100
#define TASKS_PER_T33_WAVE 4

struct t33_worker_arg {
    HANDLE port;
    int id;
    volatile LONG *wave_remaining;
    volatile LONG *stop;
    volatile LONG tasks_processed;
    pfnRtlWakeAddressSingle pRtlWakeSingle;
    LARGE_INTEGER freq;
    LARGE_INTEGER *p_t_wave_start;
    double *p_wake_times;
    int *p_wake_ids;
    volatile LONG *p_wake_idx;
};

static DWORD WINAPI t33_worker_thread(LPVOID param) {
    struct t33_worker_arg *arg = (struct t33_worker_arg *)param;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;
    LARGE_INTEGER now;

    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        if (GetQueuedCompletionStatus(arg->port, &bytes, &key, &ov, 1000)) {
            QueryPerformanceCounter(&now);
            if (key == 0xDEADBEEF) break;

            LONG idx = InterlockedIncrement(arg->p_wake_idx) - 1;
            if (idx >= 0 && idx < TASKS_PER_T33_WAVE) {
                double us = ((double)(now.QuadPart - arg->p_t_wave_start->QuadPart) * 1000000.0) / (double)arg->freq.QuadPart;
                arg->p_wake_times[idx] = us;
                arg->p_wake_ids[idx] = arg->id;
            }
            arg->tasks_processed++;

            // Simulate real game compute (25us per task)
            spin_compute_us(25.0, arg->freq);

            LONG rem = InterlockedDecrement(arg->wave_remaining);

            if (rem == 0 && arg->pRtlWakeSingle) {
                arg->pRtlWakeSingle((const void *)arg->wave_remaining);
            }
        }
    }
    return 0;
}

static int run_test_sleeping_worker_wakeup_distribution(void) {
    LOG_PRINT("[TEST 33] Sleeping Worker Wave Barrier & Concurrent Wakeup Distribution... \n");

    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    pfnRtlWaitOnAddress pRtlWait = (pfnRtlWaitOnAddress)(void *)GetProcAddress(hNtdll, "RtlWaitOnAddress");
    pfnRtlWakeAddressSingle pRtlWakeSingle = (pfnRtlWakeAddressSingle)(void *)GetProcAddress(hNtdll, "RtlWakeAddressSingle");

    if (!pRtlWait || !pRtlWakeSingle) {
        LOG_PRINT("  [FAIL] RtlWaitOnAddress / RtlWakeAddressSingle missing in ntdll!\n");
        return 1;
    }

    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  [FAIL] Failed to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    int num_workers = 6;
    volatile LONG wave_remaining = 0;
    volatile LONG stop = 0;
    volatile LONG wake_idx = 0;
    LARGE_INTEGER t_wave_start;
    double wake_times[TASKS_PER_T33_WAVE];
    int wake_ids[TASKS_PER_T33_WAVE];

    HANDLE workers[6];
    struct t33_worker_arg w_args[6];

    for (int i = 0; i < num_workers; i++) {
        memset(&w_args[i], 0, sizeof(w_args[i]));
        w_args[i].port = port;
        w_args[i].id = i;
        w_args[i].wave_remaining = &wave_remaining;
        w_args[i].stop = &stop;
        w_args[i].pRtlWakeSingle = pRtlWakeSingle;
        w_args[i].freq = freq;
        w_args[i].p_t_wave_start = &t_wave_start;
        w_args[i].p_wake_times = wake_times;
        w_args[i].p_wake_ids = wake_ids;
        w_args[i].p_wake_idx = &wake_idx;
        workers[i] = CreateThread(NULL, 0, t33_worker_thread, &w_args[i], 0, NULL);
    }

    // Let workers enter kernel sleep
    Sleep(50);

    double total_turnaround_us = 0.0;
    double total_first_wake_us = 0.0;
    double total_delta1_us = 0.0;
    double total_delta2_us = 0.0;
    double total_delta3_us = 0.0;
    double total_spread_us = 0.0;
    int valid_waves = 0;

    for (int wave = 0; wave < NUM_T33_WAVES; wave++) {
        // Inter-wave sleep to guarantee spin budgets expire and workers sleep in kernel
        Sleep(2);

        InterlockedExchange(&wake_idx, 0);
        for (int k = 0; k < TASKS_PER_T33_WAVE; k++) {
            wake_times[k] = 0.0;
            wake_ids[k] = -1;
        }

        InterlockedExchange(&wave_remaining, TASKS_PER_T33_WAVE);
        QueryPerformanceCounter(&t_wave_start);

        for (int t = 0; t < TASKS_PER_T33_WAVE; t++) {
            PostQueuedCompletionStatus(port, 1, (ULONG_PTR)(wave * TASKS_PER_T33_WAVE + t + 1), NULL);
        }

        while (InterlockedCompareExchange(&wave_remaining, 0, 0) > 0) {
            LONG cmp = InterlockedCompareExchange(&wave_remaining, 0, 0);
            if (cmp <= 0) break;
            pRtlWait((const void *)&wave_remaining, &cmp, sizeof(LONG), NULL);
        }

        LARGE_INTEGER t_done;
        QueryPerformanceCounter(&t_done);
        double wave_dur_us = ((double)(t_done.QuadPart - t_wave_start.QuadPart) * 1000000.0) / (double)freq.QuadPart;

        // Sort wake times to analyze inter-worker delays
        double sorted_w[TASKS_PER_T33_WAVE];
        for (int k = 0; k < TASKS_PER_T33_WAVE; k++) sorted_w[k] = wake_times[k];
        for (int a = 0; a < TASKS_PER_T33_WAVE - 1; a++) {
            for (int b = a + 1; b < TASKS_PER_T33_WAVE; b++) {
                if (sorted_w[b] < sorted_w[a]) {
                    double tmp = sorted_w[a];
                    sorted_w[a] = sorted_w[b];
                    sorted_w[b] = tmp;
                }
            }
        }

        double d1 = sorted_w[1] - sorted_w[0];
        double d2 = sorted_w[2] - sorted_w[1];
        double d3 = sorted_w[3] - sorted_w[2];
        double spread = sorted_w[3] - sorted_w[0];

        total_turnaround_us += wave_dur_us;
        total_first_wake_us += sorted_w[0];
        total_delta1_us += d1;
        total_delta2_us += d2;
        total_delta3_us += d3;
        total_spread_us += spread;
        valid_waves++;
    }

    InterlockedExchange(&stop, 1);
    for (int i = 0; i < num_workers; i++) {
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(num_workers, workers, TRUE, 2000);
    for (int i = 0; i < num_workers; i++) CloseHandle(workers[i]);
    CloseHandle(port);

    double avg_turnaround = total_turnaround_us / valid_waves;
    double avg_first_wake = total_first_wake_us / valid_waves;
    double avg_d1 = total_delta1_us / valid_waves;
    double avg_d2 = total_delta2_us / valid_waves;
    double avg_d3 = total_delta3_us / valid_waves;
    double avg_spread = total_spread_us / valid_waves;

    LOG_PRINT("  --- Results over %d Sleeping-Worker Waves (4 tasks across 6 workers) ---\n", valid_waves);
    LOG_PRINT("  Avg Wave Turnaround Latency:     %6.2f us/wave\n", avg_turnaround);
    LOG_PRINT("  Avg First Worker Wake Delay:     %6.2f us (post -> W0 wake)\n", avg_first_wake);
    LOG_PRINT("  Avg Inter-Worker Wake Delays:    d1=%6.2f us, d2=%6.2f us, d3=%6.2f us\n", avg_d1, avg_d2, avg_d3);
    LOG_PRINT("  Avg Total Wakeup Spread (W3-W0): %6.2f us\n", avg_spread);
    for (int i = 0; i < num_workers; i++) {
        LOG_PRINT("    Worker %d: %ld tasks\n", i, w_args[i].tasks_processed);
    }


    if (avg_spread > 40.0 || avg_d1 > 15.0 || avg_d2 > 15.0) {
        LOG_PRINT("  [DIAGNOSIS] SERIAL CASCADING WAKEUP CONFIRMED: Workers woke up sequentially!\n");
    } else {
        LOG_PRINT("  [DIAGNOSIS] PARALLEL CONCURRENT WAKEUP CONFIRMED: All workers woke up in parallel!\n");
    }

    LOG_PRINT("  [PASS] Test 33 completed successfully!\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * TEST 34: Contended Settle Pipeline Benchmark (208-Thread Engine Repro)
 * Simulates Where Winds Meet Space 501 Teleportation under heavy thread contention:
 * - 1 Coordinator thread + 6 Worker threads.
 * - 12 Background contention threads executing periodic compute bursts (~50us)
 *   and yielding timeslices via Sleep(0), simulating DX12/vkd3d and engine pools.
 * - 5,000 waves of 4 tasks (20,000 tasks total).
 * ------------------------------------------------------------------------- */
#define NUM_T34_BG_THREADS 12
#define NUM_T34_WAVES 5000
#define TASKS_PER_T34_WAVE 4

struct t34_bg_arg {
    volatile LONG *stop;
    LARGE_INTEGER freq;
};

static DWORD WINAPI t34_bg_thread(LPVOID param) {
    struct t34_bg_arg *arg = (struct t34_bg_arg *)param;
    while (!InterlockedCompareExchange(arg->stop, 0, 0)) {
        spin_compute_us(50.0, arg->freq);
        Sleep(0);
    }
    return 0;
}

static int run_test_contended_settle_pipeline(void) {
    LOG_PRINT("[TEST 34] Contended Settle Pipeline Benchmark (Space 501 208-Thread Repro)... \n");

    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    pfnRtlWaitOnAddress pRtlWait = (pfnRtlWaitOnAddress)(void *)GetProcAddress(hNtdll, "RtlWaitOnAddress");
    pfnRtlWakeAddressSingle pRtlWakeSingle = (pfnRtlWakeAddressSingle)(void *)GetProcAddress(hNtdll, "RtlWakeAddressSingle");

    if (!pRtlWait || !pRtlWakeSingle) {
        LOG_PRINT("  [FAIL] RtlWaitOnAddress / RtlWakeAddressSingle missing in ntdll!\n");
        return 1;
    }

    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("  [FAIL] Failed to create IOCP (err=%lu)\n", GetLastError());
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    int num_workers = 6;
    volatile LONG wave_remaining = 0;
    volatile LONG stop = 0;
    volatile LONG bg_stop = 0;

    HANDLE workers[6];
    struct settle_worker_arg w_args[6];

    for (int i = 0; i < num_workers; i++) {
        memset(&w_args[i], 0, sizeof(w_args[i]));
        w_args[i].port = port;
        w_args[i].id = i;
        w_args[i].wave_remaining = &wave_remaining;
        w_args[i].stop = &stop;
        w_args[i].pRtlWakeSingle = pRtlWakeSingle;
        w_args[i].freq = freq;
        w_args[i].target_compute_us = 20.0;
        workers[i] = CreateThread(NULL, 0, settle_worker_thread, &w_args[i], 0, NULL);
        w_args[i].hThread = workers[i];
    }

    HANDLE bg_threads[NUM_T34_BG_THREADS];
    struct t34_bg_arg bg_args[NUM_T34_BG_THREADS];
    for (int i = 0; i < NUM_T34_BG_THREADS; i++) {
        bg_args[i].stop = &bg_stop;
        bg_args[i].freq = freq;
        bg_threads[i] = CreateThread(NULL, 0, t34_bg_thread, &bg_args[i], 0, NULL);
    }

    Sleep(50);

    HANDLE hCoordThread = GetCurrentThread();
    double coord_cpu_start = get_thread_cpu_time_sec(hCoordThread);
    double worker_cpu_start[6];
    for (int i = 0; i < num_workers; i++) {
        worker_cpu_start[i] = get_thread_cpu_time_sec(workers[i]);
    }

    LONG vol_cs_start = 0, invol_cs_start = 0;
    get_process_context_switches(&vol_cs_start, &invol_cs_start);

    LARGE_INTEGER t_start, t_end;
    QueryPerformanceCounter(&t_start);

    for (int wave = 0; wave < NUM_T34_WAVES; wave++) {
        spin_compute_us(20.0, freq);

        InterlockedExchange(&wave_remaining, TASKS_PER_T34_WAVE);
        for (int t = 0; t < TASKS_PER_T34_WAVE; t++) {
            PostQueuedCompletionStatus(port, sizeof(int), (ULONG_PTR)(wave * TASKS_PER_T34_WAVE + t + 1), NULL);
        }

        while (InterlockedCompareExchange(&wave_remaining, 0, 0) > 0) {
            LONG cmp = InterlockedCompareExchange(&wave_remaining, 0, 0);
            if (cmp <= 0) break;
            pRtlWait((const void *)&wave_remaining, &cmp, sizeof(LONG), NULL);
        }
    }

    QueryPerformanceCounter(&t_end);
    double elapsed_ms = ((double)(t_end.QuadPart - t_start.QuadPart) * 1000.0) / (double)freq.QuadPart;
    double elapsed_sec = elapsed_ms / 1000.0;

    double coord_cpu_end = get_thread_cpu_time_sec(hCoordThread);
    double coord_cpu_sec = coord_cpu_end - coord_cpu_start;

    double total_worker_cpu_sec = 0.0;
    for (int i = 0; i < num_workers; i++) {
        double w_cpu_end = get_thread_cpu_time_sec(workers[i]);
        w_args[i].cpu_time_sec = w_cpu_end - worker_cpu_start[i];
        total_worker_cpu_sec += w_args[i].cpu_time_sec;
    }

    LONG vol_cs_end = 0, invol_cs_end = 0;
    get_process_context_switches(&vol_cs_end, &invol_cs_end);
    LONG total_vol_cs = vol_cs_end - vol_cs_start;
    LONG total_invol_cs = invol_cs_end - invol_cs_start;

    InterlockedExchange(&bg_stop, 1);
    WaitForMultipleObjects(NUM_T34_BG_THREADS, bg_threads, TRUE, 2000);
    for (int i = 0; i < NUM_T34_BG_THREADS; i++) CloseHandle(bg_threads[i]);

    InterlockedExchange(&stop, 1);
    for (int i = 0; i < num_workers; i++) {
        PostQueuedCompletionStatus(port, 0, 0xDEADBEEF, NULL);
    }
    WaitForMultipleObjects(num_workers, workers, TRUE, 2000);

    LONG total_tasks = 0;
    for (int i = 0; i < num_workers; i++) {
        total_tasks += w_args[i].tasks_processed;
        CloseHandle(workers[i]);
    }
    CloseHandle(port);

    double wave_rate = (double)NUM_T34_WAVES / elapsed_sec;
    double lat_us_per_wave = (elapsed_ms * 1000.0) / (double)NUM_T34_WAVES;
    double tasks_per_sec = (double)total_tasks / elapsed_sec;
    double coord_cpu_pct = (coord_cpu_sec / elapsed_sec) * 100.0;
    double avg_worker_cpu_pct = (total_worker_cpu_sec / (num_workers * elapsed_sec)) * 100.0;
    double worker_cpu_us_per_task = total_tasks > 0 ? (total_worker_cpu_sec * 1000000.0 / total_tasks) : 0.0;

    LOG_PRINT("  --- Contended Settle Profile (%d waves x %d tasks across %d workers + %d BG threads) ---\n",
              NUM_T34_WAVES, TASKS_PER_T34_WAVE, num_workers, NUM_T34_BG_THREADS);
    LOG_PRINT("  Wave Rate:                %.0f waves/sec\n", wave_rate);
    LOG_PRINT("  Turnaround Latency:       %.2f us/wave\n", lat_us_per_wave);
    LOG_PRINT("  Task Throughput:          %.0f tasks/sec\n", tasks_per_sec);
    LOG_PRINT("  Coordinator CPU:          %.1f%% of core\n", coord_cpu_pct);
    LOG_PRINT("  Avg Worker CPU:           %.1f%% of core\n", avg_worker_cpu_pct);
    LOG_PRINT("  Worker CPU per task:      %.2f us/task\n", worker_cpu_us_per_task);
    LOG_PRINT("  Voluntary Context Sw:     %ld (%0.f csw/s)\n", total_vol_cs, (double)total_vol_cs / elapsed_sec);
    LOG_PRINT("  Involuntary Preemptions:  %ld (%0.f ivcs/s)\n", total_invol_cs, (double)total_invol_cs / elapsed_sec);
    for (int i = 0; i < num_workers; i++) {
        LOG_PRINT("    Worker %d: %ld tasks (%.0f tasks/s), CPU=%.2fs (%.1f%%)\n",
                  i, w_args[i].tasks_processed,
                  (double)w_args[i].tasks_processed / elapsed_sec,
                  w_args[i].cpu_time_sec,
                  (w_args[i].cpu_time_sec / elapsed_sec) * 100.0);
    }

    LOG_PRINT("  [PASS] Test 34 completed successfully!\n");
    return 0;
}



struct woa_test32_arg {
    volatile LONG *var;
    volatile LONG *stop;
    volatile LONG iterations;
    pfnRtlWaitOnAddress pRtlWait;
    pfnRtlWakeAddressSingle pRtlWakeSingle;
};

static DWORD WINAPI woa_test32_worker(LPVOID p) {
    struct woa_test32_arg *a = (struct woa_test32_arg *)p;
    while (!InterlockedCompareExchange(a->stop, 0, 0)) {
        LONG expected = *a->var;
        if (expected % 2 == 1) {
            a->pRtlWait((const void *)a->var, &expected, sizeof(LONG), NULL);
        } else {
            InterlockedIncrement(a->var);
            a->pRtlWakeSingle((const void *)a->var);
            a->iterations++;
        }
    }
    return 0;
}

// -------------------------------------------------------------------------
// TEST 35: Port fallback semantics (direct wait, handle alias, close, timeout, depth)
// Must pass identically with in-process ports and with server-only ports.
// -------------------------------------------------------------------------
struct t35_wait_arg {
    HANDLE port;
    DWORD timeout_ms;
    BOOL result;
    DWORD err;
    double elapsed_ms;
};

static DWORD WINAPI t35_waiter(LPVOID param) {
    struct t35_wait_arg *a = (struct t35_wait_arg *)param;
    LARGE_INTEGER f, s, e;
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&s);
    a->result = GetQueuedCompletionStatus(a->port, &bytes, &key, &ov, a->timeout_ms);
    a->err = a->result ? 0 : GetLastError();
    QueryPerformanceCounter(&e);
    a->elapsed_ms = (e.QuadPart - s.QuadPart) * 1000.0 / f.QuadPart;
    return 0;
}

static int run_test_port_fallback_semantics(void) {
    typedef LONG (NTAPI *pfnNtQueryIoCompletion)(HANDLE, int, PVOID, ULONG, PULONG);
    pfnNtQueryIoCompletion pNtQueryIoCompletion =
        (pfnNtQueryIoCompletion)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryIoCompletion");
    DWORD bytes;
    ULONG_PTR key;
    LPOVERLAPPED ov;
    int failures = 0;

    LOG_PRINT("[TEST 35] Port fallback semantics (direct wait, handle alias, close, timeout, depth)... ");

    // (a) queue depth, then a direct wait on the port handle: signaled while packets are queued,
    // and the packets are still delivered in order afterwards
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    for (ULONG_PTR k = 1; k <= 3; k++) PostQueuedCompletionStatus(port, 10, k, NULL);
    ULONG depth = 0;
    if (!pNtQueryIoCompletion || pNtQueryIoCompletion(port, 0, &depth, sizeof(depth), NULL) != 0 || depth != 3) {
        LOG_PRINT("\n  (a) queue depth %lu, expected 3", depth);
        failures++;
    }
    DWORD w = WaitForSingleObject(port, 0);
    if (w != WAIT_OBJECT_0) {
        LOG_PRINT("\n  (a) direct wait with 3 queued packets returned %lu, expected WAIT_OBJECT_0", w);
        failures++;
    }
    for (ULONG_PTR k = 1; k <= 3; k++) {
        key = 0;
        if (!GetQueuedCompletionStatus(port, &bytes, &key, &ov, 1000) || key != k) {
            LOG_PRINT("\n  (a) packet %lu after the direct wait: key %lu", (unsigned long)k, (unsigned long)key);
            failures++;
            break;
        }
    }
    CloseHandle(port);

    // (b) the two low bits of a handle value are ignored
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    PostQueuedCompletionStatus((HANDLE)((ULONG_PTR)port | 1), 20, 0xb0b, NULL);
    key = 0;
    if (!GetQueuedCompletionStatus(port, &bytes, &key, &ov, 1000) || key != 0xb0b) {
        LOG_PRINT("\n  (b) post through handle|1 not seen through handle: key %lx, err %lu",
                  (unsigned long)key, GetLastError());
        failures++;
    }
    CloseHandle(port);

    // (c) closing the port wakes a blocked waiter with ERROR_ABANDONED_WAIT_0
    struct t35_wait_arg arg = { CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0), INFINITE };
    HANDLE t = CreateThread(NULL, 0, t35_waiter, &arg, 0, NULL);
    Sleep(50);
    CloseHandle(arg.port);
    if (WaitForSingleObject(t, 2000) != WAIT_OBJECT_0) {
        LOG_PRINT("\n  (c) waiter still blocked 2 s after CloseHandle");
        failures++;
    } else if (arg.result || arg.err != ERROR_ABANDONED_WAIT_0) {
        LOG_PRINT("\n  (c) waiter returned %d err %lu, expected ERROR_ABANDONED_WAIT_0", arg.result, arg.err);
        failures++;
    }
    CloseHandle(t);

    // (d) binding a file while a thread waits: the waiter keeps its original deadline
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    GetTempFileNameA(dir, "t35", 0, path);
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    struct t35_wait_arg darg = { CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0), 600 };
    t = CreateThread(NULL, 0, t35_waiter, &darg, 0, NULL);
    Sleep(300);
    if (file == INVALID_HANDLE_VALUE || CreateIoCompletionPort(file, darg.port, 0x5, 0) != darg.port) {
        LOG_PRINT("\n  (d) could not bind a file: err %lu", GetLastError());
        failures++;
    }
    WaitForSingleObject(t, 5000);
    if (darg.result || darg.err != WAIT_TIMEOUT || darg.elapsed_ms < 590 || darg.elapsed_ms > 800) {
        LOG_PRINT("\n  (d) 600 ms wait with a bind at 300 ms: result %d err %lu after %.1f ms",
                  darg.result, darg.err, darg.elapsed_ms);
        failures++;
    }
    CloseHandle(t);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    CloseHandle(darg.port);

    if (failures) LOG_PRINT("\n  FAILED (%d checks)\n", failures);
    else LOG_PRINT("PASSED\n");
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    int total_failures = 0;
    bool run_conformance = true;
    bool run_repro = true;
    bool run_settle = false;
    bool run_test33 = false;
    bool run_test34 = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--conformance") == 0) {
            run_conformance = true;
            run_repro = false;
        } else if (strcmp(argv[i], "--repro") == 0 || strcmp(argv[i], "--stall") == 0) {
            run_conformance = false;
            run_repro = true;
        } else if (strcmp(argv[i], "--settle") == 0 || strcmp(argv[i], "--benchmark") == 0) {
            run_conformance = false;
            run_repro = false;
            run_settle = true;
        } else if (strcmp(argv[i], "--test33") == 0 || strcmp(argv[i], "33") == 0) {
            run_conformance = false;
            run_repro = false;
            run_settle = false;
            run_test33 = true;
        } else if (strcmp(argv[i], "--test34") == 0 || strcmp(argv[i], "--contended") == 0 || strcmp(argv[i], "34") == 0) {
            run_conformance = false;
            run_repro = false;
            run_settle = false;
            run_test34 = true;
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            global_settle_workers = atoi(argv[++i]);
        }
    }


    log_file = fopen("C:\\test_iocp_suite.log", "w");
    if (!log_file) log_file = fopen("test_iocp_suite.log", "w");

    LOG_PRINT("============================================================\n");
    LOG_PRINT("   Wine NTDLL In-Process IOCP Full Test Suite (31 Tests)\n");
    LOG_PRINT("============================================================\n\n");

    if (run_conformance) {

    // -------------------------------------------------------------------------
    // TEST 1: Basic Create, Post & Dequeue
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 1] Single Post & Dequeue verification... ");
    HANDLE port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port) {
        LOG_PRINT("FAILED (CreateIoCompletionPort failed, err=%lu)\n", GetLastError());
        return 1;
    }

    if (!PostQueuedCompletionStatus(port, 1024, 0xCAFEBABE, (LPOVERLAPPED)(uintptr_t)0xDEADBEEF)) {
        LOG_PRINT("FAILED (PostQueuedCompletionStatus failed, err=%lu)\n", GetLastError());
        return 1;
    }

    DWORD bytes = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED ov = NULL;
    if (!GetQueuedCompletionStatus(port, &bytes, &key, &ov, 1000)) {
        LOG_PRINT("FAILED (GetQueuedCompletionStatus failed, err=%lu)\n", GetLastError());
        return 1;
    }

    if (bytes != 1024 || key != 0xCAFEBABE || ov != (LPOVERLAPPED)(uintptr_t)0xDEADBEEF) {
        LOG_PRINT("FAILED (Payload mismatch: bytes=%lu, key=0x%p, ov=0x%p)\n", bytes, (void*)key, ov);
        return 1;
    }
    LOG_PRINT("PASSED\n");

    // -------------------------------------------------------------------------
    // TEST 2: FIFO Order Preservation Across Chunk Boundaries (10,000 items)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 2] FIFO Monotonic Order (10,000 items across 78 chunks)... ");
    for (ULONG i = 0; i < 10000; i++) {
        PostQueuedCompletionStatus(port, i, (ULONG_PTR)i, (LPOVERLAPPED)(uintptr_t)(i * 2));
    }

    bool fifo_ok = true;
    for (ULONG i = 0; i < 10000; i++) {
        bytes = 0; key = 0; ov = NULL;
        if (!GetQueuedCompletionStatus(port, &bytes, &key, &ov, 1000)) {
            LOG_PRINT("FAILED (Early timeout at index %lu)\n", i);
            fifo_ok = false;
            break;
        }
        if (bytes != i || key != i || ov != (LPOVERLAPPED)(uintptr_t)(i * 2)) {
            LOG_PRINT("FAILED (Out of order at index %lu: got bytes=%lu, key=%lu)\n", i, bytes, (ULONG)key);
            fifo_ok = false;
            break;
        }
    }
    if (fifo_ok) LOG_PRINT("PASSED\n");
    else return 1;

    // -------------------------------------------------------------------------
    // TEST 3: Batch Dequeue (GetQueuedCompletionStatusEx)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 3] Batch Dequeue (GetQueuedCompletionStatusEx)... ");
    for (ULONG i = 0; i < 100; i++) {
        PostQueuedCompletionStatus(port, i + 500, (ULONG_PTR)(i + 1000), NULL);
    }

    OVERLAPPED_ENTRY batch[32];
    ULONG removed = 0;
    ULONG total_batch_popped = 0;

    while (total_batch_popped < 100) {
        if (!GetQueuedCompletionStatusEx(port, batch, 32, &removed, 1000, FALSE)) {
            LOG_PRINT("FAILED (GetQueuedCompletionStatusEx failed, err=%lu)\n", GetLastError());
            return 1;
        }
        for (ULONG k = 0; k < removed; k++) {
            ULONG expected_idx = total_batch_popped + k;
            if (batch[k].dwNumberOfBytesTransferred != expected_idx + 500 ||
                batch[k].lpCompletionKey != expected_idx + 1000) {
                LOG_PRINT("FAILED (Batch item mismatch at idx %lu)\n", expected_idx);
                return 1;
            }
        }
        total_batch_popped += removed;
    }
    if (total_batch_popped == 100) LOG_PRINT("PASSED (popped in batches correctly)\n");
    else { LOG_PRINT("FAILED (total_batch_popped=%lu)\n", total_batch_popped); return 1; }

    // -------------------------------------------------------------------------
    // TEST 4: Timeout Semantics (0ms non-blocking poll & 50ms bounded wait)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 4] Non-blocking poll (0ms) and bounded timeout (50ms)... ");
    LARGE_INTEGER freq, t1, t2;
    QueryPerformanceFrequency(&freq);

    // 0ms poll on empty port
    bytes = 0;
    BOOL poll_res = GetQueuedCompletionStatus(port, &bytes, &key, &ov, 0);
    DWORD poll_err = GetLastError();
    if (poll_res != FALSE || poll_err != WAIT_TIMEOUT) {
        LOG_PRINT("FAILED (0ms poll returned %d, err=%lu, expected FALSE/WAIT_TIMEOUT)\n", poll_res, poll_err);
        return 1;
    }

    // 50ms bounded wait on empty port
    QueryPerformanceCounter(&t1);
    BOOL wait_res = GetQueuedCompletionStatus(port, &bytes, &key, &ov, 50);
    QueryPerformanceCounter(&t2);
    DWORD wait_err = GetLastError();
    double elapsed_ms = ((double)(t2.QuadPart - t1.QuadPart) * 1000.0) / freq.QuadPart;

    if (wait_res != FALSE || wait_err != WAIT_TIMEOUT || elapsed_ms < 40.0 || elapsed_ms > 150.0) {
        LOG_PRINT("FAILED (50ms wait returned %d, err=%lu, elapsed=%.2f ms)\n", wait_res, wait_err, elapsed_ms);
        return 1;
    }
    CloseHandle(port);
    LOG_PRINT("PASSED (elapsed %.2f ms)\n", elapsed_ms);

    // -------------------------------------------------------------------------
    // TEST 4B: Alertable In-Process Dequeue & Kernel Wakeup Conformance
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 4B] Alertable in-process dequeue & kernel wakeup conformance... ");
    HANDLE alert_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!alert_port) {
        LOG_PRINT("FAILED (CreateIoCompletionPort failed, err=%lu)\n", GetLastError());
        return 1;
    }

    OVERLAPPED_ENTRY a_entries[4];
    ULONG a_removed = 0;

    // Subtest 1: Immediate alertable dequeue with post already queued
    PostQueuedCompletionStatus(alert_port, 10, 0xA110, (LPOVERLAPPED)(uintptr_t)0x5566);
    if (!GetQueuedCompletionStatusEx(alert_port, a_entries, 4, &a_removed, 100, TRUE) ||
        a_removed != 1 || a_entries[0].lpCompletionKey != 0xA110) {
        LOG_PRINT("FAILED (Immediate alertable dequeue failed, removed=%lu, key=%p)\n",
                  a_removed, (void *)a_entries[0].lpCompletionKey);
        CloseHandle(alert_port);
        return 1;
    }

    // Subtest 2: Alertable timeout on empty port
    if (GetQueuedCompletionStatusEx(alert_port, a_entries, 4, &a_removed, 50, TRUE) ||
        GetLastError() != WAIT_TIMEOUT) {
        LOG_PRINT("FAILED (Alertable 50ms wait failed to timeout with WAIT_TIMEOUT, err=%lu)\n", GetLastError());
        CloseHandle(alert_port);
        return 1;
    }

    // Subtest 3: Worker blocked in alertable wait woken by subsequent PostQueuedCompletionStatus
    struct alertable_worker_arg a_arg = {0};
    a_arg.port = alert_port;
    HANDLE a_th = CreateThread(NULL, 0, alertable_worker_thread, &a_arg, 0, NULL);
    while (InterlockedCompareExchange(&a_arg.ready, 0, 0) == 0) SwitchToThread();
    Sleep(30); // Guarantee thread has entered wait_loop in kernel

    PostQueuedCompletionStatus(alert_port, 25, 0xBEEF, (LPOVERLAPPED)(uintptr_t)0x7788);
    WaitForSingleObject(a_th, 1000);
    CloseHandle(a_th);

    if (!a_arg.res || a_arg.removed != 1 || a_arg.entry.lpCompletionKey != 0xBEEF) {
        LOG_PRINT("FAILED (Worker alertable wakeup failed: res=%d, removed=%lu, key=%p, err=%lu)\n",
                  a_arg.res, a_arg.removed, (void *)a_arg.entry.lpCompletionKey, a_arg.err);
        CloseHandle(alert_port);
        return 1;
    }

    CloseHandle(alert_port);
    LOG_PRINT("PASSED\n");

    // -------------------------------------------------------------------------
    // TEST 5: Multi-Threaded Stress Test (8 Producers, 8 Consumers, 200,000 Items)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 5] Multi-Threaded Stress Test (8P / 8C, 200,000 completions)... ");
    HANDLE stress_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    HANDLE producers[NUM_PRODUCERS];
    HANDLE consumers[NUM_CONSUMERS];
    total_dequeued = 0;
    producer_done = 0;

    QueryPerformanceCounter(&t1);

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        consumers[i] = CreateThread(NULL, 0, consumer_thread, stress_port, 0, NULL);
    }
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producers[i] = CreateThread(NULL, 0, producer_thread, stress_port, 0, NULL);
    }

    WaitForMultipleObjects(NUM_PRODUCERS, producers, TRUE, INFINITE);
    for (int i = 0; i < NUM_PRODUCERS; i++) CloseHandle(producers[i]);

    // Wait until all 200,000 items are consumed
    while (InterlockedCompareExchange(&total_dequeued, 0, 0) < TOTAL_STRESS_ITEMS) {
        Sleep(10);
    }

    QueryPerformanceCounter(&t2);
    double stress_elapsed_sec = (double)(t2.QuadPart - t1.QuadPart) / freq.QuadPart;
    double mops = (TOTAL_STRESS_ITEMS / stress_elapsed_sec) / 1000000.0;

    LOG_PRINT("PASSED (200k items processed in %.3f sec -> %.2f Million ops/sec)\n", stress_elapsed_sec, mops);

    for (int i = 0; i < NUM_CONSUMERS; i++) {
        WaitForSingleObject(consumers[i], 1000);
        CloseHandle(consumers[i]);
    }
    CloseHandle(stress_port);

    // -------------------------------------------------------------------------
    // TEST 6: Thread Wakeup on CloseHandle Teardown
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 6] Handle Teardown & Worker Thread Unblock... ");
    HANDLE close_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    struct close_worker_arg worker_args[4];
    HANDLE close_threads[4];

    for (int i = 0; i < 4; i++) {
        worker_args[i].port = close_port;
        worker_args[i].exited = 0;
        worker_args[i].last_err = 0;
        close_threads[i] = CreateThread(NULL, 0, close_worker_thread, &worker_args[i], 0, NULL);
    }

    Sleep(50); // Ensure all 4 threads are actively waiting in futex_wait
    CloseHandle(close_port); // Must wake all 4 threads!

    DWORD wait_close = WaitForMultipleObjects(4, close_threads, TRUE, 2000);
    if (wait_close == WAIT_TIMEOUT) {
        LOG_PRINT("FAILED (Threads hung after CloseHandle)\n");
        return 1;
    }

    for (int i = 0; i < 4; i++) CloseHandle(close_threads[i]);
    LOG_PRINT("PASSED (All 4 workers unblocked cleanly)\n");

    // -------------------------------------------------------------------------
    // TEST 7: Null Output Pointer Robustness (Crash Guard)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 7] Null Output Pointer Safety (Crash Guard)... ");
    HANDLE null_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    DWORD dummy_bytes = 0;
    LPOVERLAPPED dummy_ov = (LPOVERLAPPED)(uintptr_t)0x1;
    bool t7_ok = true;
    
    // Poll empty port with NULL key - must return FALSE with WAIT_TIMEOUT without crashing
    BOOL np_res = GetQueuedCompletionStatus(null_port, &dummy_bytes, NULL, &dummy_ov, 0);
    if (np_res != FALSE || GetLastError() != WAIT_TIMEOUT || dummy_ov != NULL) {
        LOG_PRINT("FAILED (Null poll failed, res=%d, err=%lu)\n", np_res, GetLastError());
        t7_ok = false;
    }

    // Post entry and dequeue with NULL key - must succeed and discard safely
    if (t7_ok) {
        PostQueuedCompletionStatus(null_port, 999, 888, (LPOVERLAPPED)(uintptr_t)0x777);
        np_res = GetQueuedCompletionStatus(null_port, &dummy_bytes, NULL, &dummy_ov, 1000);
        if (!np_res || dummy_bytes != 999 || dummy_ov != (LPOVERLAPPED)(uintptr_t)0x777) {
            LOG_PRINT("FAILED (Null dequeue failed, res=%d, err=%lu, ov=%p)\n", np_res, GetLastError(), dummy_ov);
            t7_ok = false;
        }
    }

    // Direct NTDLL NtRemoveIoCompletion with ALL null output pointers (key=NULL, val=NULL, iosb=NULL)
    typedef LONG (NTAPI *pfnNtRemoveIoCompletion)(HANDLE, PULONG_PTR, PULONG_PTR, PVOID, PLARGE_INTEGER);
    pfnNtRemoveIoCompletion pNtRemoveIoCompletion = (pfnNtRemoveIoCompletion)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtRemoveIoCompletion");
    if (t7_ok && pNtRemoveIoCompletion) {
        LARGE_INTEGER to;
        to.QuadPart = 0; // 0ms poll
        LONG nt_st = pNtRemoveIoCompletion(null_port, NULL, NULL, NULL, &to);
        if (nt_st != 0x00000102 /* STATUS_TIMEOUT */ && nt_st != 0x102) {
            LOG_PRINT("FAILED (NtRemoveIoCompletion null poll returned 0x%08lx)\n", (unsigned long)nt_st);
            t7_ok = false;
        } else {
            // Post and pop with all NULL pointers
            PostQueuedCompletionStatus(null_port, 111, 222, (LPOVERLAPPED)(uintptr_t)0x333);
            to.QuadPart = -10000000LL; // 1s
            nt_st = pNtRemoveIoCompletion(null_port, NULL, NULL, NULL, &to);
            if (nt_st != 0 /* STATUS_SUCCESS */) {
                LOG_PRINT("FAILED (NtRemoveIoCompletion null pop returned 0x%08lx)\n", (unsigned long)nt_st);
                t7_ok = false;
            }
        }
    }
    CloseHandle(null_port);
    if (t7_ok) LOG_PRINT("PASSED\n");
    else total_failures++;

    // -------------------------------------------------------------------------
    // TEST 8: Dynamic Overlapped File I/O Transition (Mixed Mode)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 8] Dynamic Overlapped File I/O Transition... ");
    HANDLE mix_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    // 1. Post 2 user tasks prior to file binding
    PostQueuedCompletionStatus(mix_port, 10, 100, (LPOVERLAPPED)(uintptr_t)0x1);
    PostQueuedCompletionStatus(mix_port, 20, 200, (LPOVERLAPPED)(uintptr_t)0x2);

    // 2. Create an overlapped file and bind it
    char temp_path[MAX_PATH], temp_file[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    GetTempFileNameA(temp_path, "iocp_test", 0, temp_file);

    HANDLE hFile = CreateFileA(temp_file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        CreateIoCompletionPort(hFile, mix_port, 0xF11E, 0);

        // 3. Post 2 user tasks after file binding
        PostQueuedCompletionStatus(mix_port, 30, 300, (LPOVERLAPPED)(uintptr_t)0x3);
        PostQueuedCompletionStatus(mix_port, 40, 400, (LPOVERLAPPED)(uintptr_t)0x4);

        // 4. Dequeue all 4 completions (both pre-bind and post-bind)
        int total_received = 0;
        for (int m = 0; m < 4; m++) {
            DWORD b = 0;
            ULONG_PTR k = 0;
            LPOVERLAPPED o = NULL;
            if (GetQueuedCompletionStatus(mix_port, &b, &k, &o, 2000)) {
                total_received++;
            }
        }
        CloseHandle(hFile);
        if (total_received != 4) {
            LOG_PRINT("FAILED (Expected 4 mixed completions, got %d)\n", total_received);
            return 1;
        }
    }
    CloseHandle(mix_port);
    LOG_PRINT("PASSED\n");

    // -------------------------------------------------------------------------
    // TEST 9: Foreign Handle & Non-IOCP Safety
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 9] Foreign Handle & Non-IOCP Safety... ");
    HANDLE dummy_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    DWORD f_bytes = 0;
    ULONG_PTR f_key = 0;
    LPOVERLAPPED f_ov = NULL;

    // Passing an Event handle to GetQueuedCompletionStatus must fail cleanly
    BOOL f_res = GetQueuedCompletionStatus(dummy_event, &f_bytes, &f_key, &f_ov, 0);
    DWORD f_err = GetLastError();
    if (f_res != FALSE || (f_err != ERROR_INVALID_HANDLE && f_err != WAIT_TIMEOUT)) {
        LOG_PRINT("FAILED (Foreign event handle check failed, res=%d, err=%lu)\n", f_res, f_err);
        return 1;
    }

    // Passing invalid handle (0x999999) must fail cleanly
    f_res = GetQueuedCompletionStatus((HANDLE)(uintptr_t)0x999999, &f_bytes, &f_key, &f_ov, 0);
    f_err = GetLastError();
    if (f_res != FALSE || f_err != ERROR_INVALID_HANDLE) {
        LOG_PRINT("FAILED (Invalid handle check failed, res=%d, err=%lu)\n", f_res, f_err);
        return 1;
    }
    CloseHandle(dummy_event);
    LOG_PRINT("PASSED\n");

    // -------------------------------------------------------------------------
    // TEST 10: 16-Thread LIFO Cascade & Anti-Starvation Distribution
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 10] 16-Thread Parallel Distribution & Anti-Starvation... ");
    HANDLE cascade_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    struct cascade_worker_arg cas_args[16];
    HANDLE cas_threads[16];

    for (int i = 0; i < 16; i++) {
        cas_args[i].port = cascade_port;
        cas_args[i].received = 0;
        cas_args[i].received_key = 0;
        cas_threads[i] = CreateThread(NULL, 0, cascade_worker_thread, &cas_args[i], 0, NULL);
    }

    Sleep(50); // Ensure all 16 threads are actively waiting in futex stack

    // Post 16 distinct tasks in a single burst
    for (int i = 0; i < 16; i++) {
        PostQueuedCompletionStatus(cascade_port, 100 + i, (ULONG_PTR)(i + 1), (LPOVERLAPPED)(uintptr_t)(i + 1));
    }

    DWORD wait_cas = WaitForMultipleObjects(16, cas_threads, TRUE, 2000);
    int total_cas_recvd = 0;
    for (int i = 0; i < 16; i++) {
        if (cas_args[i].received == 1) total_cas_recvd++;
        CloseHandle(cas_threads[i]);
    }
    CloseHandle(cascade_port);

    if (wait_cas == WAIT_TIMEOUT || total_cas_recvd != 16) {
        LOG_PRINT("FAILED (16-thread cascade: wait=%lu, received=%d/16)\n", wait_cas, total_cas_recvd);
        total_failures++;
    } else {
        LOG_PRINT("PASSED (All 16 workers distributed & completed)\n");
    }

    // -------------------------------------------------------------------------
    // TEST 11: Overlapped File I/O Non-Blocking Poll (dwMilliseconds = 0)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 11] Overlapped File I/O Non-Blocking Poll (0ms)... ");
    char temp_path_nb[MAX_PATH], temp_file_nb[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path_nb);
    GetTempFileNameA(temp_path_nb, "iocp_nb", 0, temp_file_nb);

    HANDLE hFileNB = CreateFileA(temp_file_nb, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFileNB != INVALID_HANDLE_VALUE) {
        HANDLE file_port_nb = CreateIoCompletionPort(hFileNB, NULL, 0x8899AABB, 0);
        OVERLAPPED ov_nb = {0};
        char write_buf_nb[512] = "Non-blocking IOCP file I/O test data buffer";
        DWORD bytes_written_nb = 0;

        WriteFile(hFileNB, write_buf_nb, sizeof(write_buf_nb), &bytes_written_nb, &ov_nb);
        // Wait briefly for disk write to complete
        Sleep(20);

        // Perform non-blocking poll with dwMilliseconds = 0
        DWORD nb_bytes = 0;
        ULONG_PTR nb_key = 0;
        LPOVERLAPPED nb_ov = NULL;
        BOOL nb_res = GetQueuedCompletionStatus(file_port_nb, &nb_bytes, &nb_key, &nb_ov, 0);

        if (!nb_res || nb_key != 0x8899AABB || nb_ov != &ov_nb) {
            LOG_PRINT("FAILED (Non-blocking file I/O poll failed, res=%d, key=0x%llx, err=%lu)\n",
                      nb_res, (unsigned long long)nb_key, GetLastError());
            total_failures++;
        } else {
            LOG_PRINT("PASSED (Overlapped completion retrieved via 0ms non-blocking poll)\n");
        }

        CloseHandle(hFileNB);
        CloseHandle(file_port_nb);
    } else {
        LOG_PRINT("SKIPPED (Could not create temp file)\n");
    }

    // -------------------------------------------------------------------------
    // TEST 12: Concurrent Mixed Consumers (Single & Batch Dequeue on Same Port)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 12] Concurrent Mixed-Consumer (Single + Batch) Stress... ");
    HANDLE mixed_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    const int MIXED_PRODUCERS = 4;
    const int MIXED_SINGLE_CONSUMERS = 4;
    const int MIXED_BATCH3_CONSUMERS = 4;
    const int MIXED_BATCH16_CONSUMERS = 4;

    HANDLE mixed_p_threads[4];
    struct mixed_worker_arg mixed_s_args[4];
    HANDLE mixed_s_threads[4];
    struct mixed_worker_arg mixed_b3_args[4];
    HANDLE mixed_b3_threads[4];
    struct mixed_worker_arg mixed_b16_args[4];
    HANDLE mixed_b16_threads[4];

    LARGE_INTEGER m_freq, m_start, m_end;
    QueryPerformanceFrequency(&m_freq);
    QueryPerformanceCounter(&m_start);

    for (int i = 0; i < MIXED_SINGLE_CONSUMERS; i++) {
        mixed_s_args[i].port = mixed_port;
        mixed_s_args[i].items_consumed = 0;
        mixed_s_args[i].exit_flag = 0;
        mixed_s_threads[i] = CreateThread(NULL, 0, mixed_single_consumer, &mixed_s_args[i], 0, NULL);
    }
    for (int i = 0; i < MIXED_BATCH3_CONSUMERS; i++) {
        mixed_b3_args[i].port = mixed_port;
        mixed_b3_args[i].items_consumed = 0;
        mixed_b3_args[i].exit_flag = 0;
        mixed_b3_threads[i] = CreateThread(NULL, 0, mixed_batch_consumer_3, &mixed_b3_args[i], 0, NULL);
    }
    for (int i = 0; i < MIXED_BATCH16_CONSUMERS; i++) {
        mixed_b16_args[i].port = mixed_port;
        mixed_b16_args[i].items_consumed = 0;
        mixed_b16_args[i].exit_flag = 0;
        mixed_b16_threads[i] = CreateThread(NULL, 0, mixed_batch_consumer, &mixed_b16_args[i], 0, NULL);
    }
    for (int i = 0; i < MIXED_PRODUCERS; i++) {
        mixed_p_threads[i] = CreateThread(NULL, 0, producer_thread, mixed_port, 0, NULL);
    }

    WaitForMultipleObjects(MIXED_PRODUCERS, mixed_p_threads, TRUE, INFINITE);
    for (int i = 0; i < MIXED_PRODUCERS; i++) CloseHandle(mixed_p_threads[i]);

    // Send poison pills (0xDEADBEEF)
    for (int i = 0; i < MIXED_SINGLE_CONSUMERS + MIXED_BATCH3_CONSUMERS + MIXED_BATCH16_CONSUMERS; i++) {
        PostQueuedCompletionStatus(mixed_port, 0, 0xDEADBEEF, NULL);
    }

    WaitForMultipleObjects(MIXED_SINGLE_CONSUMERS, mixed_s_threads, TRUE, 5000);
    WaitForMultipleObjects(MIXED_BATCH3_CONSUMERS, mixed_b3_threads, TRUE, 5000);
    WaitForMultipleObjects(MIXED_BATCH16_CONSUMERS, mixed_b16_threads, TRUE, 5000);

    for (int i = 0; i < MIXED_SINGLE_CONSUMERS; i++) {
        InterlockedExchange(&mixed_s_args[i].exit_flag, 1);
        CloseHandle(mixed_s_threads[i]);
    }
    for (int i = 0; i < MIXED_BATCH3_CONSUMERS; i++) {
        InterlockedExchange(&mixed_b3_args[i].exit_flag, 1);
        CloseHandle(mixed_b3_threads[i]);
    }
    for (int i = 0; i < MIXED_BATCH16_CONSUMERS; i++) {
        InterlockedExchange(&mixed_b16_args[i].exit_flag, 1);
        CloseHandle(mixed_b16_threads[i]);
    }
    CloseHandle(mixed_port);

    QueryPerformanceCounter(&m_end);
    double m_elapsed = (double)(m_end.QuadPart - m_start.QuadPart) / m_freq.QuadPart;

    int total_mixed_consumed = 0;
    for (int i = 0; i < MIXED_SINGLE_CONSUMERS; i++) total_mixed_consumed += mixed_s_args[i].items_consumed;
    for (int i = 0; i < MIXED_BATCH3_CONSUMERS; i++) total_mixed_consumed += mixed_b3_args[i].items_consumed;
    for (int i = 0; i < MIXED_BATCH16_CONSUMERS; i++) total_mixed_consumed += mixed_b16_args[i].items_consumed;

    LOG_PRINT("PASSED (Processed %d items across mixed 1-work, 3-work, 16-work consumers in %.3f sec -> %.2f M ops/sec)\n",
              total_mixed_consumed, m_elapsed, (total_mixed_consumed / m_elapsed) / 1000000.0);

    // -------------------------------------------------------------------------
    // TEST 13: High-Concurrency Cooperative Yield (SwitchToThread / Sleep(0))
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 13] High-Concurrency Cooperative Yield (16T, 160k yields)... ");
    const int YIELD_THREADS = 16;
    const int YIELD_ITERS_PER_THREAD = 10000;
    HANDLE yield_threads[16];
    struct yield_worker_arg yield_arg;
    yield_arg.iterations = YIELD_ITERS_PER_THREAD;
    yield_arg.completed = 0;

    LARGE_INTEGER y_freq, y_start, y_end;
    QueryPerformanceFrequency(&y_freq);
    QueryPerformanceCounter(&y_start);

    for (int i = 0; i < YIELD_THREADS; i++) {
        yield_threads[i] = CreateThread(NULL, 0, yield_worker, &yield_arg, 0, NULL);
    }

    WaitForMultipleObjects(YIELD_THREADS, yield_threads, TRUE, 15000);
    for (int i = 0; i < YIELD_THREADS; i++) CloseHandle(yield_threads[i]);
    QueryPerformanceCounter(&y_end);

    double y_elapsed = (double)(y_end.QuadPart - y_start.QuadPart) / y_freq.QuadPart;
    double y_rate = (YIELD_THREADS * YIELD_ITERS_PER_THREAD) / y_elapsed;
    double y_lat_ns = (y_elapsed * 1e9) / (YIELD_THREADS * YIELD_ITERS_PER_THREAD);

    if (yield_arg.completed != YIELD_THREADS) {
        LOG_PRINT("FAILED (Completed %ld / %d threads)\n", yield_arg.completed, YIELD_THREADS);
        total_failures++;
    } else {
        LOG_PRINT("PASSED (160k yields in %.3f sec -> %.2fM yields/sec, %.1f ns/yield)\n",
                  y_elapsed, y_rate / 1e6, y_lat_ns);
    }

    LOG_PRINT("\n============================================================\n");

    // -------------------------------------------------------------------------
    // TEST 14: Blocking Overlapped File I/O Asynchronous Read Wakeup
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 14] Blocking Overlapped File I/O Asynchronous Read Wakeup... ");
    char t14_path[MAX_PATH], t14_file[MAX_PATH];
    GetTempPathA(MAX_PATH, t14_path);
    GetTempFileNameA(t14_path, "iocp_t14", 0, t14_file);

    HANDLE hFile14 = CreateFileA(t14_file, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                 FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFile14 != INVALID_HANDLE_VALUE) {
        // Write initial data to file
        char write_data[1024];
        memset(write_data, 0x5A, sizeof(write_data));
        OVERLAPPED sync_ov = {0};
        DWORD bytes_w = 0;
        WriteFile(hFile14, write_data, sizeof(write_data), &bytes_w, &sync_ov);
        GetOverlappedResult(hFile14, &sync_ov, &bytes_w, TRUE);

        // Bind hFile14 to IOCP port
        HANDLE port14 = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        CreateIoCompletionPort(hFile14, port14, 0xFEEDFACE, 0);

        const int NUM_ASYNC_WORKERS = 4;
        struct blocked_file_io_worker_arg async_args[4];
        HANDLE async_threads[4];
        for (int i = 0; i < NUM_ASYNC_WORKERS; i++) {
            memset(&async_args[i], 0, sizeof(async_args[i]));
            async_args[i].port = port14;
            async_threads[i] = CreateThread(NULL, 0, blocked_file_io_worker, &async_args[i], 0, NULL);
        }

        // Wait until all workers are waiting
        for (int retry = 0; retry < 50; retry++) {
            LONG ready_cnt = 0;
            for (int i = 0; i < NUM_ASYNC_WORKERS; i++) ready_cnt += async_args[i].ready;
            if (ready_cnt == NUM_ASYNC_WORKERS) break;
            Sleep(10);
        }
        Sleep(50); // Ensure they entered blocking wait

        // Dispatch 4 asynchronous overlapped reads at different offsets
        OVERLAPPED read_ovs[4];
        char read_bufs[4][256];
        DWORD bytes_r[4] = {0};
        for (int i = 0; i < NUM_ASYNC_WORKERS; i++) {
            memset(&read_ovs[i], 0, sizeof(read_ovs[i]));
            read_ovs[i].Offset = i * 256;
            ReadFile(hFile14, read_bufs[i], sizeof(read_bufs[i]), &bytes_r[i], &read_ovs[i]);
        }

        DWORD wait_res = WaitForMultipleObjects(NUM_ASYNC_WORKERS, async_threads, TRUE, 3000);
        bool t14_ok = (wait_res != WAIT_TIMEOUT);
        int woken_count = 0;
        for (int i = 0; i < NUM_ASYNC_WORKERS; i++) {
            if (async_args[i].completed) woken_count++;
            if (async_args[i].key != 0xFEEDFACE) t14_ok = false;
            CloseHandle(async_threads[i]);
        }
        CloseHandle(port14);
        CloseHandle(hFile14);

        if (!t14_ok || woken_count != NUM_ASYNC_WORKERS) {
            LOG_PRINT("Subtest A (Pre-bind) FAILED: wait=%lu, woken=%d/%d\n", wait_res, woken_count, NUM_ASYNC_WORKERS);
            total_failures++;
        }

        // -----------------------------------------------------------------
        // Subtest B: Workers blocked in in-process futex BEFORE file binding
        // -----------------------------------------------------------------
        char t14b_file[MAX_PATH];
        GetTempFileNameA(t14_path, "iocp_t14b", 0, t14b_file);
        HANDLE hFile14_b = CreateFileA(t14b_file, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                       FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        char write_data_b[1024];
        memset(write_data_b, 0x3C, sizeof(write_data_b));
        OVERLAPPED sync_ov_b = {0};
        DWORD bytes_wb = 0;
        WriteFile(hFile14_b, write_data_b, sizeof(write_data_b), &bytes_wb, &sync_ov_b);
        GetOverlappedResult(hFile14_b, &sync_ov_b, &bytes_wb, TRUE);

        HANDLE port14_b = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

        struct blocked_file_io_worker_arg async_args_b[4];
        HANDLE async_threads_b[4];
        for (int i = 0; i < NUM_ASYNC_WORKERS; i++) {
            memset(&async_args_b[i], 0, sizeof(async_args_b[i]));
            async_args_b[i].port = port14_b;
            async_threads_b[i] = CreateThread(NULL, 0, blocked_file_io_worker, &async_args_b[i], 0, NULL);
        }

        // Wait until all workers are waiting in USER-SPACE futex
        for (int retry = 0; retry < 50; retry++) {
            LONG ready_cnt = 0;
            for (int i = 0; i < NUM_ASYNC_WORKERS; i++) ready_cnt += async_args_b[i].ready;
            if (ready_cnt == NUM_ASYNC_WORKERS) break;
            Sleep(10);
        }
        Sleep(100); // Ensure they are blocked in user-space futex_wait

        // DYNAMIC BINDING while workers are blocked!
        CreateIoCompletionPort(hFile14_b, port14_b, 0xCAFEBABE, 0);

        // Dispatch 4 asynchronous overlapped reads
        OVERLAPPED read_ovs_b[4];
        char read_bufs_b[4][256];
        DWORD bytes_r_b[4] = {0};
        for (int i = 0; i < NUM_ASYNC_WORKERS; i++) {
            memset(&read_ovs_b[i], 0, sizeof(read_ovs_b[i]));
            read_ovs_b[i].Offset = i * 256;
            ReadFile(hFile14_b, read_bufs_b[i], sizeof(read_bufs_b[i]), &bytes_r_b[i], &read_ovs_b[i]);
        }

        DWORD wait_res_b = WaitForMultipleObjects(NUM_ASYNC_WORKERS, async_threads_b, TRUE, 3000);
        bool t14b_ok = (wait_res_b != WAIT_TIMEOUT);
        int woken_count_b = 0;
        for (int i = 0; i < NUM_ASYNC_WORKERS; i++) {
            if (async_args_b[i].completed) woken_count_b++;
            if (async_args_b[i].key != 0xCAFEBABE) t14b_ok = false;
            CloseHandle(async_threads_b[i]);
        }
        CloseHandle(port14_b);
        CloseHandle(hFile14_b);

        if (!t14b_ok || woken_count_b != NUM_ASYNC_WORKERS) {
            LOG_PRINT("Subtest B (Dynamic bind while sleeping) FAILED: wait=%lu, woken=%d/%d\n",
                      wait_res_b, woken_count_b, NUM_ASYNC_WORKERS);
            total_failures++;
        } else if (t14_ok && woken_count == NUM_ASYNC_WORKERS) {
            LOG_PRINT("PASSED (All blocked workers woken across pre-bind & dynamic-bind)\n");
        }
    } else {
        LOG_PRINT("SKIPPED (Could not create temp file)\n");
    }

    // -------------------------------------------------------------------------
    // TEST 15: Duplicated Handle Interoperability (DuplicateHandle)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 15] Duplicated Handle Interoperability (DuplicateHandle)... ");
    HANDLE orig_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    HANDLE dup_port = NULL;
    BOOL dup_ok = DuplicateHandle(GetCurrentProcess(), orig_port, GetCurrentProcess(),
                                  &dup_port, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (dup_ok && dup_port) {
        // Case 1: Post to duplicated handle, dequeue from original handle
        PostQueuedCompletionStatus(dup_port, 55, 0x11223344, (LPOVERLAPPED)(uintptr_t)0x5566);
        DWORD b1 = 0;
        ULONG_PTR k1 = 0;
        LPOVERLAPPED ov1 = NULL;
        BOOL r1 = GetQueuedCompletionStatus(orig_port, &b1, &k1, &ov1, 1000);

        // Case 2: Post to original handle, dequeue from duplicated handle
        PostQueuedCompletionStatus(orig_port, 77, 0x55667788, (LPOVERLAPPED)(uintptr_t)0x99AA);
        DWORD b2 = 0;
        ULONG_PTR k2 = 0;
        LPOVERLAPPED ov2 = NULL;
        BOOL r2 = GetQueuedCompletionStatus(dup_port, &b2, &k2, &ov2, 1000);

        if (r1 && k1 == 0x11223344 && r2 && k2 == 0x55667788) {
            LOG_PRINT("PASSED (Cross-handle post & dequeue succeeded)\n");
        } else {
            LOG_PRINT("FAILED (Cross-handle post/dequeue: r1=%d k1=0x%llx, r2=%d k2=0x%llx)\n",
                      r1, (unsigned long long)k1, r2, (unsigned long long)k2);
            total_failures++;
        }
        CloseHandle(dup_port);
    } else {
        LOG_PRINT("FAILED (DuplicateHandle failed, err=%lu)\n", GetLastError());
        total_failures++;
    }
    CloseHandle(orig_port);

    // -------------------------------------------------------------------------
    // TEST 16: Relative Sleep Timing & Non-Alertable NtDelayExecution Conformance
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 16] Relative Sleep Timing & NtDelayExecution Conformance... ");
    typedef NTSTATUS (WINAPI *pfnNtDelayExecution)(BOOLEAN alertable, const LARGE_INTEGER *timeout);
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    pfnNtDelayExecution pNtDelay = (pfnNtDelayExecution)(void *)GetProcAddress(hNtdll, "NtDelayExecution");

    const int DELAY_ITERS = 30;
    LARGE_INTEGER d_freq, d_s1, d_e1, d_s2, d_e2, d_s3, d_e3;
    QueryPerformanceFrequency(&d_freq);

    // Subtest A: 30x Sleep(1)
    QueryPerformanceCounter(&d_s1);
    for (int i = 0; i < DELAY_ITERS; i++) Sleep(1);
    QueryPerformanceCounter(&d_e1);
    double sleep1_ms = ((double)(d_e1.QuadPart - d_s1.QuadPart) / d_freq.QuadPart) * 1000.0;

    // Subtest B: 30x NtDelayExecution(-10000 [1ms])
    LARGE_INTEGER t_1ms;
    t_1ms.QuadPart = -10000;
    QueryPerformanceCounter(&d_s2);
    if (pNtDelay) {
        for (int i = 0; i < DELAY_ITERS; i++) pNtDelay(FALSE, &t_1ms);
    }
    QueryPerformanceCounter(&d_e2);
    double nt1ms_ms = ((double)(d_e2.QuadPart - d_s2.QuadPart) / d_freq.QuadPart) * 1000.0;

    // Subtest C: 30x Sleep(0) (cooperative yield)
    QueryPerformanceCounter(&d_s3);
    for (int i = 0; i < DELAY_ITERS; i++) Sleep(0);
    QueryPerformanceCounter(&d_e3);
    double sleep0_ms = ((double)(d_e3.QuadPart - d_s3.QuadPart) / d_freq.QuadPart) * 1000.0;

    // Conformance validation: 30x 1ms sleeps must take at least 15ms total (allowing reasonable timer granularity)
    // On the busy-spin bug, 30x Sleep(1) completes in < 0.1ms.
    if (sleep1_ms < 15.0 || nt1ms_ms < 15.0) {
        LOG_PRINT("FAILED (Busy-spin detected: 30x Sleep(1)=%.3f ms [%.3f ms/call], NtDelay=%.3f ms [%.3f ms/call], Yield=%.3f ms)\n",
                  sleep1_ms, sleep1_ms / DELAY_ITERS, nt1ms_ms, nt1ms_ms / DELAY_ITERS, sleep0_ms);
        total_failures++;
    } else {
        LOG_PRINT("PASSED (Sleep(1)=%.1f ms [%.2f ms/call], NtDelay=%.1f ms [%.2f ms/call], Yield=%.3f ms)\n",
                  sleep1_ms, sleep1_ms / DELAY_ITERS, nt1ms_ms, nt1ms_ms / DELAY_ITERS, sleep0_ms);
    }

    // -------------------------------------------------------------------------
    // TEST 17: Multi-Worker Parallel Scaling & Non-Monopolization (Coordinator Pattern)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 17] Multi-Worker Parallel Scaling (Coordinator Dispatch)... ");
    HANDLE scaling_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    HANDLE scaling_threads[NUM_SCALING_WORKERS];
    struct scaling_worker_arg scaling_args[NUM_SCALING_WORKERS];
    volatile LONG worker_task_counts[NUM_SCALING_WORKERS] = {0};
    volatile LONG scaling_ready = 0;
    volatile LONG scaling_completed = 0;

    for (int i = 0; i < NUM_SCALING_WORKERS; i++) {
        scaling_args[i].port = scaling_port;
        scaling_args[i].worker_id = i;
        scaling_args[i].task_counts = worker_task_counts;
        scaling_args[i].ready_count = &scaling_ready;
        scaling_args[i].completed_count = &scaling_completed;
        scaling_threads[i] = CreateThread(NULL, 0, scaling_worker_thread, &scaling_args[i], 0, NULL);
    }

    // Wait until all 8 workers are ready and waiting on the completion port
    while (InterlockedCompareExchange(&scaling_ready, 0, 0) < NUM_SCALING_WORKERS) {
        Sleep(1);
    }
    // Small sleep to ensure threads entered GetQueuedCompletionStatusEx in the kernel
    Sleep(50);

    LARGE_INTEGER s_s1, s_e1;
    QueryPerformanceCounter(&s_s1);

    // Coordinator dispatches 64 tasks into the port
    for (int i = 0; i < NUM_COORDINATOR_TASKS; i++) {
        PostQueuedCompletionStatus(scaling_port, 1, (ULONG_PTR)(i + 1), NULL);
    }

    // Wait for all 64 tasks to complete
    WaitForMultipleObjects(NUM_SCALING_WORKERS, scaling_threads, TRUE, 5000);
    QueryPerformanceCounter(&s_e1);
    double scaling_elapsed_ms = ((double)(s_e1.QuadPart - s_s1.QuadPart) * 1000.0) / d_freq.QuadPart;

    for (int i = 0; i < NUM_SCALING_WORKERS; i++) CloseHandle(scaling_threads[i]);
    CloseHandle(scaling_port);

    int participating_workers = 0;
    for (int i = 0; i < NUM_SCALING_WORKERS; i++) {
        if (worker_task_counts[i] > 0) participating_workers++;
    }

    // Pass criteria: At least 4 distinct workers out of 8 must participate in parallel.
    // Under the daisy-chain starvation bug, only 1 worker monopolizes all tasks serially.
    if (participating_workers < 4) {
        LOG_PRINT("FAILED (Sequential monopolization: only %d/%d workers participated, elapsed=%.2f ms)\n",
                  participating_workers, NUM_SCALING_WORKERS, scaling_elapsed_ms);
        total_failures++;
    } else {
        LOG_PRINT("PASSED (%d/%d workers participated concurrently in parallel, elapsed=%.2f ms)\n",
                  participating_workers, NUM_SCALING_WORKERS, scaling_elapsed_ms);
    }

    // -------------------------------------------------------------------------
    // TEST 18: Dual-Use Port (File I/O + In-Process Concurrency & Starvation)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 18] Dual-use port (Overlapped File I/O + In-Process Post)... ");

    HANDLE hDualFile = CreateFileA("test_dual_iocp.tmp",
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL,
                                   CREATE_ALWAYS,
                                   FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                   NULL);
    if (hDualFile == INVALID_HANDLE_VALUE) {
        LOG_PRINT("FAILED (CreateFileA error %lu)\n", GetLastError());
        total_failures++;
    } else {
        // Write initial data to file
        char dummy_data[256];
        memset(dummy_data, 'A', sizeof(dummy_data));
        DWORD written = 0;
        OVERLAPPED ov_write = {0};
        WriteFile(hDualFile, dummy_data, sizeof(dummy_data), &written, &ov_write);
        WaitForSingleObject(hDualFile, 1000);

        // Create completion port and associate file
        HANDLE dual_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        CreateIoCompletionPort(hDualFile, dual_port, 0xAA55, 0);

        HANDLE dual_threads[NUM_DUAL_WORKERS];
        struct dual_worker_arg dual_args[NUM_DUAL_WORKERS];
        volatile LONG dual_ready = 0;
        volatile LONG dual_file_completed = 0;
        volatile LONG dual_inproc_completed = 0;
        volatile LONG dual_stop = 0;
        volatile LONG dual_worker_counts[NUM_DUAL_WORKERS] = {0};

        for (int i = 0; i < NUM_DUAL_WORKERS; i++) {
            dual_args[i].port = dual_port;
            dual_args[i].worker_id = i;
            dual_args[i].ready_count = &dual_ready;
            dual_args[i].file_completed = &dual_file_completed;
            dual_args[i].inproc_completed = &dual_inproc_completed;
            dual_args[i].stop = &dual_stop;
            dual_args[i].worker_task_count = &dual_worker_counts[i];
            dual_threads[i] = CreateThread(NULL, 0, dual_worker_thread, &dual_args[i], 0, NULL);
        }

        // Wait until all workers are waiting on the completion port
        while (InterlockedCompareExchange(&dual_ready, 0, 0) < NUM_DUAL_WORKERS) {
            Sleep(1);
        }
        Sleep(50);

        // Trigger 1 overlapped file read (dispatches via Wineserver async I/O completion)
        OVERLAPPED ov_read = {0};
        char read_buf[64];
        DWORD bytes_read = 0;
        ReadFile(hDualFile, read_buf, sizeof(read_buf), &bytes_read, &ov_read);

        // Allow file I/O to complete and wake threads into Wineserver
        Sleep(20);

        // Concurrently post 16 in-process tasks
        for (int i = 0; i < DUAL_INPROC_TASKS; i++) {
            PostQueuedCompletionStatus(dual_port, 1, 0x1234, NULL);
        }

        // Wait up to 2000 ms for file read and all 16 in-process tasks to complete
        DWORD wait_start = GetTickCount();
        while ((InterlockedCompareExchange(&dual_file_completed, 0, 0) < 1 ||
                InterlockedCompareExchange(&dual_inproc_completed, 0, 0) < DUAL_INPROC_TASKS) &&
               (GetTickCount() - wait_start < 2000)) {
            Sleep(5);
        }

        int dual_participating = 0;
        for (int i = 0; i < NUM_DUAL_WORKERS; i++) {
            if (dual_worker_counts[i] > 0) dual_participating++;
        }

        LONG file_done = InterlockedCompareExchange(&dual_file_completed, 0, 0);
        LONG inproc_done = InterlockedCompareExchange(&dual_inproc_completed, 0, 0);

        // Tell workers to stop and post exit packets to wake them if waiting on IOCP
        InterlockedExchange(&dual_stop, 1);
        for (int i = 0; i < NUM_DUAL_WORKERS; i++) {
            PostQueuedCompletionStatus(dual_port, 0, 0xFFFF, NULL);
        }

        // Wait for workers to exit with a 1000ms timeout
        DWORD thread_wait = WaitForMultipleObjects(NUM_DUAL_WORKERS, dual_threads, TRUE, 1000);

        for (int i = 0; i < NUM_DUAL_WORKERS; i++) {
            if (thread_wait == WAIT_TIMEOUT) TerminateThread(dual_threads[i], 1);
            CloseHandle(dual_threads[i]);
        }
        CloseHandle(dual_port);
        CloseHandle(hDualFile);

        if (file_done < 1 || inproc_done < DUAL_INPROC_TASKS) {
            LOG_PRINT("FAILED (Timeout: file=%ld/1, inproc=%ld/%d, participating=%d/%d, thread_wait=0x%lx)\n",
                      file_done, inproc_done, DUAL_INPROC_TASKS, dual_participating, NUM_DUAL_WORKERS, thread_wait);
            total_failures++;
        } else if (thread_wait == WAIT_TIMEOUT) {
            LOG_PRINT("FAILED (Deadlock: workers trapped in Wineserver IPC wait and failed to exit on wake_sem)\n");
            total_failures++;
        } else if (dual_participating < 2) {
            LOG_PRINT("FAILED (Starvation: only %d/%d workers participated, sibling workers trapped)\n",
                      dual_participating, NUM_DUAL_WORKERS);
            total_failures++;
        } else {
            LOG_PRINT("PASSED (All %d inproc + 1 file tasks handled, %d/%d workers participated)\n",
                      DUAL_INPROC_TASKS, dual_participating, NUM_DUAL_WORKERS);
        }
    }

    // -------------------------------------------------------------------------
    // TEST 19: High-Frequency Micro-Batch Dispatch & Batching Conformance (Last 3% Loading)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 19] High-Frequency Micro-Batch Dispatch & Batching (Last 3%% Loading)... ");
    HANDLE t19_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (t19_port) {
        HANDLE t19_threads[NUM_T19_WORKERS];
        struct t19_worker_arg t19_args[NUM_T19_WORKERS];
        volatile LONG t19_completed = 0;
        volatile LONG t19_stop = 0;

        for (int i = 0; i < NUM_T19_WORKERS; i++) {
            memset(&t19_args[i], 0, sizeof(t19_args[i]));
            t19_args[i].port = t19_port;
            t19_args[i].worker_id = i;
            t19_args[i].global_completed = &t19_completed;
            t19_args[i].stop = &t19_stop;
            t19_threads[i] = CreateThread(NULL, 0, t19_worker_thread, &t19_args[i], 0, NULL);
        }

        // Wait until all 6 workers are waiting
        for (;;) {
            LONG ready_cnt = 0;
            for (int i = 0; i < NUM_T19_WORKERS; i++) ready_cnt += t19_args[i].ready;
            if (ready_cnt == NUM_T19_WORKERS) break;
            Sleep(5);
        }
        Sleep(50); // Ensure they entered kernel wait

        LARGE_INTEGER t19_freq, t19_s, t19_e;
        QueryPerformanceFrequency(&t19_freq);
        QueryPerformanceCounter(&t19_s);

        for (int wave = 0; wave < T19_WAVES; wave++) {
            for (int k = 0; k < T19_WAVE_SIZE; k++) {
                PostQueuedCompletionStatus(t19_port, 1, (ULONG_PTR)(wave * T19_WAVE_SIZE + k + 1), NULL);
            }
            for (volatile int p = 0; p < 2000; p++) {
#if defined(__x86_64__) || defined(_M_X64)
                YieldProcessor();
#endif
            }
        }

        DWORD wait_t19 = GetTickCount();
        while (InterlockedCompareExchange(&t19_completed, 0, 0) < TOTAL_T19_TASKS &&
               (GetTickCount() - wait_t19 < 3000)) {
            Sleep(1);
        }
        QueryPerformanceCounter(&t19_e);
        double t19_ms = ((double)(t19_e.QuadPart - t19_s.QuadPart) * 1000.0) / t19_freq.QuadPart;

        InterlockedExchange(&t19_stop, 1);
        for (int i = 0; i < NUM_T19_WORKERS; i++) {
            PostQueuedCompletionStatus(t19_port, 0, 0xDEADBEEF, NULL);
        }
        WaitForMultipleObjects(NUM_T19_WORKERS, t19_threads, TRUE, 2000);
        for (int i = 0; i < NUM_T19_WORKERS; i++) CloseHandle(t19_threads[i]);
        CloseHandle(t19_port);

        LONG total_single = 0, total_batch = 0;
        int t19_participating = 0;
        for (int i = 0; i < NUM_T19_WORKERS; i++) {
            total_single += t19_args[i].single_wakeups;
            total_batch += t19_args[i].batch_wakeups;
            if (t19_args[i].completed > 0) t19_participating++;
        }

        if (t19_completed < TOTAL_T19_TASKS) {
            LOG_PRINT("FAILED (Completed only %ld/%d tasks in %.1f ms)\n", t19_completed, TOTAL_T19_TASKS, t19_ms);
            total_failures++;
        } else if (t19_participating < 4) {
            LOG_PRINT("FAILED (Starvation: only %d/%d workers participated)\n", t19_participating, NUM_T19_WORKERS);
            total_failures++;
        } else if (t19_ms > 200.0) {
            LOG_PRINT("FAILED (Latency excessive: %.1f ms for %d tasks)\n", t19_ms, TOTAL_T19_TASKS);
            total_failures++;
        } else {
            LOG_PRINT("PASSED (%d tasks in %.1f ms: batch=%ld, single=%ld, %d/%d workers)\n",
                      TOTAL_T19_TASKS, t19_ms, total_batch, total_single, t19_participating, NUM_T19_WORKERS);
        }
    } else {
        LOG_PRINT("FAILED (CreateIoCompletionPort failed)\n");
        total_failures++;
    }

    // -------------------------------------------------------------------------
    // TEST 20: Dual-Use High-Throughput Stream Stress (50,000 Tasks + Mid-Stream Async File I/O)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 20] Dual-Use High-Throughput Stress (50k inproc + mid-stream File I/O)... ");
    char temp_path_t20[MAX_PATH], temp_file_t20[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path_t20);
    GetTempFileNameA(temp_path_t20, "iocp_t20", 0, temp_file_t20);
    HANDLE hFileT20 = CreateFileA(temp_file_t20, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                                  FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFileT20 != INVALID_HANDLE_VALUE) {
        char init_buf[1024];
        memset(init_buf, 'Z', sizeof(init_buf));
        DWORD written_t20 = 0;
        OVERLAPPED init_ov_t20 = {0};
        WriteFile(hFileT20, init_buf, sizeof(init_buf), &written_t20, &init_ov_t20);
        GetOverlappedResult(hFileT20, &init_ov_t20, &written_t20, TRUE);

        HANDLE t20_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        CreateIoCompletionPort(hFileT20, t20_port, 0xAA55F11E, 0);

        HANDLE t20_threads[NUM_T20_WORKERS];
        struct t20_worker_arg t20_args[NUM_T20_WORKERS];
        volatile LONG t20_global_tasks = 0;
        volatile LONG t20_global_file = 0;
        volatile LONG t20_stop = 0;

        for (int i = 0; i < NUM_T20_WORKERS; i++) {
            memset(&t20_args[i], 0, sizeof(t20_args[i]));
            t20_args[i].port = t20_port;
            t20_args[i].worker_id = i;
            t20_args[i].global_tasks = &t20_global_tasks;
            t20_args[i].global_file = &t20_global_file;
            t20_args[i].stop = &t20_stop;
            t20_threads[i] = CreateThread(NULL, 0, t20_worker_thread, &t20_args[i], 0, NULL);
        }

        while (1) {
            LONG ready_cnt = 0;
            for (int i = 0; i < NUM_T20_WORKERS; i++) ready_cnt += t20_args[i].ready;
            if (ready_cnt == NUM_T20_WORKERS) break;
            Sleep(1);
        }
        Sleep(50);

        LARGE_INTEGER t20_freq, t20_start, t20_end;
        QueryPerformanceFrequency(&t20_freq);
        QueryPerformanceCounter(&t20_start);

        OVERLAPPED t20_read_ov = {0};
        char t20_read_buf[256];
        DWORD t20_read_bytes = 0;

        for (int i = 0; i < TOTAL_T20_TASKS; i++) {
            PostQueuedCompletionStatus(t20_port, 1, i + 1, NULL);
            if (i == TOTAL_T20_TASKS / 2) {
                ReadFile(hFileT20, t20_read_buf, sizeof(t20_read_buf), &t20_read_bytes, &t20_read_ov);
            }
        }

        DWORD wait_t20 = GetTickCount();
        while ((InterlockedCompareExchange(&t20_global_tasks, 0, 0) < TOTAL_T20_TASKS ||
                InterlockedCompareExchange(&t20_global_file, 0, 0) < 1) &&
               (GetTickCount() - wait_t20 < 5000)) {
            Sleep(1);
        }
        QueryPerformanceCounter(&t20_end);

        double t20_sec = (double)(t20_end.QuadPart - t20_start.QuadPart) / t20_freq.QuadPart;
        double t20_mops = (TOTAL_T20_TASKS / t20_sec) / 1000000.0;

        InterlockedExchange(&t20_stop, 1);
        for (int i = 0; i < NUM_T20_WORKERS; i++) {
            PostQueuedCompletionStatus(t20_port, 0, 0xDEADBEEF, NULL);
        }
        WaitForMultipleObjects(NUM_T20_WORKERS, t20_threads, TRUE, 2000);
        for (int i = 0; i < NUM_T20_WORKERS; i++) CloseHandle(t20_threads[i]);
        CloseHandle(t20_port);
        CloseHandle(hFileT20);

        int t20_participating = 0;
        for (int i = 0; i < NUM_T20_WORKERS; i++) {
            if (t20_args[i].tasks_completed > 0) t20_participating++;
        }

        if (t20_global_tasks < TOTAL_T20_TASKS || t20_global_file < 1) {
            LOG_PRINT("FAILED (Completed %ld/%d tasks, %ld/1 file I/O in %.3f sec)\n",
                      t20_global_tasks, TOTAL_T20_TASKS, t20_global_file, t20_sec);
            total_failures++;
        } else if (t20_participating < 4) {
            LOG_PRINT("FAILED (Starvation: only %d/%d workers participated)\n",
                      t20_participating, NUM_T20_WORKERS);
            total_failures++;
        } else {
            LOG_PRINT("PASSED (50k tasks + async I/O in %.3f sec -> %.2f Million ops/sec, %d/%d workers)\n",
                      t20_sec, t20_mops, t20_participating, NUM_T20_WORKERS);
        }
    } else {
        LOG_PRINT("FAILED (CreateFileA failed)\n");
        total_failures++;
    }

    // -------------------------------------------------------------------------
    // TEST 21: Closed-Loop Dependency Ping-Pong (Game Loading Simulation)
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 21] Closed-Loop Dependency Ping-Pong (Game Loading Pipeline)... ");
    HANDLE t21_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    HANDLE t21_wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (t21_port && t21_wave_event) {
        volatile LONG t21_wave_remaining = 0;
        volatile LONG t21_stop = 0;
        HANDLE t21_threads[NUM_T21_WORKERS];
        struct t21_worker_arg t21_args[NUM_T21_WORKERS];
        LARGE_INTEGER t21_freq, t21_start, t21_end;
        QueryPerformanceFrequency(&t21_freq);

        for (int i = 0; i < NUM_T21_WORKERS; i++) {
            t21_args[i].port = t21_port;
            t21_args[i].worker_id = i;
            t21_args[i].wave_event = t21_wave_event;
            t21_args[i].wave_remaining = &t21_wave_remaining;
            t21_args[i].stop = &t21_stop;
            t21_args[i].total_tasks = 0;
            t21_args[i].freq = t21_freq;
            t21_threads[i] = CreateThread(NULL, 0, t21_worker_thread, &t21_args[i], 0, NULL);
        }
        Sleep(50); // let workers enter GetQueuedCompletionStatus

        QueryPerformanceCounter(&t21_start);

        for (int wave = 0; wave < NUM_T21_WAVES; wave++) {
            InterlockedExchange(&t21_wave_remaining, TASKS_PER_T21_WAVE);
            for (int t = 0; t < TASKS_PER_T21_WAVE; t++) {
                PostQueuedCompletionStatus(t21_port, 1, wave * TASKS_PER_T21_WAVE + t + 1, NULL);
            }
            WaitForSingleObject(t21_wave_event, INFINITE);
        }

        QueryPerformanceCounter(&t21_end);
        double t21_ms = ((double)(t21_end.QuadPart - t21_start.QuadPart) * 1000.0) / t21_freq.QuadPart;
        double t21_lat_us = (t21_ms * 1000.0) / NUM_T21_WAVES;

        InterlockedExchange(&t21_stop, 1);
        for (int i = 0; i < NUM_T21_WORKERS; i++) {
            PostQueuedCompletionStatus(t21_port, 0, 0xDEADBEEF, NULL);
        }
        WaitForMultipleObjects(NUM_T21_WORKERS, t21_threads, TRUE, 2000);
        for (int i = 0; i < NUM_T21_WORKERS; i++) CloseHandle(t21_threads[i]);
        CloseHandle(t21_wave_event);
        CloseHandle(t21_port);

        int t21_participating = 0;
        for (int i = 0; i < NUM_T21_WORKERS; i++) {
            if (t21_args[i].total_tasks > 0) t21_participating++;
        }

        // Theoretical minimum compute time = NUM_T21_WAVES * 20us = 60ms.
        // If kernel context-switch thrashing occurs, latency is >70us and time >200ms.
        if (t21_participating < 4) {
            LOG_PRINT("FAILED (Starvation: only %d/%d workers participated)\n", t21_participating, NUM_T21_WORKERS);
            total_failures++;
        } else if (t21_lat_us > 180.0) {
            LOG_PRINT("FAILED (High round-trip latency: %.2f us/wave > 180 us, thrashing detected in %.1f ms)\n",
                      t21_lat_us, t21_ms);
            total_failures++;
        } else {
            LOG_PRINT("PASSED (%d waves of %d tasks in %.1f ms -> %.2f us/wave, %d/%d workers)\n",
                      NUM_T21_WAVES, TASKS_PER_T21_WAVE, t21_ms, t21_lat_us, t21_participating, NUM_T21_WORKERS);
        }
    } else {
        LOG_PRINT("FAILED (CreateIoCompletionPort / CreateEvent failed)\n");
        total_failures++;
    }
    // -------------------------------------------------------------------------
    // TEST 28: Cascading Server Waiter Handoff & Bound FD Parallel Drainage
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 28] Cascading Server Waiter Handoff & Bound FD Parallel Drainage... ");
    char t28_path[MAX_PATH], t28_file[MAX_PATH];
    GetTempPathA(MAX_PATH, t28_path);
    GetTempFileNameA(t28_path, "iocp_t28", 0, t28_file);

    HANDLE hFile28 = CreateFileA(t28_file, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                                 FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hFile28 != INVALID_HANDLE_VALUE) {
        char write_buf[4096];
        memset(write_buf, 0x7E, sizeof(write_buf));
        DWORD written = 0;
        OVERLAPPED sync_ov = {0};
        WriteFile(hFile28, write_buf, sizeof(write_buf), &written, &sync_ov);
        GetOverlappedResult(hFile28, &sync_ov, &written, TRUE);

        HANDLE port28 = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        CreateIoCompletionPort(hFile28, port28, 0xCA5CADE, 0);

        #define NUM_T28_WORKERS 8
        struct blocked_file_io_worker_arg t28_args[NUM_T28_WORKERS];
        HANDLE t28_threads[NUM_T28_WORKERS];
        for (int i = 0; i < NUM_T28_WORKERS; i++) {
            memset(&t28_args[i], 0, sizeof(t28_args[i]));
            t28_args[i].port = port28;
            t28_threads[i] = CreateThread(NULL, 0, blocked_file_io_worker, &t28_args[i], 0, NULL);
        }

        // Wait until all 8 workers are blocked on port28 (1 server waiter, 7 user futex)
        for (int retry = 0; retry < 50; retry++) {
            LONG ready_cnt = 0;
            for (int i = 0; i < NUM_T28_WORKERS; i++) ready_cnt += t28_args[i].ready;
            if (ready_cnt == NUM_T28_WORKERS) break;
            Sleep(10);
        }
        Sleep(50);

        // Dispatch 8 concurrent async file reads
        OVERLAPPED t28_ovs[NUM_T28_WORKERS];
        char t28_rbufs[NUM_T28_WORKERS][128];
        DWORD t28_rbytes[NUM_T28_WORKERS] = {0};
        for (int i = 0; i < NUM_T28_WORKERS; i++) {
            memset(&t28_ovs[i], 0, sizeof(t28_ovs[i]));
            t28_ovs[i].Offset = i * 128;
            ReadFile(hFile28, t28_rbufs[i], sizeof(t28_rbufs[i]), &t28_rbytes[i], &t28_ovs[i]);
        }

        DWORD t28_wait = WaitForMultipleObjects(NUM_T28_WORKERS, t28_threads, TRUE, 3000);
        bool t28_ok = (t28_wait != WAIT_TIMEOUT);
        int t28_woken = 0;
        for (int i = 0; i < NUM_T28_WORKERS; i++) {
            if (t28_args[i].completed) t28_woken++;
            if (t28_args[i].key != 0xCA5CADE) t28_ok = false;
            CloseHandle(t28_threads[i]);
        }
        CloseHandle(port28);
        CloseHandle(hFile28);

        if (!t28_ok || t28_woken != NUM_T28_WORKERS) {
            LOG_PRINT("FAILED (wait=0x%lx, woken=%d/%d, k0=0x%llx err0=%lu, k1=0x%llx err1=%lu)\n",
                      t28_wait, t28_woken, NUM_T28_WORKERS,
                      (unsigned long long)t28_args[0].key, t28_args[0].err,
                      (unsigned long long)t28_args[1].key, t28_args[1].err);
            total_failures++;
        } else {
            LOG_PRINT("PASSED (Cascaded cleanly across all %d/%d workers)\n", t28_woken, NUM_T28_WORKERS);
        }
    } else {
        LOG_PRINT("SKIPPED (CreateFile failed)\n");
    }

    // -------------------------------------------------------------------------
    // TEST 29: Job Object Completion Port Association & Notification Delivery
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 29] Job Object Completion Port Association & Notification Delivery... ");
    HANDLE port29 = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (port29) {
        struct blocked_file_io_worker_arg t29_arg;
        memset(&t29_arg, 0, sizeof(t29_arg));
        t29_arg.port = port29;

        HANDLE t29_thread = CreateThread(NULL, 0, blocked_file_io_worker, &t29_arg, 0, NULL);

        // Wait until worker is blocked on port29
        for (int retry = 0; retry < 50; retry++) {
            if (t29_arg.ready) break;
            Sleep(10);
        }
        Sleep(50);

        // Create Job Object and associate completion port
        HANDLE hJob = CreateJobObjectA(NULL, NULL);
        if (hJob) {
            JOBOBJECT_ASSOCIATE_COMPLETION_PORT assoc;
            memset(&assoc, 0, sizeof(assoc));
            assoc.CompletionKey = (PVOID)(uintptr_t)0x50B;
            assoc.CompletionPort = port29;

            BOOL sinfo_res = SetInformationJobObject(hJob, JobObjectAssociateCompletionPortInformation,
                                                     &assoc, sizeof(assoc));
            if (!sinfo_res) {
                LOG_PRINT("FAILED (SetInformationJobObject failed: %lu)\n", GetLastError());
                total_failures++;
            } else {
                // Assign current process to job to trigger JOB_OBJECT_MSG_NEW_PROCESS notification
                BOOL assign_res = AssignProcessToJobObject(hJob, GetCurrentProcess());
                DWORD t29_wait = WaitForSingleObject(t29_thread, 3000);
                if (t29_wait == WAIT_TIMEOUT) {
                    LOG_PRINT("FAILED (Deadlock: worker trapped in futex wait, failed to receive Job Object notification!)\n");
                    total_failures++;
                } else if (!t29_arg.completed || t29_arg.key != 0x50B) {
                    LOG_PRINT("FAILED (Notification mismatch: completed=%ld, key=0x%llx, bytes=%lu, err=%lu, assign_res=%d)\n",
                              t29_arg.completed, (unsigned long long)t29_arg.key, t29_arg.bytes, t29_arg.err, assign_res);
                    total_failures++;
                } else {
                    // Also verify in-process completion can still be posted and dequeued on the hybrid port
                    PostQueuedCompletionStatus(port29, 42, 0x1234, (LPOVERLAPPED)(uintptr_t)0x5678);
                    DWORD bytes = 0;
                    ULONG_PTR key = 0;
                    LPOVERLAPPED ov = NULL;
                    if (!GetQueuedCompletionStatus(port29, &bytes, &key, &ov, 1000) ||
                        bytes != 42 || key != 0x1234 || ov != (LPOVERLAPPED)(uintptr_t)0x5678) {
                        LOG_PRINT("FAILED (Dual hybrid inproc dequeue failed: bytes=%lu, key=0x%llx)\n",
                                  bytes, (unsigned long long)key);
                        total_failures++;
                    } else {
                        LOG_PRINT("PASSED (Job Object notification received and in-process dispatch verified)\n");
                    }
                }
            }
            CloseHandle(hJob);
        } else {
            LOG_PRINT("FAILED (CreateJobObjectA failed: %lu)\n", GetLastError());
            total_failures++;
        }
        CloseHandle(t29_thread);
        CloseHandle(port29);
    } else {
        LOG_PRINT("FAILED (CreateIoCompletionPort failed)\n");
        total_failures++;
    }

    // -------------------------------------------------------------------------
    // TEST 30: Un-bound Hybrid Port Dynamic Server Waiter & Zero-Leak Polling
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 30] Un-bound Hybrid Port Dynamic Server Waiter & Zero-Leak Polling... ");
    HANDLE port30 = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (port30) {
        // Part A: Zero-timeout poll loop (verifies req->alertable & 0x10 zero-leak conformance)
        int poll_failures = 0;
        for (int p = 0; p < 1000; p++) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED ov = NULL;
            if (GetQueuedCompletionStatus(port30, &bytes, &key, &ov, 0)) {
                poll_failures++;
                break;
            }
            if (GetLastError() != WAIT_TIMEOUT) {
                poll_failures++;
                break;
            }
        }

        if (poll_failures) {
            LOG_PRINT("FAILED (Part A: 0-timeout poll failed or returned unexpected error: %lu)\n", GetLastError());
            total_failures++;
        } else {
            // Part B: Server waiter designation and instant wakeup via inproc Fast path 3 / Wineserver
            struct blocked_file_io_worker_arg t30_arg;
            memset(&t30_arg, 0, sizeof(t30_arg));
            t30_arg.port = port30;

            HANDLE t30_thread = CreateThread(NULL, 0, blocked_infinite_worker, &t30_arg, 0, NULL);

            // Wait until worker is blocked on port30 (as designated server waiter)
            for (int retry = 0; retry < 50; retry++) {
                if (t30_arg.ready) break;
                Sleep(10);
            }
            Sleep(50);

            // Post completion to port30 (exercising Fast path 3 waking the server waiter)
            LARGE_INTEGER t_start, t_end, freq;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t_start);

            PostQueuedCompletionStatus(port30, 0x1337, 0xCAFE, (LPOVERLAPPED)(uintptr_t)0xBEEF);

            DWORD t30_wait = WaitForSingleObject(t30_thread, 2000);
            QueryPerformanceCounter(&t_end);
            double wake_ms = (double)(t_end.QuadPart - t_start.QuadPart) * 1000.0 / (double)freq.QuadPart;

            if (t30_wait == WAIT_TIMEOUT) {
                LOG_PRINT("FAILED (Part B: Deadlock! Worker trapped waiting for completion on un-bound port)\n");
                total_failures++;
            } else if (!t30_arg.completed || t30_arg.key != 0xCAFE || t30_arg.bytes != 0x1337 ||
                       t30_arg.ov != (LPOVERLAPPED)(uintptr_t)0xBEEF) {
                LOG_PRINT("FAILED (Part B: Completion payload mismatch: completed=%ld key=0x%llx bytes=%lu)\n",
                          t30_arg.completed, (unsigned long long)t30_arg.key, t30_arg.bytes);
                total_failures++;
            } else if (wake_ms > 250.0) {
                // If it took > 250ms, it missed instant notification and waited for the 500ms fallback!
                LOG_PRINT("FAILED (Part B: Wakeup latency %.1f ms > 250 ms: server waiter was not notified instantly!)\n", wake_ms);
                total_failures++;
            } else {
                // Part C: High-throughput concurrent inproc wave on the un-bound port
                #define T30_BURST 5000
                for (int i = 0; i < T30_BURST; i++) {
                    PostQueuedCompletionStatus(port30, i, 0x1234, NULL);
                }

                DWORD drained = 0;
                for (int i = 0; i < T30_BURST; i++) {
                    DWORD bytes = 0;
                    ULONG_PTR key = 0;
                    LPOVERLAPPED ov = NULL;
                    if (GetQueuedCompletionStatus(port30, &bytes, &key, &ov, 1000)) {
                        if (key == 0x1234) drained++;
                    }
                }

                if (drained != T30_BURST) {
                    LOG_PRINT("FAILED (Part C: Drained %lu/%d inproc burst completions)\n", drained, T30_BURST);
                    total_failures++;
                } else {
                    LOG_PRINT("PASSED (Zero-leak polling, instant server waiter wakeup %.2f ms, and 5000 burst inproc)\n", wake_ms);
                }
            }
            CloseHandle(t30_thread);
        }
        CloseHandle(port30);
    } else {
        LOG_PRINT("FAILED (CreateIoCompletionPort failed)\n");
        total_failures++;
    }

    // -------------------------------------------------------------------------
    // TEST 32: Bound FD Port In-Process Dequeue & WaitOnAddress Stress
    // -------------------------------------------------------------------------
    LOG_PRINT("[TEST 32] Bound FD Port In-Process Dequeue & WaitOnAddress Stress... ");
    do {
        // Part A: Bound FD Completion Port In-Process Dequeue
        HANDLE port32 = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (!port32) {
            LOG_PRINT("FAILED (CreateIoCompletionPort failed: %lu)\n", GetLastError());
            total_failures++;
            break;
        }

        char tmp_path[MAX_PATH];
        char tmp_file[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp_path);
        GetTempFileNameA(tmp_path, "iocp32", 0, tmp_file);
        HANDLE hFile32 = CreateFileA(tmp_file, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                     CREATE_ALWAYS, FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (hFile32 == INVALID_HANDLE_VALUE) {
            LOG_PRINT("FAILED (CreateFileA failed: %lu)\n", GetLastError());
            CloseHandle(port32);
            total_failures++;
            break;
        }

        // Bind hFile32 to port32 (sets has_bound_fd = 1)
        if (!CreateIoCompletionPort(hFile32, port32, 0x3232, 0)) {
            LOG_PRINT("FAILED (Binding file to completion port failed: %lu)\n", GetLastError());
            CloseHandle(hFile32);
            CloseHandle(port32);
            total_failures++;
            break;
        }

        // Post completion to port32
        if (!PostQueuedCompletionStatus(port32, 42, 0x3232, (LPOVERLAPPED)(uintptr_t)0xdead)) {
            LOG_PRINT("FAILED (PostQueuedCompletionStatus on bound FD port failed: %lu)\n", GetLastError());
            CloseHandle(hFile32);
            CloseHandle(port32);
            total_failures++;
            break;
        }

        // Dequeue completion
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = NULL;
        if (!GetQueuedCompletionStatus(port32, &bytes, &key, &ov, 1000) ||
            bytes != 42 || key != 0x3232 || ov != (LPOVERLAPPED)(uintptr_t)0xdead) {
            LOG_PRINT("FAILED (Part A: Dequeue completion mismatch: bytes=%lu, key=0x%llx, ov=%p)\n",
                      bytes, (unsigned long long)key, ov);
            CloseHandle(hFile32);
            CloseHandle(port32);
            total_failures++;
            break;
        }

        CloseHandle(hFile32);
        CloseHandle(port32);

        // Part B: Multi-threaded WaitOnAddress zero-deadlock stress
        HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
        pfnRtlWaitOnAddress pRtlWait = (pfnRtlWaitOnAddress)(void *)GetProcAddress(hNtdll, "RtlWaitOnAddress");
        pfnRtlWakeAddressSingle pRtlWakeSingle = (pfnRtlWakeAddressSingle)(void *)GetProcAddress(hNtdll, "RtlWakeAddressSingle");
        pfnRtlWakeAddressAll pRtlWakeAll = (pfnRtlWakeAddressAll)(void *)GetProcAddress(hNtdll, "RtlWakeAddressAll");
        if (!pRtlWait || !pRtlWakeSingle) {
            LOG_PRINT("FAILED (Part B: RtlWaitOnAddress / RtlWakeAddressSingle missing)\n");
            total_failures++;
            break;
        }

        volatile LONG test_var = 0;
        volatile LONG stop_flag = 0;
        #define NUM_T32_WOA_THREADS 4
        struct woa_test32_arg t32_args[NUM_T32_WOA_THREADS];
        HANDLE t32_threads[NUM_T32_WOA_THREADS];

        for (int i = 0; i < NUM_T32_WOA_THREADS; i++) {
            t32_args[i].var = &test_var;
            t32_args[i].stop = &stop_flag;
            t32_args[i].iterations = 0;
            t32_args[i].pRtlWait = pRtlWait;
            t32_args[i].pRtlWakeSingle = pRtlWakeSingle;
            t32_threads[i] = CreateThread(NULL, 0, woa_test32_worker, &t32_args[i], 0, NULL);
        }

        // Run for 200 ms
        Sleep(200);
        InterlockedExchange(&stop_flag, 1);
        InterlockedIncrement(&test_var);
        if (pRtlWakeAll) {
            pRtlWakeAll((const void *)&test_var);
        } else {
            for (int k = 0; k < NUM_T32_WOA_THREADS * 2; k++) pRtlWakeSingle((const void *)&test_var);
        }

        DWORD woa_wait = WaitForMultipleObjects(NUM_T32_WOA_THREADS, t32_threads, TRUE, 2000);
        for (int i = 0; i < NUM_T32_WOA_THREADS; i++) CloseHandle(t32_threads[i]);

        if (woa_wait == WAIT_TIMEOUT) {
            LOG_PRINT("FAILED (Part B: WaitOnAddress multi-threaded lost-wakeup deadlock!)\n");
            total_failures++;
        } else {
            LOG_PRINT("PASSED (Bound-FD in-process dequeue and 0-deadlock multi-threaded WaitOnAddress)\n");
        }
    } while (0);
    if (run_test_port_fallback_semantics() != 0) total_failures++;
    } /* end if (run_conformance) */

    if (run_repro) {
        LOG_PRINT("\n============================================================\n");
        LOG_PRINT("   Messiah Engine / IOCP Stall & Starvation Reproduction\n");
        LOG_PRINT("============================================================\n\n");

        if (run_test_sparse_dispatch() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_chained_jobsystem() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_dual_producer_pingpong() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_boundary_wakeup_regression() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_waitonaddress_wave_barrier() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_bound_fd_wave_storm() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_messiah_settle_pipeline() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_sleeping_worker_wakeup_distribution() != 0) total_failures++;
        LOG_PRINT("\n");
        if (run_test_contended_settle_pipeline() != 0) total_failures++;
    }

    if (run_settle) {
        if (run_test_messiah_settle_pipeline() != 0) total_failures++;
    }

    if (run_test33) {
        if (run_test_sleeping_worker_wakeup_distribution() != 0) total_failures++;
    }

    if (run_test34) {
        if (run_test_contended_settle_pipeline() != 0) total_failures++;
    }

    LOG_PRINT("\n============================================================\n");

    if (total_failures == 0) {
        LOG_PRINT("       ALL 33 CONFORMANCE, STRESS & BENCHMARK TESTS PASSED!\n");
    } else {
        LOG_PRINT("       TEST SUITE FINISHED WITH %d FAILED TESTS!\n", total_failures);
    }
    LOG_PRINT("============================================================\n");

    if (log_file) fclose(log_file);
    return total_failures > 0 ? 1 : 0;
}


