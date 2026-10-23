// util.h - общее для ygg-watchdog и ygg-control на Linux: строки, файлы,
// процессы с настоящим таймаутом, время, хэши. Всё в UTF-8 (std::string).
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <functional>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <openssl/evp.h>

namespace util {

// ---------------------------------------------------------------- строки
inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}
// только латиница (для ключей, имён, протоколов) - кириллицу не трогает
inline std::string Lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
inline std::string Upper(std::string s) {
    for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}
inline bool IEquals(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }
inline bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
inline bool EndsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
inline bool Contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }
inline bool IContains(const std::string& s, const std::string& p) { return Lower(s).find(Lower(p)) != std::string::npos; }

// Разбить по символу; пустые куски выбрасываются
inline std::vector<std::string> Split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
// Разбить по символу, пустые куски сохраняются (для TSV)
inline std::vector<std::string> SplitKeep(const std::string& s, char d) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t t = s.find(d, pos);
        out.push_back(s.substr(pos, t == std::string::npos ? std::string::npos : t - pos));
        if (t == std::string::npos) break;
        pos = t + 1;
    }
    return out;
}
inline std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) { if (cur.back() == '\r') cur.pop_back(); out.push_back(cur); }
    return out;
}
inline std::string Join(const std::vector<std::string>& v, const std::string& sep) {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) { if (i) r += sep; r += v[i]; }
    return r;
}
inline std::string Replace(std::string s, const std::string& a, const std::string& b) {
    if (a.empty()) return s;
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
    return s;
}
// табы и переводы строк - в пробелы (для однострочных записей в файлах)
inline std::string OneLine(std::string s) {
    for (auto& c : s) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}
inline long long ToLL(const std::string& s, long long def = 0) {
    try { size_t n = 0; long long v = std::stoll(Trim(s), &n); return n ? v : def; } catch (...) { return def; }
}
inline double ToD(const std::string& s, double def = -1) {
    try { size_t n = 0; double v = std::stod(Trim(s), &n); return n ? v : def; } catch (...) { return def; }
}
inline bool IsNumber(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}
inline std::string Fixed(double v, int digits = 1) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(digits) << v;
    return o.str();
}
// число с запятой (для людей): 12,5
inline std::string FixedRu(double v, int digits = 1) { return Replace(Fixed(v, digits), ".", ","); }

// --- UTF-8
// Число символов (кодовых точек) в UTF-8 строке
inline size_t U8Len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
// Первые n символов
inline std::string U8Left(const std::string& s, size_t n) {
    size_t i = 0, cnt = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (cnt == n) break;
        i += l; ++cnt;
    }
    return s.substr(0, std::min(i, s.size()));
}
// Обрезать до n символов с многоточием
inline std::string U8Fit(const std::string& s, size_t n) {
    if (U8Len(s) <= n) return s;
    if (n == 0) return "";
    return U8Left(s, n - 1) + "…";
}
// Разбор UTF-8 в кодовые точки
inline std::vector<uint32_t> U8Decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; size_t l;
        if (c < 0x80) { cp = c; l = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; l = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; l = 3; }
        else { cp = c & 0x07; l = 4; }
        for (size_t k = 1; k < l && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out.push_back(cp);
        i += l;
    }
    return out;
}
inline std::string U8Encode(uint32_t cp) {
    std::string r;
    if (cp < 0x80) r += (char)cp;
    else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
    else { r += (char)(0xF0 | (cp >> 18)); r += (char)(0x80 | ((cp >> 12) & 0x3F)); r += (char)(0x80 | ((cp >> 6) & 0x3F)); r += (char)(0x80 | (cp & 0x3F)); }
    return r;
}
// Нижний регистр с кириллицей (для поиска по набранным буквам)
inline std::string U8Lower(const std::string& s) {
    std::string r;
    for (uint32_t cp : U8Decode(s)) {
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if (cp >= 0x410 && cp <= 0x42F) cp += 32;
        else if (cp == 0x401) cp = 0x451;
        r += U8Encode(cp);
    }
    return r;
}

// ---------------------------------------------------------------- файлы
inline bool FileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
inline bool IsDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
inline long long FileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1; }
inline long long FileMTime(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long long)st.st_mtime : 0; }
inline long long FileAgeSeconds(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return -1;
    long long a = (long long)time(nullptr) - (long long)st.st_mtime;
    return a < 0 ? 0 : a;
}
inline std::string DirOf(const std::string& p) {
    size_t k = p.find_last_of('/');
    return k == std::string::npos ? "./" : p.substr(0, k + 1);
}
inline std::string BaseName(const std::string& p) {
    size_t k = p.find_last_of('/');
    return k == std::string::npos ? p : p.substr(k + 1);
}
inline std::string ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// Хвост файла (для больших логов)
inline std::string ReadFileTail(const std::string& p, size_t maxBytes) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return "";
    std::streamoff size = f.tellg();
    std::streamoff from = size > (std::streamoff)maxBytes ? size - (std::streamoff)maxBytes : 0;
    f.seekg(from);
    std::string out((size_t)(size - from), '\0');
    f.read(&out[0], (std::streamsize)out.size());
    out.resize((size_t)f.gcount());
    return out;
}
inline bool WriteFile(const std::string& p, const std::string& data, mode_t mode = 0644) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n <= 0) { close(fd); return false; }
        off += (size_t)n;
    }
    close(fd);
    return true;
}
// Атомарно: временный файл рядом + fsync + rename. При обрыве питания
// остаётся либо старый файл целиком, либо новый целиком.
inline bool WriteFileAtomic(const std::string& p, const std::string& data, int modeIn = -1) {
    mode_t mode = 0644;
    struct stat st;
    if (modeIn >= 0) mode = (mode_t)modeIn;
    else if (stat(p.c_str(), &st) == 0) mode = st.st_mode & 07777;
    std::string tmp = p + ".tmp" + std::to_string(getpid());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    size_t off = 0;
    bool ok = true;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n <= 0) { ok = false; break; }
        off += (size_t)n;
    }
    if (ok) ok = fsync(fd) == 0;
    fchmod(fd, mode);
    close(fd);
    if (!ok || rename(tmp.c_str(), p.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    int dfd = open(DirOf(p).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return true;
}
inline bool AppendLine(const std::string& p, const std::string& line) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    std::string l = line + "\n";
    bool ok = write(fd, l.data(), l.size()) == (ssize_t)l.size();
    close(fd);
    return ok;
}
inline bool CopyFile(const std::string& from, const std::string& to) {
    std::string d = ReadFile(from);
    if (d.empty() && FileSize(from) != 0) return false;
    struct stat st;
    mode_t m = stat(from.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0644;
    return WriteFile(to, d, m) && chmod(to.c_str(), m) == 0;
}
inline void MkDirs(const std::string& p) {
    std::string cur;
    for (const auto& part : Split(p, '/')) {
        cur += "/" + part;
        mkdir(cur.c_str(), 0755);
    }
}
inline std::vector<std::string> ListDir(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}
// Путь к своему исполняемому файлу и его папка
inline std::string SelfExe() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = 0;
    return buf;
}
inline std::string SelfDir() { return DirOf(SelfExe()); }

// Замок на файле (flock): две проверки одновременно не идут
struct FileLock {
    int fd = -1;
    FileLock(const std::string& path, bool wait, int waitMs = 5000) {
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return;
        if (!wait) { if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); fd = -1; } return; }
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
        while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            if (std::chrono::steady_clock::now() >= until) { close(fd); fd = -1; return; }
            usleep(100 * 1000);
        }
    }
    bool ok() const { return fd >= 0; }
    ~FileLock() { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
};

// ---------------------------------------------------------------- время
inline std::string TimeText(long long t, const char* fmt = "%Y-%m-%d %H:%M:%S") {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    char b[64];
    strftime(b, sizeof(b), fmt, &tm);
    return b;
}
inline long long Now() { return (long long)time(nullptr); }
inline void SleepMs(long long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
// «3 мин», «2 ч 5 мин», «4 д»
inline std::string Ago(long long sec) {
    if (sec < 0) sec = 0;
    if (sec < 60) return std::to_string(sec) + " с";
    if (sec < 3600) return std::to_string(sec / 60) + " мин";
    if (sec < 86400) {
        long long h = sec / 3600, m = (sec % 3600) / 60;
        return std::to_string(h) + " ч" + (m ? " " + std::to_string(m) + " мин" : "");
    }
    return std::to_string(sec / 86400) + " д";
}
inline std::string MinSec(long long sec) {
    if (sec < 0) sec = 0;
    char b[32];
    snprintf(b, sizeof(b), "%lld:%02lld", sec / 60, sec % 60);
    return b;
}
static const char* RU_MONTHS[] = { "янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек" };
inline std::string DateRu(long long t, bool withTime = true) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    char b[64];
    if (withTime) snprintf(b, sizeof(b), "%d %s %02d:%02d", tm.tm_mday, RU_MONTHS[tm.tm_mon], tm.tm_hour, tm.tm_min);
    else snprintf(b, sizeof(b), "%d %s", tm.tm_mday, RU_MONTHS[tm.tm_mon]);
    return b;
}

// ---------------------------------------------------------------- процессы
struct ProcResult { bool started = false; bool timedOut = false; int code = -1; std::string out; };

// Запуск программы с аргументами (без shell), stdout+stderr в out. Таймаут
// настоящий: зависший yggdrasilctl будет убит, проверка не повиснет.
inline ProcResult Run(const std::vector<std::string>& argv, int timeoutMs = 20000, const std::string* input = nullptr) {
    ProcResult r;
    if (argv.empty()) return r;
    int outp[2], inp[2] = { -1, -1 };
    if (pipe2(outp, O_CLOEXEC) != 0) return r;
    if (input && pipe2(inp, O_CLOEXEC) != 0) { close(outp[0]); close(outp[1]); return r; }
    pid_t pid = fork();
    if (pid < 0) { close(outp[0]); close(outp[1]); if (input) { close(inp[0]); close(inp[1]); } return r; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(outp[1], 1);
        dup2(outp[1], 2);
        if (input) dup2(inp[0], 0);
        else { int dn = open("/dev/null", O_RDONLY); if (dn >= 0) dup2(dn, 0); }
        std::vector<char*> a;
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        setenv("LC_ALL", "C.UTF-8", 1);
        execvp(a[0], a.data());
        _exit(127);
    }
    r.started = true;
    close(outp[1]);
    if (input) {
        close(inp[0]);
        size_t off = 0;
        while (off < input->size()) {
            ssize_t n = write(inp[1], input->data() + off, input->size() - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(inp[1]);
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[8192];
    bool eof = false;
    while (!eof) {
        long long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { r.timedOut = true; break; }
        struct pollfd pf = { outp[0], POLLIN, 0 };
        int pr = poll(&pf, 1, (int)std::min<long long>(left, 500));
        if (pr > 0) {
            ssize_t n = read(outp[0], buf, sizeof(buf));
            if (n > 0) { if (r.out.size() < 32 * 1024 * 1024) r.out.append(buf, (size_t)n); }
            else eof = true;
        }
    }
    close(outp[0]);
    if (r.timedOut) { kill(-pid, SIGKILL); kill(pid, SIGKILL); }
    int st = 0;
    if (r.timedOut) waitpid(pid, &st, 0);
    else {
        // вывод закрыт - ждём выхода (дочерние могли держать pipe открытым дольше)
        for (;;) {
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) break;
            if (std::chrono::steady_clock::now() >= deadline) { kill(-pid, SIGKILL); kill(pid, SIGKILL); waitpid(pid, &st, 0); r.timedOut = true; break; }
            usleep(20 * 1000);
        }
    }
    if (WIFEXITED(st)) r.code = WEXITSTATUS(st);
    else r.code = -1;
    if (r.code == 127 && r.out.empty()) r.started = false;
    return r;
}
// Есть ли программа в PATH
inline std::string Which(const std::string& name) {
    if (name.find('/') != std::string::npos) return FileExists(name) ? name : "";
    const char* path = getenv("PATH");
    std::string p = path ? path : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (const auto& d : Split(p + ":/usr/sbin:/sbin", ':')) {
        std::string f = d + "/" + name;
        if (access(f.c_str(), X_OK) == 0) return f;
    }
    return "";
}
// Запустить отдельно и не ждать (свой сеанс, вывод в /dev/null)
inline bool RunDetached(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        std::vector<char*> a;
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return true;
}
inline bool IsRoot() { return geteuid() == 0; }
inline std::string HostName() {
    char b[256] = {};
    if (gethostname(b, sizeof(b) - 1) != 0) return "unknown";
    return b;
}

// ---------------------------------------------------------------- хэши (OpenSSL)
inline std::string Hex(const unsigned char* d, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += hx[d[i] >> 4]; s += hx[d[i] & 15]; }
    return s;
}
inline std::string Digest(const EVP_MD* md, const std::string& a, const std::string& b = "") {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_MD_CTX* c = EVP_MD_CTX_new();
    EVP_DigestInit_ex(c, md, nullptr);
    if (!a.empty()) EVP_DigestUpdate(c, a.data(), a.size());
    if (!b.empty()) EVP_DigestUpdate(c, b.data(), b.size());
    EVP_DigestFinal_ex(c, out, &n);
    EVP_MD_CTX_free(c);
    return Hex(out, n);
}
inline std::string Sha256Hex(const std::string& data) { return Digest(EVP_sha256(), data); }
inline std::string Sha256OfFile(const std::string& path) {
    if (!FileExists(path)) return "";
    return Sha256Hex(ReadFile(path));
}
// Git-отпечаток содержимого (как у GitHub): SHA-1 от "blob <размер>\0" + данные
inline std::string GitBlobSha1(const std::string& data) {
    std::string pre = "blob " + std::to_string(data.size());
    pre.push_back('\0');
    return Digest(EVP_sha1(), pre, data);
}
// Целый ли исполняемый ELF (заголовок + таблица секций в пределах файла)
inline bool LooksLikeElf(const std::string& d) {
    if (d.size() < 64 || d.compare(0, 4, "\x7f" "ELF") != 0 || d[4] != 2) return false;   // только 64-бит
    uint64_t shoff = 0;
    memcpy(&shoff, d.data() + 0x28, 8);
    uint16_t shentsize = 0, shnum = 0;
    memcpy(&shentsize, d.data() + 0x3A, 2);
    memcpy(&shnum, d.data() + 0x3C, 2);
    return shoff == 0 || shoff + (uint64_t)shentsize * shnum <= d.size();
}

// ---------------------------------------------------------------- версии и адреса пиров
inline std::vector<int> VersionTuple(std::string v) {
    while (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> parts;
    for (const auto& p : Split(v, '.')) {
        std::string d;
        for (char c : p) { if (c >= '0' && c <= '9') d += c; else break; }
        parts.push_back(d.empty() ? 0 : std::atoi(d.c_str()));
    }
    return parts;
}
// b новее a?
inline bool VersionNewer(const std::string& a, const std::string& b) {
    auto ta = VersionTuple(a), tb = VersionTuple(b);
    size_t n = std::max(ta.size(), tb.size());
    ta.resize(n, 0); tb.resize(n, 0);
    return tb > ta;
}

struct PeerUri { bool ok = false; std::string proto, host, port, query; };
// tls://host:443, tls://[2001:db8::1]:443, quic://1.2.3.4:443?key=...
inline PeerUri ParseUri(const std::string& uri) {
    PeerUri u;
    size_t k = uri.find("://");
    if (k == std::string::npos) return u;
    u.proto = Lower(uri.substr(0, k));
    std::string rest = uri.substr(k + 3);
    size_t q = rest.find_first_of("?/");
    if (q != std::string::npos) { u.query = rest.substr(q); rest = rest.substr(0, q); }
    if (!rest.empty() && rest[0] == '[') {
        size_t e = rest.find(']');
        if (e == std::string::npos) return u;
        u.host = rest.substr(1, e - 1);
        if (e + 1 < rest.size() && rest[e + 1] == ':') u.port = rest.substr(e + 2);
    } else {
        size_t c = rest.rfind(':');
        if (c == std::string::npos) { u.host = rest; }
        else { u.host = rest.substr(0, c); u.port = rest.substr(c + 1); }
    }
    u.ok = !u.host.empty() && !u.proto.empty();
    return u;
}
inline std::string HostFromUri(const std::string& uri) {
    PeerUri u = ParseUri(uri);
    return u.ok ? Lower(u.host) : "";
}
inline int PortFromUri(const std::string& uri) {
    PeerUri u = ParseUri(uri);
    return u.ok ? (int)ToLL(u.port, 0) : 0;
}

// Процентное кодирование (form-urlencoded)
inline std::string UrlEncode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
        else { r += '%'; r += hx[c >> 4]; r += hx[c & 15]; }
    }
    return r;
}

} // namespace util
