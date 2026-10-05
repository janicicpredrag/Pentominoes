"""Checks the 14 drafting rules of the 1993 program against the solved draft.

For each rule "if <condition> then piece T gets a higher weight", we look at
all draft positions in which T is still in the pool and not all picks are
equally good, and compare how often picking T is optimal (by exact minimax
over the draft, with the 924 solved placement games as leaves) when the
condition holds and when it does not. Both variants (P0 or P1 places first).
"""
import os
from functools import lru_cache
NAMES = "FILNPTUVWXYZ"
idx = {c: i for i, c in enumerate(NAMES)}
def mask(s): return sum(1 << idx[c] for c in s)
w = {}  # value for the player who places first, holding the hand
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "placement_8x8.tsv")):
    if l.startswith("#") or l.startswith("first_hand"): continue
    r = l.rstrip("\n").split("\t")
    w[mask(r[0])] = int(r[2])
FULL = (1 << 12) - 1

def solve(variant):
    @lru_cache(maxsize=None)
    def val(h0, h1):  # value for the player to pick
        pool = FULL & ~(h0 | h1)
        mover = bin(h0 | h1).count("1") % 2  # 0 = P0
        if not pool:
            v0 = w[h0] if variant == 0 else -w[h1]
            return v0 if mover == 0 else -v0
        best = -2
        for t in range(12):
            if pool >> t & 1:
                c = -(val(h0 | 1 << t, h1) if mover == 0 else val(h0, h1 | 1 << t))
                best = max(best, c)
        return best
    stats = {}
    seen = set()
    def walk(h0, h1):
        if (h0, h1) in seen: return
        seen.add((h0, h1))
        pool = FULL & ~(h0 | h1)
        if not pool: return
        mover = bin(h0 | h1).count("1") % 2
        own, opp = (h0, h1) if mover == 0 else (h1, h0)
        child = {}
        for t in range(12):
            if pool >> t & 1:
                child[t] = -(val(h0 | 1 << t, h1) if mover == 0 else val(h0, h1 | 1 << t))
        best = max(child.values())
        if min(child.values()) < best:  # positions where the choice matters
            yield_pos.append((own, opp, pool, {t for t, v in child.items() if v == best}))
        for t in child:
            if mover == 0: walk(h0 | 1 << t, h1)
            else: walk(h0, h1 | 1 << t)
    yield_pos = []
    walk(0, 0)
    return yield_pos

def has(m, c): return m >> idx[c] & 1
rules = [
 ("opponent holds P", "U", 6, lambda o, p: has(p, "P")),
 ("opponent holds P", "L", 15, lambda o, p: has(p, "P")),
 ("own hand has P", "W", 6, lambda o, p: has(o, "P")),
 ("own hand has P", "L", 5, lambda o, p: has(o, "P")),
 ("own hand has U or P", "F", 2, lambda o, p: has(o, "U") or has(o, "P")),
 ("opponent holds U", "P", 2, lambda o, p: has(p, "U")),
 ("own hand has U", "Y", 4, lambda o, p: has(o, "U")),
 ("opponent holds V", "L", 2, lambda o, p: has(p, "V")),
 ("V is taken", "Z", 6, lambda o, p: has(o, "V") or has(p, "V")),
 ("opponent holds Y", "L", 2, lambda o, p: has(p, "Y")),
 ("opponent holds L", "Y", 5, lambda o, p: has(p, "L")),
 ("opponent holds L", "V", 7, lambda o, p: has(p, "L")),
 ("opponent holds L", "N", 7, lambda o, p: has(p, "L")),
 ("own hand has L", "V", 7, lambda o, p: has(o, "L")),
 ("own hand has L", "N", 10, lambda o, p: has(o, "L")),
 ("opponent holds L and V", "I", 45, lambda o, p: has(p, "L") and has(p, "V")),
 ("own hand has L and V", "I", 20, lambda o, p: has(o, "L") and has(o, "V")),
 ("opponent holds I and L", "V", 15, lambda o, p: has(p, "I") and has(p, "L")),
 ("own hand has I and L", "V", 8, lambda o, p: has(o, "I") and has(o, "L")),
]
for variant, title in ((0, "P0 places first"), (1, "P1 places first")):
    pos = solve(variant)
    print(f"\n{title}: {len(pos)} draft positions in which the choice matters")
    print(f"{'condition':24s} {'piece':5s} {'x':>3s}   {'optimal if cond':>15s}   {'otherwise':>9s}")
    for c, T, m, f in rules:
        t = idx[T]
        a = [t in opt for own, opp, pool, opt in pos if pool >> t & 1 and f(own, opp)]
        b = [t in opt for own, opp, pool, opt in pos if pool >> t & 1 and not f(own, opp)]
        pa, pb = 100 * sum(a) / len(a), 100 * sum(b) / len(b)
        print(f"{c:24s} {T:5s} {m:3d}   {pa:6.1f}% ({len(a):6d})   {pb:6.1f}%   {'agrees' if pa > pb else 'DISAGREES'}")
