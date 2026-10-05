// The twelve pentominoes and their placements on the 8x8 board.
#pragma once

#include <bitset>
#include <cstdint>
#include <utility>
#include <vector>

namespace pento {

using Bitboard = uint64_t;  // bit 8*row + col

constexpr int kBoardSize = 8;
constexpr int kNumPieces = 12;
constexpr Bitboard kFullBoard = ~Bitboard(0);

// Piece i has the name kPieceNames[i]: F I L N P T U V W X Y Z.
extern const char kPieceNames[kNumPieces + 1];

int pieceIndex(char name);  // -1 if not a piece name

inline int popcount(Bitboard b) { return int(std::bitset<64>(b).count()); }
inline Bitboard cellBit(int row, int col) { return Bitboard(1) << (row * kBoardSize + col); }

using Shape = std::vector<std::pair<int, int>>;  // cells (row, col), normalised

struct Placement {
    int piece;
    int orientation;  // index into PieceSet::orientations(piece)
    int row, col;     // offset of the orientation's bounding box
    Bitboard mask;
    Bitboard adjacent;  // empty-board cells orthogonally adjacent to the placement
};

// All orientations and placements, computed once.
class PieceSet {
public:
    static const PieceSet &get();

    const std::vector<Placement> &placements() const { return placements_; }
    const Placement &placement(int id) const { return placements_[id]; }
    // ids of the placements of a piece, ordered by orientation, row, column
    const std::vector<int> &placementsOf(int piece) const { return byPiece_[piece]; }
    const std::vector<Shape> &orientations(int piece) const { return orientations_[piece]; }
    // the orientation used to draw a piece in a hand (the flattest one)
    const Shape &displayShape(int piece) const { return orientations_[piece][display_[piece]]; }

private:
    PieceSet();
    std::vector<Placement> placements_;
    std::vector<std::vector<int>> byPiece_;
    std::vector<std::vector<Shape>> orientations_;
    std::vector<int> display_;
};

}  // namespace pento
