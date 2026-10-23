// tui.h - полноэкранный интерфейс в терминале без сторонних библиотек:
// свой холст из ячеек, рамки, цвета (truecolor или 256 цветов), разбор
// клавиш. Перерисовываются только изменившиеся ячейки - без мерцания.
#pragma once
#include "util.h"
#include <termios.h>
#include <sys/ioctl.h>
#include <csignal>
#include <atomic>

namespace tui {

struct Color { int r = -1, g = 0, b = 0; bool none() const { return r < 0; } bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b; } };
inline Color Hex(const char* h) {
    Color c;
    unsigned v = (unsigned)strtoul(h + (h[0] == '#'), nullptr, 16);
    c.r = (int)(v >> 16) & 255; c.g = (int)(v >> 8) & 255; c.b = (int)v & 255;
    return c;
}
// Палитра из макетов
namespace pal {
inline const Color BG = Hex("#0b0f14"), TXT = Hex("#e6edf3"), DIM = Hex("#7d8590"), DIM2 = Hex("#4b535d"),
    WHITE = Hex("#ffffff"), ORANGE = Hex("#ffbd3a"), RED = Hex("#ff6b6b"),
    GREEN = Hex("#3ef08a"), GFRAME = Hex("#1f8f50"), GHL = Hex("#163a27"),
    BLUE = Hex("#7cc4ff"), BFRAME = Hex("#2b6c9c"), BHL = Hex("#173352");
}

struct Cell {
    std::string ch = " ";
    Color fg, bg;
    bool bold = false;
    bool operator==(const Cell& o) const { return ch == o.ch && fg == o.fg && bg == o.bg && bold == o.bold; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

// Холст: рисуем сюда, потом Terminal::Present показывает разницу
struct Canvas {
    int w = 0, h = 0;
    Color bg = pal::BG;
    std::vector<Cell> cells;
    void Resize(int W, int H) { w = W; h = H; Clear(); }
    void Clear() {
        Cell c; c.fg = pal::TXT; c.bg = bg;
        cells.assign((size_t)std::max(0, w * h), c);
    }
    Cell* At(int x, int y) { return (x < 0 || y < 0 || x >= w || y >= h) ? nullptr : &cells[(size_t)(y * w + x)]; }
    // Текст с позиции x; возвращает x после последнего символа. maxX - граница (не включительно)
    int Put(int x, int y, const std::string& text, Color fg = pal::TXT, bool bold = false, Color bgc = Color(), int maxX = -1) {
        if (maxX < 0) maxX = w;
        for (uint32_t cp : util::U8Decode(text)) {
            if (x >= maxX) break;
            if (Cell* c = At(x, y)) {
                c->ch = util::U8Encode(cp);
                c->fg = fg;
                c->bold = bold;
                if (!bgc.none()) c->bg = bgc;
            }
            ++x;
        }
        return x;
    }
    // Несколько кусков подряд разными цветами
    struct Seg { std::string t; Color c; bool b = false; };
    // Не влезло до maxX - последний видимый символ заменяется на «…»
    int Seq(int x, int y, const std::vector<Seg>& parts, int maxX = -1) {
        if (maxX < 0) maxX = w;
        size_t total = 0;
        for (const auto& p : parts) total += util::U8Len(p.t);
        int x0 = x;
        for (const auto& p : parts) x = Put(x, y, p.t, p.c, p.b, Color(), maxX);
        if (x0 + (int)total > maxX && maxX - 1 > x0) { if (Cell* c = At(maxX - 1, y)) c->ch = "…"; }
        return x;
    }
    void FillBg(int x0, int y, int x1, Color c) { for (int x = x0; x < x1; ++x) if (Cell* p = At(x, y)) p->bg = c; }
    void FillRect(int x0, int y0, int x1, int y1, Color c) {
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) if (Cell* p = At(x, y)) { p->ch = " "; p->bg = c; }
    }
    void Box(int x, int y, int bw, int bh, const std::string& title, Color frame, Color tcol) {
        if (bw < 2 || bh < 2) return;
        std::string top = "╭", bot = "╰";
        for (int i = 0; i < bw - 2; ++i) { top += "─"; bot += "─"; }
        top += "╮"; bot += "╯";
        Put(x, y, top, frame);
        Put(x, y + bh - 1, bot, frame);
        for (int yy = y + 1; yy < y + bh - 1; ++yy) { Put(x, yy, "│", frame); Put(x + bw - 1, yy, "│", frame); }
        if (!title.empty()) Put(x + 2, y, " " + util::U8Fit(title, (size_t)std::max(0, bw - 6)) + " ", tcol, true);
    }
    // Полоска-индикатор: заполненная часть цветом, остаток - тусклыми точками
    void Bar(int x, int y, int bw, double frac, Color c) {
        frac = std::max(0.0, std::min(1.0, frac));
        int n = (int)std::lround(bw * frac);
        for (int i = 0; i < bw; ++i) Put(x + i, y, i < n ? "█" : "░", i < n ? c : pal::DIM2);
    }
};

// Клавиши
enum class K { None, Char, Up, Down, Left, Right, Enter, Esc, Tab, BackTab, PgUp, PgDn, Home, End, Backspace, Delete, CtrlC, Resize };
struct Key { K k = K::None; std::string text; bool is(char c) const { return k == K::Char && text.size() == 1 && text[0] == c; } };

inline std::atomic<bool>& ResizeFlag() { static std::atomic<bool> f{ false }; return f; }

class Terminal {
public:
    bool truecolor = true;
    Canvas canvas;

    bool Start() {
        if (!isatty(0) || !isatty(1)) return false;
        tcgetattr(0, &orig_);
        Active() = this;
        termios raw = orig_;
        raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(unsigned)(OPOST);
        raw.c_cflag |= CS8;
        raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &raw);
        const char* ct = getenv("COLORTERM");
        truecolor = ct && (util::Contains(ct, "truecolor") || util::Contains(ct, "24bit"));
        const char* term = getenv("TERM");
        if (!truecolor && term && (util::Contains(term, "direct") || util::Contains(term, "kitty") || util::Contains(term, "alacritty"))) truecolor = true;
        Write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J");
        signal(SIGWINCH, [](int) { ResizeFlag() = true; });
        signal(SIGTERM, [](int) { if (Active()) Active()->Stop(); _exit(0); });
        signal(SIGHUP, [](int) { if (Active()) Active()->Stop(); _exit(0); });
        atexit([] { if (Active()) Active()->Stop(); });
        started_ = true;
        UpdateSize();
        return true;
    }
    void Stop() {
        if (!started_) return;
        started_ = false;
        Write("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
        tcsetattr(0, TCSAFLUSH, &orig_);
    }
    // Временно отдать терминал (запустить nano, less и т.п.)
    void Suspend() { if (!started_) return; Write("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l"); tcsetattr(0, TCSAFLUSH, &orig_); suspended_ = true; }
    void Resume() {
        if (!suspended_) return;
        suspended_ = false;
        termios raw = orig_;
        raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(unsigned)(OPOST);
        raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSAFLUSH, &raw);
        Write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J");
        prev_.clear();
    }
    ~Terminal() { Stop(); }
    bool UpdateSize() {
        winsize ws{};
        int W = 80, H = 24;
        if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { W = ws.ws_col; H = ws.ws_row; }
        if (W != canvas.w || H != canvas.h) { canvas.Resize(W, H); prev_.clear(); return true; }
        canvas.Clear();
        return false;
    }
    void Present() {
        std::string out;
        out.reserve(canvas.cells.size() * 4);
        bool full = prev_.size() != canvas.cells.size();
        if (full) out += "\x1b[0m\x1b[2J";
        Color curFg, curBg;
        bool curBold = false, haveAttr = false;
        int lastX = -2, lastY = -1;
        for (int y = 0; y < canvas.h; ++y) {
            for (int x = 0; x < canvas.w; ++x) {
                const Cell& c = canvas.cells[(size_t)(y * canvas.w + x)];
                if (!full && prev_[(size_t)(y * canvas.w + x)] == c) continue;
                if (!(lastY == y && lastX == x - 1)) out += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
                if (!haveAttr || c.bold != curBold) {
                    out += c.bold ? "\x1b[1m" : "\x1b[22m";
                    curBold = c.bold;
                }
                if (!haveAttr || !(c.fg == curFg)) { out += Sgr(c.fg, true); curFg = c.fg; }
                Color bg = c.bg.none() ? canvas.bg : c.bg;
                if (!haveAttr || !(bg == curBg)) { out += Sgr(bg, false); curBg = bg; }
                haveAttr = true;
                out += c.ch;
                lastX = x; lastY = y;
            }
        }
        if (!out.empty()) Write(out);
        prev_ = canvas.cells;
    }
    // Ждать клавишу до timeoutMs. K::None - время вышло.
    Key Read(int timeoutMs) {
        if (ResizeFlag().exchange(false)) { Key k; k.k = K::Resize; return k; }
        if (pending_.empty()) {
            pollfd pf{ 0, POLLIN, 0 };
            int pr = poll(&pf, 1, timeoutMs);
            if (pr <= 0) { Key k; if (ResizeFlag().exchange(false)) k.k = K::Resize; return k; }
            char buf[256];
            ssize_t n = read(0, buf, sizeof(buf));
            if (n > 0) pending_.append(buf, (size_t)n);
        }
        return Parse();
    }

private:
    termios orig_{};
    bool started_ = false, suspended_ = false;
    std::vector<Cell> prev_;
    std::string pending_;

    static Terminal*& Active() { static Terminal* t = nullptr; return t; }
    static void Write(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = write(1, s.data() + off, s.size() - off);
            if (n <= 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
            off += (size_t)n;
        }
    }
    std::string Sgr(const Color& c, bool fg) const {
        if (c.none()) return fg ? "\x1b[39m" : "\x1b[49m";
        if (truecolor)
            return std::string(fg ? "\x1b[38;2;" : "\x1b[48;2;") + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" + std::to_string(c.b) + "m";
        auto q = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
        int idx = 16 + 36 * q(c.r) + 6 * q(c.g) + q(c.b);
        int avg = (c.r + c.g + c.b) / 3;
        if (std::abs(c.r - avg) < 10 && std::abs(c.g - avg) < 10 && std::abs(c.b - avg) < 10 && avg > 8 && avg < 238)
            idx = 232 + (avg - 8) / 10;
        return std::string(fg ? "\x1b[38;5;" : "\x1b[48;5;") + std::to_string(idx) + "m";
    }
    Key Parse() {
        Key k;
        if (pending_.empty()) return k;
        unsigned char c = (unsigned char)pending_[0];
        auto eat = [&](size_t n) { pending_.erase(0, n); };
        if (c == 0x1b) {
            if (pending_.size() == 1) {
                // одиночный Esc или начало последовательности - подождать чуть-чуть
                pollfd pf{ 0, POLLIN, 0 };
                if (poll(&pf, 1, 30) > 0) { char b[64]; ssize_t n = read(0, b, sizeof(b)); if (n > 0) pending_.append(b, (size_t)n); }
                if (pending_.size() == 1) { eat(1); k.k = K::Esc; return k; }
            }
            char t = pending_[1];
            if (t == '[' || t == 'O') {
                size_t e = 2;
                while (e < pending_.size() && !((pending_[e] >= 'A' && pending_[e] <= 'Z') || (pending_[e] >= 'a' && pending_[e] <= 'z') || pending_[e] == '~')) ++e;
                if (e >= pending_.size()) { eat(pending_.size()); return k; }
                std::string seq = pending_.substr(2, e - 2);
                char fin = pending_[e];
                eat(e + 1);
                switch (fin) {
                    case 'A': k.k = K::Up; break; case 'B': k.k = K::Down; break;
                    case 'C': k.k = K::Right; break; case 'D': k.k = K::Left; break;
                    case 'H': k.k = K::Home; break; case 'F': k.k = K::End; break;
                    case 'Z': k.k = K::BackTab; break;
                    case '~':
                        if (seq == "5") k.k = K::PgUp; else if (seq == "6") k.k = K::PgDn;
                        else if (seq == "1" || seq == "7") k.k = K::Home; else if (seq == "4" || seq == "8") k.k = K::End;
                        else if (seq == "3") k.k = K::Delete;
                        break;
                    default: break;
                }
                return k;
            }
            eat(1);
            k.k = K::Esc;
            return k;
        }
        if (c == '\r' || c == '\n') { eat(1); k.k = K::Enter; return k; }
        if (c == '\t') { eat(1); k.k = K::Tab; return k; }
        if (c == 0x7f || c == 0x08) { eat(1); k.k = K::Backspace; return k; }
        if (c == 0x03 || c == 0x04) { eat(1); k.k = K::CtrlC; return k; }
        if (c < 0x20) { eat(1); return k; }
        size_t l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (pending_.size() < l) { eat(pending_.size()); return k; }
        k.k = K::Char;
        k.text = pending_.substr(0, l);
        eat(l);
        return k;
    }
};

// Поле ввода одной строки (для поиска и форм)
struct LineEdit {
    std::string text;
    bool Handle(const Key& k) {
        if (k.k == K::Char) { text += k.text; return true; }
        if (k.k == K::Backspace) {
            if (!text.empty()) {
                size_t i = text.size() - 1;
                while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80) --i;
                text.erase(i);
            }
            return true;
        }
        return false;
    }
};

// Прокрутка списка: держит выбранную строку в видимой области
struct ListView {
    int sel = 0, top = 0;
    void Clamp(int count, int visible) {
        if (count <= 0) { sel = 0; top = 0; return; }
        sel = std::max(0, std::min(sel, count - 1));
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;
        top = std::max(0, std::min(top, std::max(0, count - visible)));
    }
    bool Handle(const Key& k, int count, int visible) {
        if (k.k == K::Up) { --sel; }
        else if (k.k == K::Down) { ++sel; }
        else if (k.k == K::PgUp) { sel -= visible; }
        else if (k.k == K::PgDn) { sel += visible; }
        else if (k.k == K::Home) { sel = 0; }
        else if (k.k == K::End) { sel = count - 1; }
        else return false;
        Clamp(count, visible);
        return true;
    }
};

} // namespace tui
