/* List every top-level window (and first-level children of visible ones) with owning process,
 * class, title, rectangle and visibility, as Wine sees them. Used to find which window hosts
 * the NetEase login dialog in Where Winds Meet.
 * Build: x86_64-w64-mingw32-gcc -O2 -Wall -o wwm_windows.exe wwm_windows.c -luser32 */
#include <windows.h>
#include <stdio.h>

static void show(HWND h, int depth)
{
    char cls[128] = "", title[256] = "";
    RECT r;
    DWORD pid = 0;
    GetClassNameA(h, cls, sizeof(cls));
    GetWindowTextA(h, title, sizeof(title));
    GetWindowRect(h, &r);
    GetWindowThreadProcessId(h, &pid);
    printf("%*s%p pid %lu vis %d [%ld,%ld %ldx%ld] class '%s' title '%s'\n", depth * 2, "", (void *)h, pid,
           IsWindowVisible(h), r.left, r.top, r.right - r.left, r.bottom - r.top, cls, title);
}

static BOOL CALLBACK child(HWND h, LPARAM depth)
{
    if (GetParent(h) == (HWND)((LPARAM *)depth)[1]) show(h, 1);
    return TRUE;
}

static BOOL CALLBACK top(HWND h, LPARAM unused)
{
    LPARAM ctx[2];
    RECT r;
    GetWindowRect(h, &r);
    if (!IsWindowVisible(h) && (r.right - r.left) < 100) return TRUE;
    show(h, 0);
    if (IsWindowVisible(h)) {
        ctx[0] = 1;
        ctx[1] = (LPARAM)h;
        EnumChildWindows(h, child, (LPARAM)ctx);
    }
    return TRUE;
}

int main(void)
{
    POINT pt;
    HWND fg = GetForegroundWindow();
    GetCursorPos(&pt);
    printf("foreground %p, cursor (%ld,%ld), window at cursor %p\n", (void *)fg, pt.x, pt.y, (void *)WindowFromPoint(pt));
    EnumWindows(top, 0);
    return 0;
}
