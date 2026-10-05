"""Simple drafting strategies for P0 against a perfect P1.

P0 always takes the first remaining piece of a fixed order: by strength
(Table 3 of the paper, with all four ways of breaking the ties V/Y and U/N),
or by the number of placements on the empty board. P1 plays perfectly
(minimax over the draft, with the 924 solved placement games as leaves).
Also reported: how often P0 wins against a P1 that picks at random.
"""
import random
import os
from functools import lru_cache

NAMES = "FILNPTUVWXYZ"
idx = {c: i for i, c in enumerate(NAMES)}
FULL = (1 << 12) - 1


def mask(s):
    return sum(1 << idx[c] for c in s)


w = {}  # value of the placement game for the player who places first, holding the hand
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "placement_8x8.tsv")):
    if line.startswith("#") or line.startswith("first_hand"):
        continue
    r = line.rstrip("\n").split("\t")
    w[mask(r[0])] = int(r[2])

PLACEMENTS = {"F": 288, "I": 64, "L": 280, "P": 336, "N": 280, "T": 144, "U": 168,
              "V": 144, "W": 144, "X": 36, "Y": 280, "Z": 144}
ORDERS = {
    "strength (L P V Y U N I F W T Z X)": "LPVYUNIFWTZX",
    "strength (L P Y V U N I F W T Z X)": "LPYVUNIFWTZX",
    "strength (L P V Y N U I F W T Z X)": "LPVYNUIFWTZX",
    "strength (L P Y V N U I F W T Z X)": "LPYVNUIFWTZX",
    "placements": "".join(sorted(NAMES, key=lambda c: (-PLACEMENTS[c], NAMES.index(c)))),
}
RESULT = {1: "P0 wins", 0: "draw", -1: "P1 wins"}

for variant, title in ((0, "P0 places first"), (1, "P1 places first")):
    def leaf(h0, h1):  # value for P0
        return w[h0] if variant == 0 else -w[h1]
    for name, order in ORDERS.items():
        def greedy(pool):
            return next(idx[c] for c in order if pool >> idx[c] & 1)

        @lru_cache(maxsize=None)
        def val(h0, h1):  # P0 follows the order, P1 minimises P0's value
            pool = FULL & ~(h0 | h1)
            if not pool:
                return leaf(h0, h1)
            if bin(h0 | h1).count("1") % 2 == 0:
                return val(h0 | 1 << greedy(pool), h1)
            return min(val(h0, h1 | 1 << t) for t in range(12) if pool >> t & 1)

        rng = random.Random(1)
        wins, n = 0, 20000
        for _ in range(n):
            h0 = h1 = 0
            for k in range(12):
                pool = FULL & ~(h0 | h1)
                if k % 2 == 0:
                    h0 |= 1 << greedy(pool)
                else:
                    h1 |= 1 << rng.choice([t for t in range(12) if pool >> t & 1])
            wins += leaf(h0, h1) == 1
        print(f"{title}, P0 by {name}: against perfect P1 {RESULT[val(0, 0)]}, "
              f"against random P1 P0 wins {100 * wins / n:.0f}%")
