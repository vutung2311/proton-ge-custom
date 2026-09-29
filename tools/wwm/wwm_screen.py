#!/usr/bin/env python3
"""Screen state of Where Winds Meet from its window contents (tools/wwm/wwm_grab.py: read-only, no
focus change), read by OCR of a few fixed regions. Used by wwm_ab_run.sh AUTO=1 to confirm what
the game shows, catch dialogs and disconnects, and time the moment the world HUD appears.

  wwm_screen.py state                         one full check now: prints "<state> | <text>"
  wwm_screen.py locate <label> [--pick lowest] [--image f]
                                              where a text label (a button) is now: prints
                                              "<fx> <fy> <conf> <pass>" as client-area fractions,
                                              exit 1 when it is not on screen
  wwm_screen.py record <dir> [--interval 1]   until the window is gone or SIGTERM: every interval
                                              save <dir>/<epoch>.jpg (1280 wide) and append a line
                                              to <dir>/states.tsv
  wwm_screen.py progress <dir> <teleport_start>
                                              times the loading percentage first showed each value
  wwm_screen.py scan <dir> <teleport_start>   from states.tsv: world_visible time (first world-HUD
                                              frame not followed by the loading overlay) and every
                                              dialog seen after the load started

States (priority order): disconnect, already_online, login_screen, world (HUD labels visible),
loading (a percentage in the progress bar region), unknown. OCR text is normalised to lowercase letters and
digits without spaces before matching, because tesseract splits and garbles words ("HideUI",
"Ểummon Mount").
Regions are fractions of the client area, measured on 2560x1440 frames.
"""
import argparse, io, os, re, signal, subprocess, sys, time, unicodedata
from PIL import Image, ImageOps
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wwm_grab import Grabber, NotFound, NotViewable

REGIONS = {
    'hud': (0.80, 0.38, 1.00, 0.56),      # right-side labels: Summon Mount / Appearance / Hide UI / Photo
    'center': (0.25, 0.30, 0.75, 0.70),   # modal dialogs
    'bottom': (0.00, 0.85, 1.00, 1.00),   # loading progress, chat hint, player id
    'progress': (0.40, 0.925, 0.60, 0.975),  # just the "Loading Data... NN%" line (0.12 s to read)
}
# Right-side HUD labels, each matched by any of its fragments (OCR drops or garbles letters:
# "Appearan", "hoto", "peara"); two different labels mean the world HUD is up.
HUD_LABELS = (('summon', 'mount'), ('appear', 'earan', 'peara'), ('hideui', 'hide'), ('photo', 'hoto', 'phot'))
DIALOGS = {
    'disconnect': ('disconnect', 'reconnect', 'networkerror', 'connectionlost', 'lostconnection',
                   'anotherdevice', 'loggedinelsewhere', 'kicked', 'timedout', 'returntologin',
                   'relogin', 'maintenance'),
    'already_online': ('alreadyonline', 'continuelogin'),
    'login_screen': ('startgame', 'switchaccount', 'selectserver'),
}
PERCENT = re.compile(r'(\d{1,3})\s?%')
OCR_ENV = dict(os.environ, OMP_THREAD_LIMIT='1')   # one core: do not compete with the game


def ocr(im, region):
    w, h = im.size
    x0, y0, x1, y1 = REGIONS[region]
    crop = ImageOps.grayscale(im.crop((int(x0 * w), int(y0 * h), int(x1 * w), int(y1 * h))))
    buf = io.BytesIO()
    crop.save(buf, 'PNG')
    r = subprocess.run(['tesseract', 'stdin', 'stdout', '-l', 'vie', '--psm', '11'], input=buf.getvalue(),
                       capture_output=True, env=OCR_ENV, check=True)
    return ' '.join(r.stdout.decode('utf-8', 'replace').split())


def norm(text):
    # Vietnamese-model OCR adds diacritics to English words ("Súmmon"): fold them away first.
    text = unicodedata.normalize('NFKD', text)
    text = ''.join(c for c in text if not unicodedata.combining(c))
    return re.sub(r'[^a-z0-9%]', '', text.lower())


def classify(texts):
    """texts: region -> raw OCR text (missing regions were not read). Returns the state name."""
    n = {k: norm(v) for k, v in texts.items()}
    joined = n.get('center', '') + n.get('bottom', '')
    for state, words in DIALOGS.items():
        if any(w in joined for w in words):
            return state
    hud = n.get('hud', '')
    # a label read together with its key is specific to the world HUD: "Hide UI F6" loses its H on
    # dark scenes, bright sky garbles it while "Appearance F5" survives
    if sum(any(w in hud for w in label) for label in HUD_LABELS) >= 2 or 'deuif6' in hud or 'rancef5' in hud:
        return 'world'
    # only the progress bar region: the bottom band's player id and FPS overlay OCR into stray "N %"
    if PERCENT.search(texts.get('progress', '')):
        return 'loading'
    return 'unknown'


def cmd_state(_):
    try:
        im = Grabber().grab()
    except NotFound:
        print('no_window |')
        return 3
    except NotViewable:
        print('unviewable |')
        return 4
    texts = {r: ocr(im, r) for r in REGIONS}
    print(f"{classify(texts)} | " + ' || '.join(f'{r}: {t}' for r, t in texts.items()))
    return 0


def cmd_record(a):
    os.makedirs(a.dir, exist_ok=True)
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    g = Grabber()
    missing_since = None
    seen = False          # "gone" only counts once the window has existed (the launcher has none)
    started = time.time()
    i = 0
    with open(os.path.join(a.dir, 'states.tsv'), 'a', buffering=1) as out:
        while not stop:
            t = time.time()
            try:
                im = g.grab()
                missing_since = None
                seen = True
            except NotFound:
                missing_since = missing_since or t
                if seen and t - missing_since > a.gone_after:
                    break  # the game has exited
                if not seen and t - started > a.appear_within:
                    print(f'window never appeared within {a.appear_within:.0f} s', file=sys.stderr)
                    return 3
                out.write(f'{t:.3f}\tno_window\t\n')
                time.sleep(a.interval)
                continue
            except NotViewable:
                out.write(f'{t:.3f}\tunviewable\t\n')
                time.sleep(a.interval)
                continue
            # Every frame reads the HUD and the loading percentage; the dialog and bottom bands are
            # read every third frame.
            texts = {'hud': ocr(im, 'hud'), 'progress': ocr(im, 'progress')}
            if i % 3 == 0:
                texts['center'] = ocr(im, 'center')
                texts['bottom'] = ocr(im, 'bottom')
            state = classify(texts)
            im.resize((1280, im.height * 1280 // im.width)).save(os.path.join(a.dir, f'{t:.3f}.jpg'), quality=80)
            out.write(f'{t:.3f}\t{state}\t' + ' || '.join(f'{r}: {x}' for r, x in texts.items()) + '\n')
            i += 1
            time.sleep(max(0.0, a.interval - (time.time() - t)))
    return 0


def progress_steps(path, t0):
    """[(percent, first epoch it was read, epoch of the previous frame whose percentage was read)]."""
    steps, prev_t, last = [], None, None
    for line in open(path):
        f = line.rstrip('\n').split('\t')
        if len(f) < 3 or f[1] in ('no_window', 'unviewable'):
            continue
        t = float(f[0])
        texts = dict(part.split(': ', 1) for part in f[2].split(' || ') if ': ' in part)
        m = re.search(r'Loading Data[^0-9]{0,8}(\d{1,3})\s?%', texts.get('progress', '') + ' ' + texts.get('bottom', ''))
        if m and t >= t0 - 5:
            pct = int(m.group(1))
            if pct <= 100 and (last is None or pct > last):
                steps.append((pct, t, prev_t))
                last = pct
        if m:
            prev_t = t  # the step happened after the last frame whose percentage was read
    return steps


def cmd_progress(a):
    for pct, t, prev in progress_steps(os.path.join(a.dir, 'states.tsv'), a.teleport_start):
        gap = f' (previous frame {prev - a.teleport_start:+.2f}s)' if prev else ''
        print(f'progress {pct} {t:.3f} {t - a.teleport_start:+.2f}s{gap}')
    return 0


def cmd_scan(a):
    rows = []
    for line in open(os.path.join(a.dir, 'states.tsv')):
        f = line.rstrip('\n').split('\t')
        txt = f[2] if len(f) > 2 else ''
        state = f[1]
        if txt:
            # re-classify from the stored OCR text, so classifier fixes apply to old recordings
            texts = dict(part.split(': ', 1) for part in txt.split(' || ') if ': ' in part)
            state = classify(texts)
        rows.append((float(f[0]), state, txt))
    rows.sort()
    t0 = a.teleport_start
    after = [r for r in rows if r[0] >= t0]
    # confirmed HUD: the first HUD frame after which the loading overlay does not come back for
    # --settle seconds and the HUD is read at least once more (OCR misses the HUD on some frames,
    # so consecutive-frame rules are too strict).
    wv = None
    for k, (t, s, _) in enumerate(after):
        if s != 'world':
            continue
        nxt = [r for r in after[k + 1:] if r[0] - t <= a.settle]
        if any(r[1] == 'loading' for r in nxt):
            continue
        if any(r[1] == 'world' for r in nxt):
            wv = t
            break
    # The loading bar OCRs reliably, the HUD labels do not (dark scenes): report the first frame
    # after the last loading-bar frame before the confirmed HUD, when only world/unknown frames
    # lie in between. Frames are one interval apart, so the overlay went away at most one
    # interval before that frame.
    if wv is not None:
        before = [r for r in after if r[0] <= wv]
        last_loading = max((k for k, r in enumerate(before) if r[1] == 'loading'), default=None)
        wv_overlay = wv
        if last_loading is not None and all(r[1] in ('world', 'unknown') for r in before[last_loading + 1:]):
            wv_overlay = before[last_loading + 1][0]
        print(f'world_visible {wv_overlay:.3f}')
        print(f'world_hud {wv:.3f}')
    else:
        print('world_visible none')
    seen = {}
    for t, s, txt in after:
        if s in DIALOGS and s not in seen:
            seen[s] = (t, txt)
    for s, (t, txt) in seen.items():
        print(f'dialog {s} {t:.3f} {t - t0:+.1f}s {txt[:200]}')
    # compact state timeline, one entry per change
    prev = None
    line = []
    for t, s, _ in after:
        if s != prev:
            line.append(f'{t - t0:+.1f}s {s}')
            prev = s
    print('timeline ' + ', '.join(line))
    return 0


def ocr_words(img, psm, may_crash=False):
    """Words tesseract reads in img: (text, conf, x, y, w, h, line key) in img pixels.

    may_crash: tesseract dies with SIGFPE on some noise crops (rain streaks read as a "line");
    for such a crop return None instead of raising, so one unreadable line does not end a search."""
    buf = io.BytesIO()
    img.save(buf, 'PNG')
    r = subprocess.run(['tesseract', 'stdin', 'stdout', '-l', 'vie', '--psm', str(psm), 'tsv'],
                       input=buf.getvalue(), capture_output=True, env=OCR_ENV)
    if r.returncode != 0:
        if may_crash and r.returncode < 0:
            print(f'tesseract killed by signal {-r.returncode} on a {img.width}x{img.height} line crop; skipped',
                  file=sys.stderr)
            return None
        raise subprocess.CalledProcessError(r.returncode, r.args, r.stdout, r.stderr)
    words = []
    for line in r.stdout.decode('utf-8', 'replace').splitlines()[1:]:
        f = line.split('\t')
        if len(f) < 12 or not f[11].strip():
            continue
        x, y, w, h = map(int, f[6:10])
        words.append((f[11], float(f[10]), x, y, w, h, (f[2], f[3], f[4])))
    return words


def find_label(im, label, min_conf, pick='best'):
    """Centre of the on-screen text label as client-area fractions (fx, fy, conf, pass), or None.

    pick: 'best' (highest confidence) or 'lowest' (largest fy) when the text occurs more than once,
    e.g. "Continue" in both the message and the button hint of the already-online dialog. Both
    passes run for 'lowest', so a small hint found only by pass 2 still counts.

    Pass 1 reads the whole window as sparse text. Small labels next to key badges ("Space
    Continue") are often missed there, so pass 2 re-reads each text line pass 1 found, cropped
    wide around it and scaled up, as a single line. Positions scale with the window, so this
    holds for any resolution, aspect ratio or UI scale."""
    target = norm(label)
    w, h = im.size
    gray = ImageOps.grayscale(im)
    s1 = max(1.0, 1440 / h)   # read at >= 1440 lines, where the menu text is ~20 px high
    base = gray.resize((round(w * s1), round(h * s1)), Image.LANCZOS) if s1 > 1 else gray
    found = []

    def consider(text, conf, cx, cy, how):
        if conf >= min_conf and target and target in norm(text):
            found.append((cx / w, cy / h, conf, how))

    def choose():
        if not found:
            return None
        return max(found, key=(lambda f: f[2]) if pick == 'best' else (lambda f: f[1]))

    words = ocr_words(base, 11)
    for text, conf, x, y, ww, hh, _ in words:
        consider(text, conf, (x + ww / 2) / s1, (y + hh / 2) / s1, 'sparse')
    if found and pick == 'best':
        return choose()
    lines = {}
    for text, conf, x, y, ww, hh, key in words:
        lines.setdefault(key, []).append((x / s1, y / s1, (x + ww) / s1, (y + hh) / s1))
    for boxes in lines.values():
        x0 = min(b[0] for b in boxes); y0 = min(b[1] for b in boxes)
        x1 = max(b[2] for b in boxes); y1 = max(b[3] for b in boxes)
        lh = max(1.0, y1 - y0)
        cx0, cx1 = max(0, x0 - 0.10 * w), min(w, x1 + 0.15 * w)
        cy0, cy1 = max(0, y0 - lh), min(h, y1 + lh)
        s2 = max(1.0, 60 / lh)   # text ~60 px high for the single-line pass
        crop = gray.crop((int(cx0), int(cy0), int(cx1), int(cy1)))
        crop = crop.resize((max(1, round(crop.width * s2)), max(1, round(crop.height * s2))), Image.LANCZOS)
        for text, conf, x, y, ww, hh, _ in ocr_words(crop, 7, may_crash=True) or ():
            consider(text, conf, int(cx0) + (x + ww / 2) / s2, int(cy0) + (y + hh / 2) / s2, 'line')
    return choose()


def cmd_locate(a):
    if a.image:
        im = Image.open(a.image).convert('RGB')
    else:
        try:
            im = Grabber().grab()
        except NotFound:
            print('no_window', file=sys.stderr)
            return 3
        except NotViewable:
            print('unviewable', file=sys.stderr)
            return 4
    found = find_label(im, a.label, a.min_conf, a.pick)
    if not found:
        print(f'label {a.label!r} not found', file=sys.stderr)
        return 1
    fx, fy, conf, how = found
    print(f'{fx:.4f} {fy:.4f} {conf:.0f} {how}')
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('state')
    lo = sub.add_parser('locate', help='print "fx fy conf pass" of a text label in the game window')
    lo.add_argument('label')
    lo.add_argument('--image', help='read this saved frame instead of grabbing the window')
    lo.add_argument('--min-conf', type=float, default=70.0)
    lo.add_argument('--pick', choices=('best', 'lowest'), default='best',
                    help='when the text occurs more than once: highest confidence, or lowest on screen')
    r = sub.add_parser('record')
    r.add_argument('dir')
    r.add_argument('--interval', type=float, default=1.0)
    r.add_argument('--gone-after', type=float, default=15.0, help='stop when the window is gone this long')
    r.add_argument('--appear-within', type=float, default=600.0, help='give up if the window never appears')
    pr = sub.add_parser('progress')
    pr.add_argument('dir')
    pr.add_argument('teleport_start', type=float)
    s = sub.add_parser('scan')
    s.add_argument('dir')
    s.add_argument('teleport_start', type=float)
    s.add_argument('--settle', type=float, default=5.0, help='seconds without the loading overlay after the HUD')
    a = ap.parse_args()
    return {'state': cmd_state, 'locate': cmd_locate, 'record': cmd_record, 'progress': cmd_progress,
            'scan': cmd_scan}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
