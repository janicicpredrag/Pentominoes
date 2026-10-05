import Pentomino
open Pentomino

-- piece indices, matching baseShapes:
-- 0=F 1=I 2=L 3=N 4=P 5=T 6=U 7=V 8=W 9=X 10=Y 11=Z
-- Values: 1 = A wins, 0 = draw, -1 = B wins.

#eval phase2AValue (allPlacements 3 3) [9] [4] true    -- X vs P, 3x3, A first
#eval phase2AValue (allPlacements 3 3) [9] [4] false   -- X vs P, 3x3, B first
#eval phase2AValue (allPlacements 4 4) [4, 9] [6, 7] true   -- {P,X} vs {U,V}, 4x4, A first
#eval phase2AValue (allPlacements 4 4) [4, 9] [6, 7] false  -- {P,X} vs {U,V}, 4x4, B first
#eval phase2AValue (allPlacements 4 3) [6] [7] true    -- U vs V, 4x3, A first
#eval phase2AValue (allPlacements 4 3) [6] [7] false   -- U vs V, 4x3, B first

-- Draw check: two 1x1-sized... pentominoes don't fit on a board with no
-- room to spare unless the board is sized exactly to fit both pieces with
-- nothing left over and neither ever blocks the other. A 5x2 board fits I
-- (5x1) exactly in one column, leaving the other column free for a second
-- I: both pieces always place, both hands empty at the end -> draw.
#eval phase2AValue (allPlacements 5 2) [1] [1] true    -- I vs I, 5x2, forced draw either order
#eval phase2AValue (allPlacements 5 2) [1] [1] false

#eval firstPlayerValueOn 3 3 [9, 4] true    -- draft {X,P} on 3x3, phase2 first mover = A
#eval firstPlayerValueOn 3 3 [9, 4] false   -- draft {X,P} on 3x3, phase2 first mover = B
#eval firstPlayerValueOn 5 4 [4, 6, 7, 9] true   -- draft {P,U,V,X} on 5x4
#eval firstPlayerValueOn 5 4 [4, 6, 7, 9] false
