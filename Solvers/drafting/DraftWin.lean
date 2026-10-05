/-!
# The draft: minimax and the guarantee it gives

This file is independent of Pentominoes and of the solver. It only talks
about the draft: twelve pieces, numbered `0..11`, are picked alternately by
P0 and P1, P0 first, until the pool is empty.

* A **hand** is a set of pieces, stored as a number whose bit `i` says whether
  piece `i` is in the hand.
* The **pool** is the set of pieces not yet *picked*. When it is empty, the
  draft is over (nothing has been placed on the board yet); the two hands are
  then used in the placement game, which P0 wins, P1 wins, or which is a draw
  (if all twelve pieces get placed).
* `W` is the **set of winning splits**: `W h0 = true` means that P0 wins the
  placement game played with hand `h0` against the other six pieces. Splits
  that P1 wins *and splits that are draws* are not in `W`: reaching `W` means
  winning, not merely not losing.

`canWin` is minimax for this win/lose question, `play` lets P0 follow
minimax against an arbitrary P1, and `minimax_wins` proves that P0 then
always ends in a winning split, whenever minimax says that P0 can win.
-/

namespace Draft

/-- Add piece `i` to a hand. -/
def add (hand i : Nat) : Nat := hand ||| 2 ^ i

/-- Minimax. `canWin W pool h0 p0ToPick` is `true` iff P0 can force the
draft to end in a winning split, when the pieces in `pool` are still to be
picked, P0 holds `h0`, and `p0ToPick` tells who picks next:

* no pieces left: the draft is over, and P0 wins iff `W h0`;
* P0 to pick: P0 can win iff **some** pick leads to a position P0 can win;
* P1 to pick: P0 can win iff **every** pick of P1 leads to such a position. -/
def canWin (W : Nat → Bool) (pool : List Nat) (h0 : Nat) (p0ToPick : Bool) : Bool :=
  if pool = [] then W h0
  else if p0ToPick then pool.attach.any fun x => canWin W (pool.erase x.1) (add h0 x.1) false
  else pool.attach.all fun x => canWin W (pool.erase x.1) h0 true
termination_by pool.length
decreasing_by
  all_goals
    have h1 := List.length_erase_of_mem x.2
    have h2 := List.length_pos_of_mem x.2
    omega

/-- P0's minimax pick: the first piece of the pool after which P0 can still
win (if there is none, simply the first piece of the pool). -/
def bestPick (W : Nat → Bool) (pool : List Nat) (h0 : Nat) (hne : pool ≠ []) : {i // i ∈ pool} :=
  match pool.attach.find? (fun x => canWin W (pool.erase x.1) (add h0 x.1) false) with
  | some x => x
  | none => ⟨pool.head hne, List.head_mem hne⟩

/-- A strategy for P1: in every position with pieces left, it picks some piece
of the pool. It may depend on the whole position in any way. -/
def Strategy := (pool : List Nat) → (h0 : Nat) → pool ≠ [] → {i // i ∈ pool}

/-- The draft played to the end, with P0 picking by minimax (`bestPick`) and
P1 following strategy `s1`. The result is P0's final hand. -/
def play (W : Nat → Bool) (s1 : Strategy) (pool : List Nat) (h0 : Nat) (p0ToPick : Bool) : Nat :=
  if hne : pool = [] then h0
  else if p0ToPick then
    -- P0 picks by minimax
    play W s1 (pool.erase (bestPick W pool h0 hne).1) (add h0 (bestPick W pool h0 hne).1) false
  else
    -- P1 picks by its strategy
    play W s1 (pool.erase (s1 pool h0 hne).1) h0 true
termination_by pool.length
decreasing_by
  · have h1 := List.length_erase_of_mem (bestPick W pool h0 hne).2
    have h2 := List.length_pos_of_mem (bestPick W pool h0 hne).2
    omega
  · have h1 := List.length_erase_of_mem (s1 pool h0 hne).2
    have h2 := List.length_pos_of_mem (s1 pool h0 hne).2
    omega

/-- If minimax says P0 can win, then P0's minimax pick keeps it so. -/
theorem bestPick_canWin (W : Nat → Bool) (pool : List Nat) (h0 : Nat) (hne : pool ≠ [])
    (h : (pool.attach.any fun x => canWin W (pool.erase x.1) (add h0 x.1) false) = true) :
    canWin W (pool.erase (bestPick W pool h0 hne).1) (add h0 (bestPick W pool h0 hne).1) false = true := by
  unfold bestPick
  split
  · rename_i x hx
    exact List.find?_some (p := fun (y : {i // i ∈ pool}) => canWin W (pool.erase y.1) (add h0 y.1) false) hx
  · rename_i hnone
    rw [List.find?_eq_none] at hnone
    rw [List.any_eq_true] at h
    obtain ⟨x, hx, hwin⟩ := h
    exact absurd hwin (hnone x hx)

/-- **The guarantee.** For any set of winning splits `W` and any strategy `s1`
of P1: if minimax says that P0 can win, then the draft in which P0 follows
minimax ends with P0 holding a winning split. -/
theorem minimax_wins (W : Nat → Bool) (s1 : Strategy) (pool : List Nat) (h0 : Nat) (p0ToPick : Bool)
    (h : canWin W pool h0 p0ToPick = true) : W (play W s1 pool h0 p0ToPick) = true := by
  rw [canWin] at h
  rw [play]
  by_cases hne : pool = []
  · simp only [hne, ite_true, dite_true] at h ⊢
    exact h
  · simp only [hne, ite_false, dite_false] at h ⊢
    cases p0ToPick
    · -- P1 to pick: whatever P1 picks, P0 can still win
      simp only [Bool.false_eq_true, ite_false] at h ⊢
      rw [List.all_eq_true] at h
      exact minimax_wins W s1 _ _ _ (h ⟨_, (s1 pool h0 hne).2⟩ (List.mem_attach _ _))
    · -- P0 to pick: P0's minimax pick keeps a win
      simp only [ite_true] at h ⊢
      exact minimax_wins W s1 _ _ _ (bestPick_canWin W pool h0 hne h)
termination_by pool.length
decreasing_by
  all_goals
    first
    | (have h1 := List.length_erase_of_mem (s1 pool h0 hne).2
       have h2 := List.length_pos_of_mem (s1 pool h0 hne).2
       omega)
    | (have h1 := List.length_erase_of_mem (bestPick W pool h0 hne).2
       have h2 := List.length_pos_of_mem (bestPick W pool h0 hne).2
       omega)

end Draft
