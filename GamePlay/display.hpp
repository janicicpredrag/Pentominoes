// Text-mode display in the terminal, with ANSI colours: the pieces of P0
// are blue, those of P1 red, the pieces placed on the board yellow, and the
// pieces still in the pool (during the draft) grey.
#pragma once

#include <string>

#include "game.hpp"

namespace pento {

// What the screen shows besides the game itself.
struct View {
    std::string names[2] = {"P0", "P1"};
    double clock[2] = {0, 0};   // seconds used by each player
    double timeLimit = 0;       // per player and game; 0 = none
    int level = -1;             // program level shown, -1 = none
    int cursorPiece = -1;       // highlighted piece (pool or hand)
    int cursorPlacement = -1;   // placement shown on the board
    std::string status;         // e.g. whose turn it is
    std::string help;           // the keys

    // the auxiliary board, showing what the program is examining
    bool showAux = false;
    std::string auxTitle;
    Bitboard auxReal = 0, auxAssumed = 0, auxArea = 0;
    int auxPlacement = -1, auxPlayer = 0;
};

class Display {
public:
    void render(const Game &game, const View &view) const;
    // the final position without clearing the screen (for non-interactive use)
    static std::string boardText(const Game &game);

    // start pages: the title page and the two pages of rules and instructions
    static void titlePage();
    static void rulesPage();
    static void keysPage();
    static void clearScreen();
};

}  // namespace pento
