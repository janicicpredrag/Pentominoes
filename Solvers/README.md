# Solvers

The programs that solve Pentominoes with and without drafting, and the Lean
definitions and proofs. The results are in `../results/`.

* `Pentomino.lean` — the pieces, their orientations and placements, and the
  rules of the drafting game (both phases), as simple executable definitions;
  shared by both games.
* `drafting/` — the drafting game: the C++ solver of the placement games
  (`solver_drafting.cpp`), the formal proof of the solution of the draft in
  Lean, and a brute-force check of the draft (see `drafting/README.md`).
* `nodrafting/` — the no-drafting game: the C++ solver
  (`solver_nodrafting.cpp`), the rules in Lean (`NoDraft.lean`), and a checker
  for small instances (`NdCheck.lean`).

## Build

    make          # the two C++ solvers: solver_drafting, solver_nodrafting
    lake build    # the Lean project: all definitions and proofs, and the
                  # checkers draftcheck and ndcheck

Both C++ solvers print their options when started without arguments; the
Lean checkers read instances from the standard input (for example
`echo "nd 5 4 4,6,7,9" | .lake/build/bin/ndcheck`).
