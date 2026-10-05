// Solver for the no-drafting game of Pentominoes: the twelve pentominoes
// form a common pool, the players alternately place any piece from the pool,
// and the player who cannot move loses (there are no draws). The rules are
// those of Solvers/nodrafting/NoDraft.lean, against which this solver is checked
// on small boards (mode `nd`).
//
// Built from the lessons of the drafting-variant solver (engine.cpp):
//  * bitboards: cell (r,c) is bit r*W+c; all placements precomputed;
//  * the game has only two values, so every search result is exact and
//    can be stored in the transposition table unconditionally;
//  * the value of a position depends only on the occupied cells and the
//    pool (not on whose turn it is), which is the table key;
//  * direct-mapped table, 2^26 entries by default (PENTOMINO_TABLE_BITS);
//  * the key is canonicalised under the board symmetries in positions with
//    at most 15 occupied cells;
//  * killer moves per number of pieces left (PENTOMINO_KILLERS, default 2),
//    history heuristic as tie-break;
//  * move ordering by a score computed only near the root, a cheap score
//    elsewhere (see ND_* settings below);
//  * work queue with claim files (O_EXCL), so that several processes share
//    the work and an interrupted run loses only the tasks in progress.
//
// Usage:
//   solver_nodrafting nd    W H pieces             value of the game on the empty board
//                                         (1 = first player wins, 0 = loses)
//   solver_nodrafting roots W H                    first moves up to symmetry, with the
//                                         number of replies to each
//   solver_nodrafting first W H idx                solve the position after first move idx
//                                         (as listed by `roots`), reply by reply
//   solver_nodrafting queue W H tasks claimsDir log
//                                         work on the tasks (first move, reply)
//                                         listed in the file `tasks`
//   solver_nodrafting replies W H idx               all replies to first move idx
//   solver_nodrafting pos   W H occHex poolHex     value for the player to move

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using u64 = uint64_t;
using Cell = std::pair<int, int>;

static const std::vector<std::vector<Cell>> baseShapes = {
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

static int W = 8, H = 8;

// ---------------------------------------------------------------- placements

static std::vector<Cell> normalize(std::vector<Cell> c) {
    int rmin = 1000, cmin = 1000;
    for (auto &p : c) { rmin = std::min(rmin, p.first); cmin = std::min(cmin, p.second); }
    for (auto &p : c) { p.first -= rmin; p.second -= cmin; }
    std::sort(c.begin(), c.end());
    return c;
}

static std::vector<std::vector<Cell>> orientationsOf(const std::vector<Cell> &cells) {
    std::vector<std::vector<Cell>> result;
    for (int refl = 0; refl < 2; refl++) {
        auto p = cells;
        if (refl) for (auto &pt : p) pt.second = -pt.second;
        for (int rot = 0; rot < 4; rot++) {
            std::vector<Cell> np;
            for (auto &pt : p) np.push_back({pt.second, -pt.first});
            p = np;
            auto norm = normalize(p);
            if (std::find(result.begin(), result.end(), norm) == result.end()) result.push_back(norm);
        }
    }
    return result;
}

static std::array<std::vector<u64>, 12> placements; // placements[piece] = cell masks
static std::array<std::vector<u64>, 12> adjacent;   // cells orthogonally adjacent to each placement

static void buildPlacements() {
    static const int dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
    for (int i = 0; i < 12; i++) {
        placements[i].clear(); adjacent[i].clear();
        for (auto &o : orientationsOf(baseShapes[i])) {
            int rs = 0, cs = 0;
            for (auto &p : o) { rs = std::max(rs, p.first); cs = std::max(cs, p.second); }
            for (int r0 = 0; r0 + rs < H; r0++)
                for (int c0 = 0; c0 + cs < W; c0++) {
                    u64 m = 0, a = 0;
                    for (auto &p : o) m |= 1ULL << ((r0 + p.first) * W + c0 + p.second);
                    for (auto &p : o)
                        for (int k = 0; k < 4; k++) {
                            int r = r0 + p.first + dr[k], c = c0 + p.second + dc[k];
                            if (r >= 0 && r < H && c >= 0 && c < W) a |= 1ULL << (r * W + c);
                        }
                    placements[i].push_back(m);
                    adjacent[i].push_back(a & ~m);
                }
        }
    }
}

// ---------------------------------------------------------------- symmetries

static int numSyms = 0;
static std::array<std::array<int, 64>, 8> symPerm;

static void buildSymmetries() {
    numSyms = 0;
    auto add = [&](auto f) {
        std::array<int, 64> perm{};
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++) {
                auto [nr, nc] = f(r, c);
                if (nr < 0 || nr >= H || nc < 0 || nc >= W) return;
                perm[r * W + c] = nr * W + nc;
            }
        symPerm[numSyms++] = perm;
    };
    add([](int r, int c) { return std::pair{r, c}; });
    add([](int r, int c) { return std::pair{r, W - 1 - c}; });
    add([](int r, int c) { return std::pair{H - 1 - r, c}; });
    add([](int r, int c) { return std::pair{H - 1 - r, W - 1 - c}; });
    if (W == H) {
        add([](int r, int c) { return std::pair{c, r}; });
        add([](int r, int c) { return std::pair{c, H - 1 - r}; });
        add([](int r, int c) { return std::pair{W - 1 - c, r}; });
        add([](int r, int c) { return std::pair{W - 1 - c, H - 1 - r}; });
    }
}

static inline u64 applySym(int s, u64 occ) {
    u64 t = 0;
    while (occ) { int b = __builtin_ctzll(occ); occ &= occ - 1; t |= 1ULL << symPerm[s][b]; }
    return t;
}

static int canonThreshold = 15; // canonicalise only with at most this many occupied cells

static inline u64 canonOcc(u64 occ) {
    if (__builtin_popcountll(occ) > canonThreshold) return occ;
    u64 best = occ;
    for (int s = 1; s < numSyms; s++) best = std::min(best, applySym(s, occ));
    return best;
}

// ---------------------------------------------------------- transposition table

struct Slot { u64 occ; uint16_t pool; uint8_t state; }; // state: 0 empty, 1 loss, 2 win
static std::vector<Slot> table;
static u64 tableMask;

static void initTable(int bits) {
    table.assign(size_t(1) << bits, Slot{0, 0, 0});
    tableMask = (u64(1) << bits) - 1;
}

static inline size_t slotIndex(u64 occ, uint32_t pool) {
    u64 h = occ * 0x9E3779B97F4A7C15ULL + pool;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return size_t(h & tableMask);
}

// ---------------------------------------------------------------- settings

static int numKillers = 2;    // PENTOMINO_KILLERS
static int historyOn = 1;     // PENTOMINO_HISTORY (tie-break)
// Move ordering. At nodes with at least `orderMinPieces` pieces left in the
// pool the moves are sorted by keyNear, elsewhere by keyFar:
//   0 = generation order (piece by piece, as in Orman's checking program)
//   1 = A: empty cells adjacent to the placement (more first)
//   2 = A + rankWeight * rank of the piece
//   3 = fewest replies of the opponent first, then A
//   4 = rank of the piece only
static int keyNear = 2, keyFar = 1, orderMinPieces = 9;
// Piece ranks from the drafting results (strong 1 .. weak 10), F I L N P T U V W X Y Z.
static int pieceRank[12] = {8, 5, 1, 6, 2, 9, 4, 3, 7, 10, 5, 5};
static int rankWeight = 1;    // ND_RANK_WEIGHT; negative = strong pieces first

static void readSettings() {
    auto geti = [](const char *n, int &v) { if (const char *e = getenv(n)) v = atoi(e); };
    geti("PENTOMINO_KILLERS", numKillers);
    geti("PENTOMINO_HISTORY", historyOn);
    geti("ND_KEY_NEAR", keyNear);
    geti("ND_KEY_FAR", keyFar);
    geti("ND_ORDER_MIN_PIECES", orderMinPieces);
    geti("ND_RANK_WEIGHT", rankWeight);
    geti("ND_CANON", canonThreshold);
    if (const char *e = getenv("ND_PIECE_RANKS")) {
        int i = 0; std::string s(e); size_t p = 0;
        while (i < 12 && p <= s.size()) { pieceRank[i++] = atoi(s.c_str() + p); p = s.find(',', p); if (p == std::string::npos) break; p++; }
    }
    if (numKillers > 16) numKillers = 16;
}

// ---------------------------------------------------------------- search

static constexpr int MAX_KILLERS = 16;
static int killerPiece[13][MAX_KILLERS];
static u64 killerMask[13][MAX_KILLERS];
static uint32_t hist[12][512];
static int placementIdx(int piece, u64 m) {
    const auto &v = placements[piece];
    return int(std::lower_bound(v.begin(), v.end(), m) - v.begin()); // see sortPlacements
}

static void initKillers() {
    for (auto &a : killerPiece) for (auto &x : a) x = -1;
    memset(killerMask, 0, sizeof killerMask);
    memset(hist, 0, sizeof hist);
}

static void recordKiller(int left, int piece, u64 m) {
    int *kp = killerPiece[left]; u64 *km = killerMask[left];
    int j = 0;
    while (j < numKillers - 1 && !(kp[j] == piece && km[j] == m)) j++;
    for (; j > 0; j--) { kp[j] = kp[j - 1]; km[j] = km[j - 1]; }
    kp[0] = piece; km[0] = m;
}

static uint64_t nodes = 0, hits = 0;
static std::chrono::steady_clock::time_point t0;
static const char *stopFile = nullptr; // abort when this file appears
static bool aborted = false;

static double elapsed() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

struct Move { u64 m; int piece; int score; uint32_t h; };

static int countReplies(u64 occ, uint32_t pool) {
    int n = 0;
    for (uint32_t p = pool; p; p &= p - 1) {
        int piece = __builtin_ctz(p);
        for (u64 m : placements[piece]) n += !(m & occ);
    }
    return n;
}

// true iff the player to move wins
static bool wins(u64 occ, uint32_t pool) {
    if ((++nodes & 0xFFFFFF) == 0 && stopFile && access(stopFile, F_OK) == 0) aborted = true;
    if (aborted) return false;
    u64 key = canonOcc(occ);
    size_t idx = slotIndex(key, pool);
    {
        const Slot &s = table[idx];
        if (s.state && s.occ == key && s.pool == pool) { hits++; return s.state == 2; }
    }
    int left = __builtin_popcount(pool);
    bool result = false;

    // killer moves first
    int kp[MAX_KILLERS]; u64 km[MAX_KILLERS]; int nk = 0;
    for (int j = 0; j < numKillers; j++) {
        int piece = killerPiece[left][j]; u64 m = killerMask[left][j];
        if (piece < 0 || !(pool >> piece & 1) || (m & occ)) continue;
        kp[nk] = piece; km[nk] = m; nk++;
        if (!wins(occ | m, pool & ~(1u << piece))) { result = true; recordKiller(left, piece, m); break; }
    }

    if (!result && !aborted) {
        Move mv[2400]; int n = 0;
        int key = left >= orderMinPieces ? keyNear : keyFar;
        for (uint32_t p = pool; p; p &= p - 1) {
            int piece = __builtin_ctz(p);
            const auto &pl = placements[piece]; const auto &ad = adjacent[piece];
            for (size_t k = 0; k < pl.size(); k++) {
                u64 m = pl[k];
                if (m & occ) continue;
                bool killer = false;
                for (int j = 0; j < nk; j++) killer |= kp[j] == piece && km[j] == m;
                if (killer) continue;
                int sc = 0;
                if (key == 1 || key == 2 || key == 3) sc = __builtin_popcountll(ad[k] & ~occ);
                if (key == 2) sc += rankWeight * pieceRank[piece];
                if (key == 4) sc = rankWeight * pieceRank[piece];
                mv[n++] = {m, piece, sc, historyOn ? hist[piece][k] : 0};
            }
        }
        if (key == 3) {
            for (int i = 0; i < n; i++)
                mv[i].score += -64 * countReplies(occ | mv[i].m, pool & ~(1u << mv[i].piece));
        }
        if (key != 0)
            std::stable_sort(mv, mv + n, [](const Move &a, const Move &b) {
                return a.score != b.score ? a.score > b.score : a.h > b.h; });
        else if (historyOn)
            std::stable_sort(mv, mv + n, [](const Move &a, const Move &b) { return a.h > b.h; });
        for (int i = 0; i < n && !aborted; i++) {
            if (!wins(occ | mv[i].m, pool & ~(1u << mv[i].piece))) {
                result = true;
                recordKiller(left, mv[i].piece, mv[i].m);
                if (historyOn) hist[mv[i].piece][placementIdx(mv[i].piece, mv[i].m)] += left * left;
                break;
            }
        }
    }
    if (aborted) return false;
    table[idx] = Slot{key, (uint16_t)pool, (uint8_t)(result ? 2 : 1)};
    return result;
}

// After wins(occ, pool) returned true: a move of the player to move that
// leaves the opponent in a lost position (children are mostly in the table).
static bool winningMove(u64 occ, uint32_t pool, int *pieceOut, u64 *maskOut) {
    for (uint32_t p = pool; p; p &= p - 1) {
        int piece = __builtin_ctz(p);
        for (u64 m : placements[piece])
            if (!(m & occ) && !wins(occ | m, pool & ~(1u << piece))) {
                *pieceOut = piece; *maskOut = m; return true;
            }
    }
    return false;
}

// placements are sorted so that placementIdx can use binary search
static void sortPlacements() {
    for (int i = 0; i < 12; i++) {
        std::vector<std::pair<u64, u64>> v;
        for (size_t k = 0; k < placements[i].size(); k++) v.push_back({placements[i][k], adjacent[i][k]});
        std::sort(v.begin(), v.end());
        for (size_t k = 0; k < v.size(); k++) { placements[i][k] = v[k].first; adjacent[i][k] = v[k].second; }
    }
}

// ---------------------------------------------------------------- root helpers

struct RootMove { int piece; u64 m; int replies; };

// first moves up to symmetry (one representative, the smallest image, per class)
static std::vector<RootMove> firstMoves(uint32_t pool) {
    std::vector<RootMove> r;
    for (int piece = 0; piece < 12; piece++) {
        if (!(pool >> piece & 1)) continue;
        for (u64 m : placements[piece]) {
            u64 c = m;
            for (int s = 1; s < numSyms; s++) c = std::min(c, applySym(s, m));
            if (c != m) continue;
            r.push_back({piece, m, countReplies(m, pool & ~(1u << piece))});
        }
    }
    return r;
}

static std::string boardString(u64 occ) {
    std::string s;
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) s += (occ >> (r * W + c) & 1) ? '#' : '.';
        if (r + 1 < H) s += '/';
    }
    return s;
}

static uint32_t parsePieces(const char *s) {
    uint32_t p = 0;
    std::string t(s);
    if (t == "all") return 0xFFF;
    size_t i = 0;
    while (i < t.size()) { p |= 1u << atoi(t.c_str() + i); size_t j = t.find(',', i); if (j == std::string::npos) break; i = j + 1; }
    return p;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s nd|roots|first|queue|pos W H ...\n", argv[0]);
        return 1;
    }
    std::string mode = argv[1];
    W = atoi(argv[2]); H = atoi(argv[3]);
    readSettings();
    buildPlacements();
    sortPlacements();
    buildSymmetries();
    int bits = 26;
    if (const char *e = getenv("PENTOMINO_TABLE_BITS")) bits = atoi(e);
    initKillers();
    t0 = std::chrono::steady_clock::now();

    if (mode == "nd") {
        initTable(bits);
        uint32_t pool = parsePieces(argc > 4 ? argv[4] : "all");
        bool w = wins(0, pool);
        printf("value=%d nodes=%llu hits=%llu time=%.3fs\n", w ? 1 : 0,
               (unsigned long long)nodes, (unsigned long long)hits, elapsed());
    } else if (mode == "pos") {
        initTable(bits);
        u64 occ = strtoull(argv[4], nullptr, 16);
        uint32_t pool = (uint32_t)strtoul(argv[5], nullptr, 16);
        bool w = wins(occ, pool);
        printf("value=%d nodes=%llu hits=%llu time=%.3fs\n", w ? 1 : 0,
               (unsigned long long)nodes, (unsigned long long)hits, elapsed());
    } else if (mode == "roots") {
        auto r = firstMoves(0xFFF);
        for (size_t i = 0; i < r.size(); i++)
            printf("%zu %c %016llx %d %s\n", i, NAMES[r[i].piece], (unsigned long long)r[i].m,
                   r[i].replies, boardString(r[i].m).c_str());
    } else if (mode == "replies") {
        // all replies to first move idx, one per line: piece maskHex occHex poolHex
        auto r = firstMoves(0xFFF);
        int idx = atoi(argv[4]);
        u64 occ1 = r[idx].m; uint32_t pool1 = 0xFFF & ~(1u << r[idx].piece);
        for (uint32_t p = pool1; p; p &= p - 1) {
            int piece = __builtin_ctz(p);
            for (u64 m : placements[piece])
                if (!(m & occ1))
                    printf("%c %016llx %016llx %03x\n", NAMES[piece], (unsigned long long)m,
                           (unsigned long long)(occ1 | m), pool1 & ~(1u << piece));
        }
    } else if (mode == "first") {
        // Solve the position after first move idx, one reply at a time; the
        // first move wins iff every reply loses for the player who made it.
        initTable(bits);
        auto r = firstMoves(0xFFF);
        int idx = atoi(argv[4]);
        u64 occ1 = r[idx].m; uint32_t pool1 = 0xFFF & ~(1u << r[idx].piece);
        int nRep = 0, nRefuted = 0;
        for (uint32_t p = pool1; p; p &= p - 1) {
            int piece = __builtin_ctz(p);
            for (u64 m : placements[piece]) {
                if (m & occ1) continue;
                uint64_t n0 = nodes; double ts = elapsed();
                bool firstWins = wins(occ1 | m, pool1 & ~(1u << piece));
                nRep++;
                printf("reply %d %c %016llx -> %s nodes=%llu time=%.1fs\n", nRep, NAMES[piece],
                       (unsigned long long)m, firstWins ? "first player wins" : "REFUTES",
                       (unsigned long long)(nodes - n0), elapsed() - ts);
                fflush(stdout);
                if (!firstWins) { nRefuted++; break; }
            }
            if (nRefuted) break;
        }
        printf("first move %d %c: %s after %d replies, nodes=%llu time=%.1fs\n", idx, NAMES[r[idx].piece],
               nRefuted ? "LOSES" : "WINS", nRep, (unsigned long long)nodes, elapsed());
    } else if (mode == "queue") {
        // tasks file: lines "firstIdx piece maskHex replyPiece replyMaskHex".
        // A task asks whether the first player wins after the two moves. A
        // first move is refuted as soon as one of its tasks says no; then its
        // other tasks are skipped (claimsDir/refuted_<firstIdx>).
        initTable(bits);
        std::string claims = argv[5], logPath = argv[6];
        std::string stop = claims + "/STOP";
        stopFile = strdup(stop.c_str());
        FILE *tf = fopen(argv[4], "r");
        if (!tf) { perror("tasks"); return 1; }
        char line[256]; int lineNo = 0;
        while (fgets(line, sizeof line, tf)) {
            lineNo++;
            if (access(stop.c_str(), F_OK) == 0) break;
            int fi; char pc, rc; unsigned long long m1, m2;
            if (sscanf(line, "%d %c %llx %c %llx", &fi, &pc, &m1, &rc, &m2) != 5) continue;
            std::string ref = claims + "/refuted_" + std::to_string(fi);
            if (access(ref.c_str(), F_OK) == 0) continue;
            std::string claim = claims + "/task_" + std::to_string(lineNo);
            int fd = open(claim.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) continue;
            close(fd);
            int p1 = int(strchr(NAMES, pc) - NAMES), p2 = int(strchr(NAMES, rc) - NAMES);
            uint32_t pool = 0xFFF & ~(1u << p1) & ~(1u << p2);
            uint64_t n0 = nodes; double ts = elapsed();
            bool firstWins = wins(m1 | m2, pool);
            if (aborted) { unlink(claim.c_str()); break; }
            double tsolve = elapsed() - ts;
            uint64_t nsolve = nodes - n0;
            // the winning third move, as a certificate that can be re-checked
            char third[64] = "-";
            int wp; u64 wm;
            if (firstWins && winningMove(m1 | m2, pool, &wp, &wm))
                snprintf(third, sizeof third, "%c %016llx", NAMES[wp], (unsigned long long)wm);
            FILE *lf = fopen(logPath.c_str(), "a");
            fprintf(lf, "%d %d %c %016llx %c %016llx %s %llu %.1f %s\n", lineNo, fi, pc, m1, rc, m2,
                    firstWins ? "win" : "REFUTED", (unsigned long long)nsolve, tsolve, third);
            fclose(lf);
            if (!firstWins) { int f2 = open(ref.c_str(), O_CREAT | O_WRONLY, 0644); if (f2 >= 0) close(f2); }
        }
        fclose(tf);
    } else {
        fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 1;
    }
    return 0;
}
