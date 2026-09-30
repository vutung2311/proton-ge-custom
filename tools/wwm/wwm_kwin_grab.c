/* Capture one window's own contents through KWin's ScreenShot2 D-Bus interface (KDE Plasma on
 * Wayland), without input, focus or stacking changes. Writes a binary PPM (P6) to stdout.
 *
 *   wwm_kwin_grab "Where Winds Meet" > frame.ppm
 *
 * The window is found by title through KWin's window runner (org.kde.krunner1 on /WindowsRunner),
 * which returns KWin's internal window id; the first match whose title is exactly the argument
 * wins. KWin only answers ScreenShot2 calls from executables named in a .desktop file with
 * X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2 (tools/wwm/wwm-kwin-grab.desktop).
 *
 * Exit codes: 0 captured, 2 usage or D-Bus failure, 3 window not found, 5 not authorized,
 * 6 unsupported pixel format.
 *
 * Build: cc -O2 -Wall -o wwm_kwin_grab wwm_kwin_grab.c $(pkg-config --cflags --libs gio-unix-2.0)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

/* QImage::Format values KWin can hand back for 8-bit captures. */
enum { QI_RGB32 = 4, QI_ARGB32 = 5, QI_ARGB32_PM = 6, QI_RGBX8888 = 16, QI_RGBA8888 = 17, QI_RGBA8888_PM = 18 };

static char *find_window(GDBusConnection *bus, const char *title)
{
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(bus, "org.kde.KWin", "/WindowsRunner", "org.kde.krunner1",
            "Match", g_variant_new("(s)", title), G_VARIANT_TYPE("(a(sssida{sv}))"),
            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    GVariantIter *it;
    const char *id, *text;
    char *found = NULL;

    if (!ret)
    {
        fprintf(stderr, "window lookup failed: %s\n", err->message);
        g_error_free(err);
        return NULL;
    }
    g_variant_get(ret, "(a(sssida{sv}))", &it);
    while (!found && g_variant_iter_next(it, "(&s&s&sid@a{sv})", &id, &text, NULL, NULL, NULL, NULL))
    {
        /* Ids are "<desktop>_<internal id>"; only exact titles, so "Where Winds Meet Launcher" or a
         * browser tab about the game never match. */
        const char *sep = strchr(id, '_');
        if (sep && !strcmp(text, title))
            found = g_strdup(sep + 1);
    }
    g_variant_iter_free(it);
    g_variant_unref(ret);
    return found;
}

static int read_all(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;

    while (got < len)
    {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    GError *err = NULL;
    GDBusConnection *bus;
    GUnixFDList *fds;
    GVariantBuilder opts;
    GVariant *ret, *meta;
    char *window;
    int pipefd[2], rc = 2;
    guint32 width = 0, height = 0, stride = 0, format = 0;
    uint8_t *pix = NULL, *row = NULL;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <exact window title>\n", argv[0]);
        return 2;
    }
    if (!(bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err)))
    {
        fprintf(stderr, "session bus: %s\n", err->message);
        g_error_free(err);
        return 2;
    }
    if (!(window = find_window(bus, argv[1])))
    {
        g_object_unref(bus);
        return 3;
    }
    if (pipe2(pipefd, O_CLOEXEC))
    {
        perror("pipe2");
        goto out_window;
    }

    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "include-decoration", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "include-cursor", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&opts, "{sv}", "native-resolution", g_variant_new_boolean(TRUE));
    fds = g_unix_fd_list_new_from_array(&pipefd[1], 1); /* takes ownership of the write end */
    ret = g_dbus_connection_call_with_unix_fd_list_sync(bus, "org.kde.KWin", "/org/kde/KWin/ScreenShot2",
            "org.kde.KWin.ScreenShot2", "CaptureWindow",
            g_variant_new("(sa{sv}h)", window, &opts, 0), G_VARIANT_TYPE("(a{sv})"),
            G_DBUS_CALL_FLAGS_NONE, 10000, fds, NULL, NULL, &err);
    g_object_unref(fds); /* closes our copy of the write end, so the read below sees EOF on error */
    if (!ret)
    {
        fprintf(stderr, "CaptureWindow: %s\n", err->message);
        rc = strstr(err->message, "NoAuthorized") || strstr(err->message, "not authorized") ? 5 : 2;
        g_error_free(err);
        goto out_pipe;
    }
    meta = g_variant_get_child_value(ret, 0);
    if (!g_variant_lookup(meta, "width", "u", &width) || !g_variant_lookup(meta, "height", "u", &height)
            || !g_variant_lookup(meta, "stride", "u", &stride) || !g_variant_lookup(meta, "format", "u", &format))
    {
        fprintf(stderr, "CaptureWindow: reply lacks width/height/stride/format\n");
        goto out_meta;
    }
    if (format != QI_RGB32 && format != QI_ARGB32 && format != QI_ARGB32_PM && format != QI_RGBX8888
            && format != QI_RGBA8888 && format != QI_RGBA8888_PM)
    {
        fprintf(stderr, "unsupported QImage format %u\n", format);
        rc = 6;
        goto out_meta;
    }
    if (!width || !height || stride < width * 4 || (size_t)stride * height > (size_t)1 << 30)
    {
        fprintf(stderr, "bad image geometry %ux%u stride %u\n", width, height, stride);
        goto out_meta;
    }
    if (!(pix = malloc((size_t)stride * height)) || !(row = malloc((size_t)width * 3)))
    {
        fprintf(stderr, "out of memory\n");
        goto out_meta;
    }
    if (read_all(pipefd[0], pix, (size_t)stride * height))
    {
        fprintf(stderr, "short image data\n");
        goto out_meta;
    }

    printf("P6\n%u %u\n255\n", width, height);
    for (guint32 y = 0; y < height; y++)
    {
        const uint8_t *s = pix + (size_t)y * stride;
        for (guint32 x = 0; x < width; x++, s += 4)
        {
            /* 32-bit QImage formats are 0xAARRGGBB words (B, G, R, A in memory); the 8888 ones
             * are bytes R, G, B, A. Game frames are opaque, so premultiplication does not matter. */
            int bgra = format == QI_RGB32 || format == QI_ARGB32 || format == QI_ARGB32_PM;
            row[x * 3 + 0] = bgra ? s[2] : s[0];
            row[x * 3 + 1] = s[1];
            row[x * 3 + 2] = bgra ? s[0] : s[2];
        }
        if (fwrite(row, 3, width, stdout) != width)
        {
            perror("write");
            goto out_meta;
        }
    }
    rc = fflush(stdout) ? 2 : 0;

out_meta:
    free(row);
    free(pix);
    g_variant_unref(meta);
    g_variant_unref(ret);
out_pipe:
    close(pipefd[0]);
out_window:
    g_free(window);
    g_object_unref(bus);
    return rc;
}
