# ultimate-terminal-tetris
## by Alextout

Tetris for the terminal, with the feel of the NES original.
One C file, no dependencies, one compiler call.

> **Note:** I developed this project purely for fun and for my own personal use. However, it is freely available for everyone. Feel free to grab it, play it, and do whatever you want with it!

## Requirements

- A C compiler — Apple clang (`xcode-select --install`) or gcc. No libraries
  are needed, not even libm.
- A terminal at least 62×24 characters. Truecolour or 256 colours look best;
  it falls back to the 8 ANSI colours on its own.
- Python 3 for the checks in `tools/` — standard library only, nothing to
  install with pip. Not needed to play.
- `make` is optional; a single `clang` call builds the same binary.

Developed and tested on **iTerm2 3.6.11** on macOS (Apple silicon). It runs in
any terminal, but iTerm2 is where it is built: from version 3.5 on it speaks
the kitty keyboard protocol, which reports key releases — and knowing how long
a key has been held is what makes real DAS/ARR possible at all. kitty,
ghostty, foot and WezTerm report them too. Everywhere else the game falls back
to timing the operating system's auto repeat, which works but feels looser.
The help panel tells you which of the two you are in.


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

## Play

```sh
make            # or: cc -O2 -o tetris tetris.c
./tetris
```

`make install` links it into `~/bin` so you can just type `tetris` anywhere.

## Controls

| | | | |
|---|---|---|---|
| left / right | move | c, shift, tab | hold |
| down | soft drop | v | solid or classic look |
| space | hard drop | h | help panel |
| up, x | rotate cw | esc | pause |
| z, ctrl | rotate ccw | r *(hold)* | restart |
| a | rotate 180 | q | quit |

## What is in it

SRS with the full wall kick tables and 180 degree spins, 7-bag randomizer,
hold, ghost piece, five previews, lock delay, guideline gravity, T-spins by
the three-corner rule, back-to-back, combos and perfect clears.

Four modes — marathon, sprint, ultra, zen — plus a name field and a scoreboard
you can open from the menu.

**Zen never resets.** Its score is banked under the name you are playing as
and picked up again the next time, so a name works like a save slot. Rename
yourself and zen starts from zero; rename back and your total is waiting. The
menu shows what you are continuing at.

## Files

Everything stays in this folder, nothing lands in your home directory. Copy
`config.example` to `config` to change settings; `scoreboard` holds the runs
and is plain text. Both are ignored by git, so your settings and scores are
yours alone.

## Checking it

```sh
make check
```

16 headless checks of the rules inside the binary, plus tools that drive the
real game on a pseudo terminal to verify the renderer and the soft drop
timing.

## Credits

Built with the help of [Claude](https://claude.com/claude-code) and
[Pi](https://github.com/badlogic/pi-coding-agent); the design, the rules
and the debugging are mine.

Grew out of the C port in **Kirill Timofeev**'s
[tetris](https://github.com/kt97679/tetris), a recreation of the game from
soviet DVK machines. None of that code is left, but it is where this started.

License: [MIT License](LICENSE).

