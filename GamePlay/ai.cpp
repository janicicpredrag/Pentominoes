#include "ai.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace pento {

namespace {

// ---------------------------------------------------------------- drafting

// Base weights of the pieces when drafting (from the 1993 program): the
// higher the weight, the more likely the piece is picked.
double baseWeight(char name) {
    switch (name) {
        case 'P': return 1000; case 'L': return 100; case 'U': return 100;
        case 'Y': return 20;   case 'V': return 20;  case 'N': return 15;
        case 'I': return 12;   case 'F': return 10;  case 'Z': return 5;
        case 'T': return 5;    case 'W': return 5;   case 'X': return 1;
    }
    return 1;
}

// Weights from the solved placement games: the percentage of games won by the
// holder of each piece (Table 3 of the paper), mapped exponentially onto the
// range of the 1993 weights, from 1 for X (27%) to 1000 for L (73%).
double solvedWeight(char name) {
    double s = 50;
    switch (name) {
        case 'L': s = 73; break; case 'P': s = 67; break; case 'V': s = 56; break;
        case 'Y': s = 56; break; case 'U': s = 51; break; case 'N': s = 51; break;
        case 'I': s = 50; break; case 'F': s = 44; break; case 'W': s = 42; break;
        case 'T': s = 41; break; case 'Z': s = 40; break; case 'X': s = 27; break;
    }
    return std::pow(10.0, 3.0 * (s - 27) / (73 - 27));
}

// ---------------------------------------------------------------- search state

// A position of the placement phase as the strategies see it. Pieces that
// a strategy treats as already committed to a reserved area are removed from
// the hand, kept in `reserved`, and their cells added to the board.
struct State {
    Bitboard board;
    uint16_t hand[2];
    uint16_t reserved[2] = {0, 0};
};

inline bool legal(const State &s, int id) {
    return !(PieceSet::get().placement(id).mask & s.board);
}

inline State apply(State s, int player, int id) {
    const Placement &pl = PieceSet::get().placement(id);
    s.board |= pl.mask;
    s.hand[player] &= ~(1u << pl.piece);
    return s;
}

// cells that `player` can still cover with one of their pieces
Bitboard coverable(const State &s, int player) {
    const PieceSet &ps = PieceSet::get();
    Bitboard m = 0;
    uint16_t pieces = s.hand[player] | s.reserved[player];
    for (int p = 0; p < kNumPieces; p++) {
        if (!(pieces >> p & 1)) continue;
        for (int id : ps.placementsOf(p)) {
            Bitboard pm = ps.placement(id).mask;
            if (!(pm & s.board)) m |= pm;
        }
    }
    return m;
}

Bitboard neighbours(Bitboard b) {
    const Bitboard notA = 0xfefefefefefefefeULL, notH = 0x7f7f7f7f7f7f7f7fULL;
    return (b << 8) | (b >> 8) | ((b << 1) & notA) | ((b >> 1) & notH);
}

// the cells of `area` that lie in connected regions of at least five cells
Bitboard largeRegions(Bitboard area) {
    Bitboard result = 0, rest = area;
    while (rest) {
        Bitboard region = rest & (~rest + 1), prev = 0;
        while (region != prev) { prev = region; region |= neighbours(region) & area; }
        rest &= ~region;
        if (popcount(region) >= 5) result |= region;
    }
    return result;
}

// A placement of one of `player`'s pieces that lies entirely in an area the
// opponent can no longer reach, or -1.
int reserveMove(const State &s, int player) {
    Bitboard safe = ~(s.board | coverable(s, 1 - player));
    if (popcount(safe) <= 4 || popcount(largeRegions(safe)) <= 4) return -1;
    const PieceSet &ps = PieceSet::get();
    for (int p = 0; p < kNumPieces; p++) {
        if (!(s.hand[player] >> p & 1)) continue;
        for (int id : ps.placementsOf(p)) {
            Bitboard pm = ps.placement(id).mask;
            if (!(pm & s.board) && !(pm & ~safe)) return id;
        }
    }
    return -1;
}

// Commits all reserved areas of `player`: as long as there is a reserve
// move, its piece is treated as placed.
void reserveAll(State &s, int player) {
    int id;
    while ((id = reserveMove(s, player)) >= 0) {
        const Placement &pl = PieceSet::get().placement(id);
        s.board |= pl.mask;
        s.hand[player] &= ~(1u << pl.piece);
        s.reserved[player] |= 1u << pl.piece;
    }
}

// ---------------------------------------------------------------- exact search

// Value of a position for the player to move: 1 win, 0 draw, -1 loss.
class Searcher {
public:
    explicit Searcher(const Ai::StopFn &stop) : stop_(stop) {}

    bool stopped() const { return stopped_; }

    int value(Bitboard board, uint16_t mover, uint16_t other) {
        if (stopped_) return 0;
        if ((++nodes_ & 0xFFFF) == 0 && stop_ && stop_()) { stopped_ = true; return 0; }
        if (!mover) return 0;  // all twelve pieces are placed
        uint64_t key = board * 0x9E3779B97F4A7C15ULL ^ (uint64_t(mover) << 12 | other);
        auto it = table_.find(key);
        if (it != table_.end() && it->second.board == board && it->second.mover == mover &&
            it->second.other == other)
            return it->second.value;
        const PieceSet &ps = PieceSet::get();
        int best = -1;
        bool any = false;
        for (int p = 0; p < kNumPieces && best < 1; p++) {
            if (!(mover >> p & 1)) continue;
            for (int id : ps.placementsOf(p)) {
                Bitboard pm = ps.placement(id).mask;
                if (pm & board) continue;
                any = true;
                int v = -value(board | pm, other, mover & ~(1u << p));
                if (stopped_) return 0;
                if (v > best) best = v;
                if (best == 1) break;
            }
        }
        if (!any) best = -1;
        table_[key] = {board, mover, other, int8_t(best)};
        return best;
    }

private:
    struct Entry { Bitboard board; uint16_t mover, other; int8_t value; };
    Ai::StopFn stop_;
    std::unordered_map<uint64_t, Entry> table_;
    uint64_t nodes_ = 0;
    bool stopped_ = false;
};


// Exact search with the techniques of the placement-phase solver: negamax
// alpha-beta over {-1, 0, +1}, a direct-mapped table of exact values,
// killer moves per number of pieces left, and the move ordering A + 2B + C
// near the root (A elsewhere). The table persists between moves.
class FastSearcher {
public:
    explicit FastSearcher(const Ai::StopFn &stop) : stop_(stop) {
        if (table().empty()) table().assign(size_t(1) << kBits, Entry{0, 0, 0, 0});
    }

    bool stopped() const { return stopped_; }

    // exact value for the player to move (mover's hand, opponent's hand)
    int value(Bitboard board, uint16_t mover, uint16_t other) {
        bool exact;
        return search(board, mover, other, -1, 1, exact);
    }

private:
    static constexpr int kBits = 22;
    static constexpr int kKillers = 2;
    struct Entry { Bitboard board; uint16_t mover, other; int8_t value; };
    static std::vector<Entry> &table() { static std::vector<Entry> t; return t; }
    struct Killer { int piece = -1; int id = -1; };
    Killer killers_[13][kKillers];

    static int rank(int piece) {  // C: strong 1 .. weak 10 (from a sample, as in the solver)
        static const int r[kNumPieces] = {8, 5, 1, 6, 2, 9, 4, 3, 7, 10, 5, 5};
        return r[piece];
    }

    static Bitboard cover(Bitboard board, uint16_t hand) {
        const PieceSet &ps = PieceSet::get();
        Bitboard m = 0;
        for (int p = 0; p < kNumPieces; p++) {
            if (!(hand >> p & 1)) continue;
            for (int id : ps.placementsOf(p)) {
                Bitboard pm = ps.placement(id).mask;
                if (!(pm & board)) m |= pm;
            }
        }
        return m;
    }

    int search(Bitboard board, uint16_t mover, uint16_t other, int alpha, int beta, bool &exact) {
        if (stopped_) { exact = false; return 0; }
        if ((++nodes_ & 0xFFFF) == 0 && stop_ && stop_()) { stopped_ = true; exact = false; return 0; }
        exact = true;
        if (!mover) return 0;  // all twelve pieces are placed
        uint64_t h = board * 0x9E3779B97F4A7C15ULL + (uint64_t(mover) << 12 | other);
        h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
        Entry &e = table()[h & ((size_t(1) << kBits) - 1)];
        if (e.value && e.board == board && e.mover == mover && e.other == other) return e.value - 2;
        const PieceSet &ps = PieceSet::get();
        int left = __builtin_popcount(mover) + __builtin_popcount(other);
        // the moves, killers first, then sorted by the score
        struct M { int id, piece, score; };
        std::vector<M> moves;
        moves.reserve(256);
        bool near = left >= 7;
        for (int p = 0; p < kNumPieces; p++) {
            if (!(mover >> p & 1)) continue;
            for (int id : ps.placementsOf(p)) {
                const Placement &pl = ps.placement(id);
                if (pl.mask & board) continue;
                int score = popcount(pl.adjacent & ~board);
                if (near) {
                    Bitboard nb = board | pl.mask;
                    Bitboard mine = cover(nb, mover & ~(1u << p));
                    Bitboard theirs = cover(nb, other);
                    score += 2 * popcount(mine & ~theirs) + rank(p);
                }
                for (int k = 0; k < kKillers; k++)
                    if (killers_[left][k].id == id) score += 1000 * (kKillers - k);
                moves.push_back({id, p, score});
            }
        }
        if (moves.empty()) return -1;
        std::stable_sort(moves.begin(), moves.end(), [](const M &a, const M &b) { return a.score > b.score; });
        int best = -2;
        bool allExact = true, exactWin = false;
        for (const M &m : moves) {
            bool ce;
            int v = -search(board | ps.placement(m.id).mask, other, mover & ~(1u << m.piece), -beta, -alpha, ce);
            if (stopped_) { exact = false; return 0; }
            allExact = allExact && ce;
            if (v > best) best = v;
            if (v == 1 && ce) exactWin = true;
            if (best > alpha) alpha = best;
            if (alpha >= beta) {
                auto &ks = killers_[left];
                if (ks[0].id != m.id) { for (int k = kKillers - 1; k > 0; k--) ks[k] = ks[k - 1]; ks[0] = {m.piece, m.id}; }
                break;
            }
        }
        exact = exactWin || allExact;
        if (exact) e = {board, mover, other, int8_t(best + 2)};
        return best;
    }

    Ai::StopFn stop_;
    uint64_t nodes_ = 0;
    bool stopped_ = false;
};

// ---------------------------------------------------------------- strategies

enum Strategy {
    kRandom = 0,          // a move that gives the opponent no reserved area
    kOpening = 1,         // a corner move after which the opponent cannot reserve
    kMakeReserve = 2,     // a move that creates a reserved area
    kBestForReserve = 3,  // make a reserve, or block the opponent's, or prepare one
    kExact = 4,           // exact minimax to the end of the game
    kOpeningPrepare = 5,  // an edge move that prepares a reserve
    kMakeOrPrepare = 6,   // make a reserve, else prepare one
    kOpeningSafe = 7,     // an edge move that the opponent cannot answer with a reserve
    kScore = 8,           // the move with the best score A + 2B + C (as in the solver)
};

const char *strategyName(int s) {
    static const char *names[] = {"random", "opening", "make reserve", "best for reserve",
                                  "exact search", "opening (prepare)", "make or prepare reserve",
                                  "opening (safe)", "score A+2B+C"};
    return names[s];
}

// strategy by level (0-6) and number of the placement move (1-12)
const int kStrategyTable[7][12] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 2, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4},
    {5, 2, 2, 2, 3, 3, 4, 4, 4, 4, 4, 4},
    {1, 6, 6, 3, 3, 3, 4, 4, 4, 4, 4, 4},
    {1, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4},
    {7, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4},
    {7, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4},
};

class Planner {
public:
    Planner(const Game &game, int player, std::mt19937 &rng, const Ai::StopFn &stop,
            const bool *analysis, const Ai::WatchFn &watch, const char *what,
            const std::array<double, kNumPieces> &w, bool strongSearch = false)
        : player_(player), opp_(1 - player), rng_(rng), stop_(stop), strong_(strongSearch),
          analysis_(analysis), watch_(watch), what_(what) {
        real_.board = game.board();
        real_.hand[0] = game.hand(0);
        real_.hand[1] = game.hand(1);
        // Pieces are tried from the weakest (lowest drafting weight) on,
        // starting at a random piece chosen with probability inversely
        // proportional to its weight.
        for (int p = 0; p < kNumPieces; p++) order_[p] = p;
        std::sort(order_.begin(), order_.end(), [&](int a, int b) { return w[a] < w[b]; });
        std::vector<double> inv(kNumPieces);
        for (int i = 0; i < kNumPieces; i++) inv[i] = 1.0 / w[order_[i]];
        start_ = std::discrete_distribution<int>(inv.begin(), inv.end())(rng_);
    }

    int choose(int strategy) {
        if (strategy == kExact) {
            if (strong_) { FastSearcher f(stop_); return exact(f); }
            Searcher plain(stop_);
            return exact(plain);
        }
        if (strategy == kScore) return scoreMove();
        State s = real_;
        reserveAll(s, player_);
        reserveAll(s, opp_);
        int id = -1;
        switch (strategy) {
            case kRandom: id = randomMove(s); break;
            case kOpening: id = opening(s); break;
            case kMakeReserve: id = makeReserve(s); break;
            case kBestForReserve: id = bestForReserve(s); break;
            case kOpeningPrepare: id = openingPrepare(s); break;
            case kMakeOrPrepare: id = makeReserve(s); if (id < 0) id = prepareReserve(s); break;
            case kOpeningSafe: id = openingSafe(s); break;
        }
        if (id < 0) id = randomMove(s);
        if (id < 0) id = randomMove(real_);  // all pieces are in reserved areas
        return id;
    }

private:
    // Calls f(id) for the legal moves of `player`, pieces in the planner's
    // order, positions from a random offset, until f returns true.
    template <class F>
    int forEachMove(const State &s, int player, F f) {
        const PieceSet &ps = PieceSet::get();
        for (int i = 0; i < kNumPieces; i++) {
            int p = order_[(start_ + i) % kNumPieces];
            if (!(s.hand[player] >> p & 1)) continue;
            const auto &ids = ps.placementsOf(p);
            int n = int(ids.size()), r = int(rng_() % n);
            for (int j = 0; j < n; j++) {
                int id = ids[(j + r) % n];
                if (!legal(s, id)) continue;
                if (player == player_) show(s, id);
                if (f(id)) return id;
            }
        }
        return -1;
    }

    // Reports the move being examined to the auxiliary board (throttled),
    // and lets the caller read the keyboard.
    void show(const State &s, int id, bool withArea = true) {
        if (stop_) stop_();  // reads the keyboard (at most every 20 ms)
        if (!analysis_ || !*analysis_ || !watch_) return;
        // As in the 1993 program, the analysis slows the program down so that
        // it can be followed: every examined move is shown for 30 ms, for at
        // most 3 seconds per move of the program; after that, only a sample
        // (one move every 40 ms) is shown.
        auto now = std::chrono::steady_clock::now();
        if (shownFor_ < std::chrono::seconds(3)) {
            shownFor_ += std::chrono::milliseconds(30);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        } else if (now - lastShow_ < std::chrono::milliseconds(40)) {
            return;
        }
        lastShow_ = now;
        Ai::Analysis a;
        a.real = real_.board;
        a.assumed = s.board & ~real_.board;
        a.placement = id;
        a.player = player_;
        a.what = what_;
        if (withArea) {
            State t = apply(s, player_, id);
            a.area = ~(t.board | coverable(t, opp_));
        }
        watch_(a);
    }

    // can `player` create a reserved area with their next move?
    bool canMakeReserve(const State &s, int player) {
        const PieceSet &ps = PieceSet::get();
        for (int p = 0; p < kNumPieces; p++) {
            if (!(s.hand[player] >> p & 1)) continue;
            for (int id : ps.placementsOf(p))
                if (legal(s, id) && reserveMove(apply(s, player, id), player) >= 0) return true;
        }
        return false;
    }

    int randomMove(const State &s) {
        int id = forEachMove(s, player_, [&](int m) {
            return reserveMove(apply(s, player_, m), opp_) < 0;
        });
        if (id < 0) id = forEachMove(s, player_, [](int) { return true; });
        return id;
    }

    int makeReserve(const State &s) {
        return forEachMove(s, player_, [&](int m) {
            return reserveMove(apply(s, player_, m), player_) >= 0;
        });
    }

    // a move that gives the opponent no reserve and after which the player
    // can create one with the next move
    int prepareReserve(const State &s) {
        return forEachMove(s, player_, [&](int m) {
            State t = apply(s, player_, m);
            return reserveMove(t, opp_) < 0 && canMakeReserve(t, player_);
        });
    }

    int bestForReserve(const State &s) {
        int id = makeReserve(s);
        if (id >= 0) return id;
        // the opponent's moves that would create a reserve for them
        const PieceSet &ps = PieceSet::get();
        std::vector<Bitboard> threats;
        for (int p = 0; p < kNumPieces; p++) {
            if (!(s.hand[opp_] >> p & 1)) continue;
            for (int m : ps.placementsOf(p))
                if (legal(s, m) && reserveMove(apply(s, opp_, m), opp_) >= 0)
                    threats.push_back(ps.placement(m).mask);
        }
        if (!threats.empty()) {
            // a move that overlaps all of them, so that none stays possible
            id = forEachMove(s, player_, [&](int m) {
                Bitboard pm = ps.placement(m).mask;
                for (Bitboard t : threats) if (!(pm & t)) return false;
                return true;
            });
            return id >= 0 ? id : prepareReserve(s);
        }
        // a move after which the opponent cannot create a reserve
        id = forEachMove(s, player_, [&](int m) { return !canMakeReserve(apply(s, player_, m), opp_); });
        return id >= 0 ? id : prepareReserve(s);
    }

    // opening moves: in the corner square of 5x5 cells, or along the top
    // edge near the corner
    static bool inCorner(Bitboard m) {
        const Bitboard corner = 0x1f1f1f1f1fULL;  // rows 0-4, columns 0-4
        return !(m & ~corner);
    }
    static bool alongTopEdge(Bitboard m) {
        Bitboard row0 = m & 0xffULL;
        return row0 && !(row0 & ~0x1fULL);
    }

    int opening(const State &s) {
        int id = forEachMove(s, player_, [&](int m) {
            return inCorner(PieceSet::get().placement(m).mask) &&
                   !canMakeReserve(apply(s, player_, m), opp_);
        });
        return id;
    }

    int openingSafe(const State &s) {
        int fallback = -1;
        int id = forEachMove(s, player_, [&](int m) {
            if (!alongTopEdge(PieceSet::get().placement(m).mask)) return false;
            State t = apply(s, player_, m);
            if (canMakeReserve(t, opp_)) return false;
            if (fallback < 0) fallback = m;
            return canMakeReserve(t, player_);
        });
        return id >= 0 ? id : fallback;
    }

    int openingPrepare(const State &s) {
        return forEachMove(s, player_, [&](int m) {
            if (!alongTopEdge(PieceSet::get().placement(m).mask)) return false;
            State t = apply(s, player_, m);
            return reserveMove(t, opp_) < 0 && canMakeReserve(t, player_);
        });
    }

    // The move with the best score A + 2B + C, as in the solver's move ordering:
    // A = empty cells adjacent to the placement, B = cells that after the move
    // only the player can still cover, C = rank of the piece (weak pieces
    // first). Ties are broken at random.
    int scoreMove() {
        static const int rank[kNumPieces] = {8, 5, 1, 6, 2, 9, 4, 3, 7, 10, 5, 5};
        const PieceSet &ps = PieceSet::get();
        int bestId = -1, bestScore = -1000000, ties = 0;
        for (int p = 0; p < kNumPieces; p++) {
            if (!(real_.hand[player_] >> p & 1)) continue;
            for (int id : ps.placementsOf(p)) {
                if (!legal(real_, id)) continue;
                const Placement &pl = ps.placement(id);
                State t = apply(real_, player_, id);
                Bitboard mine = coverable(t, player_), theirs = coverable(t, opp_);
                int sc = popcount(pl.adjacent & ~real_.board) + 2 * popcount(mine & ~theirs & ~t.board) + rank[p];
                show(real_, id, false);
                if (sc > bestScore) { bestScore = sc; bestId = id; ties = 1; }
                else if (sc == bestScore && rng_() % ++ties == 0) bestId = id;
            }
        }
        return bestId;
    }

    // Exact minimax. Plays a winning move as soon as one is found;
    // otherwise the move with the best value, and among those the one that
    // leaves the opponent the most replies after which the player does not
    // lose (the most chances for the opponent to go wrong).
    template <class S>
    int exact(S &search) {
        const PieceSet &ps = PieceSet::get();
        int bestId = -1;
        double bestScore = -10;
        for (int p = 0; p < kNumPieces; p++) {
            if (!(real_.hand[player_] >> p & 1)) continue;
            for (int id : ps.placementsOf(p)) {
                if (!legal(real_, id)) continue;
                if (bestId < 0) bestId = id;
                show(real_, id, false);
                State t = apply(real_, player_, id);
                int v = -search.value(t.board, t.hand[opp_], t.hand[player_]);
                if (search.stopped()) return bestId;
                if (v == 1) return id;
                int good = 0, replies = 0;
                for (int q = 0; q < kNumPieces; q++) {
                    if (!(t.hand[opp_] >> q & 1)) continue;
                    for (int r : ps.placementsOf(q)) {
                        if (!legal(t, r)) continue;
                        State u = apply(t, opp_, r);
                        replies++;
                        if (search.value(u.board, u.hand[player_], u.hand[opp_]) >= 0) good++;
                        if (search.stopped()) return bestId;
                    }
                }
                double score = v + (replies ? 0.5 * good / replies : 0);
                if (score > bestScore) { bestScore = score; bestId = id; }
            }
        }
        return bestId;
    }

    int player_, opp_;
    std::mt19937 &rng_;
    const Ai::StopFn &stop_;
    bool strong_;
    const bool *analysis_;
    const Ai::WatchFn &watch_;
    const char *what_;
    std::chrono::steady_clock::time_point lastShow_{};
    std::chrono::steady_clock::duration shownFor_{};
    State real_;
    std::array<int, kNumPieces> order_;
    int start_ = 0;
};

}  // namespace

// ---------------------------------------------------------------- Ai

Ai::Ai(int level, unsigned seed, Weights weights)
    : level_(std::clamp(level, 0, 6)), weights_(weights), rng_(seed) {}

std::array<double, kNumPieces> Ai::draftWeights(const Game &game, int self) const {
    std::array<double, kNumPieces> w;
    for (int p = 0; p < kNumPieces; p++)
        w[p] = (weights_ == Weights::Original1993 || weights_ == Weights::Original1993Pure)
                   ? baseWeight(kPieceNames[p]) : solvedWeight(kPieceNames[p]);
    if (weights_ == Weights::SolvedPure || weights_ == Weights::Original1993Pure) return w;
    const int opp = 1 - self;
    auto own = [&](char c) { return game.owner(pieceIndex(c)); };
    auto mul = [&](char c, double f) { w[pieceIndex(c)] *= f; };
    // Adjustments to the pieces already drafted (from the 1993 program).
    if (own('P') == opp) { mul('U', 6); mul('L', 15); }
    if (own('P') == self) { mul('W', 6); mul('L', 5); }
    if (own('U') == self || own('P') == self) mul('F', 2);
    if (own('U') == opp) mul('P', 2);
    if (own('U') == self) mul('Y', 4);
    if (own('V') == opp) mul('L', 2);
    if (own('V') >= 0) mul('Z', 6);
    if (own('Y') == opp) mul('L', 2);
    if (own('L') == opp) { mul('Y', 5); mul('V', 7); mul('N', 7); }
    if (own('L') == self) { mul('V', 7); mul('N', 10); }
    if (own('L') == opp && own('V') == opp) mul('I', 45);
    if (own('L') == self && own('V') == self) mul('I', 20);
    if (own('I') == opp && own('L') == opp) mul('V', 15);
    if (own('I') == self && own('L') == self) mul('V', 8);
    return w;
}

int Ai::choosePick(const Game &game) {
    auto w = draftWeights(game, game.toMove());
    std::vector<double> v(kNumPieces, 0.0);
    for (int p = 0; p < kNumPieces; p++) if (game.owner(p) < 0) v[p] = w[p];
    lastStrategy_ = "weighted draft";
    return std::discrete_distribution<int>(v.begin(), v.end())(rng_);
}

int Ai::choosePlacement(const Game &game, const StopFn &stop) {
    int move = std::clamp(game.placedCount() + 1, 1, 12);
    int strategy = kStrategyTable[level_][move - 1];
    if (exactFrom_ > 0 && move >= exactFrom_) strategy = kExact;
    else if (scoreStrategy_ && strategy != kExact && level_ > 0) strategy = kScore;
    lastStrategy_ = strategyName(strategy);
    if (strategy == kExact && strongSearch_) lastStrategy_ = "exact search (solver)";
    Planner planner(game, game.toMove(), rng_, stop, analysis_, watch_, strategyName(strategy),
                    draftWeights(game, game.toMove()), strongSearch_);
    return planner.choose(strategy);
}

}  // namespace pento
