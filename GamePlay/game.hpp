// The rules of Pentominoes with drafting.
//
// Drafting phase: P0 and P1 alternately pick a piece from the pool, P0
// first, until each holds six. Placement phase: the players alternately
// place one piece from their own hand, starting with the player chosen to
// place first. A player who still holds pieces but cannot place any of them
// loses; if all twelve pieces are placed, the game is a draw.
#pragma once

#include <cstdint>
#include <vector>

#include "pieces.hpp"

namespace pento {

enum class Phase { Draft, Placement, Over };

enum class Result { None, P0Wins, P1Wins, Draw };

struct Move {
    enum class Kind { Pick, Place } kind;
    int player;
    int piece;
    int placement;  // placement id, for Place
};

class Game {
public:
    explicit Game(int firstPlacer = 0);

    Phase phase() const { return phase_; }
    Result result() const { return result_; }
    int toMove() const { return toMove_; }
    int firstPlacer() const { return firstPlacer_; }

    int owner(int piece) const { return owner_[piece]; }  // -1 = still in the pool
    bool isPlaced(int piece) const { return placed_ >> piece & 1; }
    Bitboard board() const { return board_; }
    uint16_t pool() const;                    // pieces not yet picked
    uint16_t hand(int player) const;          // pieces owned and not yet placed
    std::vector<int> handInPickOrder(int player) const;  // the same, in the order they were picked
    int placedCount() const { return popcount(placed_); }
    int pickCount() const;

    bool canPlace(int placementId) const;     // legal for the player to move
    bool hasLegalPlacement(int player) const;
    std::vector<int> legalPlacements(int player, int piece = -1) const;

    void pick(int piece);                     // by the player to move
    void place(int placementId);              // by the player to move
    bool undo();                              // takes back the last move
    const std::vector<Move> &history() const { return history_; }

private:
    void updateState();

    int firstPlacer_;
    Phase phase_ = Phase::Draft;
    Result result_ = Result::None;
    int toMove_ = 0;
    int owner_[kNumPieces];
    uint16_t placed_ = 0;
    Bitboard board_ = 0;
    std::vector<Move> history_;
};

}  // namespace pento
