#include "display.hpp"

#include <cstdio>
#include <sstream>
#include <vector>

namespace pento {

namespace {

// ANSI escape sequences
const char *kReset = "\x1b[0m";
const char *kBold = "\x1b[1m";
const char *kDim = "\x1b[2m";
const char *kBlack = "\x1b[30m";
const char *kBgPlayer[2] = {"\x1b[44m", "\x1b[41m"};        // blue, red
const char *kBgPlayerBright[2] = {"\x1b[104m", "\x1b[101m"};
const char *kFgPlayer[2] = {"\x1b[34m", "\x1b[31m"};
const char *kBgPlaced = "\x1b[43m";                        // yellow
const char *kBgPool = "\x1b[100m";                         // grey
const char *kBgPoolBright = "\x1b[47m";

// Box-drawing characters, written as their UTF-8 bytes so that every
// compiler produces the same output.
const char *kHorizontal = "\xe2\x94\x80";   // horizontal line
const char *kVertical = "\xe2\x94\x82";     // vertical line
const char *kTopLeft = "\xe2\x94\x8c";      // top-left corner
const char *kTopRight = "\xe2\x94\x90";     // top-right corner
const char *kBottomLeft = "\xe2\x94\x94";   // bottom-left corner
const char *kBottomRight = "\xe2\x94\x98";  // bottom-right corner

std::string repeat(const char *s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

std::string clockText(double seconds) {
    int s = int(seconds);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return buf;
}

// Draws a row of pieces, each in its display shape, with a marker below the
// highlighted one.
// colour(piece) gives the background of the piece's cells; a piece for which
// it returns nullptr is left out, but its place stays empty.
template <class ColourFn>
std::vector<std::string> pieceRow(const std::vector<int> &pieces, ColourFn colour, int highlight) {
    const PieceSet &ps = PieceSet::get();
    std::vector<std::string> lines(4);
    for (int piece : pieces) {
        const Shape &s = ps.displayShape(piece);
        int h = 0, w = 0;
        for (auto &c : s) { h = std::max(h, c.first + 1); w = std::max(w, c.second + 1); }
        const char *bg = colour(piece);
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < w; c++) {
                bool filled = false;
                for (auto &cell : s) filled |= cell.first == r && cell.second == c;
                if (filled && bg) lines[r] += std::string(bg) + "  " + kReset;
                else lines[r] += "  ";
            }
            lines[r] += "   ";
        }
        // below the selected piece, a marker (no letters)
        std::string mark(2 * w + 3, ' ');
        if (bg && piece == highlight) mark.replace(w - 1, 2, "^^");
        lines[3] += std::string(kBold) + mark + kReset;
    }
    return lines;
}

}  // namespace

void Display::render(const Game &game, const View &view) const {
    const PieceSet &ps = PieceSet::get();
    std::ostringstream out;
    out << "\x1b[H\x1b[2J";  // home, clear screen
    out << kBold << "  PENTOMINOES WITH DRAFTING" << kReset;
    if (view.level >= 0) out << "      level " << view.level;
    out << "\n\n";

    bool drafting = game.phase() == Phase::Draft;

    auto header = [&](int player) {
        out << "  " << kFgPlayer[player] << kBold << "P" << player << " " << view.names[player]
            << kReset << "   " << clockText(view.clock[player]);
        if (view.timeLimit > 0) out << " / " << clockText(view.timeLimit);
        if (game.phase() != Phase::Over && game.toMove() == player)
            out << "   " << kBold << (drafting ? "<- to pick" : "<- to place") << kReset;
        out << "\n";
    };
    auto hand = [&](int player) {
        std::vector<int> pieces = game.handInPickOrder(player);  // each piece keeps its place
        int hl = (!drafting && game.toMove() == player) ? view.cursorPiece : -1;
        auto colour = [&](int p) { return p == hl ? kBgPlayerBright[player] : kBgPlayer[player]; };
        for (auto &l : pieceRow(pieces, colour, hl)) out << "  " << l << "\n";
    };

    header(1);
    hand(1);
    out << "\n";

    // the board, with the cursor placement in the colour of the player to move
    Bitboard cursor = 0;
    if (view.cursorPlacement >= 0) cursor = ps.placement(view.cursorPlacement).mask;
    char cursorLetter = view.cursorPlacement >= 0 ? kPieceNames[ps.placement(view.cursorPlacement).piece] : ' ';
    // the auxiliary board: real pieces yellow, pieces assumed in reserved
    // areas green, the examined move in the player's colour, and the cells
    // only that player can still reach marked with '+'
    auto auxCell = [&](int i) -> std::string {
        Bitboard b = Bitboard(1) << i;
        Bitboard m = view.auxPlacement >= 0 ? ps.placement(view.auxPlacement).mask : 0;
        if (m & b) return std::string(kBgPlayerBright[view.auxPlayer]) + "  " + kReset;
        if (view.auxReal & b) return std::string(kBgPlaced) + "  " + kReset;
        if (view.auxAssumed & b) return std::string("\x1b[42m") + "  " + kReset;
        if (view.auxArea & b) return std::string(kFgPlayer[view.auxPlayer]) + kBold + "+ " + kReset;
        return std::string(kDim) + ". " + kReset;
    };
    const std::string bar = repeat(kHorizontal, 2 * kBoardSize + 1);
    out << "           a b c d e f g h";
    if (view.showAux) out << "          " << kBold << "Analysis" << kReset << ": " << view.auxTitle;
    out << "\n         " << kTopLeft << bar << kTopRight;
    if (view.showAux) out << "      " << kTopLeft << bar << kTopRight;
    out << "\n";
    for (int r = 0; r < kBoardSize; r++) {
        out << "       " << r + 1 << " " << kVertical << " ";
        for (int c = 0; c < kBoardSize; c++) {
            int i = r * kBoardSize + c;
            if (cursor >> i & 1)
                out << kBgPlayerBright[game.toMove()] << kBlack << cursorLetter << ' ' << kReset;
            else if (game.board() >> i & 1)
                out << kBgPlaced << "  " << kReset;
            else
                out << kDim << ". " << kReset;
        }
        out << kVertical;
        if (view.showAux) {
            out << "      " << kVertical << " ";
            for (int c = 0; c < kBoardSize; c++) out << auxCell(r * kBoardSize + c);
            out << kVertical;
        }
        out << "\n";
    }
    out << "         " << kBottomLeft << bar << kBottomRight;
    if (view.showAux) out << "      " << kBottomLeft << bar << kBottomRight;
    out << "\n";
    out << "\n";

    if (drafting) {
        // all twelve pieces keep their places; picked pieces leave a gap
        auto colour = [&](int p) -> const char * {
            if (game.owner(p) >= 0) return nullptr;
            return p == view.cursorPiece ? kBgPoolBright : kBgPool;
        };
        out << "  Pool:\n";
        for (int start = 0; start < kNumPieces; start += 6) {
            std::vector<int> row;
            for (int p = start; p < start + 6; p++) row.push_back(p);
            for (auto &l : pieceRow(row, colour, view.cursorPiece)) out << "  " << l << "\n";
        }
        out << "\n";
    }

    hand(0);
    header(0);
    out << "\n  " << view.status << "\n";
    out << "  " << kDim << view.help << kReset << "\n";
    std::fputs(out.str().c_str(), stdout);
    std::fflush(stdout);
}

void Display::clearScreen() {
    std::fputs("\x1b[H\x1b[2J", stdout);
    std::fflush(stdout);
}

void Display::titlePage() {
    clearScreen();
    std::printf("\n\n\n\n\n");
    std::printf("                         %s%sP E N T O M I N O E S%s\n\n", kBold, kFgPlayer[0], kReset);
    std::printf("                             with drafting\n\n\n");
    std::printf("                              Version 2.0\n\n\n\n");
    std::printf("                   Written by Predrag Janicic, Belgrade\n\n");
    std::printf("            Version 1.00: 1993; C++ version for the terminal: 2026\n\n");
    std::printf("      Refactored from the 1993 C program with the AI assistant Claude,\n");
    std::printf("                      used through Claude Code\n\n\n");
    std::printf("                              %sPress ENTER%s", kDim, kReset);
    std::fflush(stdout);
}

void Display::rulesPage() {
    clearScreen();
    std::printf("\n                          %sThe game PENTOMINOES%s\n\n", kBold, kReset);
    std::printf(
        "     Pentominoes is a game for two players with the twelve pentominoes,\n"
        "  the shapes made of five squares. First, the players take turns picking\n"
        "  pieces, one at a time, until each has six. Then they take turns placing\n"
        "  their own pieces on the 8x8 board, in any orientation, on empty squares.\n"
        "  A player who cannot place any of their remaining pieces loses. If all\n"
        "  twelve pieces are placed, the game is a draw. Simple, isn't it?\n\n"
        "     The player who picks first is P0, the other player P1. Who places\n"
        "  the first piece is chosen before the game.\n\n");
    std::printf("                         %sThe program PENTOMINOES%s\n\n", kBold, kReset);
    std::printf(
        "     You can play against the program, two players can play against each\n"
        "  other, with the program's suggestions on request, or you can let the\n"
        "  program play against itself.\n\n"
        "     The program plays at levels 0 to 6; with two players, the level is\n"
        "  that of its suggestions. The levels differ not in the time the program\n"
        "  spends on a move, but in its strategy: at levels 1 to 6, from some\n"
        "  move on (earlier at higher levels), it searches the game to the end.\n\n"
        "     The pieces of P0 are %sblue%s, those of P1 %sred%s, the pieces placed on\n"
        "  the board %syellow%s, and the pieces not yet taken %sgrey%s.\n\n",
        kFgPlayer[0], kReset, kFgPlayer[1], kReset, "\x1b[33m", kReset, "\x1b[90m", kReset);
    std::printf("                              %sPress ENTER%s", kDim, kReset);
    std::fflush(stdout);
}

void Display::keysPage() {
    clearScreen();
    std::printf("\n                             %sHow to play%s\n\n", kBold, kReset);
    std::printf(
        "     The pieces of P0 are shown below the board, those of P1 above it,\n"
        "  and the pieces not yet taken between the board and P0's pieces.\n\n"
        "     Drafting: LEFT and RIGHT move along the pieces in the pool; ENTER\n"
        "  takes the marked piece.\n\n"
        "     Placing: LEFT and RIGHT select one of your pieces. DOWN and UP move\n"
        "  it over all the positions where it can be placed, row by row; SPACE\n"
        "  turns it to its next orientation. ENTER places it where it is shown.\n\n"
        "     At any time in your turn, 'a' shows the program's suggestion and\n"
        "  't' takes back your last move (against the program, together with the\n"
        "  program's reply).\n\n"
        "     While the program is thinking, 'b' makes it play the best move it\n"
        "  has found so far. At any time, 'o' shows or hides the program's\n"
        "  analysis on a second board: the moves it examines, and the cells that\n"
        "  after such a move only the player can still reach (+). The analysis\n"
        "  slows the program down.\n\n"
        "     The clocks show the time each player has used. With a time limit,\n"
        "  a player who exceeds it loses.\n\n"
        "     'q' or ESC leaves the program.\n\n");
    std::printf("                              %sPress ENTER%s", kDim, kReset);
    std::fflush(stdout);
}

std::string Display::boardText(const Game &game) {
    const PieceSet &ps = PieceSet::get();
    std::string s;
    for (int r = 0; r < kBoardSize; r++) {
        for (int c = 0; c < kBoardSize; c++) {
            char ch = '.';
            for (const Move &m : game.history())
                if (m.kind == Move::Kind::Place && (ps.placement(m.placement).mask >> (r * 8 + c) & 1))
                    ch = kPieceNames[m.piece];
            s += ch;
            s += ' ';
        }
        s += '\n';
    }
    return s;
}

}  // namespace pento
