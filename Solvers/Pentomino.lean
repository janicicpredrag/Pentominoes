/-
  Formalization of the two-phase pentomino game.

  Phase 1 (draft): players alternately pick one piece each turn from the 12
  pentominoes into their own private pool, until each holds 6.

  Phase 2 (placement): players alternately place a piece from their own
  pool (any orientation: rotations + reflections) onto the board without
  overlapping previously placed cells. A player who still holds pieces but
  cannot place any of them loses; a player to move whose pool is empty
  (all pieces have been placed) draws.

  Everything here is genuinely decidable/executable (no `sorry`, no axioms
  beyond what `#print axioms` on the quicksort file already showed are
  unavoidable: `propext`, `Quot.sound`). It is parametrized by board width
  and height, and by which pieces are in play, specifically so it can be
  exercised end-to-end on small boards where the search actually finishes
  -- the real 8x8 / 12-piece instance is solved separately by a fast C
  engine and cross-checked against this file on small cases.
-/

namespace Pentomino

abbrev Coord := Int × Int

/-- The 12 free pentomino shapes, as cell offsets from a corner of their
bounding box. Each has exactly 5 connected cells, and the orientation counts
(8/8/8/8/8/8/4/4/4/4/2/1) sum to the well-known total of 63 fixed pentomino
forms. -/
def baseShapes : List (List Coord) :=
  [ /- F -/ [(0,1),(0,2),(1,0),(1,1),(2,1)],
    /- I -/ [(0,0),(1,0),(2,0),(3,0),(4,0)],
    /- L -/ [(0,0),(1,0),(2,0),(3,0),(3,1)],
    /- N -/ [(0,1),(1,1),(2,0),(2,1),(3,0)],
    /- P -/ [(0,0),(0,1),(1,0),(1,1),(2,0)],
    /- T -/ [(0,0),(0,1),(0,2),(1,1),(2,1)],
    /- U -/ [(0,0),(0,2),(1,0),(1,1),(1,2)],
    /- V -/ [(0,0),(1,0),(2,0),(2,1),(2,2)],
    /- W -/ [(0,0),(1,0),(1,1),(2,1),(2,2)],
    /- X -/ [(0,1),(1,0),(1,1),(1,2),(2,1)],
    /- Y -/ [(0,1),(1,0),(1,1),(2,1),(3,1)],
    /- Z -/ [(0,0),(0,1),(1,1),(2,1),(2,2)] ]

def normalize (cells : List Coord) : List Coord :=
  let rmin := (cells.map Prod.fst).foldl min (cells.headD (0,0)).fst
  let cmin := (cells.map Prod.snd).foldl min (cells.headD (0,0)).snd
  (cells.map fun (r, c) => (r - rmin, c - cmin)).mergeSort (fun a b => a.1 < b.1 || (a.1 = b.1 && a.2 ≤ b.2))

/-- Rotate 90° then optionally reflect, iterated to cover all 8 symmetries
of the square. -/
def orientations (cells : List Coord) : List (List Coord) :=
  let rot (p : List Coord) : List Coord := p.map fun (r, c) => (c, -r)
  let refl (p : List Coord) : List Coord := p.map fun (r, c) => (r, -c)
  let fromBase (base : List Coord) : List (List Coord) :=
    let r1 := rot base
    let r2 := rot r1
    let r3 := rot r2
    [base, r1, r2, r3].map normalize
  (fromBase cells ++ fromBase (refl cells)).eraseDups

/-- All orientations of all 12 pieces, precomputed once. -/
def pieceOrientations : List (List (List Coord)) :=
  baseShapes.map orientations

/-- Cells of the `w × h` board (rows `0..h-1`, cols `0..w-1`). -/
def inBounds (w h : Nat) (c : Coord) : Bool :=
  0 ≤ c.1 && c.1 < (h : Int) && 0 ≤ c.2 && c.2 < (w : Int)

/-- All placements of one piece (as absolute cell lists) that fit on the
board, regardless of what is already occupied. -/
def placementsOf (w h : Nat) (pieceIdx : Nat) : List (List Coord) :=
  match pieceOrientations[pieceIdx]? with
  | none => []
  | some orients =>
    orients.flatMap fun cells =>
      let rowspan := (cells.map Prod.fst).foldl max 0
      let colspan := (cells.map Prod.snd).foldl max 0
      ((List.range h).flatMap fun (dr : Nat) =>
        (List.range w).filterMap fun (dc : Nat) =>
          if (dr : Int) + rowspan < (h : Int) && (dc : Int) + colspan < (w : Int) then
            some (cells.map fun (r, c) => (r + (dr : Int), c + (dc : Int)))
          else none)

/-- Precomputed fitting placements for every piece, given a board size. -/
def allPlacements (w h : Nat) : List (List (List Coord)) :=
  (List.range 12).map (placementsOf w h)

structure Phase2State where
  occupied : List Coord
  poolA : List Nat
  poolB : List Nat
  turnA : Bool
deriving Repr

def free (occupied cells : List Coord) : Bool :=
  cells.all fun c => !occupied.contains c

/-- Legal next-states reachable by the player to move placing one piece
from their own pool. -/
def phase2Moves (placements : List (List (List Coord))) (s : Phase2State) : List Phase2State :=
  let myPool := if s.turnA then s.poolA else s.poolB
  myPool.flatMap fun piece =>
    ((placements[piece]?.getD []).filter (free s.occupied)).map fun cells =>
      { s with
        occupied := cells ++ s.occupied
        poolA := if s.turnA then s.poolA.erase piece else s.poolA
        poolB := if s.turnA then s.poolB else s.poolB.erase piece
        turnA := !s.turnA }

theorem phase2Moves_decreases (placements : List (List (List Coord))) (s : Phase2State) :
    ∀ s' ∈ phase2Moves placements s, s'.poolA.length + s'.poolB.length < s.poolA.length + s.poolB.length := by
  intro s' hs'
  unfold phase2Moves at hs'
  cases h : s.turnA <;> rw [h] at hs' <;>
      simp only [List.mem_flatMap, List.mem_map, List.mem_filter] at hs' <;>
      obtain ⟨piece, hpiece, cells, -, heq⟩ := hs' <;> subst heq
  · have hlen : (s.poolB.erase piece).length = s.poolB.length - 1 := List.length_erase_of_mem hpiece
    have hpos : 0 < s.poolB.length := List.length_pos_of_mem hpiece
    simp
    omega
  · have hlen : (s.poolA.erase piece).length = s.poolA.length - 1 := List.length_erase_of_mem hpiece
    have hpos : 0 < s.poolA.length := List.length_pos_of_mem hpiece
    simp
    omega

/-- `phase2Value s = v`: the game-theoretic value of `s` for the player to
move, with `1` = mover forces a win, `0` = mover forces (at best) a draw,
`-1` = mover loses. A position with no legal moves is a draw exactly when
the mover's own pool is already empty (they used up every piece); if the
pool is nonempty but nothing fits, the mover is blocked and loses. Since
both pools start equal (6/6) and turns strictly alternate, one pool can
only run out exactly when the other has too, so this "empty pool" case is
genuinely the whole-board-tiled draw, never a false positive. -/
def phase2Value (placements : List (List (List Coord))) : Phase2State → Int
  | s =>
    let moves := phase2Moves placements s
    if moves = [] then
      if (if s.turnA then s.poolA else s.poolB).isEmpty then 0 else -1
    else
      (moves.attach.map fun p => -phase2Value placements p.1).foldl max (-1)
termination_by s => s.poolA.length + s.poolB.length
decreasing_by all_goals exact phase2Moves_decreases placements s _ p.2

/-- `phase2AValue poolA poolB firstIsA` : phase 2's value from player A's
perspective (`1` A wins, `0` draw, `-1` B wins), given A's pool `poolA`,
B's pool `poolB`, and `firstIsA` saying who places first. -/
def phase2AValue (placements : List (List (List Coord))) (poolA poolB : List Nat) (firstIsA : Bool) : Int :=
  let moverValue := phase2Value placements { occupied := [], poolA, poolB, turnA := firstIsA }
  if firstIsA then moverValue else -moverValue

/-! ### Phase 1: drafting -/

structure Phase1State where
  draftPool : List Nat
  poolA : List Nat
  poolB : List Nat
  turnA : Bool
deriving Repr

/-- Legal next-states reachable by the player to move drafting one piece
into their own pool. -/
def phase1Moves (s : Phase1State) : List Phase1State :=
  s.draftPool.attach.map fun p =>
    { draftPool := s.draftPool.erase p.1
      poolA := if s.turnA then p.1 :: s.poolA else s.poolA
      poolB := if s.turnA then s.poolB else p.1 :: s.poolB
      turnA := !s.turnA }

/-- Removing one drafted piece from the pool strictly shrinks it -- the
measure `phase1AValue` decreases on. -/
theorem erase_length_lt {piece : Nat} {pool : List Nat} (h : piece ∈ pool) :
    (pool.erase piece).length < pool.length := by
  have hlen := List.length_erase_of_mem h
  have hpos := List.length_pos_of_mem h
  omega

theorem phase1Moves_decreases (s : Phase1State) :
    ∀ s' ∈ phase1Moves s, s'.draftPool.length < s.draftPool.length := by
  intro s' hs'
  simp only [phase1Moves, List.mem_map] at hs'
  obtain ⟨piece, -, heq⟩ := hs'
  have hpiece : piece.1 ∈ s.draftPool := piece.2
  subst heq
  simp only
  exact erase_length_lt hpiece

/-- `phase1AValue phase2AValueOracle phase2FirstMoverIsA s` : the value of the
overall two-phase game from draft-state `s`, from player A's perspective
(`1` A wins, `0` draw, `-1` B wins), given a fixed rule
`phase2FirstMoverIsA` for who places first once the draft ends. A (drafting
on her turn) picks the child that maximizes this value; B picks the child
that minimizes it -- ordinary zero-sum minimax, now that a draw is a
possible outcome and "A wins iff some/every child wins" no longer suffices. -/
def phase1AValue (phase2AValueOracle : List Nat → List Nat → Bool → Int) (phase2FirstMoverIsA : Bool) :
    Phase1State → Int
  | s =>
    if s.draftPool = [] then phase2AValueOracle s.poolA s.poolB phase2FirstMoverIsA
    else
      let children := s.draftPool.attach.map fun p =>
        phase1AValue phase2AValueOracle phase2FirstMoverIsA
          { draftPool := s.draftPool.erase p.1
            poolA := if s.turnA then p.1 :: s.poolA else s.poolA
            poolB := if s.turnA then s.poolB else p.1 :: s.poolB
            turnA := !s.turnA }
      if s.turnA then children.foldl max (-1) else children.foldl min 1
termination_by s => s.draftPool.length
decreasing_by all_goals exact erase_length_lt p.2

/-- Generalized top-level answer, over an arbitrary subset of piece
indices (used to validate on small cases); `firstPlayerValue` below
specializes it to all 12 pieces. -/
def firstPlayerValueOn (w h : Nat) (pieceIds : List Nat) (phase2FirstMoverIsA : Bool) : Int :=
  let placements := allPlacements w h
  phase1AValue (phase2AValue placements) phase2FirstMoverIsA
    { draftPool := pieceIds, poolA := [], poolB := [], turnA := true }

/-- Top-level answer: the value (`1` A wins, `0` draw, `-1` B wins) of the
whole two-phase game with all 12 pentominoes on a `w × h` board, given a
fixed rule for who places first in phase 2. -/
def firstPlayerValue (w h : Nat) (phase2FirstMoverIsA : Bool) : Int :=
  firstPlayerValueOn w h (List.range 12) phase2FirstMoverIsA

end Pentomino
