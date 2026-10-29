// ygg.h - Yggdrasil на Linux: где что лежит, yggdrasilctl, конфиг, служба.
#pragma once
#include "util.h"
#include "json.h"
#include "net.h"

namespace ygg {

static const char* SERVICE = "yggdrasil";

struct Paths {
    std::string exe = "/usr/bin/yggdrasil";
    std::string ctl = "/usr/bin/yggdrasilctl";
    std::string conf = "/etc/yggdrasil/yggdrasil.conf";
    std::string adminListen;   // из конфига (AdminListen); пусто - по умолчанию yggdrasilctl
    std::vector<std::string> EndpointArgs() const {
        if (adminListen.empty()) return {};
        return { "-endpoint=" + adminListen };
    }
};

// Аргументы командной строки, как их показывает systemctl (ExecStart=...)
inline std::vector<std::string> SplitCmd(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false, any = false;
    for (char c : s) {
        if (c == '"') { q = !q; any = true; continue; }
        if (!q && (c == ' ' || c == '\t')) { if (any || !cur.empty()) out.push_back(cur); cur.clear(); any = false; continue; }
        cur += c;
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

// Где Yggdrasil: по ExecStart службы, иначе стандартные пути пакета
inline Paths Discover() {
    Paths p;
    for (const char* c : { "/usr/bin/yggdrasil", "/usr/local/bin/yggdrasil", "/usr/sbin/yggdrasil", "/opt/yggdrasil/yggdrasil" })
        if (util::FileExists(c)) { p.exe = c; break; }
    for (const char* c : { "/usr/bin/yggdrasilctl", "/usr/local/bin/yggdrasilctl", "/usr/sbin/yggdrasilctl", "/opt/yggdrasil/yggdrasilctl" })
        if (util::FileExists(c)) { p.ctl = c; break; }
    if (!util::FileExists(p.conf) && util::FileExists("/etc/yggdrasil.conf")) p.conf = "/etc/yggdrasil.conf";
    util::ProcResult r = util::Run({ "systemctl", "show", SERVICE, "-p", "ExecStart", "--value" }, 5000);
    // формат: { path=/usr/bin/yggdrasil ; argv[]=/usr/bin/yggdrasil -useconffile /etc/... ; ... }
    size_t a = r.out.find("argv[]=");
    if (a != std::string::npos) {
        size_t e = r.out.find(" ;", a);
        auto args = SplitCmd(r.out.substr(a + 7, e == std::string::npos ? std::string::npos : e - a - 7));
        if (!args.empty() && util::FileExists(args[0])) {
            p.exe = args[0];
            std::string ctl = util::DirOf(args[0]) + "yggdrasilctl";
            if (util::FileExists(ctl)) p.ctl = ctl;
        }
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            std::string k = util::Lower(args[i]);
            if (k == "-useconffile" || k == "--useconffile") p.conf = args[i + 1];
        }
    }
    return p;
}

// Адрес управления (AdminListen): из JSON конфига или прямо из файла (HJSON)
inline std::string FindAdminListen(const Paths& p, const json::Value& cfg) {
    if (cfg.isObject())
        for (const auto& kv : cfg.obj)
            if (util::Lower(kv.first) == "adminlisten" && kv.second.isString() && !kv.second.str().empty()) return kv.second.str();
    std::string raw = util::ReadFile(p.conf), low = util::Lower(raw);
    size_t k = low.find("adminlisten");
    while (k != std::string::npos) {
        size_t c = k + 11;
        while (c < raw.size() && (raw[c] == '"' || raw[c] == ' ' || raw[c] == '\t')) ++c;
        if (c < raw.size() && raw[c] == ':') {
            ++c;
            while (c < raw.size() && (raw[c] == ' ' || raw[c] == '\t' || raw[c] == '"')) ++c;
            size_t e = c;
            while (e < raw.size() && raw[e] != '"' && raw[e] != ',' && raw[e] != '\r' && raw[e] != '\n' && raw[e] != ' ' && raw[e] != '}') ++e;
            if (e > c) return raw.substr(c, e - c);
        }
        k = low.find("adminlisten", k + 11);
    }
    return "";
}

// Пути для yggdrasilctl - один раз на процесс
inline const Paths& CtlPaths() {
    static const Paths p = [] {
        Paths q = Discover();
        std::string al = FindAdminListen(q, json::Value());
        q.adminListen = util::Lower(al) == "none" ? "" : al;
        return q;
    }();
    return p;
}

// Конфиг в JSON (через сам yggdrasil, он понимает и HJSON с комментариями)
inline bool LoadConfig(const Paths& p, json::Value& out, std::string& err) {
    if (!util::FileExists(p.conf)) { err = "не найден конфиг " + p.conf; return false; }
    util::ProcResult r = util::Run({ p.exe, "-useconffile", p.conf, "-normaliseconf", "-json" }, 15000);
    if (!r.started) { err = "не запускается " + p.exe; return false; }
    json::Value v;
    if (r.code != 0 || !json::ParseLoose(r.out, v) || !v.isObject()) {
        err = "yggdrasil не принял конфиг: " + util::U8Left(util::Trim(r.out), 200);
        if (r.code != 0 && util::Contains(util::Lower(r.out), "permission denied")) err = "нет прав читать " + p.conf + " (нужен root)";
        return false;
    }
    out = v;
    return true;
}
inline bool ConfigFileValid(const Paths& p, const std::string& file) {
    if (!util::FileExists(file)) return false;
    util::ProcResult r = util::Run({ p.exe, "-useconffile", file, "-normaliseconf" }, 15000);
    return r.started && !r.timedOut && r.code == 0;
}

// Сохранить конфиг: проверка самим yggdrasil -> копия старого -> атомарная
// запись. Временный файл - рядом с конфигом и с его правами (в нём ключ).
inline std::string SaveConfig(const Paths& p, const json::Value& cfgIn, std::string* backupOut = nullptr) {
    json::Value cfg = cfgIn;
    if (cfg.isObject() && !cfg.has("AdminListen") && !p.adminListen.empty() &&
        util::Contains(util::Lower(util::ReadFile(p.conf)), "adminlisten"))
        cfg.at("AdminListen") = json::MakeString(p.adminListen);
    std::string text = json::Dump(cfg, true) + "\n";
    std::string tmp = p.conf + ".check";
    struct stat st;
    mode_t mode = stat(p.conf.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0600;
    if (!util::WriteFile(tmp, text, mode)) return "не удалось записать временный файл рядом с конфигом (нужен root)";
    util::ProcResult r = util::Run({ p.exe, "-useconffile", tmp, "-normaliseconf" }, 15000);
    unlink(tmp.c_str());
    if (!r.started) return "не запускается " + p.exe;
    if (r.code != 0) return "yggdrasil отклонил конфиг: " + util::U8Left(util::Trim(r.out), 200);
    std::string backup = p.conf + ".bak-" + util::TimeText(util::Now(), "%Y%m%d-%H%M%S");
    if (!util::CopyFile(p.conf, backup)) return "не удалось сделать копию старого конфига";
    if (!util::WriteFileAtomic(p.conf, text, (int)mode)) return "не удалось записать конфиг (копия: " + backup + ")";
    if (stat(backup.c_str(), &st) == 0) { /* права копии - как у конфига */ }
    if (backupOut) *backupOut = backup;
    return "";
}

// Вызов yggdrasilctl -json <cmd> [аргументы]. Ответ - JSON; иначе err = текст.
inline bool Ctl(const std::vector<std::string>& cmd, json::Value& out, std::string& err, int timeoutMs = 8000) {
    const Paths& p = CtlPaths();
    std::vector<std::string> a = { p.ctl };
    for (const auto& e : p.EndpointArgs()) a.push_back(e);
    a.push_back("-json");
    for (const auto& c : cmd) a.push_back(c);
    util::ProcResult r = util::Run(a, timeoutMs);
    if (!r.started) { err = "не запускается yggdrasilctl (" + p.ctl + ")"; return false; }
    if (r.timedOut) { err = "yggdrasilctl не ответил за " + std::to_string(timeoutMs / 1000) + " с"; return false; }
    json::Value v;
    if (!json::ParseLoose(r.out, v)) {
        err = util::Trim(r.out);
        if (err.empty()) err = "yggdrasilctl ничего не ответил";
        if (util::IContains(err, "permission denied")) err = "нет доступа к сокету управления Yggdrasil (нужен root)";
        else if (util::IContains(err, "no such file") || util::IContains(err, "connection refused")) err = "Yggdrasil не отвечает (служба не запущена?)";
        return false;
    }
    if (v.isObject() && !v["error"].str().empty()) { err = v["error"].str(); return false; }
    out = v;
    return true;
}
inline bool Ctl(const std::string& cmd, json::Value& out, std::string& err, int timeoutMs = 8000) {
    return Ctl(std::vector<std::string>{ cmd }, out, err, timeoutMs);
}

// ---------------------------------------------------------------- пиры
struct PeerDetail {
    std::string uri;
    bool up = false;
    bool inbound = false;
    double rttMs = -1;       // -1 = неизвестно
    double uptimeSec = -1;
    double rxBytes = -1, txBytes = -1, rxRate = -1, txRate = -1;
    std::string lastError, key, ip;
    int flaps = 0;           // переподключений за последние полчаса (из state сторожа)
    bool unstable = false;
};
// Разбор "yggdrasilctl -json getPeers" (Yggdrasil 0.5: latency в наносекундах)
inline std::vector<PeerDetail> ParsePeers(const json::Value& v) {
    std::vector<PeerDetail> out;
    const json::Value& arr = v["peers"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const json::Value& x = arr[i];
        PeerDetail d;
        d.uri = x["remote"].str();
        if (d.uri.empty()) continue;
        d.up = x["up"].boolean();
        d.inbound = x["inbound"].boolean();
        double lat = x["latency"].num(-1);
        if (d.up && lat >= 0) d.rttMs = lat / 1e6;
        d.uptimeSec = x["uptime"].num(-1);
        d.rxBytes = x["bytes_recvd"].num(-1);
        d.txBytes = x["bytes_sent"].num(-1);
        d.rxRate = x["rate_recvd"].num(-1);
        d.txRate = x["rate_sent"].num(-1);
        d.lastError = x["last_error"].str();
        d.key = x["key"].str();
        d.ip = x["address"].str();
        out.push_back(d);
    }
    return out;
}
inline std::vector<PeerDetail> GetPeers(bool* ok = nullptr, std::string* err = nullptr) {
    json::Value v;
    std::string e;
    bool good = Ctl("getPeers", v, e, 20000) && v["peers"].isArray();
    if (ok) *ok = good;
    if (err) *err = e;
    return good ? ParsePeers(v) : std::vector<PeerDetail>();
}
inline const PeerDetail* FindPeer(const std::vector<PeerDetail>& v, const std::string& uri) {
    for (const auto& d : v) if (util::IEquals(d.uri, uri)) return &d;
    return nullptr;
}

// ---------------------------------------------------------------- служба (systemd)
struct ServiceInfo {
    bool exists = false;
    std::string active;    // active / inactive / failed / activating
    std::string sub;       // running / dead / ...
    std::string enabled;   // enabled / disabled / static / masked
    long long since = 0;   // когда вошла в нынешнее состояние (unix)
    long long pid = 0;
    long long memBytes = -1;
    bool running() const { return active == "active"; }
    bool autoStart() const { return enabled == "enabled" || enabled == "enabled-runtime" || enabled == "static" || enabled == "alias"; }
};
inline ServiceInfo QueryService(const std::string& name = SERVICE) {
    ServiceInfo s;
    util::ProcResult r = util::Run({ "systemctl", "show", name, "--timestamp=unix",
                                     "-p", "LoadState,ActiveState,SubState,UnitFileState,ActiveEnterTimestamp,MainPID,MemoryCurrent" }, 5000);
    std::map<std::string, std::string> kv;
    for (const auto& l : util::Lines(r.out)) {
        size_t e = l.find('=');
        if (e != std::string::npos) kv[l.substr(0, e)] = l.substr(e + 1);
    }
    s.exists = kv["LoadState"] == "loaded";
    s.active = kv["ActiveState"];
    s.sub = kv["SubState"];
    s.enabled = kv["UnitFileState"];
    std::string ts = kv["ActiveEnterTimestamp"];
    if (!ts.empty() && ts[0] == '@') s.since = util::ToLL(ts.substr(1), 0);
    s.pid = util::ToLL(kv["MainPID"], 0);
    s.memBytes = kv["MemoryCurrent"].empty() || kv["MemoryCurrent"][0] == '[' ? -1 : util::ToLL(kv["MemoryCurrent"], -1);
    return s;
}
// systemctl <action> <name>. Пусто - успех, иначе текст ошибки.
inline std::string ServiceCtl(const std::string& action, const std::string& name = SERVICE) {
    util::ProcResult r = util::Run({ "systemctl", action, name }, 45000);
    if (!r.started) return "systemctl не найден";
    if (r.timedOut) return "systemctl " + action + " не завершился за 45 с";
    if (r.code != 0) return util::Trim(r.out).empty() ? "код " + std::to_string(r.code) : util::Trim(r.out);
    return "";
}
// Перезапуск через N секунд одноразовым таймером systemd: команда успевает
// уйти по ещё живому каналу Yggdrasil (SSH по адресу Yggdrasil).
inline std::string RestartDelayed(int seconds) {
    std::string unit = "ygg-delayed-restart-" + std::to_string(util::Now());
    util::ProcResult r = util::Run({ "systemd-run", "--unit=" + unit, "--on-active=" + std::to_string(seconds) + "s",
                                     "systemctl", "restart", SERVICE }, 15000);
    if (!r.started) return "systemd-run не найден";
    if (r.code != 0) return util::Trim(r.out);
    return "";
}

// ---------------------------------------------------------------- обновления Yggdrasil
struct UpdateInfo { bool checked = false, ok = false; std::string tag, page, err, debUrl, debName; long long at = 0; };
inline std::string InstalledVersion(const Paths& p) {
    util::ProcResult r = util::Run({ p.exe, "-version" }, 10000);
    for (const auto& l : util::Lines(r.out)) {
        size_t c = util::Lower(l).find("build version:");
        if (c != std::string::npos) return util::Trim(l.substr(c + 14));
    }
    return "";
}
inline std::string DebArch() {
    std::vector<std::string> cmd = { "dpkg", "--print-architecture" };   // отдельной переменной: иначе GCC 15 ложно предупреждает
    util::ProcResult r = util::Run(cmd, 5000);
    std::string a = util::Trim(r.out);
    return a.empty() ? "amd64" : a;
}
inline UpdateInfo CheckYggUpdate() {
    UpdateInfo u;
    u.checked = true;
    u.at = util::Now();
    net::HttpResult r = net::Get("https://api.github.com/repos/yggdrasil-network/yggdrasil-go/releases/latest", net::RequestOpts(),
                                 "Accept: application/vnd.github+json\r\n");
    json::Value v;
    if (!r.transportOk || r.status != 200 || !json::Parse(r.body, v) || v["tag_name"].str().empty()) {
        u.err = !r.transportOk ? "GitHub недоступен" : "GitHub ответил кодом " + std::to_string(r.status);
        return u;
    }
    u.ok = true;
    u.tag = v["tag_name"].str();
    if (!u.tag.empty() && (u.tag[0] == 'v' || u.tag[0] == 'V')) u.tag.erase(0, 1);
    u.page = v["html_url"].str();
    std::string arch = DebArch();
    const json::Value& as = v["assets"];
    // В релизе несколько .deb одной архитектуры: обычный yggdrasil-0.5.15-amd64.deb
    // и пакеты для роутеров (vyos, edgerouter...) - те требуют свои системы и
    // apt их не поставит. Берём точное имя, иначе - самое короткое без «роутерных» слов.
    std::string exact = "yggdrasil-" + u.tag + "-" + arch + ".deb";
    for (size_t i = 0; i < as.size(); ++i) {
        std::string n = as[i]["name"].str(), l = util::Lower(n);
        if (!util::EndsWith(l, ".deb") || !util::Contains(l, arch)) continue;
        if (util::Contains(l, "vyos") || util::Contains(l, "edgerouter") || util::Contains(l, "router") || util::Contains(l, "openwrt")) continue;
        bool better = u.debName.empty() || n == exact || (u.debName != exact && n.size() < u.debName.size());
        if (better) { u.debName = n; u.debUrl = as[i]["browser_download_url"].str(); }
    }
    return u;
}

// ---------------------------------------------------------------- адреса
inline bool IsYggAddress(const std::string& a) {
    in6_addr x;
    if (inet_pton(AF_INET6, a.c_str(), &x) != 1) return false;
    return (x.s6_addr[0] & 0xFE) == 0x02;
}
inline bool SameIp6(const std::string& a, const std::string& b) {
    in6_addr x, y;
    return inet_pton(AF_INET6, a.c_str(), &x) == 1 && inet_pton(AF_INET6, b.c_str(), &y) == 1 && memcmp(&x, &y, sizeof(x)) == 0;
}

} // namespace ygg
