# ultimate-terminal-tetris

Tetris for the terminal, with the feel of the NES original.
One C file, no dependencies, one compiler call.

*by Alextout*

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

**Handling is the NES one**: 266 ms before the auto shift starts (16 frames),
then a cell every 100 ms (6 frames), and a soft drop fixed at 30 rows a second
(one row every two frames) instead of scaling with gravity. Change it in
`config` or on the command line, `--das --arr --sdf`. `./tetris --help` lists
everything.

Terminals that speak the **kitty keyboard protocol** report key releases, which
is what makes real DAS possible; iTerm2, kitty, ghostty, foot and WezTerm do.
Elsewhere the game falls back to the OS key repeat and says so on screen.

## Files

Everything stays in this folder, nothing lands in your home directory. Copy
`config.example` to `config` to change settings; `scoreboard` holds the runs
and is plain text. Both are ignored by git, so your settings and scores are
yours alone.

## Checking it

```sh
make check
```

15 headless checks of the rules inside the binary, plus tools that drive the
real game on a pseudo terminal to verify the renderer and the soft drop
timing.

## Credits

Grew out of the C port in **Kirill Timofeev**'s
[tetris](https://github.com/kt97679/tetris), a recreation of the game from
soviet DVK machines. None of that code is left, but it is where this started.

Licensed under the [MIT License](LICENSE).
