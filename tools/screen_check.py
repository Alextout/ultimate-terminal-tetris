#!/usr/bin/env python3
"""Check that what the game paints matches what it thinks the screen holds.

The renderer only writes the cells that changed since the last frame. If that
diff is ever wrong the screen drifts away from the game state, and nothing in
the game itself would notice. This runs the game for real on a pseudo
terminal, plays a few pieces, pauses so nothing can move, and then forces a
full repaint by toggling the render style twice. The screen before and after
must be identical.

    tools/screen_check.py ./tetris [seed ...]

Exits non-zero if any run differs.
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptyterm import Game


def check(binary, seed):
    game = Game(binary, ["--seed=%s" % seed])
    game.settle()
    game.key("enter", 0.3)

    # Spread the pieces out so the stack does not top out mid-run. The gap
    # between taps has to clear the fallback key-hold window, or the game
    # reads them as one held key and charges DAS instead of stepping.
    random.seed(int(seed))
    for i in range(10):
        steps = i - 5
        for _ in range(abs(steps)):
            game.key("left" if steps < 0 else "right", 0.09)
        for _ in range(random.randint(0, 2)):
            game.key(random.choice(["x", "z", "a"]), 0.06)
        game.key("space", 0.18)

    # look before pausing: escape on the game over screen goes to the menu,
    # and the run would then look like a failure to pause
    if "GAME OVER" in game.screen.render():
        print("  seed %-4s skipped: the run ended before the check" % seed)
        game.quit()
        return None

    game.key("esc", 0.35)
    shot = game.screen.render()
    if "PAUSED" not in shot:
        print("  seed %-4s FAILED: could not pause" % seed)
        game.quit()
        return False

    before = game.screen.occupied()
    game.key("v", 0.25)          # style toggle forces a full repaint
    game.key("v", 0.30)          # and back, so the screen must match again
    after = game.screen.occupied()
    game.key("esc", 0.15)
    status = game.quit()

    differing = {pos for pos in set(before) | set(after)
                 if before.get(pos) != after.get(pos)}
    if differing:
        print("  seed %-4s FAILED: %d differing cells, first %s"
              % (seed, len(differing), sorted(differing)[:6]))
        return False
    print("  seed %-4s ok: drawn screen matches the full repaint, exit %s"
          % (seed, status))
    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    seeds = sys.argv[2:] or ["7", "21", "42"]
    print("renderer check")
    results = [check(binary, s) for s in seeds]
    failed = [r for r in results if r is False]
    ran = [r for r in results if r is not None]
    print("%d of %d runs clean" % (len(ran) - len(failed), len(ran)))
    return 1 if failed or not ran else 0


if __name__ == "__main__":
    sys.exit(main())
