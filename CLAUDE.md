# CLAUDE.md

Notes for working in this repository.

## What this is

`ultimate-terminal-tetris` — a guideline tetris for the terminal in a single C
file. No dependencies, not even libm; `make` is a single compiler call. Keep
it that way: a new dependency needs a real reason.

It began as the C port in <https://github.com/kt97679/tetris> by Kirill
Timofeev. None of that code is left, but the credit belongs in the file
header, the README and the start screen. Do not quietly drop it.

## Build and check

```sh
make                                         # must stay warning free
make check                                   # selftest plus the pty tools
./tetris --selftest                          # 16 headless checks, no terminal
tools/screen_check.py ./tetris               # renderer vs full repaint
tools/softdrop_check.py ./tetris             # soft drop timing
tools/bench.py ./tetris                      # cpu, memory, frame size
```

`--selftest` needs no terminal and is the fast feedback loop. The `tools/`
scripts drive the real binary on a pseudo terminal; they are Python 3 and
depend on nothing outside the standard library. They run a copy of the binary
from a scratch directory, because the game keeps its config and scores next to
it and a test run would otherwise rewrite the player's own files.

The game refuses to run when stdin or stdout is not a tty, so never launch it
straight from a shell tool — it would sit there forever. Go through
`tools/ptyterm.py`.

## Layout of tetris.c

Read it top to bottom; the sections are marked with banner comments.

1. Constants, piece geometry, SRS kick tables, palette
2. Configuration, key bindings
3. Terminal setup and restore
4. Screen buffer and the diffing renderer
5. Game state, randomizer, collision, gravity
6. Movement, rotation, T-spin detection, locking, scoring
7. Input: kitty protocol negotiation, escape parsing, key state
8. Handling: DAS, ARR, soft drop
9. Scoreboard file, then drawing: board, panels, overlays, menu, scoreboard
10. Config file, command line
11. Self test
12. Main loop

## Things that will bite you

**stdout must stay blocking.** The version this grew out of set `O_NONBLOCK`
on stdout. At a terminal stdin and stdout share one open file description, so
that also made reads non-blocking — which is why it was there. But it meant a
write could fail with `EAGAIN` once the terminal's roughly one kilobyte buffer
filled, and a hard drop emitted about 1.7 KB at once. Frames arrived half
drawn. `get_key` already guarded every read with `select`, so the flag bought
nothing. Do not reintroduce it.

**One write per frame.** `scr_flush` serialises the whole diff into one buffer
and writes it once. Keep it that way: a frame that reaches the terminal in
pieces can be seen half applied. Median frame is around 100 bytes; full
repaints run to a few kilobytes and are fine, because the write blocks.

**The renderer has no safety net.** Only changed cells are written, so a bug in
the diff makes the screen drift from the game state silently. Any change to
`scr_flush`, `scr_put` or the drawing code must be followed by
`tools/screen_check.py`, which forces a full repaint and compares.

**Timers accumulate.** `gravity_acc` holds up to a whole second at level 1.
Anything that shortens an interval must reset the counter, or the backlog gets
cashed in all at once — that is exactly how soft drop used to teleport the
piece down several rows on the first press. Same care applies to `g_das_acc`
and `g_arr_acc`.

**The auto shift fires on the edge, not a repeat later.** `g_das_charged`
marks the moment the delay ran out: the first cell moves right then, and the
overshoot carries into `g_arr_acc` so the rate does not drift. Resetting the
direction has to clear that flag too, or the next press skips its delay.

**Press is not repeat.** With the kitty protocol, holding a key gives one
press and then repeat events. Without it, the OS auto repeat looks like a
stream of fresh presses. Code that reacts to a *new* press must key off the
down-state edge (`g_soft_held` is the pattern), not the press event, or the
system repeat rate ends up driving the game.

**A lone escape looks like the start of a sequence.** It is only delivered
after `ESC_TIMEOUT_US` passes with nothing following. Do not shorten that
below a few dozen milliseconds or arrow keys start registering as escapes.

**Kick tables are data, not intuition.** `KICKS_JLSTZ` and `KICKS_I` are the
published SRS tables with every y negated, because this code has y growing
downwards. If you touch them, `test_tspin` is the check that matters — it
rotates a T into a slot reachable only through the third kick offset.

**Zen is one run per name, not one per session.** `board_record` updates that
player's existing zen entry instead of appending: the score replaces, lines
and time add up. `zen_carry` reads it back when a zen run starts. Because the
score is a running total, leaving mid-run must not drop it — `zen_bank` is
called on quit, on restart and on the way out of `main`, guarded by
`g_submitted` so a run is never counted twice.

**Typed text is a separate channel.** `g_text` collects characters as typed,
case intact, because the key state lowercases everything — bindings do not
care about shift, the name field does. Only the name editor reads it, and the
main loop clears it every frame it is not editing.

**The game owns its folder.** `config` and `scoreboard` sit next to the binary
and are both git-ignored, so nobody's settings or scores travel with the repo;
`config.example` is the tracked copy. Nothing is written to `~/.config` or
`~/.local`; keep it that way.

`locate_base_dir` cannot trust `argv[0]`: run as a bare command off PATH it is
just `"tetris"` with no directory, and the game would look for its files in
whatever directory the player happened to be in. It asks the system instead —
`_NSGetExecutablePath` on macOS, `/proc/self/exe` on Linux — then `realpath`s
the answer so a symlink from a bin directory resolves back here. `argv[0]` is
only the fallback, and the working directory the fallback after that.

## Conventions

- C89-ish declarations at the top of a block, four spaces, no tabs
- Comments explain *why*, not *what*. The existing ones are the calibration:
  they mostly record a constraint or a trap, not a restatement of the code
- Prefix globals with `g_`, keep them near the code that owns them
- Every new rule or scoring case gets a `--selftest` check; those are cheap
  and they are the only thing standing between a subtle rules bug and nobody
  noticing

## Not done

- The kitty protocol path has been verified by hand in iTerm2, but the
  `tools/` harness does not implement the protocol, so automated runs always
  exercise the fallback. A harness that answers the `CSI ? u` query would
  close that gap.
- The scoreboard has no way to delete a single entry from inside the game;
  the file has to be edited by hand.
