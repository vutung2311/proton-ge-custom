/*
 * Waitable-timer latency probe.
 *
 * Reproduces how Boost.Asio's win_iocp_io_context drives timers: one thread sits
 * in WaitForSingleObject() on an auto-reset waitable timer while another thread
 * re-arms it with SetWaitableTimer(relative due time, period = 5 min).
 *
 * Where Winds Meet's Asio timer thread fired every ~1 ms on GE-Proton10-34 but
 * every ~10 ms on GE-Proton11. This measures, per runner, how long a timer armed
 * for N ms really takes to fire.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -Wall tests/test_waitable_timer.c -o tests/test_waitable_timer.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 400
#define ASIO_MAX_TIMEOUT_MSEC (5 * 60 * 1000)

static LARGE_INTEGER freq;
static FILE *out;

static double now_us( void )
{
    LARGE_INTEGER t;
    QueryPerformanceCounter( &t );
    return (double)t.QuadPart * 1e6 / (double)freq.QuadPart;
}

static int cmp_double( const void *a, const void *b )
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void report( const char *name, double *v, int n )
{
    double sum = 0;
    int i;
    for (i = 0; i < n; i++) sum += v[i];
    qsort( v, n, sizeof(*v), cmp_double );
    fprintf( out, "%-44s mean=%9.1f us  p10=%9.1f  median=%9.1f  p90=%9.1f  max=%9.1f\n",
             name, sum / n, v[n / 10], v[n / 2], v[n * 9 / 10], v[n - 1] );
    fflush( out );
}

/* A: same thread arms and waits. */
static void same_thread( LONG due_ms )
{
    static double lat[ITERS];
    char name[64];
    HANDLE timer = CreateWaitableTimerW( NULL, FALSE, NULL );
    LARGE_INTEGER due;
    int i;

    due.QuadPart = -(LONGLONG)due_ms * 10000;
    for (i = 0; i < ITERS; i++)
    {
        double t0 = now_us();
        SetWaitableTimer( timer, &due, ASIO_MAX_TIMEOUT_MSEC, NULL, NULL, FALSE );
        WaitForSingleObject( timer, INFINITE );
        lat[i] = now_us() - t0;
    }
    CloseHandle( timer );
    snprintf( name, sizeof(name), "A same-thread arm+wait, due %ld ms", due_ms );
    report( name, lat, ITERS );
}

/* B: Asio pattern -- a dedicated timer thread is already blocked on the timer
 * when another thread re-arms it. */
static HANDLE b_timer, b_fired;
static volatile double b_fire_us;
static volatile LONG b_stop;

static DWORD WINAPI timer_thread( void *arg )
{
    (void)arg;
    while (!b_stop)
    {
        if (WaitForSingleObject( b_timer, INFINITE ) != WAIT_OBJECT_0) break;
        b_fire_us = now_us();
        SetEvent( b_fired );
    }
    return 0;
}

static void cross_thread( LONG due_ms )
{
    static double lat[ITERS];
    char name[64];
    LARGE_INTEGER due;
    HANDLE thread;
    int i;

    b_timer = CreateWaitableTimerW( NULL, FALSE, NULL );
    b_fired = CreateEventW( NULL, FALSE, FALSE, NULL );
    b_stop = 0;
    thread = CreateThread( NULL, 0, timer_thread, NULL, 0, NULL );
    Sleep( 50 );  /* let the timer thread block first, as Asio's does */

    due.QuadPart = -(LONGLONG)due_ms * 10000;
    for (i = 0; i < ITERS; i++)
    {
        double t0 = now_us();
        SetWaitableTimer( b_timer, &due, ASIO_MAX_TIMEOUT_MSEC, NULL, NULL, FALSE );
        WaitForSingleObject( b_fired, INFINITE );
        lat[i] = b_fire_us - t0;
    }

    b_stop = 1;
    due.QuadPart = -1;
    SetWaitableTimer( b_timer, &due, 0, NULL, NULL, FALSE );
    WaitForSingleObject( thread, INFINITE );
    CloseHandle( thread );
    CloseHandle( b_fired );
    CloseHandle( b_timer );
    snprintf( name, sizeof(name), "B cross-thread (Asio), due %ld ms", due_ms );
    report( name, lat, ITERS );
}

/* C: Sleep() reference. */
static void sleep_ref( DWORD ms )
{
    static double lat[ITERS];
    char name[64];
    int i;

    for (i = 0; i < ITERS; i++)
    {
        double t0 = now_us();
        Sleep( ms );
        lat[i] = now_us() - t0;
    }
    snprintf( name, sizeof(name), "C Sleep(%lu)", ms );
    report( name, lat, ITERS );
}

int main( int argc, char **argv )
{
    const char *log_path = argc > 1 ? argv[1] : "C:\\test_waitable_timer.log";

    if (!(out = fopen( log_path, "w" ))) out = stdout;
    QueryPerformanceFrequency( &freq );

    same_thread( 1 );
    same_thread( 2 );
    same_thread( 5 );
    cross_thread( 1 );
    cross_thread( 2 );
    cross_thread( 5 );
    sleep_ref( 1 );

    if (out != stdout) fclose( out );
    return 0;
}
