// Pentominoes with drafting: play against the program, two players against
// each other (with the program's suggestions), or let the program play
// itself. Refactored from PENTOMIN.C (1993).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

#include "ai.hpp"
#include "display.hpp"
#include "game.hpp"
#include "terminal.hpp"

using namespace pento;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    int players = 1;          // 0 = program vs program, 1 = against the program, 2 = two players
    int human = 0;            // with one player: the human is P0 (drafts first) or P1
    int firstPlacer = 0;      // who places first
    int level[2] = {2, 2};    // program level for P0 and P1
    double timeLimit = 0;     // seconds per player and game, 0 = none
    double thinkLimit = 60;   // seconds per program move
    std::string names[2] = {"", ""};
    unsigned seed = 0;
    int batch = 0;            // > 0: play this many program-vs-program games without display
    bool analysis = false;    // show the auxiliary board while the program thinks
    Ai::Weights weights[2] = {Ai::Weights::Original1993, Ai::Weights::Original1993};
    bool scoreStrategy[2] = {false, false};  // placement by the score A + 2B + C
    bool strongSearch[2] = {false, false};   // exact search with the solver's techniques
    int exactFrom[2] = {0, 0};               // start of the exact search (0 = by level)
};

// "a,b" -> two values (one value: the same for both players)
template <class F>
void perPlayer(const std::string &s, F set) {
    auto c = s.find(',');
    set(0, s.substr(0, c));
    set(1, c == std::string::npos ? s.substr(0, c) : s.substr(c + 1));
}

Ai::Weights parseWeights(const std::string &s) {
    if (s == "solved") return Ai::Weights::Solved;
    if (s == "solved-pure") return Ai::Weights::SolvedPure;
    if (s == "1993-pure") return Ai::Weights::Original1993Pure;
    return Ai::Weights::Original1993;
}

void usage(const char *prog) {
    std::printf(
        "usage: %s [options]\n"
        "  --players N        0 = program vs program, 1 = against the program, 2 = two players\n"
        "  --human P0|P1      with one player: the human drafts first (P0) or second (P1)\n"
        "  --first-placer P0|P1  who places the first piece (default P0)\n"
        "  --level L          program level 0-6 (both sides)\n"
        "  --levels L0,L1     program levels for P0 and P1\n"
        "  --time S           time limit per player and game, in seconds\n"
        "  --think S          time limit per program move, in seconds (default 60)\n"
        "  --names A,B        names of P0 and P1\n"
        "  --seed N           random seed\n"
        "  --batch N          play N program-vs-program games without display\n"
        "  --analysis         show the program's analysis while it thinks (key 'o')\n"
        "  --placement S[,S1] placement strategies: 1993 (default) or score (A + 2B + C)\n"
        "  --search S[,S1]    exact search: 1993 (default) or solver (alpha-beta, table, ...)\n"
        "  --exact-from N[,N1] start the exact search at move N (default: by level)\n"
        "  --weights W[,W1]   drafting weights: 1993 (default), solved (from the solved\n"
        "                     placement games, with the 1993 adjustments), solved-pure\n"
        "                     or 1993-pure (without the adjustments)\n"
        "Without options, the program asks for the settings.\n",
        prog);
}

int parsePlayer(const char *s) { return (s[0] == 'P' || s[0] == 'p') ? std::atoi(s + 1) : std::atoi(s); }

std::string ask(const std::string &question, const std::string &dflt) {
    std::cout << "  " << question << " [" << dflt << "]: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line) || line.empty()) return dflt;
    return line;
}

void waitForEnter() {
    std::string line;
    std::getline(std::cin, line);
}

void startPages() {
    Display::titlePage();
    waitForEnter();
    Display::rulesPage();
    waitForEnter();
    Display::keysPage();
    waitForEnter();
}

void askOptions(Options &o) {
    Display::clearScreen();
    std::cout << "\n  PENTOMINOES WITH DRAFTING: settings\n\n";
    o.players = std::atoi(ask("Number of players (0 = program vs program, 1, 2)", "1").c_str());
    if (o.players == 1) {
        std::string a = ask("Do you want to draft first? (y/n)", "y");
        o.human = (a[0] == 'n' || a[0] == 'N') ? 1 : 0;
    }
    std::string a = ask("Does the player who drafts first also place first? (y/n)", "y");
    o.firstPlacer = (a[0] == 'n' || a[0] == 'N') ? 1 : 0;
    o.level[0] = o.level[1] = std::atoi(ask("Program level (0-6)", "2").c_str());
    if (o.players == 2) {
        o.names[0] = ask("Name of the player who drafts first", "PLAYER 1");
        o.names[1] = ask("Name of the other player", "PLAYER 2");
    }
    o.timeLimit = std::atof(ask("Time limit per player and game in seconds (0 = none)", "0").c_str());
}

const char *resultText(Result r) {
    switch (r) {
        case Result::P0Wins: return "P0 wins";
        case Result::P1Wins: return "P1 wins";
        case Result::Draw: return "draw";
        default: return "not finished";
    }
}

// ---------------------------------------------------------------- the match

class Match {
public:
    Match(const Options &o, Terminal *term)
        : o_(o), term_(term), game_(o.firstPlacer) {
        for (int p = 0; p < 2; p++) {
            human_[p] = o.players == 2 || (o.players == 1 && o.human == p);
            ai_[p] = std::make_unique<Ai>(o.level[p], o.seed * 2 + p + 1, o.weights[p]);
            ai_[p]->setPlacementOptions(o.scoreStrategy[p], o.strongSearch[p], o.exactFrom[p]);
            view_.names[p] = !o.names[p].empty() ? o.names[p]
                           : o.players == 1 ? (human_[p] ? "YOU" : "PROGRAM")
                           : o.players == 0 ? "PROGRAM (level " + std::to_string(o.level[p]) + ")"
                           : (p == 0 ? "PLAYER 1" : "PLAYER 2");
        }
        view_.timeLimit = o.timeLimit;
        analysis_ = o.analysis;
        for (int p = 0; p < 2; p++)
            ai_[p]->setAnalysis(&analysis_, [this](const Ai::Analysis &a) {
                view_.showAux = true;
                view_.auxTitle = a.what;
                view_.auxReal = a.real;
                view_.auxAssumed = a.assumed;
                view_.auxArea = a.area;
                view_.auxPlacement = a.placement;
                view_.auxPlayer = a.player;
                render();
            });
        view_.level = o.players == 2 ? -1 : o.level[human_[0] ? 1 : 0];
    }

    // Plays the game; returns false if the user quit.
    bool play() {
        while (game_.phase() != Phase::Over && timeLoser_ < 0) {
            int p = game_.toMove();
            turnStart_ = Clock::now();
            bool ok = human_[p] ? humanTurn(p) : programTurn(p);
            if (!ok) return false;
        }
        showResult();
        return true;
    }

    const Game &game() const { return game_; }

private:
    double elapsedTurn() const {
        return std::chrono::duration<double>(Clock::now() - turnStart_).count();
    }

    // charges the time of the current turn to `player`; false if over the limit
    void chargeTime(int player) {
        double t = elapsedTurn();
        view_.clock[player] = used_[player] + t;
        if (o_.timeLimit > 0 && view_.clock[player] > o_.timeLimit) timeLoser_ = player;
    }
    void endTurn(int player) { chargeTime(player); used_[player] = view_.clock[player]; }

    void render() { if (term_) display_.render(game_, view_); }

    // ---------------------------------------------------- human

    bool humanTurn(int p) {
        return game_.phase() == Phase::Draft ? humanPick(p) : humanPlace(p);
    }

    std::vector<int> poolPieces() const {
        std::vector<int> v;
        for (int i = 0; i < kNumPieces; i++) if (game_.owner(i) < 0) v.push_back(i);
        return v;
    }

    // pieces of `p` that can still be placed somewhere
    std::vector<int> placeablePieces(int p) const {
        std::vector<int> v;  // in the order shown on the screen
        for (int i : game_.handInPickOrder(p))
            if (!game_.legalPlacements(p, i).empty()) v.push_back(i);
        return v;
    }

    // takes back moves until it is a human's turn again (at least one move)
    void takeBack() {
        if (game_.history().empty()) return;
        game_.undo();
        while (!game_.history().empty() && !human_[game_.toMove()]) game_.undo();
    }

    bool humanPick(int p) {
        auto pool = poolPieces();
        int idx = 0;
        view_.help = "<- -> select piece   ENTER take   a suggestion   t take back   o analysis   q quit";
        for (;;) {
            pool = poolPieces();
            if (idx >= int(pool.size())) idx = 0;
            view_.cursorPiece = pool[idx];
            view_.cursorPlacement = -1;
            view_.status = view_.names[p] + ": pick a piece";
            chargeTime(p);
            if (timeLoser_ >= 0) return true;
            render();
            Key k = term_->readKey(250);
            switch (k.code) {
                case KeyCode::Left: idx = (idx + int(pool.size()) - 1) % int(pool.size()); break;
                case KeyCode::Right: idx = (idx + 1) % int(pool.size()); break;
                case KeyCode::Enter:
                    endTurn(p);
                    game_.pick(pool[idx]);
                    view_.cursorPiece = -1;
                    return true;
                case KeyCode::Escape: return false;
                case KeyCode::Char:
                    if (k.ch == 'q' || k.ch == 'Q') return false;
                    if (k.ch == 'o' || k.ch == 'O') toggleAnalysis();
                    if (k.ch == 'a' || k.ch == 'A') {
                        int s = ai_[p]->choosePick(game_);
                        for (int i = 0; i < int(pool.size()); i++) if (pool[i] == s) idx = i;
                    }
                    if ((k.ch == 't' || k.ch == 'T') && game_.pickCount() > 0) {
                        endTurn(p);
                        takeBack();
                        return true;
                    }
                    break;
                default: break;
            }
        }
    }

    bool humanPlace(int p) {
        const PieceSet &ps = PieceSet::get();
        int pieceIdx = 0, posIdx = 0;
        view_.help = "<- -> piece   up/down position   SPACE turn   ENTER place   a suggestion   o analysis   "
                     "t take back   q quit";
        for (;;) {
            auto pieces = placeablePieces(p);
            if (pieceIdx >= int(pieces.size())) pieceIdx = 0;
            int piece = pieces[pieceIdx];
            auto places = game_.legalPlacements(p, piece);
            if (posIdx >= int(places.size())) posIdx = 0;
            view_.cursorPiece = piece;
            view_.cursorPlacement = places[posIdx];
            view_.status = view_.names[p] + ": place a piece";
            chargeTime(p);
            if (timeLoser_ >= 0) return true;
            render();
            Key k = term_->readKey(250);
            switch (k.code) {
                case KeyCode::Left:
                    pieceIdx = (pieceIdx + int(pieces.size()) - 1) % int(pieces.size()); posIdx = 0; break;
                case KeyCode::Right:
                    pieceIdx = (pieceIdx + 1) % int(pieces.size()); posIdx = 0; break;
                case KeyCode::Down: posIdx = (posIdx + 1) % int(places.size()); break;
                case KeyCode::Up: posIdx = (posIdx + int(places.size()) - 1) % int(places.size()); break;
                case KeyCode::Space: {  // first legal position of the next orientation
                    int o = ps.placement(places[posIdx]).orientation;
                    int n = int(places.size());
                    for (int j = 1; j <= n; j++) {
                        int q = (posIdx + j) % n;
                        if (ps.placement(places[q]).orientation != o) { posIdx = q; break; }
                    }
                    break;
                }
                case KeyCode::Enter:
                    endTurn(p);
                    game_.place(places[posIdx]);
                    view_.cursorPiece = view_.cursorPlacement = -1;
                    return true;
                case KeyCode::Escape: return false;
                case KeyCode::Char:
                    if (k.ch == 'q' || k.ch == 'Q') return false;
                    if (k.ch == 'o' || k.ch == 'O') toggleAnalysis();
                    if (k.ch == 'a' || k.ch == 'A') {
                        view_.status = "Looking for a suggestion (b: best so far, o: analysis on/off)";
                        render();
                        int s = ai_[p]->choosePlacement(game_, stopFn(p));
                        int sp = ps.placement(s).piece;
                        for (int i = 0; i < int(pieces.size()); i++) if (pieces[i] == sp) pieceIdx = i;
                        auto pl = game_.legalPlacements(p, sp);
                        for (int i = 0; i < int(pl.size()); i++) if (pl[i] == s) posIdx = i;
                    }
                    if ((k.ch == 't' || k.ch == 'T') && game_.placedCount() > 0) {
                        endTurn(p);
                        takeBack();
                        return true;
                    }
                    break;
                default: break;
            }
        }
    }

    // ---------------------------------------------------- program

    // Polled while the program thinks: 'b' asks for the best move so far,
    // 'o' switches the analysis board on or off.
    Ai::StopFn stopFn(int p) {
        auto start = Clock::now();
        bestRequested_ = false;
        return [this, p, start]() {
            auto now = Clock::now();
            double think = std::chrono::duration<double>(now - start).count();
            if (o_.thinkLimit > 0 && think > o_.thinkLimit) return true;
            if (now - lastPoll_ < std::chrono::milliseconds(20)) return bestRequested_;
            lastPoll_ = now;
            chargeTime(p);
            if (timeLoser_ >= 0) return true;
            if (term_) {
                for (Key k = term_->readKey(0); k.code != KeyCode::None; k = term_->readKey(0)) {
                    if (k.code != KeyCode::Char) continue;
                    if (k.ch == 'b' || k.ch == 'B') bestRequested_ = true;
                    if (k.ch == 'o' || k.ch == 'O') toggleAnalysis();
                }
            }
            return bestRequested_;
        };
    }

    // waits, but still reacts to 'o'
    void pause(int ms) {
        auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) {
            int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count());
            Key k = term_->readKey(std::max(left, 0));
            if (k.code == KeyCode::Char && (k.ch == 'o' || k.ch == 'O')) toggleAnalysis();
        }
    }

    // 'o': switches the analysis board on or off; it shows what the program
    // examines while it thinks, and stays until the program thinks again
    void toggleAnalysis() {
        analysis_ = !analysis_;
        if (!analysis_) view_.showAux = false;
        render();
    }

    bool programTurn(int p) {
        view_.cursorPiece = view_.cursorPlacement = -1;
        view_.status = view_.names[p] + (game_.phase() == Phase::Draft
                                         ? " is picking"
                                         : " is thinking (b: best so far, o: analysis on/off)");
        view_.help = "";
        render();
        if (game_.phase() == Phase::Draft) {
            int piece = ai_[p]->choosePick(game_);
            endTurn(p);
            game_.pick(piece);
        } else {
            int id = ai_[p]->choosePlacement(game_, stopFn(p));
            endTurn(p);
            if (timeLoser_ >= 0) return true;
            if (term_) {  // show the move briefly before it is made
                view_.cursorPlacement = id;
                view_.status = view_.names[p] + " plays " + kPieceNames[PieceSet::get().placement(id).piece] +
                               " (" + ai_[p]->lastStrategy() + ")";
                render();
                pause(700);
                view_.cursorPlacement = -1;
            }
            game_.place(id);
        }
        return true;
    }

    void showResult() {
        std::string text;
        if (timeLoser_ >= 0)
            text = "P" + std::to_string(1 - timeLoser_) + " " + view_.names[1 - timeLoser_] + " WINS ON TIME";
        else if (game_.result() == Result::Draw)
            text = "DRAW: all twelve pieces are placed";
        else {
            int w = game_.result() == Result::P0Wins ? 0 : 1;
            text = o_.players == 1 ? (human_[w] ? "YOU WON, CONGRATULATIONS!" : "THE PROGRAM WON")
                                   : "P" + std::to_string(w) + " " + view_.names[w] + " WON";
        }
        view_.status = text;
        view_.help = "press any key";
        view_.cursorPiece = view_.cursorPlacement = -1;
        render();
        if (term_) term_->readKey();
    }

    Options o_;
    Terminal *term_;
    Game game_;
    Display display_;
    View view_;
    bool human_[2] = {false, false};
    std::unique_ptr<Ai> ai_[2];
    double used_[2] = {0, 0};
    int timeLoser_ = -1;
    Clock::time_point turnStart_;
    bool analysis_ = false;
    bool bestRequested_ = false;
    Clock::time_point lastPoll_{};
};

int runBatch(const Options &o) {
    int wins[3] = {0, 0, 0};
    for (int g = 0; g < o.batch; g++) {
        Options og = o;
        og.players = 0;
        og.seed = o.seed + g;
        Match m(og, nullptr);
        m.play();
        Result r = m.game().result();
        wins[r == Result::P0Wins ? 0 : r == Result::P1Wins ? 1 : 2]++;
        if (o.batch == 1) {
            std::printf("draft:");
            for (const Move &mv : m.game().history())
                if (mv.kind == Move::Kind::Pick) std::printf(" P%d:%c", mv.player, kPieceNames[mv.piece]);
            std::printf("\n%s%s\n", Display::boardText(m.game()).c_str(), resultText(r));
        }
    }
    std::printf("levels P0=%d P1=%d, first placer P%d: P0 wins %d, P1 wins %d, draws %d\n",
                o.level[0], o.level[1], o.firstPlacer, wins[0], wins[1], wins[2]);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    o.seed = unsigned(std::chrono::system_clock::now().time_since_epoch().count());
    bool any = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) { usage(argv[0]); std::exit(1); }
            return argv[++i];
        };
        any = true;
        if (a == "--players") o.players = std::atoi(next());
        else if (a == "--human") o.human = parsePlayer(next());
        else if (a == "--first-placer") o.firstPlacer = parsePlayer(next());
        else if (a == "--level") o.level[0] = o.level[1] = std::atoi(next());
        else if (a == "--levels") { const char *s = next(); o.level[0] = std::atoi(s); const char *c = std::strchr(s, ','); o.level[1] = c ? std::atoi(c + 1) : o.level[0]; }
        else if (a == "--time") o.timeLimit = std::atof(next());
        else if (a == "--think") o.thinkLimit = std::atof(next());
        else if (a == "--names") { std::string s = next(); auto c = s.find(','); o.names[0] = s.substr(0, c); if (c != std::string::npos) o.names[1] = s.substr(c + 1); }
        else if (a == "--seed") o.seed = unsigned(std::atol(next()));
        else if (a == "--batch") o.batch = std::atoi(next());
        else if (a == "--analysis") o.analysis = true;
        else if (a == "--placement") perPlayer(next(), [&](int p, const std::string &v) { o.scoreStrategy[p] = v == "score"; });
        else if (a == "--search") perPlayer(next(), [&](int p, const std::string &v) { o.strongSearch[p] = v == "solver"; });
        else if (a == "--exact-from") perPlayer(next(), [&](int p, const std::string &v) { o.exactFrom[p] = std::atoi(v.c_str()); });
        else if (a == "--weights") {
            std::string s = next();
            auto c = s.find(',');
            o.weights[0] = parseWeights(s.substr(0, c));
            o.weights[1] = parseWeights(c == std::string::npos ? s.substr(0, c) : s.substr(c + 1));
        }
        else { usage(argv[0]); return a == "--help" || a == "-h" ? 0 : 1; }
    }
    Terminal::setupConsole();
    if (o.batch > 0) return runBatch(o);
    if (!Terminal::isInteractive()) {
        std::fprintf(stderr, "Interactive play needs a terminal; use --batch N for program-vs-program games.\n");
        return 1;
    }
    if (!any) {
        startPages();
        askOptions(o);
    }
    Terminal term;
    std::fputs("\x1b[?25l", stdout);  // hide the cursor
    for (;;) {
        Match m(o, &term);
        bool finished = m.play();
        if (!finished) break;
        o.seed++;
        std::fputs("\n  Another game? (y/n) ", stdout);
        std::fflush(stdout);
        Key k = term.readKey();
        if (!(k.code == KeyCode::Char && (k.ch == 'y' || k.ch == 'Y')) && k.code != KeyCode::Enter) break;
    }
    std::fputs("\x1b[?25h\x1b[0m\n", stdout);  // show the cursor again
    return 0;
}
