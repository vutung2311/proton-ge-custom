#!/usr/bin/env python3
"""Follow Where Winds Meet's obfuscated game_account.log and print each decoded line as
"<epoch seconds> <line>", flushed immediately, starting from the current end of the file.

Used by wwm_ab_run.sh AUTO=1 to detect the title screen, the Start click and the load start
without keypresses. Decoding reuses decode_caesar() from the local diagnostic tool.

usage: tools/wwm/wwm_gamelog_follow.py [log path]
"""
import importlib.util
import os
import sys
import time

TOOL = os.path.expanduser('~/Git/local_diagnostic/wwm/decrypt_game_log.py')
spec = importlib.util.spec_from_file_location('decrypt_game_log', TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

path = sys.argv[1] if len(sys.argv) > 1 else tool.DEFAULT_LOG_PATH
with open(path, 'r', encoding='latin1') as f:
    f.seek(0, os.SEEK_END)
    while True:
        line = f.readline()
        if not line:
            time.sleep(0.05)
            continue
        dec = tool.decode_caesar(line).rstrip('\r\n')
        if dec.strip():
            print(f"{time.time():.3f} {dec}", flush=True)
