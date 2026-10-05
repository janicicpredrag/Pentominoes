#include "pieces.hpp"

#include <algorithm>
#include <cstring>

namespace pento {

const char kPieceNames[kNumPieces + 1] = "FILNPTUVWXYZ";

int pieceIndex(char name) {
    const char *p = std::strchr(kPieceNames, name);
    return (p && name) ? int(p - kPieceNames) : -1;
}

namespace {

const Shape kBaseShapes[kNumPieces] = {
    /* F */ {{0, 1}, {0, 2}, {1, 0}, {1, 1}, {2, 1}},
    /* I */ {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}},
    /* L */ {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 1}},
    /* N */ {{0, 1}, {1, 1}, {2, 0}, {2, 1}, {3, 0}},
    /* P */ {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 0}},
    /* T */ {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {2, 1}},
    /* U */ {{0, 0}, {0, 2}, {1, 0}, {1, 1}, {1, 2}},
    /* V */ {{0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2}},
    /* W */ {{0, 0}, {1, 0}, {1, 1}, {2, 1}, {2, 2}},
    /* X */ {{0, 1}, {1, 0}, {1, 1}, {1, 2}, {2, 1}},
    /* Y */ {{0, 1}, {1, 0}, {1, 1}, {2, 1}, {3, 1}},
    /* Z */ {{0, 0}, {0, 1}, {1, 1}, {2, 1}, {2, 2}},
};

Shape normalize(Shape s) {
    int rmin = 100, cmin = 100;
    for (auto &c : s) { rmin = std::min(rmin, c.first); cmin = std::min(cmin, c.second); }
    for (auto &c : s) { c.first -= rmin; c.second -= cmin; }
    std::sort(s.begin(), s.end());
    return s;
}

// all distinct orientations: four rotations, with and without reflection
std::vector<Shape> orientationsOf(const Shape &base) {
    std::vector<Shape> result;
    for (int refl = 0; refl < 2; refl++) {
        Shape s = base;
        if (refl) for (auto &c : s) c.second = -c.second;
        for (int rot = 0; rot < 4; rot++) {
            for (auto &c : s) c = {c.second, -c.first};
            Shape n = normalize(s);
            if (std::find(result.begin(), result.end(), n) == result.end()) result.push_back(n);
        }
    }
    return result;
}

std::pair<int, int> extent(const Shape &s) {
    int h = 0, w = 0;
    for (auto &c : s) { h = std::max(h, c.first + 1); w = std::max(w, c.second + 1); }
    return {h, w};
}

}  // namespace

const PieceSet &PieceSet::get() {
    static const PieceSet instance;
    return instance;
}

PieceSet::PieceSet() : byPiece_(kNumPieces), orientations_(kNumPieces), display_(kNumPieces, 0) {
    for (int p = 0; p < kNumPieces; p++) {
        orientations_[p] = orientationsOf(kBaseShapes[p]);
        int best = 0;
        for (int o = 0; o < int(orientations_[p].size()); o++) {
            auto [h, w] = extent(orientations_[p][o]);
            auto [bh, bw] = extent(orientations_[p][best]);
            if (h < bh || (h == bh && w < bw)) best = o;
        }
        display_[p] = best;
        for (int o = 0; o < int(orientations_[p].size()); o++) {
            const Shape &s = orientations_[p][o];
            auto [h, w] = extent(s);
            for (int r = 0; r + h <= kBoardSize; r++)
                for (int c = 0; c + w <= kBoardSize; c++) {
                    Bitboard m = 0, adj = 0;
                    for (auto &cell : s) m |= cellBit(r + cell.first, c + cell.second);
                    for (auto &cell : s) {
                        static const int dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
                        for (int k = 0; k < 4; k++) {
                            int rr = r + cell.first + dr[k], cc = c + cell.second + dc[k];
                            if (rr >= 0 && rr < kBoardSize && cc >= 0 && cc < kBoardSize)
                                adj |= cellBit(rr, cc);
                        }
                    }
                    byPiece_[p].push_back(int(placements_.size()));
                    placements_.push_back({p, o, r, c, m, adj & ~m});
                }
        }
    }
}

}  // namespace pento
