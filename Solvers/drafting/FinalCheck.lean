import Pentomino8x8
open Draft
#eval s!"P0 places first: P0 can force a win = {canWin W1 allPieces 0 true}"
#eval s!"P1 places first: P0 can force a win = {canWin W2 allPieces 0 true}"
#check @p0_wins_P0_places_first
#check @p0_wins_P1_places_first
#check @winning_first_picks_P0_places_first
#check @winning_first_picks_P1_places_first
#print axioms minimax_wins
#print axioms p0_wins_P0_places_first
#print axioms p0_wins_P1_places_first
