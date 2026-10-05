import Pentomino

/-!
# The no-drafting game of Pentominoes

The twelve pentominoes form a common pool. The players alternately take any
piece that is still in the pool and place it on the board, in any
orientation, on empty cells. The player who cannot move loses. In
particular, if all twelve pieces have been placed, the player to move has
no move and loses, so the game never ends in a draw.

The shapes, their orientations and their placements on a `w × h` board are
those of `Pentomino.lean` (`allPlacements`), which the drafting game uses as
well.
-/

namespace NoDraft
open Pentomino

/-- A position: the occupied cells and the pieces still in the common pool.
Whose turn it is does not matter: both players take pieces from the same
pool, so the value for the player to move depends only on the position. -/
structure State where
  occupied : List Coord
  pool : List Nat
deriving Repr

/-- The positions reachable by one move: take a piece from the pool and
place it on empty cells. -/
def moves (placements : List (List (List Coord))) (s : State) : List State :=
  s.pool.flatMap fun piece =>
    ((placements[piece]?.getD []).filter (free s.occupied)).map fun cells =>
      { occupied := cells ++ s.occupied, pool := s.pool.erase piece }

theorem moves_decreases (placements : List (List (List Coord))) (s : State) :
    ∀ s' ∈ moves placements s, s'.pool.length < s.pool.length := by
  intro s' hs'
  simp only [moves, List.mem_flatMap, List.mem_map, List.mem_filter] at hs'
  obtain ⟨piece, hpiece, cells, -, heq⟩ := hs'
  subst heq
  have hlen := List.length_erase_of_mem hpiece
  have hpos := List.length_pos_of_mem hpiece
  simp only
  omega

/-- `wins placements s` is `true` iff the player to move in `s` can force a
win: some move leads to a position that the opponent, then to move, cannot
win. With no legal move, `any` over the empty list is `false`: the player to
move loses. -/
def wins (placements : List (List (List Coord))) (s : State) : Bool :=
  (moves placements s).attach.any fun p => !wins placements p.1
termination_by s.pool.length
decreasing_by exact moves_decreases placements s _ p.2

/-- The rule, stated as a theorem: the player to move wins iff some move
leads to a position lost for the opponent. -/
theorem wins_iff (placements : List (List (List Coord))) (s : State) :
    wins placements s = true ↔ ∃ s' ∈ moves placements s, wins placements s' = false := by
  rw [wins]
  simp only [List.any_eq_true, List.mem_attach, true_and, Bool.not_eq_true']
  constructor
  · rintro ⟨⟨s', hs'⟩, h⟩
    exact ⟨s', hs', h⟩
  · rintro ⟨s', hs', h⟩
    exact ⟨⟨s', hs'⟩, h⟩

/-- A position without legal moves is lost for the player to move. -/
theorem no_moves_loses (placements : List (List (List Coord))) (s : State)
    (h : moves placements s = []) : wins placements s = false := by
  rw [wins, h]
  rfl

/-- Does the first player win the game with the pieces `pieceIds` on the
empty `w × h` board? -/
def firstPlayerWinsOn (w h : Nat) (pieceIds : List Nat) : Bool :=
  wins (allPlacements w h) { occupied := [], pool := pieceIds }

/-- Does the player to move win after the given cells are occupied and only
the pieces `pool` are left? (Used to check the C++ solver on positions given
by their occupied cells.) -/
def winsAfter (w h : Nat) (occupied : List Coord) (pool : List Nat) : Bool :=
  wins (allPlacements w h) { occupied, pool }

/-- The statement this formalisation is about: the first player wins the
no-drafting game with all twelve pentominoes on the 8 × 8 board. (It is a
proposition, not a computation: evaluating `firstPlayerWinsOn 8 8` directly
is far too slow.) -/
def FirstPlayerWins8x8 : Prop := firstPlayerWinsOn 8 8 (List.range 12) = true

end NoDraft
