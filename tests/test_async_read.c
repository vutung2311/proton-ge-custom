/* File I/O completion paths and timers that Where Winds Meet's streaming may depend on, measured
 * the same way under two Wine builds. Reads are 64 KB, one outstanding at a time, from a file that
 * is already in the page cache, so the numbers are Wine's completion path, not the disk.
 *
 *   test_async_read.exe <file> [count]
 *
 * Latency modes (also count reads that completed inline, i.e. ReadFile returned TRUE):
 *   sync      ReadFile on a plain handle
 *   ovl+event FILE_FLAG_OVERLAPPED, wait on the OVERLAPPED event (infinite)
 *   ovl+poll  FILE_FLAG_OVERLAPPED, poll the event with a 100 ms timeout (the game's pattern)
 *   iocp      FILE_FLAG_OVERLAPPED bound to a completion port, GetQueuedCompletionStatus
 *   iocp+ex   same, GetQueuedCompletionStatusEx
 * Completion-notification semantics (SetFileCompletionNotificationModes, which kernelbase implements
 * with NtSetInformationFile(FileIoCompletionNotificationInformation); the game imports the latter):
 *   skip      FILE_SKIP_COMPLETION_PORT_ON_SUCCESS: an inline success must queue no packet, a pending
 *             read must still queue one
 *   skip+ev   plus FILE_SKIP_SET_EVENT_ON_HANDLE: the file handle must not be signalled either
 *   noskip    no modes: every read, inline or pending, must queue exactly one packet
 * Timers (the game imports CreateThreadpoolTimer/SetThreadpoolTimer and SetWaitableTimerEx):
 *   tp-timer  thread-pool timer, periods 16/100/800/1000 ms, window 0 and 50 ms
 *   wt-timer  periodic waitable timer (SetWaitableTimerEx), periods 100/800 ms
 * Output goes to C:\test_async_read.log. */
#include <windows.h>
#include <stdio.h>

#define CHUNK 65536

static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static ULONGLONG chunk_offset(int i, LARGE_INTEGER size)
{
    ULONGLONG off = ((ULONGLONG)i * CHUNK) % (size.QuadPart - CHUNK);
    return off & ~(ULONGLONG)(CHUNK - 1);
}

static void run(FILE *log, const char *name, const char *path, int count, int mode)
{
    static char buf[CHUNK];
    DWORD flags = mode ? FILE_FLAG_OVERLAPPED : 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, flags, NULL);
    HANDLE port = NULL, ev = NULL;
    LARGE_INTEGER size;
    double t0, t1, worst = 0;
    int i, ok = 0, inl = 0;

    if (h == INVALID_HANDLE_VALUE) { fprintf(log, "%-10s open failed %lu\n", name, GetLastError()); return; }
    GetFileSizeEx(h, &size);
    if (mode >= 3)
    {
        port = CreateIoCompletionPort(h, NULL, 1, 0);
        if (!port) { fprintf(log, "%-10s CreateIoCompletionPort failed %lu\n", name, GetLastError()); CloseHandle(h); return; }
    }
    else if (mode) ev = CreateEventA(NULL, TRUE, FALSE, NULL);

    t0 = now_ms();
    for (i = 0; i < count; i++)
    {
        OVERLAPPED ovl = {0};
        ULONGLONG off = chunk_offset(i, size);
        DWORD got = 0;
        double s = now_ms();
        BOOL done;

        ovl.Offset = (DWORD)off;
        ovl.OffsetHigh = (DWORD)(off >> 32);
        ovl.hEvent = ev;
        if (mode == 0)
        {
            LARGE_INTEGER li; li.QuadPart = off;
            SetFilePointerEx(h, li, NULL, FILE_BEGIN);
            done = ReadFile(h, buf, CHUNK, &got, NULL);
        }
        else
        {
            done = ReadFile(h, buf, CHUNK, &got, &ovl);
            if (done) inl++;
            else if (GetLastError() != ERROR_IO_PENDING) { fprintf(log, "%-10s ReadFile failed %lu\n", name, GetLastError()); break; }
            if (mode == 1) done = GetOverlappedResult(h, &ovl, &got, TRUE);
            else if (mode == 2)
            {
                while (!HasOverlappedIoCompleted(&ovl)) WaitForSingleObject(ev, 100);
                done = GetOverlappedResult(h, &ovl, &got, FALSE);
            }
            else if (mode == 3)
            {
                ULONG_PTR key; LPOVERLAPPED p;
                done = GetQueuedCompletionStatus(port, &got, &key, &p, INFINITE);
            }
            else
            {
                OVERLAPPED_ENTRY e; ULONG n;
                done = GetQueuedCompletionStatusEx(port, &e, 1, &n, INFINITE, FALSE);
                got = e.dwNumberOfBytesTransferred;
            }
        }
        if (done && got == CHUNK) ok++;
        s = now_ms() - s;
        if (s > worst) worst = s;
    }
    t1 = now_ms();
    fprintf(log, "%-10s %6d reads ok=%-6d %8.1f ms total  %7.3f ms/read  worst %7.2f ms  %6.0f MB/s  inline %d\n",
            name, count, ok, t1 - t0, (t1 - t0) / count, worst, ok * (CHUNK / 1048576.0) / ((t1 - t0) / 1000.0), inl);
    if (port) CloseHandle(port);
    if (ev) CloseHandle(ev);
    CloseHandle(h);
}

/* Completion-notification semantics. For every read: did ReadFile complete inline, was a packet
 * queued for it (looked for up to 20 ms after an inline success, up to 2 s for a pending read), and
 * was the file handle signalled. The expected counts follow from the modes (see the header). */
static void run_skip(FILE *log, const char *name, const char *path, int count, UCHAR modes)
{
    static char buf[CHUNK];
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    HANDLE port;
    LARGE_INTEGER size;
    int i, inl = 0, pend = 0, inl_pkt = 0, pend_nopkt = 0, sig = 0, err = 0, badpkt = 0;

    if (h == INVALID_HANDLE_VALUE) { fprintf(log, "%-10s open failed %lu\n", name, GetLastError()); return; }
    GetFileSizeEx(h, &size);
    port = CreateIoCompletionPort(h, NULL, 7, 0);
    if (!port) { fprintf(log, "%-10s CreateIoCompletionPort failed %lu\n", name, GetLastError()); CloseHandle(h); return; }
    if (modes && !SetFileCompletionNotificationModes(h, modes))
    {
        fprintf(log, "%-10s SetFileCompletionNotificationModes failed %lu\n", name, GetLastError());
        CloseHandle(port); CloseHandle(h); return;
    }
    for (i = 0; i < count; i++)
    {
        OVERLAPPED ovl = {0};
        ULONGLONG off = chunk_offset(i, size);
        DWORD got;
        ULONG_PTR key;
        LPOVERLAPPED p;
        BOOL r;

        ovl.Offset = (DWORD)off;
        ovl.OffsetHigh = (DWORD)(off >> 32);
        r = ReadFile(h, buf, CHUNK, NULL, &ovl);
        if (r)
        {
            inl++;
            if (GetQueuedCompletionStatus(port, &got, &key, &p, 20))
            {
                inl_pkt++;
                if (p != &ovl || key != 7) badpkt++;
            }
        }
        else if (GetLastError() == ERROR_IO_PENDING)
        {
            pend++;
            if (!GetQueuedCompletionStatus(port, &got, &key, &p, 2000)) pend_nopkt++;
            else if (p != &ovl || key != 7 || got != CHUNK) badpkt++;
        }
        else err++;
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) sig++;
    }
    fprintf(log, "%-10s %6d reads: inline %d (packet queued anyway %d), pending %d (no packet %d), "
            "handle signalled %d, wrong packets %d, errors %d\n",
            name, count, inl, inl_pkt, pend, pend_nopkt, sig, badpkt, err);
    CloseHandle(port);
    CloseHandle(h);
}

#define MAXFIRE 256
static volatile LONG tp_n;
static double tp_t[MAXFIRE];

static VOID CALLBACK tp_cb(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_TIMER timer)
{
    LONG n = InterlockedIncrement(&tp_n);
    if (n <= MAXFIRE) tp_t[n - 1] = now_ms();
}

static void report_intervals(FILE *log, const char *name, DWORD period, DWORD window, double start, int n)
{
    double sum = 0, mn = 1e9, mx = 0;
    int i;
    if (n > MAXFIRE) n = MAXFIRE;
    for (i = 1; i < n; i++)
    {
        double d = tp_t[i] - tp_t[i - 1];
        sum += d;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
    }
    if (n < 2) fprintf(log, "%-10s period %4lu window %3lu: only %d firings\n", name, period, window, n);
    else fprintf(log, "%-10s period %4lu window %3lu: %3d firings, first after %7.1f ms, interval mean %7.2f min %7.2f max %7.2f ms\n",
                 name, period, window, n, tp_t[0] - start, sum / (n - 1), mn, mx);
}

static void run_tptimer(FILE *log, DWORD period, DWORD window, DWORD span)
{
    PTP_TIMER t = CreateThreadpoolTimer(tp_cb, NULL, NULL);
    FILETIME due;
    ULARGE_INTEGER u;
    double start;

    if (!t) { fprintf(log, "tp-timer   CreateThreadpoolTimer failed %lu\n", GetLastError()); return; }
    tp_n = 0;
    u.QuadPart = (ULONGLONG)(-(LONGLONG)100 * 10000);   /* first firing 100 ms from now */
    due.dwLowDateTime = u.LowPart;
    due.dwHighDateTime = u.HighPart;
    start = now_ms();
    SetThreadpoolTimer(t, &due, period, window);
    Sleep(span);
    SetThreadpoolTimer(t, NULL, 0, 0);
    WaitForThreadpoolTimerCallbacks(t, TRUE);
    CloseThreadpoolTimer(t);
    report_intervals(log, "tp-timer", period, window, start, tp_n);
}

static void run_wttimer(FILE *log, LONG period, DWORD span)
{
    HANDLE t = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    double start, end;
    int n = 0;

    if (!t) { fprintf(log, "wt-timer   CreateWaitableTimerExW failed %lu\n", GetLastError()); return; }
    due.QuadPart = -100LL * 10000;
    start = now_ms();
    if (!SetWaitableTimerEx(t, &due, period, NULL, NULL, NULL, 0))
    {
        fprintf(log, "wt-timer   SetWaitableTimerEx failed %lu\n", GetLastError());
        CloseHandle(t);
        return;
    }
    end = start + span;
    while (now_ms() < end && n < MAXFIRE)
    {
        if (WaitForSingleObject(t, (DWORD)(end - now_ms()) + 1) != WAIT_OBJECT_0) break;
        tp_t[n++] = now_ms();
    }
    CancelWaitableTimer(t);
    CloseHandle(t);
    report_intervals(log, "wt-timer", period, 0, start, n);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    int count = argc > 2 ? atoi(argv[2]) : 5000;
    int skip_count = count < 2000 ? count : 2000;
    FILE *log = fopen("C:\\test_async_read.log", "w");
    if (!log || !path) return 1;
    fprintf(log, "file %s, %d x %d KB reads, one outstanding\n", path, count, CHUNK / 1024);
    run(log, "sync",      path, count, 0);
    run(log, "ovl+event", path, count, 1);
    run(log, "ovl+poll",  path, count, 2);
    run(log, "iocp",      path, count, 3);
    run(log, "iocp+ex",   path, count, 4);
    run_skip(log, "noskip",  path, skip_count, 0);
    run_skip(log, "skip",    path, skip_count, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
    run_skip(log, "skip+ev", path, skip_count, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
    fflush(log);
    run_tptimer(log, 16, 0, 1100);
    run_tptimer(log, 100, 0, 2100);
    run_tptimer(log, 800, 0, 6600);
    run_tptimer(log, 800, 50, 6600);
    run_tptimer(log, 1000, 0, 6100);
    run_wttimer(log, 100, 2100);
    run_wttimer(log, 800, 6600);
    fclose(log);
    return 0;
}
