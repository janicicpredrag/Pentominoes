# Pentominoes with drafting is solved

Exact solutions of the two-player game Pentominoes on the 8×8 board, with and
without drafting, and a program for playing the drafting game.

In the **drafting** game, the players first take turns picking pieces until
each has six, and then take turns placing their own pieces; the player who
cannot place any of their remaining pieces loses, and the game is a draw if
all twelve pieces are placed. The player who drafts first wins, whether that
player also places first or second. In the **no-drafting** game, the players
place pieces from a common pool; the first player wins, as shown by
Orman (1996).

## Contents

* `Solvers/` — the solvers and the proofs:
  * `Pentomino.lean` — the pieces, their placements and the rules of the
    drafting game in Lean, as simple executable definitions;
  * `drafting/` — the C++ solver of the 924 placement games
    (`solver_drafting.cpp`) and the formal proof in Lean of the solution of
    the draft;
  * `nodrafting/` — the C++ solver of the no-drafting game
    (`solver_nodrafting.cpp`) and its rules in Lean.
* `results/placement_8x8.tsv` — the values of all 924 placement games, with
  the winner in both variants of the drafting game; `results/check_*.py` —
  two analyses based on this table.
* `GamePlay/` — a program for playing the drafting game in the terminal,
  refactored from the author's program of 1993.

## Building

    cd Solvers && make && lake build    # C++ solvers; Lean definitions and proofs
    cd GamePlay && make && ./pentomino  # the playing program

The C++ code is standard C++17; the Lean code needs Lean 4 (the version is
given in `Solvers/lean-toolchain`).

## Author

Predrag Janičić, Faculty of Mathematics, University of Belgrade.
The software was developed with the AI assistant Claude (Anthropic), used
through Claude Code, in an interactive process guided by the author.

## Licence

This work is licensed under the Creative Commons Attribution-NoDerivatives
4.0 International licence (CC BY-ND 4.0); see `LICENSE`.
