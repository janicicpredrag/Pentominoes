#include "terminal.hpp"

#ifdef _WIN32

// ---------------------------------------------------------------- Windows

#include <conio.h>
#include <io.h>
#include <windows.h>

#include <cstdio>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace pento {

void Terminal::setupConsole() {
    // ANSI colours and UTF-8 output (box characters)
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
}

bool Terminal::isInteractive() { return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout)); }

// _getch() reads keys without echo and without waiting for ENTER, so there
// is no mode to switch.
Terminal::Terminal() { raw_ = true; }
Terminal::~Terminal() {}

Key Terminal::readKey(int timeoutMs) {
    ULONGLONG start = GetTickCount64();
    while (!_kbhit()) {
        if (timeoutMs >= 0 && GetTickCount64() - start >= ULONGLONG(timeoutMs)) return {};
        Sleep(10);
    }
    int c = _getch();
    if (c == 0 || c == 224) {  // a special key: a second code follows
        switch (_getch()) {
            case 72: return {KeyCode::Up};
            case 80: return {KeyCode::Down};
            case 75: return {KeyCode::Left};
            case 77: return {KeyCode::Right};
        }
        return {};
    }
    if (c == '\r' || c == '\n') return {KeyCode::Enter};
    if (c == 27) return {KeyCode::Escape};
    if (c == ' ') return {KeyCode::Space};
    if (c == 8) return {KeyCode::Backspace};
    return {KeyCode::Char, char(c)};
}

}  // namespace pento

#else

// ---------------------------------------------------------------- POSIX (Linux, macOS)

#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace pento {

namespace {
termios savedTermios;

int readByte(int timeoutMs) {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, timeoutMs) <= 0) return -1;
    unsigned char c;
    return read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
}
}  // namespace

void Terminal::setupConsole() {}  // terminals understand ANSI and UTF-8

bool Terminal::isInteractive() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

Terminal::Terminal() {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &savedTermios) != 0) return;
    termios t = savedTermios;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    raw_ = tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
}

Terminal::~Terminal() {
    if (raw_) tcsetattr(STDIN_FILENO, TCSANOW, &savedTermios);
}

Key Terminal::readKey(int timeoutMs) {
    int c = readByte(timeoutMs);
    if (c < 0) return {};
    if (c == 27) {  // escape, or the start of an arrow-key sequence
        int c2 = readByte(30);
        if (c2 < 0) return {KeyCode::Escape};
        if (c2 == '[' || c2 == 'O') {
            switch (readByte(30)) {
                case 'A': return {KeyCode::Up};
                case 'B': return {KeyCode::Down};
                case 'C': return {KeyCode::Right};
                case 'D': return {KeyCode::Left};
            }
        }
        return {};
    }
    if (c == '\n' || c == '\r') return {KeyCode::Enter};
    if (c == ' ') return {KeyCode::Space};
    if (c == 127 || c == 8) return {KeyCode::Backspace};
    return {KeyCode::Char, char(c)};
}

}  // namespace pento

#endif
