# ultimate-terminal-tetris

Tetris for the terminal, with the feel of the NES original.
Single C file, no dependencies, one compiler call.

```
                MARATHON
    HOLD                               NEXT
               │        ████        │             CONTROLS
               │        ████        │     ██
               │                    │   ██████    left/right  move
               │                    │             down        soft drop
               │                    │   ██        space       hard drop
   SCORE       │                    │   ██████    up / x      rotate cw
   162         │                    │             z / ctrl    rotate ccw
               │                    │    ████     a           rotate 180
   LINES       │                    │    ████     c / shift   hold
   0 / 150     │                    │             v           style
               │                    │   ████      h           this help
   LEVEL       │                    │     ████    esc         pause
   1           │        ▓▓▓▓        │             r           restart
               │        ▓▓▓▓  ██    │  ████████   q           quit
   TIME        │          ██████    │
   0:03.220    │          ██        │             DAS 266ms
               │    ████  ██        │             ARR 100ms
   PPS         │      ██████        │             SOFT 30/s
   1.55        │    ████████        │
               │  ████  ██████      │
               └────────────────────┘
```

## Build

```sh
clang -O2 -o tetris tetris.c
./tetris
```

Nothing else is needed: no libraries, not even libm. It builds with gcc just
as well. Tested on macOS with Apple clang and on an arm64 Mac.

To have it as a command anywhere, link the binary into a directory on your
PATH:

```sh
ln -s "$PWD/tetris" /opt/homebrew/bin/tetris     # or ~/bin, /usr/local/bin, ...
```

The game asks the system where its own binary is and follows the symlink, so
it still finds `config` and `scoreboard` in this folder no matter which
directory you start it from. Rebuilding keeps the link working; removing the
command is one `rm` on the link.

## What is in it

Mechanics follow the tetris guideline:

- **SRS** rotation with the full wall kick tables, plus 180 degree spins
- **7-bag** randomizer, so you never wait long for the piece you need
- **Hold**, **ghost piece**, and five **previews**
- **Lock delay** of 500 ms with the usual 15 move resets, bounded by the
  lowest row the piece has reached
- Guideline **gravity**, `(0.8 - (level-1) * 0.007) ^ (level-1)` seconds a row
- **T-spins** by the three-corner rule, told apart from minis by which corners
  are filled and whether the rotation needed the last kick offset
- **Back-to-back**, **combos** and **perfect clears**

Four modes: marathon (150 lines), sprint (40 lines against the clock), ultra
(two minutes for the highest score) and zen (no goal).

Everything the game reads or writes stays in this folder: `config` for the
settings and `scoreboard` for the results, both next to the binary. Nothing is
scattered into `~/.config` or `~/.local`.

## Name and scoreboard

The start screen has a name field. Select it, press enter, type, press enter
again — that name goes on every run you start afterwards, and it is remembered
between sessions.

`SCOREBOARD` in the same menu opens the table without leaving the game. Left
and right switch between the modes, escape goes back. Sprint ranks on time,
because it is a race; every other mode ranks on score. After a run the game
over screen tells you which place you took.

The `scoreboard` file is plain text, one run per line, so you can read it,
edit it or throw it away without the game's help:

```
player Alextout
ZEN 2140 12 84150000 0 2026-09-09 Alextout
SPRINT 3800 40 61300000 1 2026-09-09 Alextout
```

Mode, score, lines, time in microseconds, whether the run reached its goal,
the date, and the name. The game keeps the best fifty runs per mode.

## Controls

| | |
|---|---|
| left / right | move |
| down | soft drop |
| space | hard drop |
| up, x | rotate clockwise |
| z, ctrl | rotate counter-clockwise |
| a | rotate 180 |
| c, shift, tab | hold |
| v | switch between the solid and the classic `[]` look |
| h | show or hide the help panel |
| esc | pause, and back to the menu after a game |
| r | hold to restart |
| q | quit |

## Handling

The three numbers a stacker actually cares about:

| | default | meaning |
|---|---|---|
| `das` | 266 ms | how long you hold a direction before it starts repeating |
| `arr` | 100 ms | how fast it repeats afterwards; 0 slides straight to the wall |
| `sdf` | 30 rows/s | soft drop speed |

The defaults are the NES timings: 16 frames before the auto shift kicks in,
then one cell every 6 frames. The first repeat lands the moment the delay runs
out rather than a repeat later. If you are used to a modern stacker, `das=133
arr=0` is the usual setting there and slides the piece straight to the wall.

Soft drop runs at a **fixed rate** rather than as a multiple of gravity, so it
feels the same at every level. The default of 30 rows a second is the NES rate
of one row every two frames at 60 Hz. It can only ever speed the piece up, so
at high levels, where gravity is already quicker, holding down does nothing —
same as on the NES. `--sdf=0` drops instantly.

Set them on the command line or in the `config` file next to the binary. That
file ships with every option commented out and its default written down, so
uncommenting a line is the whole job:

```ini
das  = 266
arr  = 100
sdf  = 30

style = solid      # or classic
ghost = 1

# bindings: several keys per action, comma separated
key_hard_drop  = space
key_rotate_cw  = up, x
key_rotate_ccw = z, lctrl
key_hold       = c, lshift, tab
```

`./tetris --help` lists every option.

## Restart is a hold

`r` does not reset the run on a stray press. Hold it for about seven tenths of
a second and a bar fills in behind the `r restart` line in the help panel;
the run only ends when the bar is full. Letting go early cancels it.

## Key releases, and why they matter

Real DAS needs to know how long a key has been held, and a terminal normally
never tells you when a key goes up — you only see the operating system's auto
repeat. Terminals that speak the **kitty keyboard protocol** do report
releases, so the game asks for it on startup and uses it when it is there.
That covers iTerm2 3.5+, kitty, ghostty, foot and WezTerm.

Everywhere else it falls back to timing the auto repeat, which works but feels
looser. The help panel says which mode you are in.

## Checking it still works

The binary carries its own test suite, which runs the game logic headless with
a fixed seed — no terminal involved, so it is fine in CI:

```sh
./tetris --selftest
```

Fifteen checks, covering the kick tables against known cases, the bag, the
three-corner rule, scoring with back-to-back and combos, the clear animation,
the soft drop rate and the mode goals.

Rendering cannot be tested that way, so `tools/` drives the real binary on a
pseudo terminal:

```sh
tools/screen_check.py ./tetris      # drawn screen vs a forced full repaint
tools/softdrop_check.py ./tetris    # soft drop timing
tools/bench.py ./tetris             # cpu, memory, frame sizes
```

`screen_check.py` is the important one. The renderer only writes the cells
that changed, and if that diff is ever wrong the screen drifts away from the
game state without the game noticing. The check pauses the game, forces a full
repaint, and requires the two screens to be identical.

## Credits

This started as the C port in **Kirill Timofeev**'s `tetris`, a recreation of
the tetris that ran on soviet DVK machines, PDP-11 clones:
<https://github.com/kt97679/tetris>. That project implements the same game in
seven languages and is worth a look.

None of the original code survives here — the board, the piece tables, the
rotation system, the input layer and the renderer were all rewritten — but it
is where this one started, and Kirill deserves the credit for it. The original
is under the [WTFPL](http://www.wtfpl.net/).
