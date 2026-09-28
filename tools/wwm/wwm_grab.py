#!/usr/bin/env python3
"""Capture the Where Winds Meet window's own contents without touching the desktop: no input, no
focus or stacking change. XWayland redirects every top-level window to an off-screen pixmap
(Composite), so the pixmap holds the game's last presented frame even when other windows cover it.
A minimized (unmapped) window has no pixmap.

  tools/wwm/wwm_grab.py out.png [--title "Where Winds Meet"]
Exit codes: 0 captured, 3 window not found, 4 window not viewable (minimized or unmapped).

As a module: Grabber(title).grab() returns a PIL RGB image, or raises NotFound / NotViewable.
"""
import argparse, sys
from Xlib import X, display
from Xlib.error import BadDrawable, BadMatch, BadWindow
from Xlib.ext import composite
from PIL import Image


class NotFound(Exception):
    pass


class NotViewable(Exception):
    pass


class Grabber:
    def __init__(self, title='Where Winds Meet'):
        self.title = title
        self.d = display.Display()
        if not self.d.has_extension('Composite'):
            raise RuntimeError('X server lacks the Composite extension')
        self.root = self.d.screen().root
        self.net_wm_name = self.d.intern_atom('_NET_WM_NAME')
        self.utf8 = self.d.intern_atom('UTF8_STRING')
        self.win = None

    def _title(self, w):
        p = w.get_full_property(self.net_wm_name, self.utf8)
        if p:
            return p.value.decode('utf-8', 'replace')
        n = w.get_wm_name()
        return n.decode('latin1') if isinstance(n, bytes) else n

    def _find(self, w):
        try:
            if self._title(w) == self.title:
                return w
            for c in w.query_tree().children:
                r = self._find(c)
                if r:
                    return r
        except (BadDrawable, BadWindow):
            pass  # window vanished while walking the tree
        return None

    def grab(self):
        """The window's client area as an RGB image (cached window lookup, re-found if it dies)."""
        for attempt in (0, 1):
            if self.win is None:
                self.win = self._find(self.root)
                if self.win is None:
                    raise NotFound(self.title)
            try:
                return self._grab(self.win)
            except (BadDrawable, BadWindow):
                self.win = None  # stale handle (window recreated); look it up once more
        raise NotFound(self.title)

    def _grab(self, win):
        if win.get_attributes().map_state != X.IsViewable:
            raise NotViewable(self.title)
        geo = win.get_geometry()
        # The top-level ancestor is the redirected window; read the client area out of its pixmap.
        top = win
        while True:
            parent = top.query_tree().parent
            if parent.id == self.root.id:
                break
            top = parent
        pix = composite.name_window_pixmap(top)
        try:
            off = top.translate_coords(win, 0, 0)  # (0,0) when the window is not reparented
            img = pix.get_image(off.x, off.y, geo.width, geo.height, X.ZPixmap, 0xffffffff)
        except BadMatch:
            raise NotViewable(self.title)
        finally:
            pix.free()
        return Image.frombytes('RGBX', (geo.width, geo.height), img.data, 'raw', 'BGRX').convert('RGB')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--title', default='Where Winds Meet')
    a = ap.parse_args()
    try:
        im = Grabber(a.title).grab()
    except NotFound:
        print(f'window "{a.title}" not found', file=sys.stderr)
        return 3
    except NotViewable:
        print(f'window "{a.title}" is not viewable (minimized or unmapped)', file=sys.stderr)
        return 4
    im.save(a.out)
    print(f'{a.out}: {im.width}x{im.height}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
