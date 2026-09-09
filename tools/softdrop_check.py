#!/usr/bin/env python3
"""Measure the soft drop against a real terminal.

Two properties matter. A single press must move the piece the same distance no
matter when it lands relative to the gravity counter: the counter holds up to
a whole second at level 1, and cashing that backlog in as soft drop steps used
to teleport the piece down several rows. And while held, the rate has to match
the configured rows per second.

The rate is timed only over the stretches where the piece is actually falling.
Lock delay and respawn otherwise dominate the average and make a correct
implementation look about twice too slow.

    tools/softdrop_check.py ./tetris
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptyterm import Game

# Deliberately slow, so the fallback key-hold window cannot add a second row
# and the press is measured on its own.
TAP_RATE = 3


def tap_distance(binary, delay):
    """Rows moved by one press, after letting gravity accumulate for `delay`."""
    game = Game(binary, ["--mode=zen", "--seed=4", "--sdf=%d" % TAP_RATE])
    game.settle()
    game.pump(delay)
    before = game.screen.piece_top()
    game.key("down", 0.11)
    after = game.screen.piece_top()
    game.quit()
    if before is None or after is None:
        return None
    return after - before


def fall_rate(binary, rows_per_second, seconds=4.0):
    """Milliseconds per row while the key is held, ignoring idle stretches."""
    game = Game(binary, ["--mode=zen", "--seed=4", "--sdf=%d" % rows_per_second])
    game.settle()
    previous = game.screen.piece_top()
    rows = 0
    falling = 0.0
    start = last_change = time.time()
    while time.time() - start < seconds:
        game.key("down", 0.008)
        top = game.screen.piece_top()
        now = time.time()
        if top is None:
            continue
        if previous is not None:
            delta = top - previous
            if delta > 0:
                rows += delta
                falling += now - last_change
                last_change = now
            elif delta < 0:
                last_change = now        # a new piece spawned, do not count
            elif now - last_change > 0.25:
                last_change = now        # sitting in lock delay, do not count
        previous = top
    game.quit()
    return rows, falling


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    failed = 0

    print("one press, measured at seven points in the gravity cycle")
    distances = [tap_distance(binary, d)
                 for d in (0.05, 0.20, 0.35, 0.50, 0.65, 0.80, 0.95)]
    good = [d for d in distances if d is not None]
    print("  rows moved: %s" % good)
    if not good:
        print("  FAILED: could not read the piece position")
        failed += 1
    elif min(good) != max(good):
        print("  FAILED: varies with timing, the gravity backlog is leaking in")
        failed += 1
    else:
        print("  ok: constant at %d row(s), no backlog" % good[0])

    print("held, rate against the setting")
    for rate in (10, 30, 60):
        rows, seconds = fall_rate(binary, rate)
        if not rows or seconds <= 0:
            print("  %2d rows/s: FAILED, no movement seen" % rate)
            failed += 1
            continue
        measured = seconds / rows * 1000
        expected = 1000.0 / rate
        off = abs(measured - expected) / expected
        print("  %2d rows/s: %5.0f ms per row, expected %3.0f ms  %s"
              % (rate, measured, expected, "ok" if off < 0.25 else "FAILED"))
        if off >= 0.25:
            failed += 1

    print("all good" if not failed else "%d check(s) failed" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
