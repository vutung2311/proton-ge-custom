/* Click inside the Where Winds Meet window from within Wine, so the desktop compositor never
 * sees synthetic input (no KDE "Remote Control" prompt, no focus requirement, no risk of
 * hitting another application's window).
 *
 *   wine wwm_click.exe <fx> <fy>                 # click at a fraction of the game's client area
 *   wine wwm_click.exe --post <fx> <fy>          # post WM_LBUTTONDOWN/UP to the window instead
 *   wine wwm_click.exe --class <cls> [--post] <fx> <fy>
 *                                                # target a top-level window by class, e.g. the
 *                                                # NetEase login dialog MPAY_SWITCH_ACCOUNT
 *   wine wwm_click.exe --fake-active --post <fx> <fy>
 *                                                # first post WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS so
 *                                                # the game treats itself as active (desktop focus is
 *                                                # not touched), then post the click
 *   wine wwm_click.exe [--fake-active] --key <return|space|escape|e|f> 0 0
 *                                                # post a key press to the window instead of a click
 *
 * Run with the same runner's wine and WINEPREFIX as the game so it talks to the game's
 * wineserver. Exit code: 0 clicked, 2 usage, 3 game window not found.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -Wall -o wwm_click.exe wwm_click.c -luser32
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *cls = NULL, *key = NULL;
    int post = 0, fake_active = 0, i = 1;
    double fx, fy;
    HWND hwnd;
    RECT rc;
    POINT pt;

    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--post")) post = 1;
        else if (!strcmp(argv[i], "--class") && i + 1 < argc) cls = argv[++i];
        else if (!strcmp(argv[i], "--fake-active")) fake_active = 1;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else break;
    }
    if (argc - i != 2) {
        fprintf(stderr, "usage: wwm_click.exe [--class <cls>] [--post] <fx> <fy>\n");
        return 2;
    }
    fx = atof(argv[i]);
    fy = atof(argv[i + 1]);
    if (fx < 0 || fx > 1 || fy < 0 || fy > 1) {
        fprintf(stderr, "fx/fy must be fractions in [0,1]\n");
        return 2;
    }
    hwnd = cls ? FindWindowA(cls, NULL) : FindWindowA(NULL, "Where Winds Meet");
    if (!hwnd) {
        fprintf(stderr, "window %s not found\n", cls ? cls : "\"Where Winds Meet\"");
        return 3;
    }
    GetClientRect(hwnd, &rc);
    pt.x = (LONG)(rc.right * fx);
    pt.y = (LONG)(rc.bottom * fy);

    if (fake_active) {
        PostMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageA(hwnd, WM_NCACTIVATE, TRUE, 0);
        PostMessageA(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessageA(hwnd, WM_SETFOCUS, 0, 0);
        Sleep(100);
    }
    if (key) {
        static const struct { const char *name; UINT vk, scan; } keys[] = {
            { "return", VK_RETURN, 0x1C }, { "space", VK_SPACE, 0x39 }, { "escape", VK_ESCAPE, 0x01 },
            { "e", 'E', 0x12 }, { "f", 'F', 0x21 },
        };
        unsigned k;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            if (!_stricmp(key, keys[k].name)) {
                LPARAM down = 1 | (keys[k].scan << 16), up = down | (1u << 30) | (1u << 31);
                PostMessageA(hwnd, WM_KEYDOWN, keys[k].vk, down);
                if (keys[k].vk == VK_RETURN || keys[k].vk == VK_SPACE || keys[k].vk >= 'A')
                    PostMessageA(hwnd, WM_CHAR, keys[k].vk == VK_RETURN ? '\r' : keys[k].vk == VK_SPACE ? ' ' : keys[k].vk + 32, down);
                Sleep(60);
                PostMessageA(hwnd, WM_KEYUP, keys[k].vk, up);
                printf("posted key %s\n", keys[k].name);
                return 0;
            }
        }
        fprintf(stderr, "unknown key %s\n", key);
        return 2;
    }

    if (post) {
        LPARAM lp = MAKELPARAM(pt.x, pt.y);
        PostMessageA(hwnd, WM_MOUSEMOVE, 0, lp);
        Sleep(30);
        PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        Sleep(60);
        PostMessageA(hwnd, WM_LBUTTONUP, 0, lp);
        printf("posted click at client (%ld,%ld) of %ldx%ld\n", pt.x, pt.y, rc.right, rc.bottom);
        return 0;
    }

    ClientToScreen(hwnd, &pt);
    SetForegroundWindow(hwnd);
    {
        INPUT in[3];
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE;
        in[0].mi.dx = (LONG)(((pt.x - vx) * 65535.0) / (vw - 1));
        in[0].mi.dy = (LONG)(((pt.y - vy) * 65535.0) / (vh - 1));
        in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in[1] = in[0];
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in[2] = in[0];
        in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, &in[0], sizeof(INPUT));
        Sleep(40);
        SendInput(1, &in[1], sizeof(INPUT));
        Sleep(70);
        SendInput(1, &in[2], sizeof(INPUT));
    }
    printf("clicked screen (%ld,%ld)\n", pt.x, pt.y);
    return 0;
}
