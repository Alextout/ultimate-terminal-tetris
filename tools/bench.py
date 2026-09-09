#!/usr/bin/env python3
"""Measure what the game costs: cpu, memory, and how big a frame is.

Frame size matters beyond bandwidth. The renderer sends one write per frame,
and a terminal write blocks once its buffer (around a kilobyte) is full. The
game must never depend on a write completing instantly, and small frames keep
it well clear of that edge. Bursts separated by a gap are counted as frames.

    tools/bench.py ./tetris [seconds]
"""

import os
import resource
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptyterm import Game

FRAME_GAP = 0.006


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    game = Game(binary, ["--mode=zen", "--seed=5"])

    frames = []
    current = 0
    last = time.time()
    start = time.time()
    next_key = 0.0
    keys = ["left", "right", "x", "space"]

    while time.time() - start < duration:
        ready, _, _ = select.select([game.fd], [], [], 0.002)
        now = time.time()
        if ready:
            try:
                data = os.read(game.fd, 65536)
            except OSError:
                break
            if not data:
                break
            if now - last > FRAME_GAP and current:
                frames.append(current)
                current = 0
            current += len(data)
            last = now
        elif current and now - last > FRAME_GAP:
            frames.append(current)
            current = 0
        if now - start > next_key:            # ~10 keypresses a second
            game.key(keys[int(now * 10) % len(keys)], 0)
            next_key = now - start + 0.1
    if current:
        frames.append(current)

    game.quit()
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)

    frames.sort()
    print("cpu      %.2fs over %.0fs, %.1f%% of one core"
          % (cpu, duration, cpu / duration * 100))
    print("memory   %.1f MB peak" % (after.ru_maxrss / 1024 / 1024))
    if frames:
        print("frames   %d seen, median %d B, 95th %d B, largest %d B"
              % (len(frames), frames[len(frames) // 2],
                 frames[int(len(frames) * 0.95)], frames[-1]))
        print("         %d frames over 1 KB (full repaints)"
              % sum(1 for f in frames if f > 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
