// net.h - сеть для Linux: TCP с таймаутом, HTTPS через OpenSSL, прокси
// (HTTP CONNECT и SOCKS5), перебор маршрутов, пинг.
//
// Как достучаться до сайта (Telegram, GitHub) - маршруты по очереди, пока
// какой-то не сработает:
//   1. тот, что сработал в прошлый раз для этого сайта (routes.txt);
//   2. PROXY= из notify.conf;
//   3. подсказка от VPN-клиента: первая строка /run/yozmor/local_proxy.txt
//      или /etc/yozmor/local_proxy.txt (например "127.0.0.1:2080" или
//      "socks5://127.0.0.1:10808") - так свой клиент может сообщить порт;
//   4. напрямую (работает, когда VPN в TUN-режиме или его нет);
//   5. переменные окружения https_proxy / all_proxy;
//   6. автопоиск: какие известные VPN-клиенты слушают порты на 127.0.0.1,
//      каждый порт проверяется как HTTP-прокси, потом как SOCKS5.
// Сработавший маршрут запоминается.
#pragma once
#include "util.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

namespace net {

// --- настройки маршрутов (их задаёт программа: сторож - из notify.conf)
struct Settings {
    std::string stateDir;            // где routes.txt
    std::string proxyOverride;       // PROXY= из notify.conf
    std::vector<std::string> proxyClients;   // свои VPN-клиенты для автопоиска
    std::string userAgent = "ygg-watchdog";
};
inline Settings& Cfg() { static Settings s; return s; }

// ---------------------------------------------------------------- TCP
inline void SetTimeouts(int fd, int ms) {
    timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Подключиться к host:port. err: dns / timeout / refused / unreachable
inline int ConnectTcp(const std::string& host, int port, int timeoutMs, std::string* err = nullptr, double* ms = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "dns";
        return -1;
    }
    int result = -1;
    std::string e = "timeout";
    for (addrinfo* ai = res; ai && result < 0; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) continue;
        auto t0 = std::chrono::steady_clock::now();
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            e = (errno == ECONNREFUSED) ? "refused" : (errno == ENETUNREACH || errno == EHOSTUNREACH) ? "unreachable" : "refused";
            close(fd);
            continue;
        }
        if (rc != 0) {
            pollfd pf{ fd, POLLOUT, 0 };
            int pr = poll(&pf, 1, timeoutMs);
            if (pr <= 0) { close(fd); continue; }
            int soerr = 0; socklen_t len = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
            if (soerr != 0) {
                e = (soerr == ECONNREFUSED) ? "refused" : (soerr == ENETUNREACH || soerr == EHOSTUNREACH) ? "unreachable" : "timeout";
                close(fd);
                continue;
            }
        }
        if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        result = fd;
    }
    freeaddrinfo(res);
    if (result < 0 && err) *err = e;
    return result;
}

inline bool SendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}
inline bool RecvN(int fd, unsigned char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// Прокси: "127.0.0.1:2080" (HTTP), "http://...", "socks5://...", "socks5h://..."
struct ProxySpec { bool ok = false; bool socks = false; std::string host; int port = 0; };
inline ProxySpec ParseProxy(std::string s) {
    ProxySpec p;
    s = util::Trim(s);
    if (s.empty()) return p;
    std::string low = util::Lower(s);
    if (util::StartsWith(low, "socks")) p.socks = true;
    size_t k = s.find("://");
    if (k != std::string::npos) s = s.substr(k + 3);
    while (!s.empty() && s.back() == '/') s.pop_back();
    size_t at = s.rfind('@');
    if (at != std::string::npos) s = s.substr(at + 1);   // логин:пароль не поддерживаем - просто отбрасываем
    if (!s.empty() && s[0] == '[') {
        size_t e = s.find(']');
        if (e == std::string::npos) return p;
        p.host = s.substr(1, e - 1);
        if (e + 2 <= s.size()) p.port = (int)util::ToLL(s.substr(e + 2), 0);
    } else {
        size_t c = s.rfind(':');
        if (c == std::string::npos) return p;
        p.host = s.substr(0, c);
        p.port = (int)util::ToLL(s.substr(c + 1), 0);
    }
    p.ok = !p.host.empty() && p.port > 0;
    return p;
}
inline std::string ProxyText(const ProxySpec& p) {
    std::string h = p.host.find(':') != std::string::npos ? "[" + p.host + "]" : p.host;
    return std::string(p.socks ? "socks5://" : "http://") + h + ":" + std::to_string(p.port);
}

// Туннель к host:port через прокси (на уже открытом к прокси сокете)
inline bool HttpConnectTunnel(int fd, const std::string& host, int port) {
    std::string hp = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    if (!SendAll(fd, "CONNECT " + hp + " HTTP/1.1\r\nHost: " + hp + "\r\n\r\n")) return false;
    std::string resp;
    char c;
    while (resp.size() < 4096) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        resp += c;
        if (util::EndsWith(resp, "\r\n\r\n")) break;
    }
    return resp.size() >= 12 && util::StartsWith(resp, "HTTP/1.") && resp.compare(9, 3, "200") == 0;
}
inline bool Socks5Tunnel(int fd, const std::string& host, int port) {
    unsigned char g[3] = { 5, 1, 0 };
    if (!SendAll(fd, std::string((char*)g, 3))) return false;
    unsigned char r[2];
    if (!RecvN(fd, r, 2) || r[0] != 5 || r[1] != 0) return false;
    if (host.size() > 255) return false;
    std::string req;
    req += (char)5; req += (char)1; req += (char)0; req += (char)3;
    req += (char)host.size(); req += host;
    req += (char)((port >> 8) & 0xFF); req += (char)(port & 0xFF);
    if (!SendAll(fd, req)) return false;
    unsigned char h[4];
    if (!RecvN(fd, h, 4) || h[0] != 5 || h[1] != 0) return false;
    size_t skip = h[3] == 1 ? 4 : h[3] == 4 ? 16 : 0;
    if (h[3] == 3) { unsigned char l; if (!RecvN(fd, &l, 1)) return false; skip = l; }
    std::vector<unsigned char> rest(skip + 2);
    return RecvN(fd, rest.data(), rest.size());
}

// ---------------------------------------------------------------- HTTP(S)
struct Route { std::string kind; std::string proxy; std::string label; };   // direct / proxy
struct HttpResult {
    bool transportOk = false;   // сервер ответил (любым кодом)
    int status = 0;
    std::string body, err, via, location;
    bool complete = true;       // тело пришло целиком
};

struct Url { bool ok = false; bool tls = true; std::string host, path; int port = 443; };
inline Url ParseUrl(const std::string& u) {
    Url r;
    std::string rest;
    if (util::StartsWith(u, "https://")) { r.tls = true; r.port = 443; rest = u.substr(8); }
    else if (util::StartsWith(u, "http://")) { r.tls = false; r.port = 80; rest = u.substr(7); }
    else return r;
    size_t sl = rest.find('/');
    std::string hp = sl == std::string::npos ? rest : rest.substr(0, sl);
    r.path = sl == std::string::npos ? "/" : rest.substr(sl);
    size_t c = hp.rfind(':');
    if (c != std::string::npos && hp.find(']') == std::string::npos) { r.port = (int)util::ToLL(hp.substr(c + 1), r.port); hp = hp.substr(0, c); }
    r.host = hp;
    r.ok = !r.host.empty();
    return r;
}

inline SSL_CTX* TlsCtx() {
    static SSL_CTX* ctx = [] {
        signal(SIGPIPE, SIG_IGN);   // обрыв соединения - ошибка записи, а не падение программы
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(c);
        SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
        return c;
    }();
    return ctx;
}

// Разбор ответа: статус, заголовки, тело (chunked / Content-Length)
inline void ParseResponse(const std::string& raw, HttpResult& r) {
    size_t he = raw.find("\r\n\r\n");
    if (he == std::string::npos || !util::StartsWith(raw, "HTTP/1.")) { r.err = "ответ не похож на HTTP"; return; }
    r.transportOk = true;
    r.status = (int)util::ToLL(raw.substr(9, 3), 0);
    std::string head = raw.substr(0, he);
    bool chunked = false;
    long long clen = -1;
    for (const auto& line : util::Lines(head)) {
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = util::Lower(util::Trim(line.substr(0, c))), v = util::Trim(line.substr(c + 1));
        if (k == "transfer-encoding" && util::IContains(v, "chunked")) chunked = true;
        if (k == "content-length") clen = util::ToLL(v, -1);
        if (k == "location") r.location = v;
    }
    std::string body = raw.substr(he + 4);
    if (chunked) {
        std::string out;
        size_t i = 0;
        r.complete = false;
        while (i < body.size()) {
            size_t e = body.find("\r\n", i);
            if (e == std::string::npos) break;
            long long n = strtoll(body.substr(i, e - i).c_str(), nullptr, 16);
            i = e + 2;
            if (n == 0) { r.complete = true; break; }
            if (i + (size_t)n > body.size()) { out += body.substr(i); break; }
            out += body.substr(i, (size_t)n);
            i += (size_t)n + 2;
        }
        r.body = out;
    } else {
        if (clen >= 0 && (long long)body.size() < clen) r.complete = false;
        if (clen >= 0 && (long long)body.size() > clen) body.resize((size_t)clen);
        r.body = body;
    }
}

// Один запрос одним маршрутом
inline HttpResult HttpOnce(const Route& rt, const std::string& method, const Url& u, const std::string& headers,
                           const std::string* body, int timeoutMs, size_t maxBody) {
    HttpResult r;
    r.via = rt.label;
    int fd = -1;
    std::string e;
    if (rt.kind == "proxy") {
        ProxySpec ps = ParseProxy(rt.proxy);
        if (!ps.ok) { r.err = "неверный адрес прокси " + rt.proxy; return r; }
        fd = ConnectTcp(ps.host, ps.port, 4000, &e);
        if (fd < 0) { r.err = "прокси " + rt.proxy + " не отвечает"; return r; }
        SetTimeouts(fd, 10000);
        bool ok = ps.socks ? Socks5Tunnel(fd, u.host, u.port) : HttpConnectTunnel(fd, u.host, u.port);
        if (!ok) { close(fd); r.err = "прокси " + rt.proxy + " не пустил к " + u.host; return r; }
    } else {
        fd = ConnectTcp(u.host, u.port, 6000, &e);
        if (fd < 0) { r.err = e == "dns" ? "имя " + u.host + " не находится (DNS)" : "нет соединения с " + u.host + " (" + e + ")"; return r; }
    }
    SetTimeouts(fd, timeoutMs);
    SSL* ssl = nullptr;
    if (u.tls) {
        ssl = SSL_new(TlsCtx());
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, u.host.c_str());
        SSL_set1_host(ssl, u.host.c_str());
        if (SSL_connect(ssl) != 1) {
            long vr = SSL_get_verify_result(ssl);
            r.err = vr != X509_V_OK ? std::string("сертификат не прошёл проверку: ") + X509_verify_cert_error_string(vr)
                                    : "не удалось установить TLS с " + u.host;
            SSL_free(ssl);
            close(fd);
            return r;
        }
    }
    std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + "\r\nUser-Agent: " + Cfg().userAgent +
                      "\r\nAccept-Encoding: identity\r\nConnection: close\r\n" + headers;
    if (body) req += "Content-Length: " + std::to_string(body->size()) + "\r\n";
    req += "\r\n";
    if (body) req += *body;
    bool sent;
    if (ssl) sent = SSL_write(ssl, req.data(), (int)req.size()) == (int)req.size();
    else sent = SendAll(fd, req);
    if (!sent) {
        r.err = "запрос не ушёл";
    } else {
        std::string raw;
        char buf[16384];
        for (;;) {
            int n = ssl ? SSL_read(ssl, buf, sizeof(buf)) : (int)recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, (size_t)n);
            if (raw.size() > maxBody + 65536) { r.complete = false; break; }
        }
        if (raw.empty()) r.err = "сервер ничего не ответил";
        else ParseResponse(raw, r);
    }
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    close(fd);
    return r;
}

// --- маршруты
inline std::string RouteToText(const Route& r) { return r.kind + "|" + r.proxy + "|" + r.label; }
inline bool RouteFromText(const std::string& t, Route& r) {
    auto f = util::SplitKeep(t, '|');
    if (f.size() < 3) return false;
    r.kind = f[0]; r.proxy = f[1]; r.label = f[2];
    return r.kind == "direct" || (r.kind == "proxy" && !r.proxy.empty());
}
inline std::string RoutesPath() { return (Cfg().stateDir.empty() ? util::SelfDir() : Cfg().stateDir) + "routes.txt"; }
inline std::string CachedRoute(const std::string& host) {
    for (const auto& l : util::Lines(util::ReadFile(RoutesPath()))) {
        size_t t = l.find('\t');
        if (t != std::string::npos && l.substr(0, t) == util::Lower(host)) return l.substr(t + 1);
    }
    return "";
}
inline void SaveRoute(const std::string& host, const Route& r) {
    std::string h = util::Lower(host), out;
    for (const auto& l : util::Lines(util::ReadFile(RoutesPath()))) {
        size_t t = l.find('\t');
        if (t != std::string::npos && l.substr(0, t) != h) out += l + "\n";
    }
    out += h + "\t" + RouteToText(r) + "\n";
    util::WriteFileAtomic(RoutesPath(), out);   // без прав - молча не запишется
}
// Подсказки от VPN-клиента
inline std::vector<std::string> ProxyHints() {
    std::vector<std::string> v;
    for (const char* f : { "/run/yozmor/local_proxy.txt", "/etc/yozmor/local_proxy.txt" }) {
        auto lines = util::Lines(util::ReadFile(f));
        if (!lines.empty()) {
            std::string h = util::Trim(lines[0]);
            if (!h.empty() && std::find(v.begin(), v.end(), h) == v.end()) v.push_back(h);
        }
    }
    return v;
}
inline std::vector<std::string> ProxyClientNames() {
    std::vector<std::string> v = { "karing", "sing-box", "singbox", "xray", "v2ray", "nekoray", "nekobox", "throne", "hiddify",
                                   "clash", "mihomo", "naive", "happ", "invisible", "outline", "ss-local", "sslocal",
                                   "trojan", "hysteria", "tuic", "v2raya", "amnezia" };
    for (const auto& x : Cfg().proxyClients) { std::string t = util::Lower(util::Trim(x)); if (!t.empty()) v.push_back(t); }
    return v;
}

// Слушающие порты на 127.0.0.1 / 0.0.0.0 / ::1 / :: и чьи они (нужен root, чтобы видеть чужие процессы)
struct Listener { int port = 0; long long inode = 0; std::string proc; };
inline std::vector<Listener> LocalListeners() {
    std::vector<Listener> out;
    std::map<long long, size_t> byInode;
    for (const char* f : { "/proc/net/tcp", "/proc/net/tcp6" }) {
        auto lines = util::Lines(util::ReadFile(f));
        for (size_t i = 1; i < lines.size(); ++i) {
            std::istringstream ss(lines[i]);
            std::string sl, local, rem, st, q, tr, rt, uid, to, inode;
            ss >> sl >> local >> rem >> st >> q >> tr >> rt >> uid >> to >> inode;
            if (st != "0A") continue;
            size_t c = local.find(':');
            if (c == std::string::npos) continue;
            std::string addr = local.substr(0, c);
            bool okAddr = addr == "0100007F" || addr == "00000000" || addr == "00000000000000000000000000000000" ||
                          addr == "00000000000000000000000001000000";
            if (!okAddr) continue;
            Listener l;
            l.port = (int)strtol(local.substr(c + 1).c_str(), nullptr, 16);
            l.inode = util::ToLL(inode, 0);
            bool dup = false;
            for (const auto& x : out) if (x.port == l.port) dup = true;
            if (dup || l.inode == 0) continue;
            byInode[l.inode] = out.size();
            out.push_back(l);
        }
    }
    if (byInode.empty()) return out;
    for (const auto& pid : util::ListDir("/proc")) {
        if (!util::IsNumber(pid)) continue;
        std::string fdDir = "/proc/" + pid + "/fd";
        DIR* d = opendir(fdDir.c_str());
        if (!d) continue;
        std::string comm;
        while (dirent* e = readdir(d)) {
            char buf[128];
            ssize_t n = readlink((fdDir + "/" + e->d_name).c_str(), buf, sizeof(buf) - 1);
            if (n <= 0) continue;
            buf[n] = 0;
            if (strncmp(buf, "socket:[", 8) != 0) continue;
            long long ino = atoll(buf + 8);
            auto it = byInode.find(ino);
            if (it == byInode.end()) continue;
            if (comm.empty()) comm = util::Trim(util::ReadFile("/proc/" + pid + "/comm"));
            out[it->second].proc = comm;
        }
        closedir(d);
    }
    return out;
}

// Пускает ли прокси на 127.0.0.1:port к host:443 (HTTP CONNECT, потом SOCKS5)
inline std::string TestLocalProxy(int port, const std::string& host) {
    for (bool socks : { false, true }) {
        int fd = ConnectTcp("127.0.0.1", port, 1000);
        if (fd < 0) return "";
        SetTimeouts(fd, 6000);
        bool ok = socks ? Socks5Tunnel(fd, host, 443) : HttpConnectTunnel(fd, host, 443);
        close(fd);
        if (ok) return std::string(socks ? "socks5://" : "http://") + "127.0.0.1:" + std::to_string(port);
    }
    return "";
}
inline std::vector<Route> ScanLocalProxies(const std::string& host) {
    std::vector<Route> out;
    auto names = ProxyClientNames();
    int tried = 0;
    for (const auto& l : LocalListeners()) {
        if (out.size() >= 3 || tried >= 20) break;
        std::string low = util::Lower(l.proc);
        bool known = false;
        for (const auto& k : names) if (!k.empty() && low.find(k) != std::string::npos) { known = true; break; }
        if (!known) continue;
        ++tried;
        std::string px = TestLocalProxy(l.port, host);
        if (!px.empty()) out.push_back({ "proxy", px, l.proc + " (" + px + ")" });
    }
    return out;
}
inline std::string EnvProxy() {
    for (const char* k : { "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY" }) {
        const char* v = getenv(k);
        if (v && *v) return v;
    }
    return "";
}

struct RequestOpts {
    bool allowProxies = true;   // false - только напрямую (GeoIP: страна выхода самого Yggdrasil)
    int timeoutMs = 30000;
    size_t maxBody = 64 * 1024 * 1024;
};

// Ход перебора путей последнего запроса в этом потоке - для test-notify и текста ошибки
inline std::vector<std::string>& Trace() { thread_local std::vector<std::string> t; return t; }
inline std::string TraceText() {
    std::string s;
    for (const auto& x : Trace()) { if (!s.empty()) s += "; "; s += x; }
    return s;
}
// Дошли ли до самого сайта, а не до ошибки прокси по дороге
inline bool ReachedSite(const std::string& host, const HttpResult& r) {
    if (!r.transportOk) return false;
    if (r.status == 407 || r.status == 502 || r.status == 503 || r.status == 504) return false;   // так отвечает прокси без выхода
    if (util::Lower(host) == "api.telegram.org") return util::Contains(r.body, "\"ok\"");   // Telegram всегда отвечает JSON с "ok"
    return true;
}

// Запрос по URL с перебором маршрутов и переходами по редиректам
inline HttpResult Request(const std::string& method, const std::string& url, const std::string& headers = "",
                          const std::string* body = nullptr, const RequestOpts& o = RequestOpts()) {
    Url u = ParseUrl(url);
    HttpResult last;
    if (!u.ok) { last.err = "неверная ссылка " + url; return last; }
    Trace().clear();
    std::vector<Route> routes;
    auto add = [&](const Route& r) {
        for (const auto& x : routes) if (x.kind == r.kind && x.proxy == r.proxy) return;
        routes.push_back(r);
    };
    Route cached;
    bool haveCached = o.allowProxies && RouteFromText(CachedRoute(u.host), cached);
    if (haveCached) add(cached);
    if (o.allowProxies) {
        if (!Cfg().proxyOverride.empty()) add({ "proxy", Cfg().proxyOverride, "PROXY из notify.conf (" + Cfg().proxyOverride + ")" });
        for (const auto& h : ProxyHints()) add({ "proxy", h, "прокси, указанный клиентом (" + h + ")" });
    }
    add({ "direct", "", "напрямую" });
    if (o.allowProxies) { std::string ep = EnvProxy(); if (!ep.empty()) add({ "proxy", ep, "прокси из окружения (" + ep + ")" }); }

    std::set<std::string> tried;
    auto attempt = [&](const Route& rt) {
        if (!tried.insert(rt.kind + "|" + rt.proxy).second) return false;   // автопоиск нашёл уже проверенный путь
        Url cur = u;
        std::string m = method;
        const std::string* b = body;
        for (int hop = 0; hop < 5; ++hop) {
            HttpResult r = HttpOnce(rt, m, cur, headers, b, o.timeoutMs, o.maxBody);
            if (!r.transportOk) {
                Trace().push_back(rt.label + " - " + (r.err.empty() ? "нет ответа" : r.err));
                if (!last.transportOk) last = r;
                return false;
            }
            if ((r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) && !r.location.empty()) {
                Url nu = ParseUrl(r.location);
                if (!nu.ok) { last = r; return true; }
                if (r.status == 303) { m = "GET"; b = nullptr; }
                cur = nu;
                continue;
            }
            if (!ReachedSite(cur.host, r)) {
                // ответ есть, но не от сайта (прокси без выхода в интернет) - пробуем следующий путь
                Trace().push_back(rt.label + " - ответ HTTP " + std::to_string(r.status) + " не от " + cur.host + " (прокси без выхода?)");
                if (!last.transportOk) last = r;
                return false;
            }
            Trace().push_back(rt.label + " - дошло");
            last = r;
            if (o.allowProxies && (!haveCached || RouteToText(rt) != RouteToText(cached))) SaveRoute(u.host, rt);
            return true;
        }
        return true;
    };
    for (const auto& rt : routes) if (attempt(rt)) return last;
    if (o.allowProxies) {
        static std::mutex mu;
        static std::map<std::string, std::vector<Route>> scanned;   // автопоиск - один раз на сайт за запуск
        std::vector<Route> found;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!scanned.count(u.host)) scanned[u.host] = ScanLocalProxies(u.host);
            found = scanned[u.host];
        }
        if (found.empty()) Trace().push_back("автопоиск: известные VPN-клиенты не слушают подходящие порты на 127.0.0.1");
        for (const auto& rt : found) if (attempt(rt)) return last;
    }
    if (last.transportOk && !ReachedSite(u.host, last)) { last.transportOk = false; last.err = "до " + u.host + " не дошли"; }
    return last;
}
inline HttpResult Get(const std::string& url, const RequestOpts& o = RequestOpts(), const std::string& headers = "") {
    return Request("GET", url, headers, nullptr, o);
}

// ---------------------------------------------------------------- проверки связности
// Есть ли интернет вообще: TCP к нескольким устойчивым адресам, без DNS
inline bool HasInternet() {
    for (const char* ip : { "1.1.1.1", "8.8.8.8", "9.9.9.9" }) {
        int fd = ConnectTcp(ip, 443, 3000);
        if (fd >= 0) { close(fd); return true; }
    }
    return false;
}
inline bool DnsWorks() {
    for (const char* n : { "github.com", "cloudflare.com" }) {
        addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(n, "443", &hints, &res) == 0) { freeaddrinfo(res); return true; }
    }
    return false;
}

// TCP-проба многих адресов параллельно (каталог, поиск замены). ms = -1 - не
// подключился; err: dns / timeout / refused / udp (quic не проверить) / bad
struct Probe { std::string host; int port = 0; double ms = -1; std::string err; };
inline std::vector<Probe> ProbeMany(const std::vector<std::string>& uris, int waitMs = 15000) {
    auto res = std::make_shared<std::vector<Probe>>(uris.size());
    auto left = std::make_shared<std::atomic<int>>(0);
    for (size_t i = 0; i < uris.size(); ++i) {
        Probe& p = (*res)[i];
        util::PeerUri pu = util::ParseUri(uris[i]);
        p.host = util::Lower(pu.host);
        p.port = (int)util::ToLL(pu.port, 0);
        if (pu.proto == "quic") { p.err = "udp"; continue; }
        if (!pu.ok || p.port == 0) { p.err = "bad"; continue; }
        ++*left;
        std::thread([res, left, i] {
            Probe tmp = (*res)[i];
            std::string e;
            double ms = -1;
            int fd = ConnectTcp(tmp.host, tmp.port, 3000, &e, &ms);
            if (fd >= 0) { close(fd); tmp.ms = ms; tmp.err.clear(); }
            else { tmp.err = e; }
            (*res)[i] = tmp;
            --*left;
        }).detach();
    }
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (*left > 0 && std::chrono::steady_clock::now() < until) util::SleepMs(50);
    // зависшие потоки дописывают в свою копию res (shared_ptr) - здесь отдаём снимок
    std::vector<Probe> snap = *res;
    for (auto& p : snap) if (p.ms < 0 && p.err.empty()) p.err = "timeout";
    return snap;
}

// Пинг IPv6 через системный ping. err: "" / timeout / local_block / unreachable / other
struct PingResult { bool ok = false; double ms = -1; std::string err, out; };
inline PingResult Ping6(const std::string& addr, int timeoutMs = 2000) {
    PingResult r;
    int w = std::max(1, timeoutMs / 1000);
    util::ProcResult p = util::Run({ "ping", "-6", "-n", "-c", "1", "-W", std::to_string(w), addr }, timeoutMs + 3000);
    r.out = p.out;
    size_t t = p.out.find("time=");
    if (p.code == 0 && t != std::string::npos) {
        r.ok = true;
        r.ms = util::ToD(p.out.substr(t + 5), 0);
        return r;
    }
    std::string low = util::Lower(p.out);
    if (!p.started) r.err = "other";
    else if (util::Contains(low, "operation not permitted") || util::Contains(low, "permission denied")) r.err = "local_block";
    else if (util::Contains(low, "unreachable")) r.err = "unreachable";
    else r.err = "timeout";
    return r;
}

} // namespace net
