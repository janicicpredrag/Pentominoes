import NoDraft
open Pentomino NoDraft

/-! Reads lines `nd W H p1,p2,...` (pieces in the common pool, empty board)
and prints `1` if the first player wins, `0` otherwise. Used to check the
C++ solver on small instances. -/

def parseList (s : String) : List Nat :=
  if s == "-" then [] else (s.splitOn ",").map String.toNat!

partial def loop (stdin : IO.FS.Stream) : IO Unit := do
  let line ← stdin.getLine
  if line.isEmpty then return
  let ws := (line.trimAscii.toString.splitOn " ").filter (· ≠ "")
  match ws with
  | ["nd", w, h, xs] => IO.println (if firstPlayerWinsOn w.toNat! h.toNat! (parseList xs) then 1 else 0)
  | _ => IO.println "?"
  (← IO.getStdout).flush
  loop stdin

def main : IO Unit := do loop (← IO.getStdin)
