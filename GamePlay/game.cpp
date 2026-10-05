#include "game.hpp"

#include <stdexcept>

namespace pento {

Game::Game(int firstPlacer) : firstPlacer_(firstPlacer) {
    for (int &o : owner_) o = -1;
}

uint16_t Game::pool() const {
    uint16_t m = 0;
    for (int p = 0; p < kNumPieces; p++) if (owner_[p] < 0) m |= 1u << p;
    return m;
}

uint16_t Game::hand(int player) const {
    uint16_t m = 0;
    for (int p = 0; p < kNumPieces; p++)
        if (owner_[p] == player && !isPlaced(p)) m |= 1u << p;
    return m;
}

std::vector<int> Game::handInPickOrder(int player) const {
    std::vector<int> v;
    for (const Move &m : history_)
        if (m.kind == Move::Kind::Pick && m.player == player && !isPlaced(m.piece)) v.push_back(m.piece);
    return v;
}

int Game::pickCount() const { return kNumPieces - popcount(pool()); }

bool Game::canPlace(int id) const {
    if (phase_ != Phase::Placement) return false;
    const Placement &pl = PieceSet::get().placement(id);
    return owner_[pl.piece] == toMove_ && !isPlaced(pl.piece) && !(pl.mask & board_);
}

bool Game::hasLegalPlacement(int player) const {
    const PieceSet &ps = PieceSet::get();
    for (int p = 0; p < kNumPieces; p++) {
        if (owner_[p] != player || isPlaced(p)) continue;
        for (int id : ps.placementsOf(p))
            if (!(ps.placement(id).mask & board_)) return true;
    }
    return false;
}

std::vector<int> Game::legalPlacements(int player, int piece) const {
    const PieceSet &ps = PieceSet::get();
    std::vector<int> v;
    for (int p = 0; p < kNumPieces; p++) {
        if (owner_[p] != player || isPlaced(p) || (piece >= 0 && p != piece)) continue;
        for (int id : ps.placementsOf(p))
            if (!(ps.placement(id).mask & board_)) v.push_back(id);
    }
    return v;
}

void Game::pick(int piece) {
    if (phase_ != Phase::Draft || owner_[piece] >= 0) throw std::logic_error("illegal pick");
    owner_[piece] = toMove_;
    history_.push_back({Move::Kind::Pick, toMove_, piece, -1});
    updateState();
}

void Game::place(int id) {
    if (!canPlace(id)) throw std::logic_error("illegal placement");
    const Placement &pl = PieceSet::get().placement(id);
    placed_ |= 1u << pl.piece;
    board_ |= pl.mask;
    history_.push_back({Move::Kind::Place, toMove_, pl.piece, id});
    updateState();
}

bool Game::undo() {
    if (history_.empty()) return false;
    Move m = history_.back();
    history_.pop_back();
    if (m.kind == Move::Kind::Pick) {
        owner_[m.piece] = -1;
    } else {
        placed_ &= ~(1u << m.piece);
        board_ &= ~PieceSet::get().placement(m.placement).mask;
    }
    updateState();
    return true;
}

// Recomputes the phase, the player to move and the result from the history.
void Game::updateState() {
    result_ = Result::None;
    int picks = pickCount();
    if (picks < kNumPieces) {
        phase_ = Phase::Draft;
        toMove_ = picks % 2;  // P0 picks first
        return;
    }
    phase_ = Phase::Placement;
    int placedNow = placedCount();
    toMove_ = (firstPlacer_ + placedNow) % 2;
    if (placedNow == kNumPieces) {
        phase_ = Phase::Over;
        result_ = Result::Draw;
    } else if (!hasLegalPlacement(toMove_)) {
        phase_ = Phase::Over;
        result_ = toMove_ == 0 ? Result::P1Wins : Result::P0Wins;
    }
}

}  // namespace pento
