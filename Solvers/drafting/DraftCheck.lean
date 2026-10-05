/-!
# Who wins the drafting game? An independent check in Lean

This program decides the two drafting variants of 8×8 Pentominoes from the
table of solved placement games, `results/placement_8x8.tsv`, which the C++
solver produced. It does not trust the solver's own solution of the draft: it
reads only the outcome of each placement game and solves the draft again, by
plain minimax over the complete draft tree, without pruning, memoisation or
any other optimisation.

**The draft.** The twelve pentominoes start in a common pool. P0 and P1 pick
alternately, P0 first, until each holds six. The result of the draft is then
decided by the placement game played with the two hands.

**The table.** For every split of the pieces, the table gives the winner of
the placement game in both variants:
* column `winner_P0_places_first`: P0 holds `first_hand` and places first;
* column `winner_P1_places_first`: P0 holds `first_hand` and P1 places first.

**Values.** A value is from P0's point of view: `1` = P0 wins, `0` = draw,
`-1` = P1 wins. P0 maximises it, P1 minimises it.

**Hands.** A hand is a set of pieces, stored as a 12-bit number: piece `i` of
`pieceNames` is in the hand iff bit `i` is set.
-/

/-- The twelve pentominoes; piece `i` corresponds to bit `i` of a hand. -/
def pieceNames : List Char := ['F', 'I', 'L', 'N', 'P', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z']

/-- The hand containing all twelve pieces. -/
def allPieces : Nat := 2 ^ 12 - 1

/-- The hand containing only piece `i`. -/
def single (i : Nat) : Nat := 2 ^ i

/-- Is piece `i` in `hand`? -/
def has (hand i : Nat) : Bool := hand.testBit i

/-- The pieces (indices `0..11`) of a hand, in increasing order. -/
def piecesOf (hand : Nat) : List Nat := (List.range 12).filter (has hand)

/-- The index of a piece letter, if it is one of the twelve. -/
def pieceIndex (c : Char) : Option Nat := (List.range 12).find? fun i => pieceNames[i]! == c

/-- Parse a hand such as `"FILNPT"`; fails on unknown or repeated letters. -/
def parseHand (s : String) : Option Nat :=
  s.toList.foldlM (init := 0) fun hand c => do
    let i ← pieceIndex c
    if has hand i then none else some (hand + single i)

/-- Print a hand as letters. -/
def showHand (hand : Nat) : String := String.ofList ((piecesOf hand).map fun i => pieceNames[i]!)

/-- Parse a winner as a value for P0. -/
def parseWinner : String → Option Int
  | "P0"   => some 1
  | "draw" => some 0
  | "P1"   => some (-1)
  | _      => none

/-! ## The draft by plain minimax -/

/-- `draftValue leaf n pool h0` is the value for P0 of the draft position in
which the `n` pieces of `pool` are still to be picked and P0 holds `h0`
(P1 holds the remaining pieces, `allPieces - pool - h0`).

`12 - n` picks have been made; since P0 picks first and the players
alternate, P0 is to pick when that number is even. When the pool is empty,
the draft is over and `leaf h0` gives the outcome of the placement game.

The recursion is on `n`, so Lean checks that it terminates. -/
def draftValue (leaf : Nat → Int) : Nat → Nat → Nat → Int
  | 0,     _,    h0 => leaf h0
  | n + 1, pool, h0 =>
    let p0ToPick := (12 - (n + 1)) % 2 == 0
    let values := (piecesOf pool).map fun i =>
      draftValue leaf n (pool - single i) (if p0ToPick then h0 + single i else h0)
    if p0ToPick then values.foldl max (-1) else values.foldl min 1

/-- The value for P0 of each possible first pick of P0, followed by the best
of these, which is the value of the whole draft (P0 picks first and
maximises). -/
def firstPicks (leaf : Nat → Int) : List (Nat × Int) :=
  (List.range 12).map fun i => (i, draftValue leaf 11 (allPieces - single i) (single i))

/-! ## Reading and checking the table -/

/-- One row of the table: P0's hand, P1's hand, the value when P0 places
first and the value when P1 places first. -/
structure Row where
  h0 : Nat
  h1 : Nat
  p0First : Int
  p1First : Int

/-- Parse one data line of the table (tab-separated columns:
first_hand, second_hand, value, winner, winner_P0_places_first,
winner_P1_places_first, nodes, time_s). -/
def parseRow (line : String) : Except String Row := do
  match line.splitOn "\t" with
  | a :: b :: v :: w :: c0 :: c1 :: _ =>
    let some h0 := parseHand a | throw s!"bad hand {a}"
    let some h1 := parseHand b | throw s!"bad hand {b}"
    let some x := parseWinner c0 | throw s!"bad winner {c0}"
    let some y := parseWinner c1 | throw s!"bad winner {c1}"
    -- the `value`/`winner` columns must agree with `winner_P0_places_first`
    let some v' := v.toInt? | throw s!"bad value {v}"
    unless v' == x do throw s!"value {v} disagrees with {c0} in {line}"
    unless (w == "first" && x == 1) || (w == "draw" && x == 0) || (w == "second" && x == -1) do
      throw s!"winner {w} disagrees with {c0} in {line}"
    return { h0, h1, p0First := x, p1First := y }
  | _ => throw s!"bad line {line}"

/-- Read the table and check it thoroughly. Returns the two leaf functions
(P0 places first, P1 places first), indexed by P0's hand. -/
def readTable (path : String) : IO ((Nat → Int) × (Nat → Int)) := do
  let lines ← IO.FS.lines path
  let data := lines.toList.filter fun l => !(l.startsWith "#" || l.startsWith "first_hand" || l.isEmpty)
  let rows ← match data.mapM parseRow with
    | .ok rs => pure rs
    | .error e => throw (IO.userError e)
  -- every row: two hands of six pieces that together form all twelve
  for r in rows do
    unless (piecesOf r.h0).length == 6 && (piecesOf r.h1).length == 6 && r.h0 + r.h1 == allPieces
        && (piecesOf r.h0).all (fun i => !has r.h1 i) do
      throw (IO.userError s!"row {showHand r.h0} / {showHand r.h1} is not a split of the twelve pieces")
  -- every split of the twelve pieces occurs exactly once
  let splits := (List.range (allPieces + 1)).filter fun h => (piecesOf h).length == 6
  unless splits.length == 924 do throw (IO.userError "internal: expected 924 splits")
  for h in splits do
    let n := (rows.filter fun r => r.h0 == h).length
    unless n == 1 do throw (IO.userError s!"split {showHand h} occurs {n} times")
  unless rows.length == 924 do throw (IO.userError s!"expected 924 rows, found {rows.length}")
  -- the leaf tables, as arrays indexed by P0's hand
  let mut a : Array Int := Array.replicate (allPieces + 1) 0
  let mut b : Array Int := Array.replicate (allPieces + 1) 0
  for r in rows do
    a := a.set! r.h0 r.p0First
    b := b.set! r.h0 r.p1First
  -- consistency of the two columns: with P1 placing first, P0's result with
  -- hand H is minus the result of the player who places first with the other
  -- hand, i.e. minus the P0-places-first value of the swapped split
  for r in rows do
    unless b[r.h0]! == - a[r.h1]! do
      throw (IO.userError s!"columns inconsistent for {showHand r.h0}")
  IO.println s!"table: {rows.length} rows, all 924 splits present exactly once, all checks passed"
  let count (t : Array Int) (x : Int) := (splits.filter fun h => t[h]! == x).length
  IO.println s!"  P0 places first: P0 wins {count a 1}, draws {count a 0}, P1 wins {count a (-1)}"
  IO.println s!"  P1 places first: P0 wins {count b 1}, draws {count b 0}, P1 wins {count b (-1)}"
  return (fun h => a[h]!, fun h => b[h]!)

/-! ## Main -/

def describe : Int → String
  | 1 => "P0 wins"
  | 0 => "draw"
  | _ => "P1 wins"

def solve (title : String) (leaf : Nat → Int) : IO Unit := do
  let picks := firstPicks leaf
  let value := (picks.map (·.2)).foldl max (-1)
  IO.println s!"{title}: {describe value}"
  IO.println s!"  value of each first pick of P0: {String.intercalate ", " (picks.map fun (i, v) => s!"{pieceNames[i]!}:{v}")}"
  IO.println s!"  winning first picks: {String.ofList ((picks.filter (·.2 == 1)).map fun (i, _) => pieceNames[i]!)}"

def main (args : List String) : IO Unit := do
  let path := args.headD "../results/placement_8x8.tsv"
  let (p0First, p1First) ← readTable path
  solve "P0 drafts first and places first" p0First
  solve "P0 drafts first, P1 places first" p1First
