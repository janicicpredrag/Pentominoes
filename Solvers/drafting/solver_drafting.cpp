// Fast solver for the two-phase pentomino game, matching the semantics of
// Solvers/Pentomino.lean (cross-checked against it on small cases).
//
// Board cells are bit indices r*W+c in a uint64_t (W,H <= 8 so it fits).
// Piece ids 0..11 = F,I,L,N,P,T,U,V,W,X,Y,Z, same order as baseShapes in
// Solvers/Pentomino.lean.
//
// Phase 2 state = (occupied bitboard, remaining-piece-mask for A, for B,
// turn). The transposition table is GLOBAL and shared across every
// division of pieces we solve phase 2 for, since a state's future value
// depends only on (occupied, remA, remB, turn) -- not on how the players
// got there. This sharing is what gives the full 8x8/12-piece attempt any
// chance of finishing.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using u64 = uint64_t;
using Cell = std::pair<int,int>;

static std::vector<std::vector<Cell>> baseShapes = {
    /* F */ {{0,1},{0,2},{1,0},{1,1},{2,1}},
    /* I */ {{0,0},{1,0},{2,0},{3,0},{4,0}},
    /* L */ {{0,0},{1,0},{2,0},{3,0},{3,1}},
    /* N */ {{0,1},{1,1},{2,0},{2,1},{3,0}},
    /* P */ {{0,0},{0,1},{1,0},{1,1},{2,0}},
    /* T */ {{0,0},{0,1},{0,2},{1,1},{2,1}},
    /* U */ {{0,0},{0,2},{1,0},{1,1},{1,2}},
    /* V */ {{0,0},{1,0},{2,0},{2,1},{2,2}},
    /* W */ {{0,0},{1,0},{1,1},{2,1},{2,2}},
    /* X */ {{0,1},{1,0},{1,1},{1,2},{2,1}},
    /* Y */ {{0,1},{1,0},{1,1},{2,1},{3,1}},
    /* Z */ {{0,0},{0,1},{1,1},{2,1},{2,2}},
};
static const char *NAMES = "FILNPTUVWXYZ";

static std::vector<Cell> normalize(std::vector<Cell> c) {
    int rmin = 1000, cmin = 1000;
    for (auto &p : c) { rmin = std::min(rmin, p.first); cmin = std::min(cmin, p.second); }
    for (auto &p : c) { p.first -= rmin; p.second -= cmin; }
    std::sort(c.begin(), c.end());
    return c;
}

static std::vector<std::vector<Cell>> orientationsOf(std::vector<Cell> cells) {
    std::vector<std::vector<Cell>> result;
    for (int refl = 0; refl < 2; refl++) {
        auto p = cells;
        if (refl) for (auto &pt : p) pt.second = -pt.second;
        for (int rot = 0; rot < 4; rot++) {
            std::vector<Cell> np;
            for (auto &pt : p) np.push_back({pt.second, -pt.first});
            p = np;
            auto norm = normalize(p);
            if (std::find(result.begin(), result.end(), norm) == result.end())
                result.push_back(norm);
        }
    }
    return result;
}

int W = 8, H = 8;

// placements[piece] = list of bitboards.
// placementAdj[piece][k] = bitboard of board cells orthogonally adjacent to
// placements[piece][k]'s footprint but not part of it -- precomputed once
// so the move-ordering heuristic below (rank candidate moves by how much
// free space they still border) is just a popcount-and-mask at each node,
// not a per-node neighbor scan.
static std::array<std::vector<u64>, 12> placements;
static std::array<std::vector<u64>, 12> placementAdj;
// placementIndex[piece][mask] = k such that placements[piece][k] == mask
// (used by the history heuristic to find the index of a killer move).
static std::unordered_map<u64, int> placementIndex[12];

static void buildPlacements() {
    static const int dRow[4] = {-1, 1, 0, 0};
    static const int dCol[4] = {0, 0, -1, 1};
    for (int i = 0; i < 12; i++) {
        placements[i].clear();
        placementAdj[i].clear();
        for (auto &orient : orientationsOf(baseShapes[i])) {
            int rowspan = 0, colspan = 0;
            for (auto &p : orient) { rowspan = std::max(rowspan, p.first); colspan = std::max(colspan, p.second); }
            for (int dr = 0; dr + rowspan < H; dr++)
                for (int dc = 0; dc + colspan < W; dc++) {
                    u64 mask = 0;
                    for (auto &p : orient) mask |= (1ULL << ((dr + p.first) * W + (dc + p.second)));
                    u64 adj = 0;
                    for (auto &p : orient) {
                        int r = dr + p.first, c = dc + p.second;
                        for (int k = 0; k < 4; k++) {
                            int nr = r + dRow[k], nc = c + dCol[k];
                            if (nr >= 0 && nr < H && nc >= 0 && nc < W) adj |= (1ULL << (nr * W + nc));
                        }
                    }
                    adj &= ~mask; // exclude the shape's own cells
                    placementIndex[i][mask] = (int)placements[i].size();
                    placements[i].push_back(mask);
                    placementAdj[i].push_back(adj);
                }
        }
    }
}

// Board symmetries (dihedral group of the square, or its order-4
// subgroup for a non-square board): used ONLY to canonicalize the
// transposition-table key. The recursive search itself always simulates
// moves on the real `occ` -- only the cache lookup/store key is replaced
// by min-over-symmetries, so states that are rotations/reflections of
// each other share one cache entry. This can't change correctness (the
// game value of a position is invariant under whole-board symmetry, since
// `placements[piece]` already contains every orientation at every
// position), only how often the cache hits. It's cheapest exactly when it
// matters most: cost is O(popcount(occ)) per lookup, and popcount(occ) is
// smallest in the wide-open early game, which is exactly where symmetry
// redundancy (and the previous heuristics' blind spot) is largest.
static int numSyms;
static std::array<std::array<int, 64>, 8> symPerm;

static void buildSymmetries() {
    numSyms = 0;
    auto tryAdd = [&](auto f) {
        std::array<int, 64> perm{};
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++) {
                auto [nr, nc] = f(r, c);
                if (nr < 0 || nr >= H || nc < 0 || nc >= W) return; // doesn't map grid to itself
                perm[r * W + c] = nr * W + nc;
            }
        symPerm[numSyms++] = perm;
    };
    tryAdd([](int r, int c) { return std::pair{r, c}; });                   // identity
    tryAdd([](int r, int c) { return std::pair{r, W - 1 - c}; });           // flip H
    tryAdd([](int r, int c) { return std::pair{H - 1 - r, c}; });           // flip V
    tryAdd([](int r, int c) { return std::pair{H - 1 - r, W - 1 - c}; });   // rot180
    tryAdd([](int r, int c) { return std::pair{c, r}; });                   // transpose (only if W==H)
    tryAdd([](int r, int c) { return std::pair{c, H - 1 - r}; });           // rot90
    tryAdd([](int r, int c) { return std::pair{W - 1 - c, r}; });           // rot270
    tryAdd([](int r, int c) { return std::pair{W - 1 - c, H - 1 - r}; });   // anti-diagonal
    fprintf(stderr, "symmetries: %d\n", numSyms);
}

// Above this many occupied cells, skip symmetry minimization and use the
// raw occupancy as the cache key. Symmetry redundancy (multiple reachable
// states that are rotations/reflections of each other) is largest in the
// wide-open early game and shrinks fast as pieces pile up and break the
// board's symmetry -- but canonOcc's cost (O(popcount(occ)) per symmetry,
// times up to 8 symmetries) grows the *other* way. Measured on the 8x8
// full run: 9% table-hit rate against 65% collision rate, meaning most of
// that per-node canonicalization cost was being paid and then clobbered
// before ever being reused. Capping it to early-game only keeps the payoff
// and cuts the wasted work.
static int canonThreshold = 15;

static inline u64 canonOcc(u64 occ) {
    if (__builtin_popcountll(occ) > canonThreshold) return occ;
    u64 best = occ;
    for (int s = 1; s < numSyms; s++) {
        u64 t = 0, x = occ;
        while (x) {
            int bit = __builtin_ctzll(x);
            x &= x - 1;
            t |= (1ULL << symPerm[s][bit]);
        }
        if (t < best) best = t;
    }
    return best;
}

// Fixed-size, direct-mapped transposition table: memory is capped at
// (1 << tableBits) * sizeof(Slot) regardless of how many distinct states
// get visited. A collision just overwrites the old entry (no chaining, no
// eviction bookkeeping) -- we trade some recomputation for a hard memory
// ceiling, since the state space is far too large to memoize exhaustively
// at 8x8 (a single division alone passed 68M distinct states before
// exhausting a 4GB unordered_map).
struct Slot {
    u64 occ;
    uint32_t rem;   // (remA << 13) | (remB << 1) | turn
    uint8_t state;  // 0 = empty, 1 = loss(-1), 2 = draw(0), 3 = win(+1)
};

static inline int8_t stateToValue(uint8_t state) { return (int8_t)state - 2; }
static inline uint8_t valueToState(int8_t value) { return (uint8_t)(value + 2); }
static std::vector<Slot> table;
static u64 tableMask;
// A/B lever for measuring whether the transposition table is worth its
// per-node overhead (canonOcc + hash + a likely cache-missing memory
// access) at all, versus just letting the killer-move heuristic and move
// ordering do the pruning. When false, moverValue skips canonOcc, the
// lookup, and the store entirely -- not just "use a tiny table" (which
// would still pay the full per-node cost and just collide constantly).
static bool ttEnabled = true;

static void initTable(int tableBits) {
    size_t n = size_t(1) << tableBits;
    table.assign(n, Slot{0, 0, 0});
    tableMask = n - 1;
    fprintf(stderr, "table: 2^%d = %zu slots, %.2f GB\n",
            tableBits, n, n * sizeof(Slot) / 1e9);
}

static inline size_t slotIndex(u64 occ, uint32_t rem) {
    u64 h = occ * 0x9E3779B97F4A7C15ULL + rem;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (size_t)(h & tableMask);
}

static uint64_t nodeCount = 0, hitCount = 0, collisionCount = 0;
static std::chrono::steady_clock::time_point startTime;
static uint64_t lastReport = 0;

static void maybeReport() {
    if ((nodeCount & 0xFFFFFF) != 0) return; // every ~16M nodes
    if (nodeCount == lastReport) return;
    lastReport = nodeCount;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    fprintf(stderr, "  progress: nodes=%llu hits=%llu collisions=%llu t=%.1fs\n",
            (unsigned long long)nodeCount, (unsigned long long)hitCount,
            (unsigned long long)collisionCount, t);
}

// Killer-move table, indexed by "pieces remaining" (a monotonically
// decreasing proxy for ply/depth shared across every division and every
// branch of the search, the same way a chess engine indexes killers by
// depth-from-root). Records the last (piece, placement) that caused a
// cutoff at that stage of the game; since sibling positions at the same
// stage are often still similar (same rough occupancy density, often
// overlapping legal moves), replaying that move first is disproportionately
// likely to cut off again immediately, without ever building the
// constrained-piece ordering or touching the transposition table.
static constexpr int MAX_REMAINING = 25;
// -1 = "no killer recorded for this stage yet". Must be set explicitly --
// these are plain static arrays, so C++ zero-initializes them, and 0 is
// piece F's real index -- leaving the default at 0 (with killerMask
// defaulting to 0, an empty bitboard) previously made the very first visit
// to any "remaining" bucket treat "place piece F on no cells at all" as a
// legal move, since an empty mask trivially satisfies `!(kMask & occ)`.
// That phantom move let a player discard piece F for free without
// covering any board cells, corrupting the search whenever F was still in
// hand the first time its bucket was reached.
//
// Each stage keeps a list of up to `numKillers` killer moves (default 5,
// set with PENTOMINO_KILLERS), most recent first. A move that wins for the
// mover is moved to (or inserted at) the front of its stage's list, pushing
// the least recently successful killer off the end.
static constexpr int MAX_KILLERS = 64;
static int numKillers = 5;
// Move ordering: 0 = free cells bordering the placement only; 1 = that plus
// the cells only the mover can still cover (see moverValue). Set with
// PENTOMINO_ORDER.
static int orderMode = 1;
// The extra term of orderMode 1 is computed only at nodes where at least
// this many pieces are left in both hands together (it is costly, and good
// ordering matters most near the root). Set with PENTOMINO_ORDER_MIN_PIECES.
static int orderMinPieces = 9;

// History heuristic: hist[piece][k] accumulates remaining^2 every time the
// k-th placement of `piece` causes a cutoff (a win, or a result good enough
// for the alpha-beta window), anywhere in the tree and across splits. With
// historyMode 1 moves are sorted by history and then by the score above;
// with historyMode 2 by the score and then by history; 0 disables it.
// Set with PENTOMINO_HISTORY.
static int historyMode = 0;
// Key used by orderMode 5 below the root at nodes with >= orderMinPieces
// pieces left (set with PENTOMINO_ORDER_KEY): 1 = mob, 2 = mobB, 3 = blend,
// 4 = diff. See moverValue.
static int orderKey = 1;
// Weight of B in orderMode 1's score A + bWeight * B (PENTOMINO_B_WEIGHT).
static int bWeight = 1;
// Weight of A in orderMode 1's score at nodes where B is used (PENTOMINO_A_WEIGHT).
static int aWeight = 1;
// Optional extra terms of orderMode 1 at nodes with >= orderMinPieces pieces
// left (all 0 = off): cWeight * C, where C = (cells the mover can still
// cover) - (cells the opponent can still cover); deadWeight * (opponent pieces
// the move leaves without a legal placement - own pieces it does so to), plus
// a large bonus for a move after which the opponent cannot move at all; and
// scarceWeight * (reduction in legal placements of the opponent's piece with
// the fewest). Set with PENTOMINO_C_WEIGHT, PENTOMINO_DEAD_W, PENTOMINO_SCARCE_W.
static int cWeight = 0, deadWeight = 0, scarceWeight = 0;
// Piece bonus (PENTOMINO_PIECE_BONUS): 0 = off, 1 = added to the score at
// nodes with >= orderMinPieces pieces left, 2 = at all nodes. The bonus ranks
// the pieces from strong (1) to weak (10) by how often their holder won the
// placement games solved so far, so that weak pieces are placed first
// (order F I L N P T U V W X Y Z; Y and Z, not yet seen, get the middle value).
static int pieceBonusMode = 0;
static int pieceBonus[12] = {8, 5, 1, 6, 2, 9, 4, 3, 7, 10, 5, 5}; // PENTOMINO_PIECE_RANKS overrides
static constexpr int MAX_PLACEMENTS = 512;
static uint64_t hist[12][MAX_PLACEMENTS];
static inline void addHistory(int piece, u64 mask, int remaining) {
    if (!historyMode) return;
    auto it = placementIndex[piece].find(mask);
    if (it != placementIndex[piece].end()) hist[piece][it->second] += (uint64_t)remaining * remaining;
}
static int killerPiece[MAX_REMAINING][MAX_KILLERS];
static u64 killerMask[MAX_REMAINING][MAX_KILLERS];
static void initKillers() {
    for (int i = 0; i < MAX_REMAINING; i++)
        for (int j = 0; j < MAX_KILLERS; j++) { killerPiece[i][j] = -1; killerMask[i][j] = 0; }
}
static void recordKiller(int remaining, int piece, u64 mask) {
    int *kp = killerPiece[remaining];
    u64 *km = killerMask[remaining];
    int j = 0;
    while (j < numKillers - 1 && !(kp[j] == piece && km[j] == mask)) j++;
    for (; j > 0; j--) { kp[j] = kp[j - 1]; km[j] = km[j - 1]; }
    kp[0] = piece; km[0] = mask;
}

static const char *valueLabel(int v) { return v > 0 ? "WINS" : v < 0 ? "loses" : "DRAWS"; }

// Returns the value (1 = win, 0 = draw, -1 = loss) for the player to move
// (encoded by `turn`, true = A) from this phase-2 state, given a negamax
// alpha-beta window (alpha,beta in {-1,0,1}): the true value only needs to
// be pinned down exactly if it falls strictly between alpha and beta,
// otherwise a move that already proves "at least beta" or "at most alpha"
// lets every sibling still unexplored be skipped, since it can no longer
// change what the parent picks. Callers that need the true, un-bounded
// value (the two top-level oracles below) always pass the full window
// (-1, 1), under which this function can never return anything but the
// exact value -- alpha can only ever reach beta by `best` reaching the
// domain's true max, 1.
//
// A position with no legal moves is a draw exactly when the mover's own
// pool (`myPool`) is already empty -- both pools start equal at 6/6 and
// turns strictly alternate, so one can only run dry when the other has
// too, i.e. the whole board got tiled; a nonempty pool with nothing left
// to place is a genuine block, and the mover loses under normal play.
//
// `isRoot` turns on move-by-move progress reporting for the top-level
// call: since we know the exact number of legal root moves up front,
// "i/N tried" is a real, meaningful completion fraction -- unlike the
// open-ended node counter, which has no known total to compare against.
// Because the root is always searched with the full window, every value
// printed there is exact; a value printed for some other, narrow-window
// call elsewhere in the tree would only be a bound, but isRoot is only
// ever set on the outermost, full-window call.
// `*exactOut` is set to whether the returned value is the true value of the
// position, as opposed to only a bound implied by the alpha-beta window. A
// node is exact iff it found a win whose child value was exact, or it
// searched every move and every child value was exact. Only exact values are
// stored in the transposition table.
static int moverValue(u64 occ, uint32_t remA, uint32_t remB, bool turn, int alpha, int beta, bool *exactOut, bool isRoot = false) {
    nodeCount++;
    maybeReport();
    uint32_t rem = 0;
    u64 cocc = 0;
    size_t idx = 0;
    bool hadEntry = false;
    if (ttEnabled) {
        rem = (remA << 13) | (remB << 1) | (turn ? 1 : 0);
        cocc = canonOcc(occ); // cache key only -- simulation below still uses real `occ`
        idx = slotIndex(cocc, rem);
        const Slot &slot = table[idx];
        // Cached entries are only ever stored once proven exact (see the
        // `exact` bookkeeping below), so a hit is always safe to return
        // as-is, regardless of the caller's window -- no bound/flag
        // machinery needed, unlike a general alpha-beta transposition table.
        if (slot.state != 0 && slot.occ == cocc && slot.rem == rem) {
            hitCount++;
            *exactOut = true;
            return stateToValue(slot.state);
        }
        hadEntry = slot.state != 0;
    }

    uint32_t myPool = turn ? remA : remB;
    int remaining = __builtin_popcount(remA) + __builtin_popcount(remB);

    bool anyMove = false;
    int best = -1;
    bool allChildrenExact = true; // every child value seen so far was exact
    bool exactWin = false;        // some exact child value proved a win (+1)

    int rootDone = 0, rootTotal = 0;
    bool cutoff = false;

    // Try this stage's killer moves first, most recent first, skipping any
    // that are not legal here. The list is copied because the search below
    // may reorder it.
    int kPieces[MAX_KILLERS];
    u64 kMasks[MAX_KILLERS];
    int nTried = 0; // killers actually tried, kept in kPieces/kMasks[0..nTried)
    {
        int cp[MAX_KILLERS];
        u64 cm[MAX_KILLERS];
        memcpy(cp, killerPiece[remaining], sizeof(int) * numKillers);
        memcpy(cm, killerMask[remaining], sizeof(u64) * numKillers);
        for (int j = 0; j < numKillers && !cutoff; j++) {
            int kPiece = cp[j];
            u64 kMask = cm[j];
            if (kPiece < 0 || !(myPool & (1u << kPiece)) || (kMask & occ)) continue;
            kPieces[nTried] = kPiece; kMasks[nTried] = kMask; nTried++;
            anyMove = true;
            uint32_t nrA = turn ? (remA & ~(1u << kPiece)) : remA;
            uint32_t nrB = turn ? remB : (remB & ~(1u << kPiece));
            bool childExact;
            int val = -moverValue(occ | kMask, nrA, nrB, !turn, -beta, -alpha, &childExact);
            allChildrenExact = allChildrenExact && childExact;
            if (val >= 1 && childExact) exactWin = true;
            if (val > best) best = val;
            if (val >= 1) recordKiller(remaining, kPiece, kMask);
            if (best > alpha) alpha = best;
            if (isRoot) {
                rootDone++;
                fprintf(stderr, "  root move (killer %d) piece=%c -> mover %s | nodes=%llu t=%.1fs\n",
                        j + 1, NAMES[kPiece], valueLabel(val),
                        (unsigned long long)nodeCount,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count());
            }
            if (alpha >= beta) { cutoff = true; addHistory(kPiece, kMask, remaining); }
        }
    }

    if (!cutoff) {
        // Flat move ordering: pool every legal (piece, placement) move from
        // the whole hand into one list, ranked by how much free board space
        // it still borders -- decreasing count of free cells orthogonally
        // adjacent to the placement's footprint (placementAdj, precomputed
        // in buildPlacements). Moves that hug more open space are tried
        // first, on the theory that they're less likely to immediately wall
        // off a chunk of board that some later piece needs.
        //
        // With orderMode 1 (the default), at nodes with at least
        // orderMinPieces pieces left, the score of a move also counts
        // the cells that, after the move, the mover can still cover with one
        // of their remaining pieces but the opponent cannot cover with any of
        // theirs -- territory that stays reserved for the mover.
        struct MoveOpt { int piece; u64 mask; int score; int idx; };
        static constexpr int MAX_MOVES = 4096; // worst case: a 6-piece hand on an empty 8x8 board is well under this
        MoveOpt moves[MAX_MOVES];
        int nMoves = 0;
        {
            uint32_t pool = myPool;
            while (pool) {
                int piece = __builtin_ctz(pool);
                pool &= pool - 1;
                const auto &pm = placements[piece];
                const auto &pa = placementAdj[piece];
                for (size_t k = 0; k < pm.size(); k++) {
                    u64 mask = pm[k];
                    if (mask & occ) continue;
                    bool tried = false; // already tried above as a killer
                    for (int j = 0; j < nTried && !tried; j++)
                        tried = kPieces[j] == piece && kMasks[j] == mask;
                    if (tried) continue;
                    if (nMoves < MAX_MOVES)
                        moves[nMoves++] = {piece, mask, __builtin_popcountll(pa[k] & ~occ) +
                            (pieceBonusMode == 2 || (pieceBonusMode == 1 && remaining >= orderMinPieces) ? pieceBonus[piece] : 0),
                            (int)k};
                }
            }
        }
        // orderMode 1: score = A + B at nodes with >= orderMinPieces pieces left,
        //              A elsewhere.
        // orderMode 2: score = A at nodes with > orderMinPieces pieces left,
        //              B alone at nodes with <= orderMinPieces pieces left.
        // (A = empty cells bordering the placement, B = cells only the mover
        // can still cover after the move.)
        // orderMode 3: at nodes with >= orderMinPieces pieces left, sort by the
        //              opponent's mobility after the move (number of legal
        //              placements the opponent still has; fewest first), with
        //              A breaking ties; A elsewhere.
        // orderMode 6: score = A + C at nodes with >= orderMinPieces pieces
        // left, A elsewhere, where C = (cells the mover can still cover with
        // their remaining pieces) - (cells the opponent can still cover).
        bool useB = orderMode == 1 ? remaining >= orderMinPieces
                  : orderMode == 2 ? remaining <= orderMinPieces
                  : orderMode == 6 ? remaining >= orderMinPieces : false;
        // orderMode 4: A at nodes with >= orderMinPieces pieces left (near the
        //              root), opponent's mobility (fewest first, A breaking
        //              ties) at the deeper nodes.
        bool useMobility = orderMode == 3 ? remaining >= orderMinPieces
                         : orderMode == 4 ? remaining < orderMinPieces : false;
        if (useMobility && nMoves > 1) {
            static constexpr int MAX_PL = 2 * MAX_MOVES;
            u64 opMask[MAX_PL];
            int nOp = 0;
            uint32_t opPool = turn ? remB : remA;
            while (opPool) {
                int piece = __builtin_ctz(opPool);
                opPool &= opPool - 1;
                for (u64 mask : placements[piece])
                    if (!(mask & occ) && nOp < MAX_PL) opMask[nOp++] = mask;
            }
            for (int i = 0; i < nMoves; i++) {
                u64 m = moves[i].mask;
                int mobility = 0;
                for (int j = 0; j < nOp; j++) mobility += !(opMask[j] & m);
                // lexicographic: fewer opponent placements first, then larger A
                moves[i].score = -64 * mobility + moves[i].score;
            }
        }
        // orderMode 5: A + B at the root; below the root, at nodes with >=
        // orderMinPieces pieces left, one of these keys (orderKey), and A at
        // the other nodes:
        //   1 mob   fewest opponent placements first, then A
        //   2 mobB  fewest opponent placements, then B, then A
        //   3 blend 8*(A + B) - opponent placements
        //   4 diff  most (own placements - opponent placements), then A
        int key5 = orderMode != 5 ? -1 : isRoot ? 0 : remaining >= orderMinPieces ? orderKey : -1;
        if (key5 >= 0 && nMoves > 1) {
            static constexpr int MAX_PL = 2 * MAX_MOVES;
            u64 myMask[MAX_MOVES + MAX_KILLERS];
            int myPiece[MAX_MOVES + MAX_KILLERS];
            int nMy = 0;
            for (int j = 0; j < nTried; j++) { myMask[nMy] = kMasks[j]; myPiece[nMy++] = kPieces[j]; }
            for (int j = 0; j < nMoves; j++) { myMask[nMy] = moves[j].mask; myPiece[nMy++] = moves[j].piece; }
            u64 opMask[MAX_PL];
            int nOp = 0;
            uint32_t opPool = turn ? remB : remA;
            while (opPool) {
                int piece = __builtin_ctz(opPool);
                opPool &= opPool - 1;
                for (u64 mask : placements[piece])
                    if (!(mask & occ) && nOp < MAX_PL) opMask[nOp++] = mask;
            }
            bool needB = key5 == 0 || key5 == 2 || key5 == 3;
            for (int i = 0; i < nMoves; i++) {
                u64 m = moves[i].mask;
                int q = moves[i].piece;
                int a = moves[i].score;
                u64 mine = 0, theirs = 0;
                int opMob = 0, myMob = 0;
                for (int j = 0; j < nOp; j++) {
                    u64 alive = 0 - (u64)!(opMask[j] & m);
                    theirs |= opMask[j] & alive;
                    opMob += alive & 1;
                }
                if (needB || key5 == 4)
                    for (int j = 0; j < nMy; j++) {
                        u64 alive = 0 - (u64)(myPiece[j] != q && !(myMask[j] & m));
                        mine |= myMask[j] & alive;
                        myMob += alive & 1;
                    }
                int b = needB ? __builtin_popcountll(mine & ~theirs) : 0;
                switch (key5) {
                    case 0: moves[i].score = a + b; break;
                    case 1: moves[i].score = -64 * opMob + a; break;
                    case 2: moves[i].score = -4096 * opMob + 64 * b + a; break;
                    case 3: moves[i].score = 8 * (a + b) - opMob; break;
                    case 4: moves[i].score = 64 * (myMob - opMob) + a; break;
                }
            }
        }
        if (useB && nMoves > 1) {
            // All legal placements of the mover (the killers tried above
            // included) and of the opponent in the current position.
            static constexpr int MAX_PL = 2 * MAX_MOVES;
            u64 myMask[MAX_MOVES + MAX_KILLERS];
            int myPiece[MAX_MOVES + MAX_KILLERS];
            int nMy = 0;
            for (int j = 0; j < nTried; j++) { myMask[nMy] = kMasks[j]; myPiece[nMy++] = kPieces[j]; }
            for (int j = 0; j < nMoves; j++) { myMask[nMy] = moves[j].mask; myPiece[nMy++] = moves[j].piece; }
            u64 opMask[MAX_PL];
            int opPiece[MAX_PL];
            int nOp = 0;
            uint32_t opPool = turn ? remB : remA;
            while (opPool) {
                int piece = __builtin_ctz(opPool);
                opPool &= opPool - 1;
                for (u64 mask : placements[piece])
                    if (!(mask & occ) && nOp < MAX_PL) { opPiece[nOp] = piece; opMask[nOp++] = mask; }
            }
            bool extras = orderMode == 1 && (cWeight || deadWeight || scarceWeight);
            int opBefore[12] = {0}, myBefore[12] = {0}, scarcest = -1;
            if (extras) {
                for (int j = 0; j < nOp; j++) opBefore[opPiece[j]]++;
                for (int j = 0; j < nMy; j++) myBefore[myPiece[j]]++;
                for (int p = 0; p < 12; p++)
                    if (opBefore[p] > 0 && (scarcest < 0 || opBefore[p] < opBefore[scarcest])) scarcest = p;
            }
            uint32_t opHand = turn ? remB : remA;
            for (int i = 0; i < nMoves; i++) {
                u64 m = moves[i].mask;
                int q = moves[i].piece;
                u64 mine = 0, theirs = 0;
                int opAfter[12] = {0}, myAfter[12] = {0};
                if (extras) {
                    for (int j = 0; j < nMy; j++) {
                        u64 alive = (u64)(myPiece[j] != q && !(myMask[j] & m));
                        mine |= myMask[j] & (0 - alive);
                        myAfter[myPiece[j]] += (int)alive;
                    }
                    for (int j = 0; j < nOp; j++) {
                        u64 alive = (u64)!(opMask[j] & m);
                        theirs |= opMask[j] & (0 - alive);
                        opAfter[opPiece[j]] += (int)alive;
                    }
                } else {
                // Branch-free unions of the placements that survive the move.
                for (int j = 0; j < nMy; j++)
                    mine |= myMask[j] & (0 - (u64)(myPiece[j] != q && !(myMask[j] & m)));
                for (int j = 0; j < nOp; j++)
                    theirs |= opMask[j] & (0 - (u64)!(opMask[j] & m));
                }
                if (orderMode == 6) {
                    moves[i].score += __builtin_popcountll(mine) - __builtin_popcountll(theirs);
                    continue;
                }
                int b = __builtin_popcountll(mine & ~theirs);
                if (orderMode == 1 && aWeight != 1) {
                    int bonus = pieceBonusMode ? pieceBonus[q] : 0; // useB implies remaining >= orderMinPieces
                    moves[i].score = aWeight * (moves[i].score - bonus) + bonus;
                }
                moves[i].score = orderMode == 2 ? b : moves[i].score + bWeight * b;
                if (extras) {
                    int score = moves[i].score + cWeight * (__builtin_popcountll(mine) - __builtin_popcountll(theirs));
                    if (deadWeight) {
                        int opKills = 0, myKills = 0, opLeft = 0;
                        for (int p = 0; p < 12; p++) {
                            opKills += opBefore[p] > 0 && opAfter[p] == 0;
                            myKills += p != q && myBefore[p] > 0 && myAfter[p] == 0;
                            opLeft += opAfter[p];
                        }
                        score += deadWeight * (opKills - myKills);
                        if (opLeft == 0 && opHand) score += 1000000; // opponent cannot move: immediate win
                    }
                    if (scarceWeight && scarcest >= 0) score += scarceWeight * (opBefore[scarcest] - opAfter[scarcest]);
                    moves[i].score = score;
                }
            }
        }
        if (historyMode == 1)
            std::sort(moves, moves + nMoves, [](const MoveOpt &a, const MoveOpt &b) {
                uint64_t ha = hist[a.piece][a.idx], hb = hist[b.piece][b.idx];
                return ha != hb ? ha > hb : a.score > b.score; });
        else if (historyMode == 2)
            std::sort(moves, moves + nMoves, [](const MoveOpt &a, const MoveOpt &b) {
                return a.score != b.score ? a.score > b.score : hist[a.piece][a.idx] > hist[b.piece][b.idx]; });
        else
            std::sort(moves, moves + nMoves, [](const MoveOpt &a, const MoveOpt &b) { return a.score > b.score; });
        anyMove = anyMove || nMoves > 0;

        if (isRoot) rootTotal = rootDone + nMoves; // killers tried are already counted in rootDone

        for (int mi = 0; mi < nMoves; mi++) {
            int piece = moves[mi].piece;
            u64 mask = moves[mi].mask;
            uint32_t nrA = turn ? (remA & ~(1u << piece)) : remA;
            uint32_t nrB = turn ? remB : (remB & ~(1u << piece));
            bool childExact;
            int val = -moverValue(occ | mask, nrA, nrB, !turn, -beta, -alpha, &childExact);
            allChildrenExact = allChildrenExact && childExact;
            if (val >= 1 && childExact) exactWin = true;
            if (val > best) {
                best = val;
                if (val >= 1) recordKiller(remaining, piece, mask);
            }
            if (best > alpha) alpha = best;
            if (isRoot) {
                rootDone++;
                fprintf(stderr, "  root move %d/%d piece=%c -> mover %s | nodes=%llu t=%.1fs\n",
                        rootDone, rootTotal, NAMES[piece], valueLabel(val),
                        (unsigned long long)nodeCount,
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count());
            }
            if (alpha >= beta) {
                cutoff = true;
                if (historyMode) hist[piece][moves[mi].idx] += (uint64_t)remaining * remaining;
                break;
            }
        }
    }
    if (!anyMove) best = myPool == 0 ? 0 : -1;
    bool exact = exactWin || (!cutoff && allChildrenExact);
    *exactOut = exact;
    if (ttEnabled && exact) {
        if (hadEntry) collisionCount++;
        table[idx] = Slot{cocc, rem, valueToState((int8_t)best)};
    }
    return best;
}

static int phase2AValue(uint32_t poolA, uint32_t poolB, bool firstIsA) {
    bool exact;
    int mover = moverValue(0, poolA, poolB, firstIsA, -1, 1, &exact, /*isRoot=*/true);
    return firstIsA ? mover : -mover;
}

// ---- Phase 1: draft ----
// draftPool/poolA/poolB as 12-bit masks; turn derived from popcount.
// A (drafting on her turn) picks the child maximizing the eventual A-value;
// B picks the child minimizing it -- ordinary zero-sum minimax, now that a
// draw is a possible outcome and "A wins iff some/every child wins" no
// longer suffices.
static std::unordered_map<u64, int8_t> draftMemo; // key = (draftPool<<12)|poolA

static int phase1AValue(uint32_t draftPool, uint32_t poolA, uint32_t poolB, bool phase2FirstIsA) {
    if (draftPool == 0) return phase2AValue(poolA, poolB, phase2FirstIsA);
    u64 key = ((u64)draftPool << 12) | poolA;
    auto it = draftMemo.find(key);
    if (it != draftMemo.end()) return it->second;

    int drafted = 12 - __builtin_popcount(draftPool);
    bool turnA = (drafted % 2 == 0); // A drafts on picks 0,2,4,... (0-indexed)

    int result = turnA ? -1 : 1; // worst case for whoever is optimizing this node
    uint32_t pool = draftPool;
    while (pool) {
        int piece = __builtin_ctz(pool);
        pool &= pool - 1;
        uint32_t rest = draftPool & ~(1u << piece);
        int child = turnA
            ? phase1AValue(rest, poolA | (1u << piece), poolB, phase2FirstIsA)
            : phase1AValue(rest, poolA, poolB | (1u << piece), phase2FirstIsA);
        if (turnA) result = std::max(result, child); else result = std::min(result, child);
    }
    draftMemo[key] = (int8_t)result;
    return result;
}

// ---- CLI ----

static uint32_t parseMask(const char *s) {
    uint32_t m = 0;
    if (!*s) return m;
    char buf[256]; strncpy(buf, s, sizeof(buf)-1); buf[sizeof(buf)-1]=0;
    char *tok = strtok(buf, ",");
    while (tok) { m |= (1u << atoi(tok)); tok = strtok(nullptr, ","); }
    return m;
}

static std::string maskNames(uint32_t mask) {
    std::string s;
    for (int i = 0; i < 12; i++) if (mask & (1u << i)) s += NAMES[i];
    return s;
}

// Runs phase2 for every one of the C(12,6) = 924 ways to split the 12
// pentominoes into two 6-piece pools, with a fixed rule for who places
// first. This is exactly the set of leaf positions phase1AValue(0xFFF, ...)
// would eventually visit anyway (draftMemo doesn't skip any of them -- the
// draft minimax has no pruning), but as a flat loop instead of buried
// inside the draft recursion: each division's outcome is printed and
// logged the moment it finishes, so a run that gets interrupted still
// leaves a readable, growing record instead of one opaque final number.
// Splits the 924 divisions across `numWorkers` independent OS processes by
// ordinal (division i is this process's iff i % numWorkers == workerIndex),
// each with its own separate transposition/killer tables -- no shared
// mutable state, so no locking needed, at the cost of losing the
// cross-division cache sharing the single-process version got for free.
// That trade only makes sense because a division now resolves in well
// under two minutes on its own (move ordering + alpha-beta + threshold
// tuning did the heavy lifting already); losing shared caching to instead
// run 4 of them at once on a 4-core box is a straightforward win. Each
// worker writes its own log file, so nothing needs to coordinate writes;
// the caller launches one process per core (see the usage message) and can
// `cat`/`sort` the per-worker logs together afterward.
static void runDivisions(bool firstIsA, int workerIndex, int numWorkers, const std::string &logPath) {
    FILE *log = fopen(logPath.c_str(), "a");
    if (!log) { fprintf(stderr, "could not open log file %s\n", logPath.c_str()); exit(1); }
    auto runStart = std::chrono::steady_clock::now();
    {
        std::time_t now = std::time(nullptr);
        char ts[64]; std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        fprintf(log, "=== run start %s, W=%d H=%d firstIsA=%d worker=%d/%d ===\n",
                ts, W, H, firstIsA ? 1 : 0, workerIndex, numWorkers);
        fflush(log);
    }

    int total = 0;
    for (uint32_t s = 0; s < (1u << 12); s++) if (__builtin_popcount(s) == 6) total++;
    int myTotal = 0;
    for (int i = workerIndex; i < total; i += numWorkers) myTotal++;

    int globalIdx = -1, done = 0, aWins = 0, draws = 0, bWins = 0;
    for (uint32_t poolA = 0; poolA < (1u << 12); poolA++) {
        if (__builtin_popcount(poolA) != 6) continue;
        globalIdx++;
        if (globalIdx % numWorkers != workerIndex) continue; // not this worker's division

        uint32_t poolB = 0xFFF & ~poolA;
        uint64_t nodesBefore = nodeCount;
        auto divStart = std::chrono::steady_clock::now();
        int value = phase2AValue(poolA, poolB, firstIsA);
        auto divEnd = std::chrono::steady_clock::now();
        done++;
        if (value > 0) aWins++; else if (value < 0) bWins++; else draws++;

        const char *label = value > 0 ? "A_wins" : value < 0 ? "B_wins" : "draw";
        double divTime = std::chrono::duration<double>(divEnd - divStart).count();
        double totalTime = std::chrono::duration<double>(divEnd - runStart).count();
        std::string line = "worker " + std::to_string(workerIndex) + "/" + std::to_string(numWorkers) +
            " [" + std::to_string(done) + "/" + std::to_string(myTotal) +
            " | global " + std::to_string(globalIdx + 1) + "/" + std::to_string(total) + "] " +
            "poolA={" + maskNames(poolA) + "} poolB={" + maskNames(poolB) + "} firstIsA=" +
            (firstIsA ? "1" : "0") + " -> result=" + label + " value=" + std::to_string(value) +
            " nodes=" + std::to_string((unsigned long long)(nodeCount - nodesBefore)) +
            " time=" + std::to_string(divTime) + "s" +
            " | totals so far: A_wins=" + std::to_string(aWins) + " draw=" + std::to_string(draws) +
            " B_wins=" + std::to_string(bWins) + " elapsed=" + std::to_string(totalTime) + "s";

        printf("%s\n", line.c_str()); fflush(stdout);
        fprintf(log, "%s\n", line.c_str()); fflush(log);
    }
    fclose(log);
}

// Like runDivisions, but with a shared work queue instead of a fixed share of
// the splits: the worker walks through all 924 splits in the usual order and
// takes each split whose claim file it manages to create in `claimsDir`
// (O_CREAT|O_EXCL, so exactly one worker gets each split). A worker that
// finishes early thus keeps taking splits until none are left, and a restart
// skips every split whose claim file already exists -- the launcher creates
// claim files for the splits already present in the logs and removes stale
// claims of splits that were interrupted.
static void runQueue(bool firstIsA, const std::string &claimsDir, const std::string &logPath) {
    FILE *log = fopen(logPath.c_str(), "a");
    if (!log) { fprintf(stderr, "could not open log file %s\n", logPath.c_str()); exit(1); }
    auto runStart = std::chrono::steady_clock::now();
    {
        std::time_t now = std::time(nullptr);
        char ts[64]; std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        fprintf(log, "=== run start %s, W=%d H=%d firstIsA=%d queue=%s pid=%d ===\n",
                ts, W, H, firstIsA ? 1 : 0, claimsDir.c_str(), (int)getpid());
        fflush(log);
    }
    int total = 0;
    for (uint32_t s = 0; s < (1u << 12); s++) if (__builtin_popcount(s) == 6) total++;
    int globalIdx = -1, done = 0, aWins = 0, draws = 0, bWins = 0;
    for (uint32_t poolA = 0; poolA < (1u << 12); poolA++) {
        if (__builtin_popcount(poolA) != 6) continue;
        globalIdx++;
        std::string claim = claimsDir + "/" + maskNames(poolA);
        int fd = open(claim.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue; // taken by another worker, or already solved
            fprintf(stderr, "could not create claim %s\n", claim.c_str()); exit(1);
        }
        dprintf(fd, "%d\n", (int)getpid());
        close(fd);

        uint32_t poolB = 0xFFF & ~poolA;
        uint64_t nodesBefore = nodeCount;
        fprintf(stderr, "  split poolA={%s} (global %d/%d)\n", maskNames(poolA).c_str(), globalIdx + 1, total);
        auto divStart = std::chrono::steady_clock::now();
        int value = phase2AValue(poolA, poolB, firstIsA);
        auto divEnd = std::chrono::steady_clock::now();
        done++;
        if (value > 0) aWins++; else if (value < 0) bWins++; else draws++;

        const char *label = value > 0 ? "A_wins" : value < 0 ? "B_wins" : "draw";
        double divTime = std::chrono::duration<double>(divEnd - divStart).count();
        double totalTime = std::chrono::duration<double>(divEnd - runStart).count();
        std::string line = "queue pid " + std::to_string((int)getpid()) + " [" + std::to_string(done) +
            " | global " + std::to_string(globalIdx + 1) + "/" + std::to_string(total) + "] " +
            "poolA={" + maskNames(poolA) + "} poolB={" + maskNames(poolB) + "} firstIsA=" +
            (firstIsA ? "1" : "0") + " -> result=" + label + " value=" + std::to_string(value) +
            " nodes=" + std::to_string((unsigned long long)(nodeCount - nodesBefore)) +
            " time=" + std::to_string(divTime) + "s" +
            " | totals so far: A_wins=" + std::to_string(aWins) + " draw=" + std::to_string(draws) +
            " B_wins=" + std::to_string(bWins) + " elapsed=" + std::to_string(totalTime) + "s";
        printf("%s\n", line.c_str()); fflush(stdout);
        fprintf(log, "%s\n", line.c_str()); fflush(log);
    }
    fclose(log);
}

// ---- Combine: turn a completed `divisions` sweep into the real full-game
// answer, including phase 1 (the draft). `divisions` only reports each
// split's phase2 value in isolation; it never runs the draft's own
// max/min over those 924 values the way `full`'s phase1AValue does. Here
// we parse the (possibly many, one per parallel worker) log files
// `divisions` wrote, build a poolA-mask -> value lookup from them, and
// run the same draft minimax as phase1AValue -- except every leaf is a
// table lookup instead of a fresh phase2 search, so this finishes in
// well under a second even though phase2 itself, computed 924 times, was
// the expensive part of the whole project.
static int8_t divisionValue[4096];

static void loadDivisionsLog(const std::string &path, bool expectedFirstIsA, int &loaded) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) { fprintf(stderr, "could not open %s\n", path.c_str()); exit(1); }
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        const char *pa = strstr(line, "poolA={");
        const char *fi = strstr(line, "firstIsA=");
        const char *va = strstr(line, "value=");
        if (!pa || !fi || !va) continue; // not a division result line (e.g. the run-start banner)
        pa += 7; // skip "poolA={"
        uint32_t mask = 0;
        while (*pa && *pa != '}') {
            const char *p = strchr(NAMES, *pa);
            if (p) mask |= (1u << (int)(p - NAMES));
            pa++;
        }
        bool firstIsA = fi[9] != '0'; // char right after "firstIsA="
        if (firstIsA != expectedFirstIsA) continue; // a log mixing both isn't this run's data
        if (__builtin_popcountll(mask) != 6) continue; // malformed/partial line, skip
        int value = atoi(va + 6);
        divisionValue[mask] = (int8_t)value;
        loaded++;
    }
    fclose(f);
}

static std::unordered_map<u64, int8_t> combineMemo; // key = (draftPool<<12)|poolA, same scheme as draftMemo

static int combineDraft(uint32_t draftPool, uint32_t poolA, uint32_t poolB) {
    if (draftPool == 0) return divisionValue[poolA];
    u64 key = ((u64)draftPool << 12) | poolA;
    auto it = combineMemo.find(key);
    if (it != combineMemo.end()) return it->second;

    int drafted = 12 - __builtin_popcount(draftPool);
    bool turnA = (drafted % 2 == 0);
    int result = turnA ? -1 : 1;
    uint32_t pool = draftPool;
    while (pool) {
        int piece = __builtin_ctz(pool);
        pool &= pool - 1;
        uint32_t rest = draftPool & ~(1u << piece);
        int child = turnA
            ? combineDraft(rest, poolA | (1u << piece), poolB)
            : combineDraft(rest, poolA, poolB | (1u << piece));
        result = turnA ? std::max(result, child) : std::min(result, child);
    }
    combineMemo[key] = (int8_t)result;
    return result;
}

static void runCombine(bool firstIsA, const std::vector<std::string> &logPaths) {
    std::fill(std::begin(divisionValue), std::end(divisionValue), (int8_t)-128); // sentinel: not loaded
    int loaded = 0;
    for (auto &p : logPaths) loadDivisionsLog(p, firstIsA, loaded);

    int expected = 0;
    for (uint32_t s = 0; s < (1u << 12); s++) if (__builtin_popcount(s) == 6) expected++;
    printf("loaded %d division result lines from %zu file(s) (expect %d distinct divisions)\n",
           loaded, logPaths.size(), expected);

    int missing = 0;
    for (uint32_t s = 0; s < (1u << 12); s++) {
        if (__builtin_popcount(s) != 6) continue;
        if (divisionValue[s] == -128) {
            if (missing < 10) fprintf(stderr, "missing division poolA={%s}\n", maskNames(s).c_str());
            missing++;
        }
    }
    if (missing > 0) {
        fprintf(stderr, "%d division(s) missing from the given log file(s) -- cannot compute the full-game answer "
                        "until every one of the %d splits has been run (see the `divisions` mode).\n", missing, expected);
        exit(1);
    }

    auto t0 = std::chrono::steady_clock::now();
    int ans = combineDraft(0xFFF, 0, 0);
    auto t1 = std::chrono::steady_clock::now();
    const char *label = ans > 0 ? "A_wins" : ans < 0 ? "B_wins" : "draw";
    printf("result=%s value=%d draft_states=%zu time=%.3fs\n",
           label, ans, combineMemo.size(), std::chrono::duration<double>(t1 - t0).count());
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  %s phase2     W H poolA(csv ids) poolB(csv ids) firstIsA(0/1)\n"
            "  %s phase1     W H pieces(csv ids) phase2FirstIsA(0/1)\n"
            "  %s full       W H phase2FirstIsA(0/1)     # all 12 pieces\n"
            "  %s divisions  W H phase2FirstIsA(0/1) workerIndex numWorkers [logfile]\n"
            "                # all C(12,6)=924 splits; this process handles the divisions\n"
            "                # with (ordinal %% numWorkers == workerIndex). Run one process\n"
            "                # per core with workerIndex=0..numWorkers-1 for parallelism\n"
            "                # (e.g. numWorkers=1 workerIndex=0 to run every division alone).\n"
            "  %s queue      W H phase2FirstIsA(0/1) claimsDir logfile\n"
            "                # all 924 splits from a shared work queue: each split is taken by\n"
            "                # the worker that creates its claim file in claimsDir first\n"
            "  %s combine    phase2FirstIsA(0/1) logfile [logfile2 ...]\n"
            "                # turns one or more completed `divisions` logs (e.g. one per\n"
            "                # worker) into the real full-game answer, running phase 1's\n"
            "                # draft minimax as table lookups instead of fresh phase2 runs.\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }
    std::string mode = argv[1];
    if (mode == "combine") {
        bool phase2FirstIsA = atoi(argv[2]) != 0;
        std::vector<std::string> logPaths;
        for (int i = 3; i < argc; i++) logPaths.push_back(argv[i]);
        if (logPaths.empty()) { fprintf(stderr, "combine needs at least one logfile\n"); return 1; }
        runCombine(phase2FirstIsA, logPaths);
        return 0;
    }
    W = atoi(argv[2]); H = atoi(argv[3]);
    buildPlacements();
    buildSymmetries();
    if (const char *e = getenv("PENTOMINO_KILLERS")) numKillers = std::max(1, std::min(MAX_KILLERS, atoi(e)));
    fprintf(stderr, "killers per stage: %d\n", numKillers);
    if (const char *e = getenv("PENTOMINO_ORDER")) orderMode = atoi(e);
    if (const char *e = getenv("PENTOMINO_ORDER_MIN_PIECES")) orderMinPieces = atoi(e);
    fprintf(stderr, "move ordering: %d (extra term with >= %d pieces left)\n", orderMode, orderMinPieces);
    if (const char *e = getenv("PENTOMINO_ORDER_KEY")) orderKey = atoi(e);
    if (const char *e = getenv("PENTOMINO_B_WEIGHT")) bWeight = atoi(e);
    if (const char *e = getenv("PENTOMINO_A_WEIGHT")) aWeight = atoi(e);
    if (const char *e = getenv("PENTOMINO_C_WEIGHT")) cWeight = atoi(e);
    if (const char *e = getenv("PENTOMINO_DEAD_W")) deadWeight = atoi(e);
    if (const char *e = getenv("PENTOMINO_SCARCE_W")) scarceWeight = atoi(e);
    if (const char *e = getenv("PENTOMINO_PIECE_BONUS")) pieceBonusMode = atoi(e);
    if (const char *e = getenv("PENTOMINO_PIECE_RANKS")) {
        // 12 comma-separated ranks in the order F I L N P T U V W X Y Z
        std::string v = e; size_t pos = 0;
        for (int i = 0; i < 12; i++) {
            size_t next = v.find(',', pos);
            pieceBonus[i] = atoi(v.substr(pos, next == std::string::npos ? std::string::npos : next - pos).c_str());
            if (next == std::string::npos) break;
            pos = next + 1;
        }
    }
    if (pieceBonusMode) {
        fprintf(stderr, "piece bonus: %d, ranks", pieceBonusMode);
        for (int i = 0; i < 12; i++) fprintf(stderr, " %c=%d", NAMES[i], pieceBonus[i]);
        fprintf(stderr, "\n");
    }
    if (orderMode == 1) fprintf(stderr, "extra terms (mode 1): C %d, dead %d, scarce %d\n", cWeight, deadWeight, scarceWeight);
    if (orderMode == 1) fprintf(stderr, "B weight (mode 1): %d\n", bWeight);
    if (orderMode == 5) fprintf(stderr, "order key (mode 5): %d\n", orderKey);
    if (const char *e = getenv("PENTOMINO_HISTORY")) historyMode = atoi(e);
    fprintf(stderr, "history heuristic: %d\n", historyMode);
    initKillers();
    if (const char *e = getenv("PENTOMINO_NO_TT")) ttEnabled = atoi(e) == 0;
    if (const char *e = getenv("PENTOMINO_CANON_THRESHOLD")) canonThreshold = atoi(e);
    if (ttEnabled) {
        int tableBits = 24; // 16M slots * 13B(padded 16) ~= 256MB by default
        if (const char *e = getenv("PENTOMINO_TABLE_BITS")) tableBits = atoi(e);
        initTable(tableBits);
    } else {
        fprintf(stderr, "transposition table disabled (PENTOMINO_NO_TT set)\n");
    }

    startTime = std::chrono::steady_clock::now();
    auto t0 = startTime;

    auto resultLabel = [](int v) { return v > 0 ? "A_wins" : v < 0 ? "B_wins" : "draw"; };

    if (mode == "phase2") {
        uint32_t poolA = parseMask(argv[4]);
        uint32_t poolB = parseMask(argv[5]);
        bool firstIsA = atoi(argv[6]) != 0;
        int ans = phase2AValue(poolA, poolB, firstIsA);
        auto t1 = std::chrono::steady_clock::now();
        printf("result=%s value=%d nodes=%llu hits=%llu collisions=%llu time=%.3fs\n",
               resultLabel(ans), ans, (unsigned long long)nodeCount,
               (unsigned long long)hitCount, (unsigned long long)collisionCount,
               std::chrono::duration<double>(t1 - t0).count());
    } else if (mode == "phase1") {
        uint32_t pieces = parseMask(argv[4]);
        bool phase2FirstIsA = atoi(argv[5]) != 0;
        int ans = phase1AValue(pieces, 0, 0, phase2FirstIsA);
        auto t1 = std::chrono::steady_clock::now();
        printf("result=%s value=%d nodes=%llu hits=%llu collisions=%llu draft_memo=%zu time=%.3fs\n",
               resultLabel(ans), ans, (unsigned long long)nodeCount,
               (unsigned long long)hitCount, (unsigned long long)collisionCount, draftMemo.size(),
               std::chrono::duration<double>(t1 - t0).count());
    } else if (mode == "full") {
        bool phase2FirstIsA = atoi(argv[4]) != 0;
        int ans = phase1AValue(0xFFF, 0, 0, phase2FirstIsA);
        auto t1 = std::chrono::steady_clock::now();
        printf("result=%s value=%d nodes=%llu hits=%llu collisions=%llu draft_memo=%zu time=%.3fs\n",
               resultLabel(ans), ans, (unsigned long long)nodeCount,
               (unsigned long long)hitCount, (unsigned long long)collisionCount, draftMemo.size(),
               std::chrono::duration<double>(t1 - t0).count());
    } else if (mode == "divisions") {
        bool phase2FirstIsA = atoi(argv[4]) != 0;
        int workerIndex = atoi(argv[5]);
        int numWorkers = atoi(argv[6]);
        std::string logPath = (argc > 7) ? argv[7]
            : ("divisions_" + std::to_string(W) + "x" + std::to_string(H) +
               "_w" + std::to_string(workerIndex) + "-of-" + std::to_string(numWorkers) + ".log");
        printf("worker %d/%d logging to %s\n", workerIndex, numWorkers, logPath.c_str());
        runDivisions(phase2FirstIsA, workerIndex, numWorkers, logPath);
    } else if (mode == "queue") {
        if (argc < 7) { fprintf(stderr, "queue needs: W H firstIsA claimsDir logfile\n"); return 1; }
        runQueue(atoi(argv[4]) != 0, argv[5], argv[6]);
    }
    return 0;
}
