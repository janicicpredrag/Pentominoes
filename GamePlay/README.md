# Pentominoes with drafting — terminal game

C++17 refactoring of the program PENTOMIN.C (Predrag Janičić,
1993), with the DOS graphics replaced by a text display in the terminal.

## Building and playing

    make
    ./pentomino                         # asks for the settings
    ./pentomino --players 1 --level 4   # you draft first, against level 4
    ./pentomino --batch 40 --levels 2,6 # 40 program-vs-program games, no display

`./pentomino --help` lists all options. Colours: the pieces of P0 (who drafts
first) are blue, those of P1 red, the pieces placed on the board yellow, and
the pieces still in the pool grey.

Keys: left/right select a piece; during the placement, up/down move the
piece over its legal positions and SPACE turns it; ENTER confirms; `a` asks
the program for a suggestion; `t` takes back moves; `b` makes the program
play the best move found so far; `o` switches the analysis board on or off
at any time (also `--analysis`); `q` or ESC quits.

The analysis board, to the right of the main board, shows what the program
is examining while it thinks, as in the 1993 program: the pieces on the board
(yellow), the pieces it assumes placed in reserved areas (green), the move it
is examining (in the player's colour), and the cells that, after this move,
only the player can still reach (`+`). As in 1993, the analysis slows the
program down: each examined move is shown for 30 ms, for at most 3 seconds
per move; the board stays until the program thinks again.

Without options, the program starts with a title page and two pages of rules
and instructions, and then asks for the settings.

## Portability

The code is standard C++17. Only the keyboard input (`terminal.cpp`) depends
on the system: POSIX (`termios`, `poll`) on Linux and macOS, and `<conio.h>`
on Windows, where the program also switches the console to ANSI colours and
UTF-8 output. On Windows, use Windows Terminal or a recent console window
(Windows 10 or later); build with MinGW (`make`) or with Visual Studio
(compile the six `.cpp` files). Tested on Linux with GCC and Clang; the
Windows and macOS builds have not been tested yet.

## Structure

| File | Contents |
|---|---|
| `pieces.*` | the twelve pentominoes, their orientations and placements (bitboards) |
| `game.*` | the rules: drafting, placement, legal moves, take-back, result |
| `ai.*` | the computer player (drafting weights and placement strategies of the 1993 program) |
| `display.*` | the text display with ANSI colours |
| `terminal.*` | keyboard input in raw mode |
| `main.cpp` | settings, the human and program turns, clocks, the game loop |

The display and the input are used only by `main.cpp`; the rules and the
computer player do not depend on them.

## The computer player (as in 1993)

* Drafting: a random choice weighted by the strength of the pieces
  (P 1000, L and U 100, Y and V 20, N 15, I 12, F 10, T, W and Z 5, X 1),
  with the weights adjusted to the pieces both players already hold.
* Placement: a strategy chosen by the level (0–6) and the number of the
  move. Most strategies use *reserved areas*: empty cells that the opponent
  can no longer cover, so that a piece of the player that fits there can be
  placed at any later time. Strategies: random (but not giving the opponent a
  reserve), opening moves near a corner, creating a reserve, blocking all the
  opponent's ways to create one, preparing a reserve for the next move, and,
  from move 5–8 on depending on the level, exact minimax to the end of the
  game.
