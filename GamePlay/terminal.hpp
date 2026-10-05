// Keyboard input from the terminal, in raw mode (no echo, no line buffering).
#pragma once

namespace pento {

enum class KeyCode { None, Left, Right, Up, Down, Enter, Space, Escape, Backspace, Char };

struct Key {
    KeyCode code = KeyCode::None;
    char ch = 0;  // for KeyCode::Char
};

class Terminal {
public:
    Terminal();   // switches the terminal to raw mode
    ~Terminal();  // restores it

    // Waits for a key at most timeoutMs milliseconds (-1 = forever);
    // returns KeyCode::None on timeout.
    Key readKey(int timeoutMs = -1);

    // are standard input and output a terminal?
    static bool isInteractive();

    // Prepares the console for ANSI colours and UTF-8 output (needed on
    // Windows; nothing to do elsewhere). Call once at the start.
    static void setupConsole();

private:
    bool raw_ = false;
};

}  // namespace pento
