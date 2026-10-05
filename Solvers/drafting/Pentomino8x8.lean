import DraftWin
import DraftTable

/-!
# The 8×8 drafting game: P0 wins

Pieces are numbered as in the solver: F I L N P T U V W X Y Z = 0 … 11.
The sets of winning splits come from the table of placement games
(`DraftTable.lean`, generated from `results/placement_8x8.tsv`).

The general guarantee is `Draft.minimax_wins` (in `DraftWin.lean`). What is
specific to 8×8 Pentominoes is only the fact that minimax says "P0 can win"
from the start of the draft; this is established by evaluating `canWin`
(`native_decide`, which runs the compiled code).
-/

open Draft

def pieceNames : List Char := ['F', 'I', 'L', 'N', 'P', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z']

/-- The hand written as letters, e.g. `"FILNPT"`. -/
def handOf (s : String) : Nat :=
  s.toList.foldl (init := 0) fun h c =>
    match (List.range 12).find? (fun i => pieceNames[i]! == c) with
    | some i => add h i
    | none => h

/-- The number of pieces in a hand. -/
def size (h : Nat) : Nat := ((List.range 12).filter (fun i => h.testBit i)).length

/-- A set of hands as a lookup table indexed by the hand. -/
def lookup (hands : List String) : Array Bool :=
  (hands.map handOf).foldl (fun a h => a.set! h true) (Array.replicate 4096 false)

def table1 : Array Bool := lookup DraftTable.winsP0PlacesFirst
def table2 : Array Bool := lookup DraftTable.winsP1PlacesFirst

/-- Winning splits when P0 places first. -/
def W1 (h0 : Nat) : Bool := table1[h0]!
/-- Winning splits when P1 places first. -/
def W2 (h0 : Nat) : Bool := table2[h0]!

/-- All twelve pieces, the pool at the start of the draft. -/
def allPieces : List Nat := List.range 12

/-! ## Sanity checks of the data -/

/-- Every listed hand consists of six distinct valid pieces, and no hand is
listed twice. -/
theorem table_wellformed :
    DraftTable.winsP0PlacesFirst.all (fun s => s.length == 6 && size (handOf s) == 6) &&
    DraftTable.winsP1PlacesFirst.all (fun s => s.length == 6 && size (handOf s) == 6) &&
    (DraftTable.winsP0PlacesFirst.map handOf).eraseDups.length == 381 &&
    (DraftTable.winsP1PlacesFirst.map handOf).eraseDups.length == 537 := by native_decide

/-! ## Variant 1: P0 drafts first and places first -/

/-- Minimax: P0 can force a winning split. -/
theorem canWin_P0_places_first : canWin W1 allPieces 0 true = true := by native_decide

/-- **P0 wins.** Whatever P1 does, P0 following minimax ends the draft with a
hand with which P0 wins the placement game. -/
theorem p0_wins_P0_places_first (s1 : Strategy) :
    W1 (play W1 s1 allPieces 0 true) = true :=
  minimax_wins W1 s1 allPieces 0 true canWin_P0_places_first

/-- The winning first picks are exactly L, P and Y (pieces 2, 4, 10). -/
theorem winning_first_picks_P0_places_first :
    allPieces.filter (fun i => canWin W1 (allPieces.erase i) (add 0 i) false) = [2, 4, 10] := by
  native_decide

/-! ## Variant 2: P0 drafts first, P1 places first -/

theorem canWin_P1_places_first : canWin W2 allPieces 0 true = true := by native_decide

/-- **P0 wins** also in this variant. -/
theorem p0_wins_P1_places_first (s1 : Strategy) :
    W2 (play W2 s1 allPieces 0 true) = true :=
  minimax_wins W2 s1 allPieces 0 true canWin_P1_places_first

/-- Every first pick except X (piece 9) wins. -/
theorem winning_first_picks_P1_places_first :
    allPieces.filter (fun i => canWin W2 (allPieces.erase i) (add 0 i) false) =
      [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11] := by
  native_decide
