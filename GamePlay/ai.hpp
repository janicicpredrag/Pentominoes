// The computer player, refactored from the 1993 program PENTOMIN.C.
//
// Drafting: a random choice weighted by the strength of the pieces, with the
// weights adjusted to the pieces both players already hold.
//
// Placement: one of several strategies, chosen by the level (0-6) and the
// number of the move. Most of them are built on the notion of a *reserved*
// area: empty cells that the opponent can no longer cover with any of their
// pieces. A piece of the player that fits entirely into such an area can be
// placed there at any later time, whatever the opponent does. From some move
// on (depending on the level) the program searches the game tree to the end
// (exact minimax).
#pragma once

#include <array>
#include <functional>
#include <random>
#include <string>

#include "game.hpp"

namespace pento {

class Ai {
public:
    // stop() is polled during long searches; when it returns true, the
    // program plays the best move found so far.
    using StopFn = std::function<bool()>;

    // What the program is examining, for the auxiliary board ("analysis").
    struct Analysis {
        Bitboard real = 0;      // pieces really on the board
        Bitboard assumed = 0;   // pieces assumed placed in reserved areas
        int placement = -1;     // the move being examined
        Bitboard area = 0;      // after it, the cells only the player can reach
        int player = 0;
        std::string what;       // the strategy
    };
    using WatchFn = std::function<void(const Analysis &)>;

    // Drafting weights: those of the 1993 program, or weights derived from
    // the strength of the pieces in the solved placement games (Table 3 of
    // the paper), with or without the 1993 adjustments to the pieces
    // already drafted ("Pure": without them).
    enum class Weights { Original1993, Original1993Pure, Solved, SolvedPure };

    Ai(int level, unsigned seed, Weights weights = Weights::Original1993);

    // Improvements of the placement phase, derived from the solvers:
    // scoreStrategy - instead of the 1993 heuristics, play the move with the
    //   best score A + 2B + C (the move ordering of the solver);
    // strongSearch - exact search with the solver's techniques (alpha-beta,
    //   exact-value table, killer moves, move ordering);
    // exactFrom - start the exact search at this move (1-12; 0 = as the level says).
    void setPlacementOptions(bool scoreStrategy, bool strongSearch, int exactFrom) {
        scoreStrategy_ = scoreStrategy; strongSearch_ = strongSearch; exactFrom_ = exactFrom;
    }

    int level() const { return level_; }
    void setLevel(int level) { level_ = level; }

    int choosePick(const Game &game);                        // a piece from the pool
    int choosePlacement(const Game &game, const StopFn &stop);  // a placement id

    const std::string &lastStrategy() const { return lastStrategy_; }

    // While *analysis is true, watch() is called with the moves being
    // examined (at most every few hundredths of a second).
    void setAnalysis(const bool *analysis, WatchFn watch) { analysis_ = analysis; watch_ = std::move(watch); }

    // weights of the pieces for `player`, as used when drafting
    std::array<double, kNumPieces> draftWeights(const Game &game, int player) const;

private:
    int level_;
    Weights weights_;
    bool scoreStrategy_ = false, strongSearch_ = false;
    int exactFrom_ = 0;
    std::mt19937 rng_;
    std::string lastStrategy_;
    const bool *analysis_ = nullptr;
    WatchFn watch_;
};

}  // namespace pento
