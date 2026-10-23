// core.cpp - логика сторожа Yggdrasil для Linux (перенос Windows-версии).
//
//   - основные пиры - из yggdrasil.conf; живых пиров держится 3-7: меньше -
//     сторож добирает резервные, больше - лишние резервные убирает;
//   - замена ищется по очереди: выход трафика (GeoIP) -> регион поездки ->
//     домашний регион -> страна основных пиров; сначала быстрая проба портов,
//     потом настоящее рукопожатие через сам Yggdrasil;
//   - пир, который "Up", но постоянно переподключается, живым не считается;
//   - разбор "почему нет связи", устройства с диагностикой, крайний перебор,
//     рабочая копия конфига, батарея, Telegram, обновления из репозитория.
//
// Подключается в main.cpp и в tests/tests.cpp (одна единица трансляции).
#include "../common/util.h"
#include "../common/json.h"
#include "../common/net.h"
#include "../common/ygg.h"
#include "version.h"
#include <atomic>
#include <mutex>
#include <optional>

using util::Join;

// ------------------------------------------------------------------ файлы рядом с программой
std::string g_dirOverride;   // для тестов
std::string ProgDir() { return g_dirOverride.empty() ? util::SelfDir() : g_dirOverride; }
std::string LogPath()      { return ProgDir() + "watchdog.log"; }
std::string StatePath()    { return ProgDir() + "state.txt"; }
std::string EventsPath()   { return ProgDir() + "events.log"; }
std::string HistoryPath()  { return ProgDir() + "history.log"; }
std::string PeerStatsPath(){ return ProgDir() + "peer_stats.txt"; }
std::string PeerCacheDir() { return ProgDir() + "peers_cache/"; }
std::string NotifyPath()   { return ProgDir() + "notify.conf"; }
std::string VpnApprovalPath() { return ProgDir() + "vpn_hook_approval.txt"; }
std::string TgLastPath()   { return ProgDir() + "telegram_last.txt"; }
std::string UpdateFailedPath() { return ProgDir() + "update_failed.txt"; }
std::string CitiesPath()   { return ProgDir() + "cities.dat"; }
std::string TickLockPath() { return ProgDir() + ".tick.lock"; }

const char* CURRENT_VERSION = WATCHDOG_VERSION;

// ------------------------------------------------------------------ журнал
bool g_verbose = false;
bool g_quiet = false;        // окно (TUI) работает в этом же процессе - в консоль не писать
std::mutex g_logMu;

void WriteOut(const std::string& s) {
    if (g_quiet) return;
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}
void Log(const std::string& level, const std::string& msg) {
    if (level == "DEBUG" && !g_verbose) return;
    std::string line = "[" + util::TimeText(util::Now()) + "] " + level + ": " + msg;
    std::lock_guard<std::mutex> lk(g_logMu);
    WriteOut(line + "\n");
    util::AppendLine(LogPath(), line);
}
void LogInfo(const std::string& m)  { Log("INFO", m); }
void LogError(const std::string& m) { Log("ERROR", m); }
void LogDebug(const std::string& m) { Log("DEBUG", m); }
// только на экран, без записи в лог (списки, справка, ошибки ввода)
void Print(const std::string& m) { WriteOut(m + "\n"); }
void PrintError(const std::string& m) { Print("Ошибка: " + m); }

// Журнал значимых событий (для окна): unix-время<TAB>уровень<TAB>текст.
// Уровни: ok / warn / alert / info.
void AppendEvent(const std::string& level, const std::string& message) {
    util::AppendLine(EventsPath(), std::to_string(util::Now()) + "\t" + level + "\t" + util::OneLine(message));
}

// Лог не растёт бесконечно: строки старше 14 суток выбрасываются
const int LOG_RETENTION_DAYS = 14;
void CleanOldLogEntries() {
    long long sz = util::FileSize(LogPath());
    if (sz < 256 * 1024) return;
    std::string content = util::ReadFile(LogPath());
    long long cutoff = util::Now() - LOG_RETENTION_DAYS * 86400LL;
    std::string kept;
    bool dropped = false;
    for (const auto& line : util::Lines(content)) {
        struct tm tm{};
        if (line.size() > 21 && line[0] == '[' && strptime(line.c_str() + 1, "%Y-%m-%d %H:%M:%S", &tm)) {
            tm.tm_isdst = -1;
            if ((long long)mktime(&tm) < cutoff) { dropped = true; continue; }
        }
        kept += line + "\n";
    }
    if (dropped) util::WriteFileAtomic(LogPath(), kept);
}

// ------------------------------------------------------------------ notify.conf
struct NotifyConfig {
    std::string botToken, chatId, proxy;
    std::vector<std::string> proxyClients;
    bool valid = false;
};
NotifyConfig LoadNotifyConfig() {
    NotifyConfig c;
    for (const auto& line : util::Lines(util::ReadFile(NotifyPath()))) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = util::Trim(line.substr(0, eq)), v = util::Trim(line.substr(eq + 1));
        if (k == "BOT_TOKEN") c.botToken = v;
        else if (k == "CHAT_ID") c.chatId = v;
        else if (k == "PROXY") c.proxy = v;
        else if (k == "PROXY_CLIENTS") for (const auto& x : util::Split(v, ',')) c.proxyClients.push_back(util::Trim(x));
    }
    c.valid = !c.botToken.empty() && !c.chatId.empty();
    return c;
}
// Настройки сети - один раз при старте
void ApplyNetSettings() {
    NotifyConfig c = LoadNotifyConfig();
    net::Cfg().stateDir = ProgDir();
    net::Cfg().proxyOverride = c.proxy;
    net::Cfg().proxyClients = c.proxyClients;
    net::Cfg().userAgent = std::string("ygg-watchdog/") + CURRENT_VERSION;
}

// ------------------------------------------------------------------ Telegram
std::string g_tgLastVia, g_tgLastErr;
bool SendTelegram(const std::string& message) {
    NotifyConfig c = LoadNotifyConfig();
    if (!c.valid) return false;
    std::string body = "chat_id=" + util::UrlEncode(c.chatId) + "&text=" + util::UrlEncode("[" + util::HostName() + "] " + message);
    net::HttpResult r = net::Request("POST", "https://api.telegram.org/bot" + c.botToken + "/sendMessage",
                                     "Content-Type: application/x-www-form-urlencoded\r\n", &body);
    bool ok = r.transportOk && r.status == 200;
    std::string err;
    if (!r.transportOk) err = "не достучался до api.telegram.org ни напрямую, ни через прокси (" + r.err + ")";
    else if (!ok) {
        json::Value v;
        std::string desc = json::Parse(r.body, v) ? v["description"].str() : "";
        err = "сервер ответил HTTP " + std::to_string(r.status) + (desc.empty() ? "" : ": " + desc);
    }
    g_tgLastVia = ok ? r.via : "";
    g_tgLastErr = err;
    util::WriteFileAtomic(TgLastPath(), std::to_string(util::Now()) + "\t" + (ok ? "1" : "0") + "\t" + util::OneLine(g_tgLastVia) +
                                        "\t" + util::OneLine(err) + "\n");
    if (!ok) LogError("Telegram: сообщение не отправлено - " + err);
    return ok;
}
// Значимое событие: в журнал событий и в Telegram
void Notify(const std::string& level, const std::string& message) {
    AppendEvent(level, message);
    SendTelegram(message);
}

// ------------------------------------------------------------------ города и страны
struct City {
    std::string countryCode, name, nameRu;
    double lat = 0, lon = 0;
    long population = 0;
    std::string displayName() const { return nameRu.empty() ? name : nameRu; }
    std::string displayLabel() const { return nameRu.empty() ? name : name + " (" + nameRu + ")"; }
};
const std::vector<City>& LoadCityDatabase() {
    static std::vector<City> cities;
    static bool loaded = false;
    if (loaded) return cities;
    loaded = true;
    std::string raw = util::ReadFile(CitiesPath());
    if (raw.empty()) { LogError("Не найден cities.dat рядом с программой - поиск по городам (расстояние) работать не будет"); return cities; }
    for (const auto& line : util::Lines(raw)) {
        auto f = util::SplitKeep(line, '\t');
        if (f.size() < 5) continue;
        City c;
        c.countryCode = f[0]; c.name = f[1];
        c.lat = util::ToD(f[2], 0); c.lon = util::ToD(f[3], 0);
        c.population = (long)util::ToLL(f[4], 0);
        if (f.size() >= 6) c.nameRu = f[5];
        if (c.name.empty()) continue;
        cities.push_back(c);
    }
    return cities;
}
double HaversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0, k = M_PI / 180.0;
    double dLat = (lat2 - lat1) * k, dLon = (lon2 - lon1) * k;
    double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * k) * cos(lat2 * k) * sin(dLon / 2) * sin(dLon / 2);
    return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}
std::optional<City> FindCity(const std::vector<City>& db, const std::string& iso, const std::string& name) {
    for (const auto& c : db) if (util::IEquals(c.countryCode, iso) && util::IEquals(c.name, name)) return c;
    return std::nullopt;
}
std::vector<City> GetCitiesForCountry(const std::vector<City>& db, const std::string& iso) {
    std::vector<City> out;
    for (const auto& c : db) if (util::IEquals(c.countryCode, iso)) out.push_back(c);
    std::sort(out.begin(), out.end(), [](const City& a, const City& b) { return a.name < b.name; });
    return out;
}
const std::vector<std::pair<std::string, std::string>> COUNTRY_ISO = {
    {"armenia","AM"},{"australia","AU"},{"austria","AT"},{"brazil","BR"},{"canada","CA"},{"chile","CL"},
    {"czechia","CZ"},{"finland","FI"},{"france","FR"},{"germany","DE"},{"hong-kong","HK"},{"hungary","HU"},
    {"india","IN"},{"indonesia","ID"},{"japan","JP"},{"latvia","LV"},{"luxembourg","LU"},{"moldova","MD"},
    {"netherlands","NL"},{"new-zealand","NZ"},{"norway","NO"},{"philippines","PH"},{"poland","PL"},{"romania","RO"},
    {"russia","RU"},{"saudi-arabia","SA"},{"singapore","SG"},{"slovakia","SK"},{"south-africa","ZA"},{"spain","ES"},
    {"sweden","SE"},{"switzerland","CH"},{"taiwan","TW"},{"turkey","TR"},{"ukraine","UA"},{"united-kingdom","GB"},
    {"united-states","US"},
};
const std::vector<std::pair<std::string, std::string>> COUNTRY_RU = {
    {"armenia","Армения"},{"australia","Австралия"},{"austria","Австрия"},{"brazil","Бразилия"},{"canada","Канада"},
    {"chile","Чили"},{"czechia","Чехия"},{"finland","Финляндия"},{"france","Франция"},{"germany","Германия"},
    {"hong-kong","Гонконг"},{"hungary","Венгрия"},{"india","Индия"},{"indonesia","Индонезия"},{"japan","Япония"},
    {"latvia","Латвия"},{"luxembourg","Люксембург"},{"moldova","Молдова"},{"netherlands","Нидерланды"},
    {"new-zealand","Новая Зеландия"},{"norway","Норвегия"},{"philippines","Филиппины"},{"poland","Польша"},
    {"romania","Румыния"},{"russia","Россия"},{"saudi-arabia","Саудовская Аравия"},{"singapore","Сингапур"},
    {"slovakia","Словакия"},{"south-africa","ЮАР"},{"spain","Испания"},{"sweden","Швеция"},{"switzerland","Швейцария"},
    {"taiwan","Тайвань"},{"turkey","Турция"},{"ukraine","Украина"},{"united-kingdom","Великобритания"},{"united-states","США"},
};
std::string SlugToIso(const std::string& slug) { for (const auto& [s, i] : COUNTRY_ISO) if (util::IEquals(s, slug)) return i; return ""; }
std::string IsoToSlug(const std::string& iso) { for (const auto& [s, i] : COUNTRY_ISO) if (util::IEquals(i, iso)) return s; return ""; }
std::string SlugToRu(const std::string& slug) { for (const auto& [s, r] : COUNTRY_RU) if (util::IEquals(s, slug)) return r; return slug; }

// ------------------------------------------------------------------ база публичных пиров (GitHub, с кэшем)
// Кэш в peers_cache/: свежий (до 6 ч) - без запроса к GitHub (бережёт лимит
// api.github.com: 60 запросов в час); GitHub недоступен - кэш любой давности.
const long long PEER_CACHE_FRESH_SECONDS = 6 * 3600;
const char* PEERS_TREE_URL = "https://api.github.com/repos/yggdrasil-network/public-peers/git/trees/master?recursive=1";
const char* PEERS_RAW_BASE = "https://raw.githubusercontent.com/yggdrasil-network/public-peers/master/";
bool g_githubFailed = false;   // в этой проверке GitHub не ответил (для разбора «не VPN ли»)

std::string FetchCached(const std::string& name, const std::string& url, const std::string& mustContain) {
    std::string path = PeerCacheDir() + name;
    long long age = util::FileAgeSeconds(path);
    if (age >= 0 && age < PEER_CACHE_FRESH_SECONDS) {
        std::string c = util::ReadFile(path);
        if (!c.empty()) return c;
    }
    net::HttpResult r = net::Get(url, net::RequestOpts(), url.find("api.github.com") != std::string::npos ? "Accept: application/vnd.github+json\r\n" : "");
    if (r.transportOk && r.status == 200 && r.complete && !r.body.empty() && (mustContain.empty() || util::Contains(r.body, mustContain))) {
        util::MkDirs(PeerCacheDir());
        util::WriteFileAtomic(path, r.body);
        return r.body;
    }
    if (!r.transportOk) { g_githubFailed = true; LogError("HTTP-запрос не удался: " + url + " - " + r.err); }
    else if (r.status != 200) LogError("HTTP " + std::to_string(r.status) + " от " + url);
    if (age >= 0) {
        std::string c = util::ReadFile(path);
        if (!c.empty()) { LogInfo("GitHub недоступен - использую сохранённую базу пиров (" + name + ", возраст " + std::to_string(age / 3600) + " ч)"); return c; }
    }
    return "";
}
struct CountryFile { std::string slug, path; };
std::vector<CountryFile> ListCountryFiles() {
    std::vector<CountryFile> out;
    json::Value v;
    std::string body = FetchCached("tree.json", PEERS_TREE_URL, "\"tree\"");
    if (body.empty() || !json::Parse(body, v)) return out;
    const json::Value& tree = v["tree"];
    for (size_t i = 0; i < tree.size(); ++i) {
        std::string p = tree[i]["path"].str();
        if (tree[i]["type"].str() != "blob" || !util::EndsWith(p, ".md")) continue;
        auto parts = util::Split(p, '/');
        if (parts.size() != 2) continue;
        std::string slug = util::Lower(parts[1].substr(0, parts[1].size() - 3));
        if (slug == "readme") continue;
        out.push_back({ slug, p });
    }
    return out;
}
struct PeerCandidate { std::string uri, city; };
std::vector<PeerCandidate> ParseCountryMarkdown(const std::string& text) {
    std::vector<PeerCandidate> out;
    std::string city = "Unknown";
    for (const auto& line : util::Lines(text)) {
        std::string t = util::Trim(line);
        if (!t.empty() && t[0] == '#') {
            size_t k = t.find_first_not_of('#');
            if (k != std::string::npos) city = util::Trim(t.substr(k));
            continue;
        }
        size_t pos = 0;
        while ((pos = line.find('`', pos)) != std::string::npos) {
            size_t e = line.find('`', pos + 1);
            if (e == std::string::npos) break;
            std::string uri = util::Trim(line.substr(pos + 1, e - pos - 1));
            std::string proto = util::Lower(uri.substr(0, uri.find("://")));
            if (uri.find("://") != std::string::npos && (proto == "tcp" || proto == "tls" || proto == "quic" || proto == "ws" || proto == "wss"))
                out.push_back({ uri, city });
            pos = e + 1;
        }
    }
    return out;
}
std::vector<PeerCandidate> FetchCountryPeers(const std::string& slug) {
    auto files = ListCountryFiles();
    std::string path;
    for (const auto& f : files) if (f.slug == slug) { path = f.path; break; }
    if (path.empty()) {
        LogError(files.empty() ? "Нет списка стран: GitHub недоступен, а сохранённой базы пиров ещё нет"
                               : "Страна не найдена в репозитории: " + slug);
        return {};
    }
    return ParseCountryMarkdown(FetchCached(slug + ".md", std::string(PEERS_RAW_BASE) + path, "://"));
}
struct CountryEntry { std::string slug, ru; bool supported = false; };
std::vector<CountryEntry> GetSortedCountryList() {
    std::vector<CountryEntry> out;
    for (const auto& f : ListCountryFiles()) {
        if (f.slug == "i2p" || f.slug == "lokinet" || f.slug == "tor") continue;   // не страны, а оверлей-сети
        out.push_back({ f.slug, SlugToRu(f.slug), !SlugToIso(f.slug).empty() });
    }
    std::sort(out.begin(), out.end(), [](const CountryEntry& a, const CountryEntry& b) { return a.ru < b.ru; });
    return out;
}
// GeoIP: страна выхода трафика - только напрямую, без прокси
std::string DetectEgressIso() {
    struct P { const char* url; const char* field; };
    for (const P& p : { P{ "http://ip-api.com/json/?fields=countryCode", "countryCode" }, P{ "https://ipinfo.io/json", "country" },
                        P{ "https://ipapi.co/json/", "country" } }) {
        net::RequestOpts o;
        o.allowProxies = false;
        o.timeoutMs = 8000;
        net::HttpResult r = net::Get(p.url, o);
        json::Value v;
        if (!r.transportOk || r.status != 200 || !json::Parse(r.body, v)) continue;
        std::string iso = util::Upper(v[p.field].str());
        if (iso.size() == 2) { LogDebug(std::string("GeoIP (") + p.url + ") определил страну: " + iso); return iso; }
    }
    LogDebug("GeoIP: ни один провайдер не ответил");
    return "";
}
// Страна основных пиров - перебором файлов стран (дорого, поэтому последней)
std::string DeriveRegionFromMainPeers(const std::vector<std::string>& mainPeers) {
    auto files = ListCountryFiles();
    for (const auto& mp : mainPeers) {
        std::string host = util::HostFromUri(mp);
        if (host.empty()) continue;
        for (const auto& f : files)
            for (const auto& p : FetchCountryPeers(f.slug))
                if (util::HostFromUri(p.uri) == host) return f.slug;
    }
    return "";
}

// ------------------------------------------------------------------ состояние (state.txt, ключ=значение)
struct Device { std::string name, host; };
struct PeerTrack { bool up = false; double uptime = -1; long long at = 0; std::vector<long long> flaps; };

struct WatchdogState {
    std::vector<std::string> backupPeers;
    std::string backupRegion;
    std::string regionalCountry, regionalCity;   // регион поездки - настройка поиска
    long long internetDownSince = 0;
    std::string homeRegion, homeCity;
    std::string egressRegion;
    long long egressRegionCheckedAt = 0;
    std::vector<std::string> crisisCountrySlugs;
    size_t crisisCountryIndex = 0, crisisPeerIndex = 0;
    long long crisisLastFullScanCompletedAt = 0;
    std::vector<Device> devices;
    std::map<std::string, long long> deviceDownSince, deviceEscalatedUntil;
    std::map<std::string, std::vector<std::string>> devicePeers, deviceTempPeers;
    std::map<std::string, std::string> deviceWhy;
    bool batteryCriticalMode = false;
    int batteryCriticalEnteredPercent = -1;
    long long updateLastCheckedAt = 0;
    std::string updateNotifiedTag, yggUpdateNotifiedTag;
    long long lastTickAt = 0;
    bool outageNotified = false, connLost = false;
    std::string lastPhase;   // ok / backup / down / crisis / no_internet / battery / service_down / error
    long long topupRetryAt = 0;
    std::map<std::string, long long> failedCandidates;
    std::map<std::string, PeerTrack> peerTracks;
    bool serviceNotified = false, configNotified = false;
    std::string configError, configRestoreFrom, tunProblem;
    long long firewallCheckedAt = 0;
    bool firewallNotified = false;
    bool vpnServerDownNotified = false;
    std::string diagVerdict, diagAdvice;
    bool diagAnyUp = false;
    std::vector<std::string> diagPeers;   // uri<TAB>kind<TAB>text<TAB>error
};

const int FLAP_THRESHOLD = 3;
const long long FLAP_WINDOW_SECONDS = 30 * 60;
const long long FAILED_CANDIDATE_TTL_SECONDS = 3600;

WatchdogState ParseState(const std::string& text) {
    WatchdogState s;
    long long now = util::Now();
    std::vector<std::string> oldRegional;
    for (const auto& line : util::Lines(text)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto num = [&](long long d = 0) { return util::ToLL(v, d); };
        if (k == "regional_peers") oldRegional = util::Split(v, ';');
        else if (k == "backup_peers") s.backupPeers = util::Split(v, ';');
        else if (k == "backup_region") s.backupRegion = v;
        else if (k == "regional_country") s.regionalCountry = v;
        else if (k == "regional_city") s.regionalCity = v;
        else if (k == "internet_down_since") s.internetDownSince = num();
        else if (k == "home_region") s.homeRegion = v;
        else if (k == "home_city") s.homeCity = v;
        else if (k == "egress_region") s.egressRegion = v;
        else if (k == "egress_region_checked_at") s.egressRegionCheckedAt = num();
        else if (k == "crisis_country_slugs") s.crisisCountrySlugs = util::Split(v, ';');
        else if (k == "crisis_country_index") s.crisisCountryIndex = (size_t)num();
        else if (k == "crisis_peer_index") s.crisisPeerIndex = (size_t)num();
        else if (k == "crisis_last_full_scan_completed_at") s.crisisLastFullScanCompletedAt = num();
        else if (k == "devices") {
            for (const auto& e : util::Split(v, ';')) {
                size_t c = e.find(',');
                if (c != std::string::npos) s.devices.push_back({ e.substr(0, c), e.substr(c + 1) });
            }
        } else if (k == "device_down_since" || k == "device_escalated_until") {
            auto& m = k == "device_down_since" ? s.deviceDownSince : s.deviceEscalatedUntil;
            for (const auto& e : util::Split(v, ';')) {
                size_t c = e.find_last_of(':');
                if (c != std::string::npos) m[e.substr(0, c)] = util::ToLL(e.substr(c + 1));
            }
        } else if (k == "device_peers" || k == "device_temp_peers" || k == "device_why") {
            size_t t = v.find('\t');
            if (t == std::string::npos || t == 0) continue;
            std::string name = v.substr(0, t), rest = v.substr(t + 1);
            if (k == "device_why") s.deviceWhy[name] = rest;
            else (k == "device_peers" ? s.devicePeers : s.deviceTempPeers)[name] = util::Split(rest, ' ');
        }
        else if (k == "battery_critical_mode") s.batteryCriticalMode = v == "1";
        else if (k == "battery_critical_entered_percent") s.batteryCriticalEnteredPercent = (int)num(-1);
        else if (k == "update_last_checked_at") s.updateLastCheckedAt = num();
        else if (k == "update_notified_tag") s.updateNotifiedTag = v;
        else if (k == "ygg_update_notified_tag") s.yggUpdateNotifiedTag = v;
        else if (k == "last_tick_at") s.lastTickAt = num();
        else if (k == "last_phase") s.lastPhase = v;
        else if (k == "outage_notified") s.outageNotified = v == "1";
        else if (k == "conn_lost") s.connLost = v == "1";
        else if (k == "topup_retry_at") s.topupRetryAt = num();
        else if (k == "failed_candidate") {
            size_t bar = v.find('|');
            if (bar == std::string::npos) continue;
            long long t = util::ToLL(v.substr(0, bar));
            if (now - t < FAILED_CANDIDATE_TTL_SECONDS) s.failedCandidates[v.substr(bar + 1)] = t;
        } else if (k == "peer_track") {
            auto f = util::SplitKeep(v, '\t');
            if (f.size() < 4 || f[0].empty()) continue;
            PeerTrack tr;
            tr.up = f[1] == "1";
            tr.uptime = util::ToD(f[2], -1);
            tr.at = util::ToLL(f[3]);
            if (f.size() >= 5) for (const auto& x : util::Split(f[4], ',')) { long long t = util::ToLL(x); if (now - t < FLAP_WINDOW_SECONDS) tr.flaps.push_back(t); }
            s.peerTracks[f[0]] = tr;
        }
        else if (k == "service_notified") s.serviceNotified = v == "1";
        else if (k == "config_notified") s.configNotified = v == "1";
        else if (k == "config_error") s.configError = v;
        else if (k == "config_restore_from") s.configRestoreFrom = v;
        else if (k == "tun_problem") s.tunProblem = v;
        else if (k == "firewall_checked_at") s.firewallCheckedAt = num();
        else if (k == "firewall_notified") s.firewallNotified = v == "1";
        else if (k == "vpn_server_down_notified") s.vpnServerDownNotified = v == "1";
        else if (k == "diag_verdict") s.diagVerdict = v;
        else if (k == "diag_advice") s.diagAdvice = v;
        else if (k == "diag_any_up") s.diagAnyUp = v == "1";
        else if (k == "diag_peer") s.diagPeers.push_back(v);
    }
    for (const auto& p : oldRegional)
        if (std::find(s.backupPeers.begin(), s.backupPeers.end(), p) == s.backupPeers.end()) s.backupPeers.push_back(p);
    return s;
}

// Переход с Python-версии: state.json -> state.txt (один раз)
WatchdogState MigrateFromJson(const std::string& text) {
    WatchdogState s;
    json::Value v;
    if (!json::Parse(text, v)) return s;
    for (size_t i = 0; i < v["backup_peers"].size(); ++i) s.backupPeers.push_back(v["backup_peers"][i].str());
    for (size_t i = 0; i < v["regional_peers"].size(); ++i) {
        std::string p = v["regional_peers"][i].str();
        if (!p.empty() && std::find(s.backupPeers.begin(), s.backupPeers.end(), p) == s.backupPeers.end()) s.backupPeers.push_back(p);
    }
    s.backupRegion = v["backup_region"].str();
    if (v["regional_countries"].size() > 0) {
        std::string spec = v["regional_countries"][0].str();
        size_t sl = spec.find('/');
        s.regionalCountry = sl == std::string::npos ? spec : spec.substr(0, sl);
        if (sl != std::string::npos) s.regionalCity = spec.substr(sl + 1);
    }
    s.homeRegion = v["home_region"].str();
    s.homeCity = v["home_city"].str();
    s.egressRegion = v["egress_region"].str();
    s.internetDownSince = v["internet_down_since"].i64(0);
    for (const auto& kv : v["devices"].obj) s.devices.push_back({ kv.first, kv.second.str() });
    s.updateNotifiedTag = v["update_notified_tag"].str();
    return s;
}

std::string FormatState(const WatchdogState& s) {
    std::string t;
    auto put = [&](const char* k, const std::string& v) { t += k; t += "="; t += v; t += "\n"; };
    auto b = [](bool x) { return std::string(x ? "1" : "0"); };
    auto n = [](long long x) { return std::to_string(x); };
    put("backup_peers", Join(s.backupPeers, ";"));
    put("backup_region", s.backupRegion);
    put("regional_country", s.regionalCountry);
    put("regional_city", s.regionalCity);
    put("internet_down_since", n(s.internetDownSince));
    put("home_region", s.homeRegion);
    put("home_city", s.homeCity);
    put("egress_region", s.egressRegion);
    put("egress_region_checked_at", n(s.egressRegionCheckedAt));
    put("crisis_country_slugs", Join(s.crisisCountrySlugs, ";"));
    put("crisis_country_index", n((long long)s.crisisCountryIndex));
    put("crisis_peer_index", n((long long)s.crisisPeerIndex));
    put("crisis_last_full_scan_completed_at", n(s.crisisLastFullScanCompletedAt));
    std::vector<std::string> devs, down, esc;
    for (const auto& d : s.devices) devs.push_back(d.name + "," + d.host);
    for (const auto& [k, v] : s.deviceDownSince) down.push_back(k + ":" + n(v));
    for (const auto& [k, v] : s.deviceEscalatedUntil) esc.push_back(k + ":" + n(v));
    put("devices", Join(devs, ";"));
    put("device_down_since", Join(down, ";"));
    put("device_escalated_until", Join(esc, ";"));
    for (const auto& [k, v] : s.devicePeers) if (!v.empty()) put("device_peers", k + "\t" + Join(v, " "));
    for (const auto& [k, v] : s.deviceTempPeers) if (!v.empty()) put("device_temp_peers", k + "\t" + Join(v, " "));
    for (const auto& [k, v] : s.deviceWhy) if (!v.empty()) put("device_why", k + "\t" + util::OneLine(v));
    put("battery_critical_mode", b(s.batteryCriticalMode));
    put("battery_critical_entered_percent", n(s.batteryCriticalEnteredPercent));
    put("update_last_checked_at", n(s.updateLastCheckedAt));
    put("update_notified_tag", s.updateNotifiedTag);
    put("ygg_update_notified_tag", s.yggUpdateNotifiedTag);
    put("last_tick_at", n(s.lastTickAt));
    put("last_phase", s.lastPhase);
    put("outage_notified", b(s.outageNotified));
    put("conn_lost", b(s.connLost));
    put("topup_retry_at", n(s.topupRetryAt));
    for (const auto& [u, t2] : s.failedCandidates) put("failed_candidate", n(t2) + "|" + u);
    for (const auto& [u, tr] : s.peerTracks) {
        std::vector<std::string> fl;
        for (long long x : tr.flaps) fl.push_back(n(x));
        put("peer_track", u + "\t" + b(tr.up) + "\t" + util::Fixed(tr.uptime, 0) + "\t" + n(tr.at) + "\t" + Join(fl, ","));
    }
    put("service_notified", b(s.serviceNotified));
    put("config_notified", b(s.configNotified));
    put("config_error", util::OneLine(s.configError));
    put("config_restore_from", s.configRestoreFrom);
    put("tun_problem", util::OneLine(s.tunProblem));
    put("firewall_checked_at", n(s.firewallCheckedAt));
    put("firewall_notified", b(s.firewallNotified));
    put("vpn_server_down_notified", b(s.vpnServerDownNotified));
    put("diag_verdict", s.diagVerdict);
    put("diag_advice", s.diagAdvice);
    put("diag_any_up", b(s.diagAnyUp));
    for (const auto& d : s.diagPeers) put("diag_peer", d);
    return t;
}

WatchdogState LoadState() {
    std::string text = util::ReadFile(StatePath());
    if (text.empty() && !util::FileExists(StatePath())) {
        std::string old = util::ReadFile(ProgDir() + "state.json");
        if (!old.empty()) return MigrateFromJson(old);
    }
    return ParseState(text);
}
void SaveState(const WatchdogState& s) {
    if (!util::WriteFileAtomic(StatePath(), FormatState(s), 0600)) LogError("Не удалось записать state.txt");
    std::string old = ProgDir() + "state.json";
    if (util::FileExists(old)) rename(old.c_str(), (old + ".migrated").c_str());
}
// Сохранение из проверки: настройки пользователя (регионы, устройства) берутся
// с диска - за время проверки их могли поменять в окне, затирать нельзя.
void SaveTickState(WatchdogState& s) {
    WatchdogState disk = LoadState();
    s.regionalCountry = disk.regionalCountry; s.regionalCity = disk.regionalCity;
    s.homeRegion = disk.homeRegion; s.homeCity = disk.homeCity;
    s.devices = disk.devices; s.devicePeers = disk.devicePeers;
    for (auto it = s.deviceDownSince.begin(); it != s.deviceDownSince.end();) {
        bool still = false;
        for (const auto& d : s.devices) if (d.name == it->first) still = true;
        it = still ? std::next(it) : s.deviceDownSince.erase(it);
    }
    SaveState(s);
}

// ------------------------------------------------------------------ статистика надёжности (peer_stats.txt)
// Строка на пир и сутки: день<TAB>uri<TAB>проверок<TAB>рабочих<TAB>обрывов<TAB>рукопожатий<TAB>удачных.
// Хранится 35 суток; надёжность - доля удач с затуханием (полураспад 7 суток).
struct DayStat { long long checks = 0, up = 0, flaps = 0, tries = 0, triesOk = 0; };
using PeerStats = std::map<std::pair<long long, std::string>, DayStat>;
const long long STATS_KEEP_DAYS = 35;
const double STATS_HALF_LIFE_DAYS = 7.0;
const long long STATS_MIN_SAMPLES = 10;
long long DayNumber(long long t) { return t / 86400; }

PeerStats ParsePeerStats(const std::string& text) {
    PeerStats ps;
    for (const auto& line : util::Lines(text)) {
        auto f = util::SplitKeep(line, '\t');
        if (f.size() < 7 || f[1].empty()) continue;
        DayStat d;
        d.checks = util::ToLL(f[2]); d.up = util::ToLL(f[3]); d.flaps = util::ToLL(f[4]);
        d.tries = util::ToLL(f[5]); d.triesOk = util::ToLL(f[6]);
        ps[{ util::ToLL(f[0]), f[1] }] = d;
    }
    return ps;
}
std::string FormatPeerStats(const PeerStats& ps) {
    std::string t;
    for (const auto& [k, d] : ps)
        t += std::to_string(k.first) + "\t" + k.second + "\t" + std::to_string(d.checks) + "\t" + std::to_string(d.up) + "\t" +
             std::to_string(d.flaps) + "\t" + std::to_string(d.tries) + "\t" + std::to_string(d.triesOk) + "\n";
    return t;
}
struct PeerObservation { long long checks = 0, up = 0, flaps = 0, tries = 0, triesOk = 0; };
void RecordPeerStats(const std::map<std::string, PeerObservation>& obs) {
    if (obs.empty()) return;
    util::FileLock lock(PeerStatsPath() + ".lock", true);
    if (!lock.ok()) { LogDebug("peer_stats.txt занят - статистика этой проверки пропущена"); return; }
    PeerStats ps = ParsePeerStats(util::ReadFile(PeerStatsPath()));
    long long today = DayNumber(util::Now());
    for (auto it = ps.begin(); it != ps.end();) it = (today - it->first.first > STATS_KEEP_DAYS) ? ps.erase(it) : std::next(it);
    for (const auto& [uri, o] : obs) {
        DayStat& d = ps[{ today, uri }];
        d.checks += o.checks; d.up += o.up; d.flaps += o.flaps; d.tries += o.tries; d.triesOk += o.triesOk;
    }
    util::WriteFileAtomic(PeerStatsPath(), FormatPeerStats(ps));
}
struct Reliability { double score = -1; long long samples = 0, flaps = 0, tries = 0, triesOk = 0; };
std::map<std::string, Reliability> ComputeReliability(const PeerStats& ps, long long today) {
    std::map<std::string, std::pair<double, double>> acc;
    std::map<std::string, Reliability> out;
    for (const auto& [k, d] : ps) {
        double age = std::max(0.0, (double)(today - k.first));
        double w = std::pow(0.5, age / STATS_HALF_LIFE_DAYS);
        auto& a = acc[k.second];
        a.first += w * (double)(d.up + d.triesOk);
        a.second += w * (double)(d.checks + d.tries);
        Reliability& r = out[k.second];
        r.samples += d.checks + d.tries; r.flaps += d.flaps; r.tries += d.tries; r.triesOk += d.triesOk;
    }
    for (auto& [uri, r] : out) {
        const auto& a = acc[uri];
        if (r.samples >= STATS_MIN_SAMPLES && a.second > 0) r.score = a.first / a.second;
    }
    return out;
}
std::map<std::string, Reliability> LoadReliability() {
    return ComputeReliability(ParsePeerStats(util::ReadFile(PeerStatsPath())), DayNumber(util::Now()));
}
// Проверенно хорошие (>= 90%) - вперёд, плохие (< 50%) - в конец; порядок внутри групп сохраняется
template <class T>
void OrderByReliability(std::vector<T>& items, const std::map<std::string, Reliability>& rel,
                        const std::function<std::string(const T&)>& uriOf, int* good = nullptr, int* bad = nullptr) {
    auto group = [&](const T& x) {
        auto it = rel.find(uriOf(x));
        if (it == rel.end() || it->second.score < 0) return 1;
        return it->second.score >= 0.9 ? 0 : it->second.score < 0.5 ? 2 : 1;
    };
    std::stable_sort(items.begin(), items.end(), [&](const T& a, const T& b) { return group(a) < group(b); });
    if (good) { *good = 0; for (const auto& x : items) if (group(x) == 0) ++*good; }
    if (bad) { *bad = 0; for (const auto& x : items) if (group(x) == 2) ++*bad; }
}

// ------------------------------------------------------------------ yggdrasilctl: пиры и рукопожатия
using ygg::PeerDetail;
bool g_lastPeersOk = false;
std::vector<std::pair<std::string, bool>> g_handshakeResults;   // итоги проб за запуск -> статистика
std::map<std::string, PeerObservation> g_observations;

std::vector<PeerDetail> GetPeerDetails() {
    std::string err;
    auto v = ygg::GetPeers(&g_lastPeersOk, &err);
    if (!g_lastPeersOk) LogDebug("getPeers не ответил: " + err);
    return v;
}
void CtlQuiet(const std::vector<std::string>& args) { json::Value v; std::string e; ygg::Ctl(args, v, e, 20000); }
void AddPeer(const std::string& uri) { LogDebug("addPeer " + uri); CtlQuiet({ "addPeer", "uri=" + uri }); }
void RemovePeer(const std::string& uri) { LogDebug("removePeer " + uri); CtlQuiet({ "removePeer", "uri=" + uri }); }

const int HANDSHAKE_TIMEOUT_MS = 15000;
const int HANDSHAKE_POLL_MS = 1500;
// Настоящая проверка: подключаем пир к Yggdrasil и ждём Up
bool VerifyPeerHandshake(const std::string& uri) {
    AddPeer(uri);
    auto start = std::chrono::steady_clock::now();
    bool ok = false;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < HANDSHAKE_TIMEOUT_MS) {
        auto d = GetPeerDetails();
        const PeerDetail* p = ygg::FindPeer(d, uri);
        if (p && p->up) { ok = true; break; }
        if (p && !p->lastError.empty()) break;   // Yggdrasil уже сказал, что не поднимется
        util::SleepMs(HANDSHAKE_POLL_MS);
    }
    RemovePeer(uri);
    g_handshakeResults.push_back({ uri, ok });
    return ok;
}
void FlushPeerStats() {
    for (const auto& [uri, ok] : g_handshakeResults) { auto& o = g_observations[uri]; o.tries++; if (ok) o.triesOk++; }
    g_handshakeResults.clear();
    RecordPeerStats(g_observations);
    g_observations.clear();
}

// «Up», но постоянно переподключается: uptime между проверками должен расти на
// прошедшее время; меньше - соединение рвалось. 3 раза за полчаса = нестабилен.
std::set<std::string> UpdatePeerTracks(WatchdogState& st, const std::vector<PeerDetail>& details, long long now) {
    std::set<std::string> newFlaps;
    std::map<std::string, PeerTrack> next;
    for (const auto& d : details) {
        PeerTrack t;
        auto it = st.peerTracks.find(d.uri);
        if (it != st.peerTracks.end()) {
            const PeerTrack& prev = it->second;
            t.flaps = prev.flaps;
            long long gap = now - prev.at;
            if (prev.at > 0 && gap > 0 && gap < 3600 && prev.up) {
                if (!d.up) { t.flaps.push_back(now); newFlaps.insert(d.uri); }
                else if (d.uptimeSec >= 0 && prev.uptime >= 0 && d.uptimeSec + 30 < prev.uptime + (double)gap) { t.flaps.push_back(now); newFlaps.insert(d.uri); }
            }
        }
        t.flaps.erase(std::remove_if(t.flaps.begin(), t.flaps.end(), [&](long long x) { return now - x >= FLAP_WINDOW_SECONDS; }), t.flaps.end());
        t.up = d.up; t.uptime = d.uptimeSec; t.at = now;
        next[d.uri] = t;
    }
    st.peerTracks = next;
    return newFlaps;
}
void MarkUnstable(std::vector<PeerDetail>& details, const WatchdogState& st) {
    for (auto& d : details) {
        auto it = st.peerTracks.find(d.uri);
        d.flaps = it == st.peerTracks.end() ? 0 : (int)it->second.flaps.size();
        d.unstable = d.flaps >= FLAP_THRESHOLD;
    }
}

// ------------------------------------------------------------------ история (history.log)
// ts<TAB>фаза<TAB>интернет(1/0)<TAB>uri=U|мс;uri=D|-1;...  Хранится 8 суток.
const long long HISTORY_RETENTION_SECONDS = 8 * 86400;
struct HistSample { long long ts = 0; bool up = false; double ms = -1; };

void ParseHistoryLine(const std::string& line, std::map<std::string, std::vector<HistSample>>& out) {
    auto f = util::SplitKeep(line, '\t');
    if (f.size() < 4) return;
    long long ts = util::ToLL(f[0], 0);
    if (!ts) return;
    for (const auto& item : util::Split(f[3], ';')) {
        size_t a = item.rfind("=U|"), b = item.rfind("=D|");
        size_t m = a == std::string::npos ? b : (b == std::string::npos ? a : std::max(a, b));
        if (m == std::string::npos || m == 0) continue;
        HistSample s;
        s.ts = ts;
        s.up = item[m + 1] == 'U';
        s.ms = util::ToD(item.substr(m + 3), -1);
        out[item.substr(0, m)].push_back(s);
    }
}
std::map<std::string, std::vector<HistSample>> LoadHistory() {
    std::map<std::string, std::vector<HistSample>> h;
    for (const auto& l : util::Lines(util::ReadFile(HistoryPath()))) ParseHistoryLine(l, h);
    return h;
}
struct LatencyAgg { long long n = 0, up = 0, rttN = 0; double avg = -1, p95 = -1, jitter = -1, min = -1, max = -1; };
LatencyAgg Aggregate(const std::vector<HistSample>& v, long long from) {
    LatencyAgg a;
    std::vector<double> r;
    for (const auto& s : v) {
        if (s.ts < from) continue;
        a.n++;
        if (s.up) a.up++;
        if (s.up && s.ms >= 0) r.push_back(s.ms);
    }
    a.rttN = (long long)r.size();
    if (r.empty()) return a;
    double sum = 0;
    for (double x : r) sum += x;
    a.avg = sum / (double)r.size();
    double var = 0;
    for (double x : r) var += (x - a.avg) * (x - a.avg);
    a.jitter = std::sqrt(var / (double)r.size());
    std::sort(r.begin(), r.end());
    a.min = r.front(); a.max = r.back();
    size_t rank = (size_t)std::ceil(0.95 * (double)r.size());
    a.p95 = r[std::max<size_t>(1, rank) - 1];
    return a;
}
void PruneHistoryIfLarge() {
    std::string head = util::ReadFile(HistoryPath()).substr(0, 32);
    long long first = util::ToLL(head.substr(0, head.find('\t')), 0);
    if (!first) return;
    long long cutoff = util::Now() - HISTORY_RETENTION_SECONDS;
    if (first >= cutoff - 86400) return;
    std::string kept;
    for (const auto& line : util::Lines(util::ReadFile(HistoryPath()))) {
        if (line.empty()) continue;
        if (util::ToLL(line.substr(0, line.find('\t')), 0) >= cutoff) kept += line + "\n";
    }
    util::WriteFileAtomic(HistoryPath(), kept);
}
void RecordHistory(const std::string& phase) {
    WatchdogState st = LoadState();
    bool internet = st.internetDownSince == 0 && phase != "no_internet";
    std::string peers;
    if (phase != "battery")
        for (const auto& d : GetPeerDetails()) {
            if (!peers.empty()) peers += ";";
            peers += d.uri + "=" + (d.up ? "U|" : "D|") + (d.rttMs < 0 ? std::string("-1") : util::Fixed(d.rttMs, 1));
        }
    util::AppendLine(HistoryPath(), std::to_string(util::Now()) + "\t" + phase + "\t" + (internet ? "1" : "0") + "\t" + peers);
    PruneHistoryIfLarge();
}

// ------------------------------------------------------------------ батарея (ноутбуки)
// <= 5% и не на зарядке: все остальные проверки выключаются до восстановления
const int BATTERY_CRITICAL_PERCENT = 5;
struct BatteryStatus { int percent = -1; bool charging = false; };
std::optional<BatteryStatus> ReadBattery() {
    std::optional<BatteryStatus> res;
    bool ac = false;
    for (const auto& n : util::ListDir("/sys/class/power_supply")) {
        std::string d = "/sys/class/power_supply/" + n + "/";
        std::string type = util::Trim(util::ReadFile(d + "type"));
        if (type == "Mains" && util::Trim(util::ReadFile(d + "online")) == "1") ac = true;
        if (type != "Battery" || res) continue;
        std::string cap = util::Trim(util::ReadFile(d + "capacity"));
        if (cap.empty()) continue;
        BatteryStatus b;
        b.percent = (int)util::ToLL(cap, -1);
        std::string st = util::Lower(util::Trim(util::ReadFile(d + "status")));
        b.charging = st == "charging" || st == "full";
        if (b.percent >= 0) res = b;
    }
    if (res && ac) res->charging = true;
    return res;
}
bool CheckBattery(WatchdogState& st) {
    auto b = ReadBattery();
    if (!b) return true;
    if (st.batteryCriticalMode) {
        bool exited = b->charging || (st.batteryCriticalEnteredPercent >= 0 && b->percent > st.batteryCriticalEnteredPercent);
        if (!exited) return false;
        st.batteryCriticalMode = false;
        st.batteryCriticalEnteredPercent = -1;
        std::string m = "Заряд " + std::to_string(b->percent) + "% (зарядка: " + (b->charging ? "да" : "нет") +
                        ") - аварийный режим по батарее выключен, проверки возобновлены.";
        LogInfo(m); Notify("ok", m);
        return true;
    }
    if (b->percent <= BATTERY_CRITICAL_PERCENT && !b->charging) {
        st.batteryCriticalMode = true;
        st.batteryCriticalEnteredPercent = b->percent;
        std::string m = "Заряд " + std::to_string(b->percent) + "% (не на зарядке) - вхожу в аварийный режим по батарее, все проверки выключены до восстановления заряда.";
        LogInfo(m); Notify("alert", m);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ интернет
// Сырые IP отвечают, а DNS - нет: часто завис отрицательный кэш systemd-resolved
// после сбоя сети - перезапуск Yggdrasil его не сбрасывает. Пробуем сбросить сами.
bool g_dnsFlushedThisRun = false;
bool HasInternetFull() {
    if (getenv("YGGWD_TEST_INTERNET")) return true;   // только для проверки в песочнице без выхода в сеть
    if (!net::HasInternet()) return false;
    if (net::DnsWorks()) return true;
    if (!g_dnsFlushedThisRun) {
        g_dnsFlushedThisRun = true;
        util::ProcResult r = util::Run({ "resolvectl", "flush-caches" }, 5000);
        if (r.started && r.code == 0) {
            LogInfo("Сырые IP отвечают, а имена не находятся - сбросил кэш DNS (resolvectl flush-caches)");
            if (net::DnsWorks()) return true;
        }
    }
    return false;
}
bool CheckInternet(WatchdogState& st) {
    bool ok = HasInternetFull();
    if (!ok) {
        if (st.internetDownSince == 0) {
            st.internetDownSince = util::Now();
            AppendEvent("alert", "Пропал интернет целиком - замену пирам не ищу, жду восстановления.");
            LogInfo("Похоже, пропал интернет целиком (не только пиры Yggdrasil) - жду восстановления.");
        }
        return false;
    }
    if (st.internetDownSince != 0) {
        long long down = st.internetDownSince, up = util::Now(), d = up - down;
        st.internetDownSince = 0;
        std::string m = "Интернет пропадал с " + util::TimeText(down, "%H:%M:%S") + " до " + util::TimeText(up, "%H:%M:%S") +
                        " (простой " + std::to_string(d / 60) + " мин " + std::to_string(d % 60) + " сек).";
        LogInfo(m); Notify("ok", m);
    }
    return true;
}

// ------------------------------------------------------------------ фаервол (только чтение)
std::optional<std::string> CheckFirewallDiagnostics() {
    util::ProcResult r = util::Run({ "ufw", "status", "verbose" }, 5000);
    if (r.started && r.code == 0 && util::IContains(r.out, "status: active")) {
        std::string low = util::Lower(r.out);
        if (util::Contains(low, "deny (outgoing)") || util::Contains(low, "reject (outgoing)"))
            return std::string("ufw активен, исходящие по умолчанию запрещены - проверь, есть ли правило-исключение (sudo ufw status verbose).");
    }
    for (const char* tool : { "iptables", "ip6tables" }) {
        r = util::Run({ tool, "-L", "OUTPUT", "-n" }, 5000);
        if (r.started && r.code == 0) {
            auto lines = util::Lines(r.out);
            std::string first = lines.empty() ? "" : util::Lower(lines[0]);
            if (util::Contains(first, "policy drop") || util::Contains(first, "policy reject"))
                return std::string(tool) + ": цепочка OUTPUT по умолчанию DROP/REJECT - возможен запрет исходящих соединений (sudo " + tool + " -L OUTPUT -n -v).";
        }
    }
    r = util::Run({ "nft", "list", "ruleset" }, 5000);
    if (r.started && r.code == 0) {
        std::string low = util::Lower(r.out);
        size_t k = 0;
        while ((k = low.find("hook output", k)) != std::string::npos) {
            size_t e = low.find('}', k);
            std::string blk = low.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (util::Contains(blk, "policy drop"))
                return std::string("nftables: цепочка output с policy drop - возможен запрет исходящих соединений (sudo nft list ruleset).");
            k += 11;
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ локальный VPN (xray / v2ray / sing-box)
// GitHub не открывается, хотя интернет есть, а на машине работает локальный
// прокси - проверяем, отвечает ли через него сервер VPN. Сам VPN сторож не трогает.
int LocalSocksPort() {
    bool active = false;
    for (const char* s : { "xray", "v2ray", "sing-box" }) {
        util::ProcResult r = util::Run({ "systemctl", "is-active", "--quiet", s }, 5000);
        if (r.started && r.code == 0) { active = true; break; }
    }
    if (!active) return 0;
    for (const char* path : { "/usr/local/etc/xray/config.json", "/etc/xray/config.json", "/usr/local/etc/v2ray/config.json",
                              "/etc/v2ray/config.json", "/etc/sing-box/config.json" }) {
        json::Value cfg;
        if (!json::Parse(util::ReadFile(path), cfg)) continue;
        for (const char* key : { "inbounds" }) {
            const json::Value& in = cfg[key];
            for (size_t i = 0; i < in.size(); ++i) {
                std::string proto = in[i]["protocol"].str(in[i]["type"].str());
                long long port = in[i]["port"].i64(in[i]["listen_port"].i64(0));
                if ((proto == "socks" || proto == "mixed") && port > 0) return (int)port;
            }
        }
    }
    return 10808;
}
bool ProxyServerAnswers(int port) {
    for (int i = 0; i < 3; ++i) {
        int fd = net::ConnectTcp("127.0.0.1", port, 2000);
        if (fd < 0) return false;
        net::SetTimeouts(fd, 8000);
        bool ok = net::Socks5Tunnel(fd, "www.google.com", 443);
        close(fd);
        if (ok) return true;
        util::SleepMs(1000);
    }
    return false;
}
void CheckLocalVpn(WatchdogState& st) {
    int port = LocalSocksPort();
    if (!port) return;
    if (ProxyServerAnswers(port)) { st.vpnServerDownNotified = false; LogInfo("GitHub недоступен, но сервер VPN через локальный прокси (порт " + std::to_string(port) + ") отвечает"); return; }
    if (st.vpnServerDownNotified) return;
    st.vpnServerDownNotified = true;
    Notify("alert", "VPN-сервер не отвечает через локальный прокси (socks " + std::to_string(port) + "). Скорее всего сервер сменился или лёг: "
                    "обнови подписку или ссылку сервера. Сам VPN сторож не трогает.");
}

// ------------------------------------------------------------------ служба Yggdrasil
// Остановлена, а автозапуск включён - значит, должна работать: запускаем.
// Автозапуск выключен - похоже, остановили намеренно: только сообщаем.
bool EnsureYggdrasilService(WatchdogState& st) {
    ygg::ServiceInfo si = ygg::QueryService();
    if (!si.exists) return true;   // службы с таким именем нет (запущен иначе) - решит yggdrasilctl
    for (int i = 0; i < 15 && si.active == "activating"; ++i) { util::SleepMs(1000); si = ygg::QueryService(); }
    if (si.running()) { st.serviceNotified = false; return true; }
    if (!si.autoStart()) {
        LogInfo("Служба Yggdrasil остановлена, автозапуск выключен - не трогаю, проверка пиров пропущена.");
        if (!st.serviceNotified) {
            st.serviceNotified = true;
            Notify("warn", "Служба Yggdrasil остановлена, и автозапуск у неё выключен - похоже, её остановили намеренно. "
                           "Сторож её не запускает; пиры не проверяются, пока служба стоит.");
        }
        return false;
    }
    LogInfo("Служба Yggdrasil остановлена (автозапуск включён) - запускаю");
    std::string err = ygg::ServiceCtl("start");
    if (!err.empty()) {
        LogError("Служба Yggdrasil не запускается: " + err);
        if (!st.serviceNotified) {
            st.serviceNotified = true;
            Notify("alert", "Служба Yggdrasil остановлена и не запускается: " + err + ". Посмотри: journalctl -u yggdrasil -n 50");
        }
        return false;
    }
    st.serviceNotified = false;
    Notify("warn", "Служба Yggdrasil была остановлена - запустил её снова.");
    for (int i = 0; i < 10; ++i) {
        util::SleepMs(2000);
        bool any = false;
        for (const auto& d : GetPeerDetails()) if (d.up) any = true;
        if (any) break;
    }
    return true;
}
void RestartYggdrasilNow() {
    Print("Перезапускаю службу Yggdrasil немедленно...");
    Print("Внимание: если ты подключён к этому компьютеру ЧЕРЕЗ Yggdrasil - соединение сейчас оборвётся. "
          "Если это твой единственный канал - используй отложенный перезапуск.");
    std::string e = ygg::ServiceCtl("restart");
    if (e.empty()) LogInfo("Служба Yggdrasil перезапущена.");
    else LogError("Перезапуск службы Yggdrasil не удался: " + e);
}
void RestartYggdrasilDelayed(int sec) {
    std::string e = ygg::RestartDelayed(sec);
    if (e.empty()) LogInfo("Перезапуск службы запланирован через " + std::to_string(sec) + " с (в " +
                           util::TimeText(util::Now() + sec, "%H:%M:%S") + "). Успей отключиться, если сидишь по Yggdrasil.");
    else LogError("Не удалось запланировать перезапуск: " + e);
}

// ------------------------------------------------------------------ конфиг: рабочая копия и восстановление
struct ConfigInfo { bool ok = false, found = false; ygg::Paths paths; std::string err, ifName; std::vector<std::string> peers; };
ConfigInfo LoadConfigInfo() {
    ConfigInfo ci;
    ci.paths = ygg::CtlPaths();
    if (!util::FileExists(ci.paths.conf)) { ci.err = "не найден yggdrasil.conf (" + ci.paths.conf + ")"; return ci; }
    ci.found = true;
    json::Value cfg;
    if (!ygg::LoadConfig(ci.paths, cfg, ci.err)) return ci;
    ci.ok = true;
    for (size_t i = 0; i < cfg["Peers"].size(); ++i) if (cfg["Peers"][i].isString()) ci.peers.push_back(cfg["Peers"][i].str());
    ci.ifName = cfg["IfName"].str("auto");
    return ci;
}
std::vector<std::string> ReadMainPeers(bool quiet = false) {
    ConfigInfo ci = LoadConfigInfo();
    if (!ci.ok && !quiet) LogError("Не удалось прочитать конфиг: " + ci.err);
    std::vector<std::string> out;
    for (const auto& p : ci.peers) if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}
std::string GoodConfigPath(const ygg::Paths& p) { return p.conf + ".good"; }
// Проверенная копия: .good, иначе самая свежая из .bak-…
std::string FindConfigRestoreCandidate(const ygg::Paths& p) {
    std::string good = GoodConfigPath(p);
    if (ygg::ConfigFileValid(p, good)) return good;
    std::vector<std::pair<long long, std::string>> baks;
    std::string dir = util::DirOf(p.conf), prefix = util::BaseName(p.conf) + ".bak-";
    for (const auto& n : util::ListDir(dir))
        if (util::StartsWith(n, prefix)) baks.push_back({ util::FileAgeSeconds(dir + n), dir + n });
    std::sort(baks.begin(), baks.end());
    for (size_t i = 0; i < baks.size() && i < 10; ++i) if (ygg::ConfigFileValid(p, baks[i].second)) return baks[i].second;
    return "";
}
void CheckConfigHealth(WatchdogState& st, const ConfigInfo& ci) {
    if (ci.ok) {
        std::string cur = util::ReadFile(ci.paths.conf), good = GoodConfigPath(ci.paths);
        if (!cur.empty() && util::ReadFile(good) != cur && !util::WriteFileAtomic(good, cur, 0600))
            LogError("Не удалось обновить рабочую копию конфига " + good);
        st.configError.clear(); st.configRestoreFrom.clear();
        if (st.configNotified) { st.configNotified = false; Notify("ok", "yggdrasil.conf снова читается нормально."); }
        return;
    }
    LogError("Конфиг не читается: " + ci.err);
    st.configError = ci.err;
    st.configRestoreFrom = ci.found ? FindConfigRestoreCandidate(ci.paths) : "";
    if (!st.configNotified) {
        st.configNotified = true;
        std::string how = st.configRestoreFrom.empty() ? "Проверенной рабочей копии рядом нет - исправь файл вручную."
            : "Есть проверенная рабочая копия (" + util::BaseName(st.configRestoreFrom) + "): ygg-watchdog → «Управление» → [r], "
              "заменит только после подтверждения.";
        Notify("alert", "yggdrasil.conf не читается: " + ci.err + ". Служба работает со старыми настройками, пока её не перезапустить; "
                        "после перезапуска может не подняться. " + how);
    }
}
// restore-config: заменить битый конфиг проверенной копией (битый остаётся рядом)
void CmdRestoreConfig() {
    const ygg::Paths& p = ygg::CtlPaths();
    json::Value tmp;
    std::string err;
    if (ygg::LoadConfig(p, tmp, err)) { Print("Конфиг читается нормально - восстанавливать нечего."); return; }
    std::string from = FindConfigRestoreCandidate(p);
    if (from.empty()) { PrintError("рабочей копии конфига рядом нет (.good или .bak-…) - исправь файл вручную."); return; }
    std::string data = util::ReadFile(from);
    if (data.empty()) { PrintError("не удалось прочитать " + from); return; }
    std::string broken = p.conf + ".broken-" + util::TimeText(util::Now(), "%Y%m%d-%H%M%S");
    if (util::FileExists(p.conf) && !util::CopyFile(p.conf, broken)) { PrintError("не удалось сохранить текущий файл как " + broken + " - ничего не менял."); return; }
    if (!util::WriteFileAtomic(p.conf, data)) { PrintError("не удалось записать " + p.conf); return; }
    if (!ygg::LoadConfig(p, tmp, err)) { PrintError("после замены конфиг всё равно не читается: " + err); return; }
    LogInfo("Конфиг восстановлен из " + from + " (битый сохранён как " + broken + ")");
    AppendEvent("ok", "yggdrasil.conf восстановлен из рабочей копии " + util::BaseName(from));
    ygg::ServiceInfo si = ygg::QueryService();
    if (si.exists && !si.running()) {
        std::string e = ygg::ServiceCtl("start");
        Print(e.empty() ? "Служба Yggdrasil запущена." : "Служба Yggdrasil не запустилась: " + e);
    }
    WatchdogState st = LoadState();
    st.configError.clear(); st.configRestoreFrom.clear();
    SaveState(st);
}

// ------------------------------------------------------------------ сетевые интерфейсы (ip)
// Интерфейс, у которого есть адрес addr
std::string IfaceWithAddr(const std::string& addr) {
    util::ProcResult r = util::Run({ "ip", "-6", "-o", "addr", "show" }, 5000);
    for (const auto& l : util::Lines(r.out)) {
        std::istringstream ss(l);
        std::string idx, name, fam, a;
        ss >> idx >> name >> fam >> a;
        size_t sl = a.find('/');
        if (sl != std::string::npos) a = a.substr(0, sl);
        if (ygg::SameIp6(a, addr)) { size_t at = name.find('@'); return at == std::string::npos ? name : name.substr(0, at); }
    }
    return "";
}
bool IfaceUp(const std::string& name) {
    std::string f = util::Trim(util::ReadFile("/sys/class/net/" + name + "/flags"));
    return !f.empty() && (strtol(f.c_str(), nullptr, 16) & 1);
}
// Через какой интерфейс уйдёт пакет к адресу
std::string RouteDev(const std::string& target) {
    util::ProcResult r = util::Run({ "ip", "-6", "route", "get", target }, 5000);
    std::istringstream ss(r.out);
    std::string w, prev;
    while (ss >> w) { if (prev == "dev") return w; prev = w; }
    return "";
}
bool HasYggRoute(const std::string& ifname) {
    util::ProcResult r = util::Run({ "ip", "-6", "route", "show", "200::/7" }, 5000);
    return util::Contains(r.out, "dev " + ifname);
}
// Включённые интерфейсы, похожие на VPN-туннели (кроме адаптера Yggdrasil)
std::vector<std::string> VpnLikeIfaces(const std::string& yggIf) {
    std::vector<std::string> out;
    static const char* words[] = { "tun", "wg", "tap", "sing", "xray", "v2ray", "clash", "mihomo", "nekoray", "hiddify", "tailscale",
                                   "zt", "outline", "amnezia", "awg", "proton", "nord", "vpn", "ppp" };
    for (const auto& n : util::ListDir("/sys/class/net")) {
        if (n == yggIf || n == "lo" || !IfaceUp(n)) continue;
        std::string low = util::Lower(n);
        bool vpn = util::Contains(low, "vpn");
        for (const char* w : words) if (util::StartsWith(low, w)) vpn = true;
        if (vpn) out.push_back(n);
    }
    return out;
}

// Адаптер Yggdrasil (TUN): пиры могут быть живы, а трафик на 200::/7 всё равно не пойдёт
void CheckTunHealth(WatchdogState& st, const ConfigInfo& ci) {
    if (!ci.ok || util::Lower(ci.ifName) == "none") { st.tunProblem.clear(); return; }
    json::Value tun, self;
    std::string err, problem;
    if (!ygg::Ctl("getTUN", tun, err)) return;
    if (!tun["enabled"].boolean()) {
        problem = "Сетевой адаптер Yggdrasil (TUN) не создан, хотя в конфиге он включён (IfName: " + ci.ifName + "). Пиры могут работать, "
                  "но трафик на адреса Yggdrasil не идёт. Обычно помогает перезапуск службы; проверь, есть ли /dev/net/tun.";
    } else if (ygg::Ctl("getSelf", self, err)) {
        std::string addr = self["address"].str(), name = tun["name"].str();
        std::string ifn = addr.empty() ? "" : IfaceWithAddr(addr);
        if (!addr.empty() && ifn.empty())
            problem = "Ни у одного интерфейса нет адреса Yggdrasil " + addr + " - трафик на адреса Yggdrasil не пойдёт. Перезапусти службу Yggdrasil.";
        else if (!ifn.empty() && !IfaceUp(ifn))
            problem = "Интерфейс Yggdrasil «" + ifn + "» выключен - трафик Yggdrasil не идёт. Перезапусти службу (или ip link set " + ifn + " up).";
        else if (!ifn.empty() && !HasYggRoute(ifn))
            problem = "У интерфейса Yggdrasil нет маршрута 200::/7 - трафик на адреса Yggdrasil уйдёт не туда. Перезапусти службу Yggdrasil; "
                      "если повторится - проверь, не меняет ли маршруты VPN-клиент.";
        (void)name;
    }
    if (problem.empty()) {
        if (!st.tunProblem.empty()) { st.tunProblem.clear(); Notify("ok", "Сетевой адаптер Yggdrasil снова в порядке."); }
        return;
    }
    LogError(problem);
    if (st.tunProblem != problem) { st.tunProblem = problem; Notify("alert", problem); }
}

// ------------------------------------------------------------------ VPN-скрипт (одобренный вручную, привязан к SHA-256)
bool ApproveVpnCommand(const std::string& path) {
    if (!util::FileExists(path)) { PrintError("файл скрипта не найден: " + path); return false; }
    std::string h = util::Sha256OfFile(path);
    if (h.empty()) { LogError("Не удалось посчитать SHA-256 скрипта: " + path); return false; }
    if (!util::WriteFileAtomic(VpnApprovalPath(), "path=" + path + "\nsha256=" + h + "\n", 0600)) { LogError("Не удалось сохранить одобрение VPN-скрипта"); return false; }
    LogInfo("VPN-скрипт одобрен: " + path + " (SHA-256: " + h + ")");
    return true;
}
void ClearVpnApproval() { unlink(VpnApprovalPath().c_str()); LogInfo("Одобрение VPN-скрипта снято."); }
std::string ApprovedVpnPath() {
    std::string path, hash;
    for (const auto& l : util::Lines(util::ReadFile(VpnApprovalPath()))) {
        if (util::StartsWith(l, "path=")) path = l.substr(5);
        else if (util::StartsWith(l, "sha256=")) hash = l.substr(7);
    }
    return path.empty() || hash.empty() ? "" : path + "\t" + hash;
}
// Путь к скрипту - только если он не менялся с момента одобрения
std::optional<std::string> GetApprovedVpnCommand() {
    std::string ph = ApprovedVpnPath();
    if (ph.empty()) return std::nullopt;
    std::string path = ph.substr(0, ph.find('\t')), hash = ph.substr(ph.find('\t') + 1);
    if (util::Sha256OfFile(path) != hash) { LogError("VPN-скрипт изменился с момента одобрения (или недоступен) - отказываюсь запускать: " + path); return std::nullopt; }
    return path;
}
bool RunApprovedVpnCommand(const std::string& arg) {
    auto p = GetApprovedVpnCommand();
    if (!p) return false;
    LogInfo("Запускаю одобренный VPN-скрипт: " + *p + " " + arg);
    util::Run({ *p, arg }, 60000);
    return true;
}

// ------------------------------------------------------------------ устройства
// Устройство на связи: сначала порты SSH/SMB/RDP (Windows по умолчанию не
// отвечает на пинг), потом ICMP. Серия из 3 попыток - разовая заминка не обрыв.
bool PingOnce(const std::string& host, int timeoutMs) {
    for (int port : { 22, 445, 3389 }) {
        int fd = net::ConnectTcp(host, port, timeoutMs);
        if (fd >= 0) { close(fd); return true; }
    }
    if (host.find(':') != std::string::npos) return net::Ping6(host, std::max(1000, timeoutMs)).ok;
    util::ProcResult r = util::Run({ "ping", "-n", "-c", "1", "-W", "2", host }, 6000);
    return r.code == 0;
}
bool PingDeviceDebounced(const std::string& host) {
    for (int i = 0; i < 3; ++i) {
        if (PingOnce(host, 1000)) return true;
        if (i < 2) util::SleepMs(2000);
    }
    return false;
}
bool IsYggdrasilResponsive(double* ms = nullptr) {
    auto t0 = std::chrono::steady_clock::now();
    json::Value v;
    std::string e;
    bool ok = ygg::Ctl("getSelf", v, e, 10000);
    double el = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms) *ms = el;
    return ok && el < 5000;
}
bool YggKnowsPath(const std::string& target) {
    for (auto c : { std::pair<const char*, const char*>{ "getPaths", "paths" }, { "getSessions", "sessions" } }) {
        json::Value v;
        std::string e;
        if (!ygg::Ctl(c.first, v, e)) continue;
        for (size_t i = 0; i < v[c.second].size(); ++i) if (ygg::SameIp6(v[c.second][i]["address"].str(), target)) return true;
    }
    return false;
}
// Почему устройство недоступно - проверки по порядку, первая найденная причина
struct DeviceDiag { std::string kind, text, vpn; std::vector<std::pair<bool, std::string>> steps; };
DeviceDiag DiagnoseDevice(const std::string& host) {
    DeviceDiag dg;
    if (!ygg::IsYggAddress(host)) { dg.kind = "other"; dg.text = "не отвечает (подробный разбор - только для адресов Yggdrasil 200::/7)"; return dg; }
    double ms = 0;
    if (!IsYggdrasilResponsive(&ms)) {
        dg.steps.push_back({ false, "Yggdrasil на этом компьютере не отвечает" });
        dg.kind = "ygg_down"; dg.text = "Yggdrasil на этом компьютере не отвечает (служба зависла или перегружена)";
        return dg;
    }
    dg.steps.push_back({ true, "Yggdrasil отвечает быстро (" + util::FixedRu(ms / 1000.0, 2) + " с)" });
    json::Value self;
    std::string e;
    std::string selfAddr = ygg::Ctl("getSelf", self, e) ? self["address"].str() : "";
    std::string yggIf = selfAddr.empty() ? "" : IfaceWithAddr(selfAddr);
    auto vpns = VpnLikeIfaces(yggIf);
    dg.vpn = Join(vpns, ", ");
    std::string vpnNote = vpns.empty() ? "" : " Включены интерфейсы VPN-клиентов: " + dg.vpn + ".";
    if (!selfAddr.empty() && (yggIf.empty() || !IfaceUp(yggIf))) {
        dg.kind = "route";
        dg.text = yggIf.empty() ? "у интерфейса Yggdrasil на этом компьютере нет его адреса - трафик Yggdrasil не идёт"
                                : "интерфейс Yggdrasil «" + yggIf + "» на этом компьютере выключен";
        dg.steps.push_back({ false, dg.text });
        return dg;
    }
    std::string dev = RouteDev(host);
    if (dev.empty()) {
        dg.kind = "route"; dg.text = "у системы нет маршрута к адресу устройства (нет маршрута 200::/7 через интерфейс Yggdrasil)." + vpnNote;
        dg.steps.push_back({ false, "маршрута к устройству нет" });
        return dg;
    }
    if (!yggIf.empty() && dev != yggIf) {
        dg.kind = "route";
        dg.text = "пакеты к устройству уходят через «" + dev + "», а не через интерфейс Yggdrasil - маршрут перехвачен (обычно VPN-клиентом в TUN-режиме)." + vpnNote;
        dg.steps.push_back({ false, "маршрут перехвачен: через " + dev });
        return dg;
    }
    dg.steps.push_back({ true, "маршрут через интерфейс Yggdrasil" + (yggIf.empty() ? std::string() : " (" + yggIf + ")") });
    net::PingResult pr = net::Ping6(host, 2000);
    if (!pr.ok) pr = net::Ping6(host, 2000);
    if (pr.ok) { dg.kind = "ok"; dg.text = "отвечает на ping"; dg.steps.push_back({ true, "ping: " + util::Fixed(pr.ms, 0) + " мс" }); return dg; }
    if (pr.err == "local_block") {
        dg.kind = "local_block";
        dg.text = vpns.empty() ? "пакет к устройству блокируется прямо на этом компьютере (Operation not permitted) - исходящий IPv6 режет фаервол"
                               : "пакет к устройству блокируется прямо на этом компьютере. Похоже, VPN-клиент (" + dg.vpn + ") режет исходящий IPv6";
        dg.steps.push_back({ false, "ping: пакет не выпускает свой фаервол" });
        return dg;
    }
    dg.steps.push_back({ false, "ping: нет ответа" });
    if (!YggKnowsPath(host)) {
        dg.kind = "no_path";
        dg.text = "сеть Yggdrasil не находит путь до устройства: оно выключено, Yggdrasil на нём не работает, или ваши пиры не связаны между собой";
        dg.steps.push_back({ false, "пути до узла в сети нет" });
        return dg;
    }
    dg.steps.push_back({ true, "путь до узла в сети есть" });
    dg.kind = "no_answer";
    dg.text = "путь до устройства в сети есть, но оно не отвечает - причина на самом устройстве (спит, завис Yggdrasil или фаервол там)";
    return dg;
}

const long long DEVICE_ESCALATION_COOLDOWN_SECONDS = 3600;
std::vector<std::pair<std::string, std::string>> CheckDevices(WatchdogState& st) {
    std::vector<std::pair<std::string, std::string>> events;
    if (st.devices.empty()) return events;
    long long now = util::Now();
    // все устройства пингуются одновременно
    std::vector<std::string> hosts;
    for (const auto& d : st.devices) hosts.push_back(d.host);
    auto res = std::make_shared<std::vector<int>>(hosts.size(), -1);
    std::vector<std::thread> th;
    for (size_t i = 0; i < hosts.size(); ++i) th.emplace_back([res, i, h = hosts[i]] { (*res)[i] = PingDeviceDebounced(h) ? 1 : 0; });
    for (auto& t : th) t.join();

    auto live = GetPeerDetails();
    auto removeTemp = [&](const std::string& name) {
        auto it = st.deviceTempPeers.find(name);
        if (it == st.deviceTempPeers.end()) return std::string();
        std::vector<std::string> removed;
        for (const auto& u : it->second)
            if (std::find(st.backupPeers.begin(), st.backupPeers.end(), u) == st.backupPeers.end()) { RemovePeer(u); removed.push_back(u); }
        st.deviceTempPeers.erase(it);
        return Join(removed, ", ");
    };
    for (size_t di = 0; di < st.devices.size(); ++di) {
        const Device dev = st.devices[di];
        bool reachable = (*res)[di] == 1;
        auto downIt = st.deviceDownSince.find(dev.name);
        bool wasDown = downIt != st.deviceDownSince.end();
        std::string who = "Устройство '" + dev.name + "' (" + dev.host + ")";
        if (reachable) {
            if (wasDown) {
                long long dt = now - downIt->second;
                st.deviceDownSince.erase(dev.name);
                st.deviceEscalatedUntil.erase(dev.name);
                st.deviceWhy.erase(dev.name);
                std::string removed = removeTemp(dev.name);
                events.push_back({ "device_recovered", who + " снова на связи. Было недоступно " + std::to_string(dt / 60) + " мин " +
                                   std::to_string(dt % 60) + " сек." + (removed.empty() ? "" : " Временные общие пиры убраны: " + removed + ".") });
            }
            continue;
        }
        DeviceDiag dg = DiagnoseDevice(dev.host);
        if (dg.kind == "ok") continue;
        std::string prevWhy = st.deviceWhy[dev.name];
        st.deviceWhy[dev.name] = dg.text;
        if (!wasDown) {
            st.deviceDownSince[dev.name] = now;
            events.push_back({ "device_unreachable", who + " не отвечает: " + dg.text + "." });
            continue;   // меры - только если недоступность подтвердится на следующей проверке
        }
        if (dg.text != prevWhy) events.push_back({ "device_unreachable", who + " по-прежнему недоступно, причина другая: " + dg.text + "." });
        auto esc = st.deviceEscalatedUntil.find(dev.name);
        if (esc != st.deviceEscalatedUntil.end() && now < esc->second) continue;   // меры - не чаще раза в час
        st.deviceEscalatedUntil[dev.name] = now + DEVICE_ESCALATION_COOLDOWN_SECONDS;
        std::string action;
        bool tried = false;
        if (dg.kind == "ygg_down" || dg.kind == "route") {
            action = dg.kind == "ygg_down" ? "перезапускаю службу Yggdrasil" : "перезапускаю службу Yggdrasil - она заново создаст интерфейс и маршрут 200::/7";
            if (dg.kind == "route" && !dg.vpn.empty()) action += " (если маршрут снова перехватит VPN-клиент - не поможет)";
            ygg::ServiceCtl("restart");
            util::SleepMs(15000);
            tried = true;
        } else if (dg.kind == "local_block") {
            if (RunApprovedVpnCommand("restart")) { action = "запустил одобренный VPN-скрипт"; util::SleepMs(10000); tried = true; }
            else action = "в VPN-клиент и фаервол сторож не лезет - нужно поправить руками";
        } else if (dg.kind == "no_path") {
            std::vector<std::string> add;
            auto pit = st.devicePeers.find(dev.name);
            if (pit != st.devicePeers.end())
                for (const auto& u : pit->second) if (!u.empty() && !ygg::FindPeer(live, u)) add.push_back(u);
            if (!add.empty()) {
                for (const auto& u : add) AddPeer(u);
                auto& tmp = st.deviceTempPeers[dev.name];
                for (const auto& u : add) if (std::find(tmp.begin(), tmp.end(), u) == tmp.end()) tmp.push_back(u);
                action = "временно подключил общие пиры с устройством: " + Join(add, ", ");
                util::SleepMs(15000);
                tried = true;
            } else if (pit == st.devicePeers.end() || pit->second.empty()) {
                action = "общий пир для устройства не задан (экран «Устройства») - с ним маршрут нашёлся бы надёжнее";
            } else {
                action = "общие пиры с устройством уже подключены - похоже, оно само выключено или без связи";
            }
        } else {
            action = "с этого компьютера тут ничего не исправить";
        }
        LogInfo(who + ": " + dg.text + " - " + action);
        std::string result;
        if (tried) result = PingDeviceDebounced(dev.host) ? " Помогло: устройство отвечает." : " Пока не помогло.";
        events.push_back({ tried ? "device_action" : "device_unreachable", who + " недоступно: " + dg.text + ". Что делаю: " + action + "." + result });
    }
    return events;
}
void RunDeviceChecks(WatchdogState& st) {
    for (const auto& [kind, msg] : CheckDevices(st)) {
        LogInfo(msg);
        Notify(kind == "device_recovered" ? "ok" : kind == "device_action" ? "warn" : "alert", msg);
    }
}

// ------------------------------------------------------------------ крайний перебор
// Последняя надежда: все пиры всех стран, по времени за проверку, с курсором
// (следующая проверка продолжает с того же места); после полного прохода - 12 ч пауза.
const long long CRISIS_COOLDOWN_SECONDS = 12 * 3600;
const long long CRISIS_BUDGET_SECONDS = 90;
bool CrisisCooldownActive(const WatchdogState& st) {
    return st.crisisLastFullScanCompletedAt != 0 && util::Now() - st.crisisLastFullScanCompletedAt < CRISIS_COOLDOWN_SECONDS;
}
bool CrisisScanStep(WatchdogState& st, const std::set<std::string>& excludeHosts, long long budgetSec) {
    long long limit = std::min(budgetSec, CRISIS_BUDGET_SECONDS);
    if (CrisisCooldownActive(st)) { LogDebug("Крайний перебор: пауза после прошлого полного прохода ещё не закончилась"); return false; }
    if (st.crisisCountrySlugs.empty()) {
        auto files = ListCountryFiles();
        if (files.empty()) { LogError("Крайний перебор: не удалось получить список стран - повторю в следующий раз"); return false; }
        for (const auto& f : files) st.crisisCountrySlugs.push_back(f.slug);
        st.crisisCountryIndex = 0; st.crisisPeerIndex = 0;
        LogInfo("=== Крайний перебор: начинаю полный перебор пиров по всем странам (" + std::to_string(st.crisisCountrySlugs.size()) + ") ===");
        Notify("warn", "Крайний перебор: обычный поиск резервного пира не сработал. Начинаю полный перебор пиров по всем странам в фоне.");
    }
    auto start = std::chrono::steady_clock::now();
    while (st.crisisCountryIndex < st.crisisCountrySlugs.size()) {
        std::string slug = st.crisisCountrySlugs[st.crisisCountryIndex];
        auto peers = FetchCountryPeers(slug);
        while (st.crisisPeerIndex < peers.size()) {
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() >= limit) {
                LogInfo("Крайний перебор: время на эту проверку закончилось, продолжу со страны '" + slug + "', пир #" + std::to_string(st.crisisPeerIndex + 1));
                return false;
            }
            const auto& c = peers[st.crisisPeerIndex];
            if (!excludeHosts.count(util::HostFromUri(c.uri))) {
                LogInfo("Крайний перебор: проверяю " + c.uri + " (" + slug + "/" + c.city + ")");
                if (VerifyPeerHandshake(c.uri)) {
                    Notify("ok", "Крайний перебор: найден рабочий пир - " + c.uri + " (" + slug + "/" + c.city + ")");
                    AddPeer(c.uri);
                    st.backupPeers.push_back(c.uri);
                    st.backupRegion = slug;
                    st.crisisPeerIndex++;
                    return true;
                }
            }
            st.crisisPeerIndex++;
        }
        st.crisisCountryIndex++;
        st.crisisPeerIndex = 0;
    }
    LogError("Крайний перебор: все страны пройдены, рабочий пир не найден. Фаервол программа сама не трогает - только читает правила и сообщает.");
    Notify("alert", "Крайний перебор: полный перебор всех пиров завершён без результата.");
    st.crisisLastFullScanCompletedAt = util::Now();
    st.crisisCountrySlugs.clear();
    st.crisisCountryIndex = 0; st.crisisPeerIndex = 0;
    return false;
}

// ------------------------------------------------------------------ поиск резервных пиров
struct TickBudget {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    long long limitSec = 150;
    long long Elapsed() const { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count(); }
    long long Left() const { return limitSec - Elapsed(); }
};
const int MIN_ALIVE_PEERS = 3;
const int MAX_ALIVE_PEERS = 7;
const long long TOPUP_RETRY_SECONDS = 30 * 60;
const long long EGRESS_REFRESH_SECONDS = 3600;

void RefreshEgressRegion(WatchdogState& st) {
    long long now = util::Now();
    if (st.egressRegionCheckedAt != 0 && now - st.egressRegionCheckedAt < EGRESS_REFRESH_SECONDS) return;
    st.egressRegionCheckedAt = now;
    std::string iso = DetectEgressIso();
    if (iso.empty()) return;
    std::string slug = IsoToSlug(iso);
    if (slug.empty()) { LogDebug("GeoIP: страна " + iso + " не входит в поддерживаемые"); return; }
    if (st.egressRegion != slug) LogInfo("Регион выхода трафика: " + (st.egressRegion.empty() ? std::string("(не было)") : st.egressRegion) + " -> " + slug);
    st.egressRegion = slug;
}
void SortCandidatesByCity(std::vector<PeerCandidate>& c, const std::string& region, const std::string& cityName) {
    if (cityName.empty()) return;
    std::string iso = SlugToIso(region);
    if (iso.empty()) return;
    const auto& db = LoadCityDatabase();
    auto target = FindCity(db, iso, cityName);
    if (!target) return;
    std::vector<std::pair<double, PeerCandidate>> scored;
    for (const auto& p : c) {
        auto pc = FindCity(db, iso, p.city);
        scored.push_back({ pc ? HaversineKm(target->lat, target->lon, pc->lat, pc->lon) : 1e9, p });
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    c.clear();
    for (const auto& s : scored) c.push_back(s.second);
}
struct FoundPeer { std::string uri, city, region; };
std::vector<FoundPeer> SearchBackups(WatchdogState& st, const std::vector<std::string>& mainPeers, std::set<std::string>& usedHosts,
                                     int need, TickBudget& budget, bool& exhausted) {
    std::vector<FoundPeer> found;
    exhausted = true;
    std::vector<std::string> tried;
    long long now = util::Now();
    const auto rel = LoadReliability();
    auto searchRegion = [&](const std::string& region, const std::string& why) -> bool {
        if (region.empty()) return false;
        for (const auto& t : tried) if (util::IEquals(t, region)) return false;
        tried.push_back(region);
        if (budget.Left() < 25) { exhausted = false; return true; }
        LogInfo("Ищу пиры: " + SlugToRu(region) + " (" + why + ")");
        auto cands = FetchCountryPeers(region);
        std::string city = util::IEquals(region, st.regionalCountry) ? st.regionalCity : util::IEquals(region, st.homeRegion) ? st.homeCity : "";
        SortCandidatesByCity(cands, region, city);
        int good = 0, bad = 0;
        OrderByReliability<PeerCandidate>(cands, rel, [](const PeerCandidate& c) { return c.uri; }, &good, &bad);
        if (good || bad) LogInfo("  по прошлому опыту: " + std::to_string(good) + " надёжных вперёд, " + std::to_string(bad) + " ненадёжных в конец");
        std::vector<PeerCandidate> fresh;
        for (const auto& c : cands) {
            if (std::find(mainPeers.begin(), mainPeers.end(), c.uri) != mainPeers.end()) continue;
            if (std::find(st.backupPeers.begin(), st.backupPeers.end(), c.uri) != st.backupPeers.end()) continue;
            if (usedHosts.count(util::HostFromUri(c.uri))) continue;
            auto f = st.failedCandidates.find(c.uri);
            if (f != st.failedCandidates.end() && now - f->second < FAILED_CANDIDATE_TTL_SECONDS) continue;
            fresh.push_back(c);
        }
        if (fresh.empty()) { LogInfo("  новых кандидатов нет"); return false; }
        std::vector<std::string> uris;
        for (const auto& c : fresh) uris.push_back(c.uri);
        auto pr = net::ProbeMany(uris);
        std::vector<PeerCandidate> order;
        for (size_t i = 0; i < fresh.size(); ++i) if (pr[i].ms >= 0) order.push_back(fresh[i]);
        size_t open = order.size();
        for (size_t i = 0; i < fresh.size(); ++i) if (pr[i].err == "udp") order.push_back(fresh[i]);
        LogInfo("  порт отвечает у " + std::to_string(open) + " из " + std::to_string(fresh.size()));
        long long regionStart = budget.Elapsed();
        for (const auto& c : order) {
            if ((int)found.size() >= need) return true;
            if (budget.Left() < 20) { exhausted = false; return true; }
            if (budget.Elapsed() - regionStart > 60) { exhausted = false; return false; }   // шанс другим странам
            std::string host = util::HostFromUri(c.uri);
            if (usedHosts.count(host)) continue;
            LogInfo("  проверяю рукопожатие: " + c.uri + " (" + c.city + ")");
            if (VerifyPeerHandshake(c.uri)) {
                LogInfo("    Up - добавляю как резервный");
                AddPeer(c.uri);
                st.backupPeers.push_back(c.uri);
                st.backupRegion = region;
                usedHosts.insert(host);
                found.push_back({ c.uri, c.city, region });
            } else {
                LogInfo("    не поднялся");
                st.failedCandidates[c.uri] = util::Now();
            }
        }
        return (int)found.size() >= need;
    };
    RefreshEgressRegion(st);
    if (searchRegion(st.egressRegion, "выход трафика")) return found;
    if (searchRegion(st.regionalCountry, "регион поездки")) return found;
    if (searchRegion(st.homeRegion, "домашний регион")) return found;
    if ((int)found.size() < need && budget.Left() >= 60) {
        std::string derived = DeriveRegionFromMainPeers(mainPeers);
        if (searchRegion(derived, "страна основных пиров")) return found;
    }
    if (tried.empty()) LogError("Регион для поиска не определён: GeoIP не ответил, регион поездки и домашний не заданы, страну основных пиров найти не удалось.");
    return found;
}

// ------------------------------------------------------------------ разбор «почему нет связи»
struct PeerDiag { std::string uri, kind, text, error; };
struct Diagnosis { std::vector<PeerDiag> peers; std::string verdict, advice; bool anyUp = false; };
void ClassifyPeerError(PeerDiag& p) {
    const std::string& e = p.error;
    auto has = [&](const char* w) { return util::IContains(e, w); };
    if (e.empty()) { p.kind = "waiting"; p.text = "ещё подключается, ошибки пока нет"; }
    else if (has("no such host") || has("lookup")) { p.kind = "dns"; p.text = "имя сервера не находится (DNS)"; }
    else if (has("EOF") || has("reset by peer") || has("forcibly closed")) { p.kind = "eof"; p.text = "сервер обрывает соединение (EOF)"; }
    else if (has("timeout") || has("timed out") || has("deadline")) { p.kind = "timeout"; p.text = "сервер не отвечает (таймаут)"; }
    else if (has("refused")) { p.kind = "refused"; p.text = "порт закрыт (соединение отклонено)"; }
    else if (has("unreachable") || has("no route")) { p.kind = "unreachable"; p.text = "нет маршрута до сервера"; }
    else if (has("tls") || has("certificate") || has("handshake")) { p.kind = "tls"; p.text = "не прошло рукопожатие (TLS / ключ)"; }
    else { p.kind = "other"; p.text = "ошибка: " + e; }
}
// internet: 1 - точно есть, 0 - точно нет, -1 - проверить самому
Diagnosis Diagnose(const std::vector<std::string>& mainPeers, const std::vector<PeerDetail>& details, int internet = -1) {
    Diagnosis dg;
    int nDns = 0, nEof = 0, nTimeout = 0, nRefused = 0, nUnreach = 0, nTls = 0, nOther = 0, nWait = 0, nAbsent = 0, nUnstable = 0;
    for (const auto& uri : mainPeers) {
        PeerDiag pd;
        pd.uri = uri;
        const PeerDetail* f = ygg::FindPeer(details, uri);
        if (!f) { pd.kind = "absent"; pd.text = "Yggdrasil не знает этот пир (служба не перечитала конфиг?)"; ++nAbsent; }
        else if (f->up && f->unstable) { pd.kind = "unstable"; pd.text = "подключается и снова рвётся (" + std::to_string(f->flaps) + " раз за полчаса)"; ++nUnstable; }
        else if (f->up) { pd.kind = "up"; pd.text = "работает"; dg.anyUp = true; }
        else {
            pd.error = f->lastError;
            ClassifyPeerError(pd);
            if (pd.kind == "dns") ++nDns; else if (pd.kind == "eof") ++nEof; else if (pd.kind == "timeout") ++nTimeout;
            else if (pd.kind == "refused") ++nRefused; else if (pd.kind == "unreachable") ++nUnreach;
            else if (pd.kind == "tls") ++nTls; else if (pd.kind == "waiting") ++nWait; else ++nOther;
        }
        dg.peers.push_back(pd);
    }
    if (mainPeers.empty()) { dg.verdict = "В конфиге нет основных пиров."; dg.advice = "Добавь хотя бы два пира: экран «Пиры» → [a]."; return dg; }
    if (dg.anyUp) {
        int dead = nDns + nEof + nTimeout + nRefused + nUnreach + nTls + nOther + nAbsent + nUnstable;
        dg.verdict = dead == 0 ? "Связь есть, все основные пиры работают."
                               : "Связь есть, но " + std::to_string(dead) + " из " + std::to_string(mainPeers.size()) + " основных пиров не работают.";
        dg.advice = dead == 0 ? "" : "Пока держит хотя бы один пир, всё в порядке. Мёртвые лучше заменить.";
        return dg;
    }
    bool netOk = internet < 0 ? HasInternetFull() : internet == 1;
    if (!netOk) { dg.verdict = "Нет интернета вообще - дело не в пирах."; dg.advice = "Проверь Wi-Fi или кабель, роутер, VPN. Сторож сам заметит, когда сеть вернётся."; return dg; }
    int total = (int)mainPeers.size();
    if (nDns > 0) {
        if (!net::DnsWorks()) { dg.verdict = "Не работает DNS на этом компьютере: имена серверов не находятся."; dg.advice = "Проверь DNS в настройках сети или VPN. Пиры с IP-адресом вместо имени работают и без DNS."; return dg; }
        if (nDns == total) { dg.verdict = "DNS работает, но имён всех основных пиров больше нет - сервера, похоже, закрылись."; dg.advice = "Замени основные пиры: выбери новые в каталоге."; return dg; }
    }
    if (nUnstable == total) {
        dg.verdict = "Пиры подключаются и тут же отваливаются: соединение рвётся каждые несколько минут, трафик не успевает пойти. "
                     "Похоже, провайдер обрывает долгие соединения с этими серверами.";
        dg.advice = "Нужен пир поближе, лучше в своей стране - сторож добирает резервные сам; или подключение через VPN.";
        return dg;
    }
    if (nTimeout + nUnreach == total) { dg.verdict = "Сервера пиров не отвечают: похоже, провайдер или фаервол режет эти соединения."; dg.advice = "Попробуй пиры на портах 443/80, пиры с tls:// или quic://, либо через VPN."; return dg; }
    if (nEof == total) { dg.verdict = "Сервера принимают соединение и сразу обрывают его."; dg.advice = "Так бывает при DPI-блокировке или перегрузке сервера. Попробуй tls:// пиры или другие сервера."; return dg; }
    if (nRefused == total) { dg.verdict = "Сервера живы, но порты пиров закрыты - Yggdrasil на них больше не работает."; dg.advice = "Замени основные пиры."; return dg; }
    if (nWait == total) { dg.verdict = "Yggdrasil только что запущен или переподключается - ошибок пока нет."; dg.advice = "Подожди пару минут."; return dg; }
    if (nAbsent == total) { dg.verdict = "Пиров из конфига нет в работающем Yggdrasil."; dg.advice = "Перезапусти службу Yggdrasil, чтобы она перечитала конфиг."; return dg; }
    std::string parts;
    auto add = [&](int n, const char* w) { if (n) { if (!parts.empty()) parts += ", "; parts += std::to_string(n) + " - " + w; } };
    add(nDns, "имя не находится"); add(nEof, "обрыв"); add(nTimeout, "таймаут"); add(nRefused, "порт закрыт");
    add(nUnreach, "нет маршрута"); add(nTls, "TLS"); add(nOther, "другое"); add(nWait, "подключается"); add(nAbsent, "нет в службе"); add(nUnstable, "рвётся");
    dg.verdict = "Все основные пиры не работают, причины разные: " + parts + ".";
    dg.advice = "Замени мёртвые пиры на живые из каталога.";
    return dg;
}
std::string DiagnosisShort(const Diagnosis& dg) {
    std::string s = dg.verdict;
    for (const auto& p : dg.peers) if (p.kind != "up") s += "\n- " + util::HostFromUri(p.uri) + ": " + p.text;
    if (!dg.advice.empty()) s += "\n" + dg.advice;
    return s;
}
std::string DiagnoseNowText(const std::vector<std::string>& mainPeers) {
    auto d = GetPeerDetails();
    MarkUnstable(d, LoadState());
    return DiagnosisShort(Diagnose(mainPeers, d));
}
void UpdateDiagnosisState(WatchdogState& st, const std::string& phase) {
    st.diagVerdict.clear(); st.diagAdvice.clear(); st.diagPeers.clear(); st.diagAnyUp = false;
    bool checked = phase == "ok" || phase == "backup" || phase == "down" || phase == "crisis" || phase == "no_internet";
    if (!checked) return;
    auto mp = ReadMainPeers(true);
    auto d = GetPeerDetails();
    MarkUnstable(d, st);
    Diagnosis dg = Diagnose(mp, d, phase == "no_internet" ? 0 : 1);
    st.diagVerdict = util::OneLine(dg.verdict);
    st.diagAdvice = util::OneLine(dg.advice);
    st.diagAnyUp = dg.anyUp;
    for (const auto& p : dg.peers)
        st.diagPeers.push_back(util::OneLine(p.uri) + "\t" + util::OneLine(p.kind) + "\t" + util::OneLine(p.text) + "\t" + util::OneLine(p.error));
}

// ------------------------------------------------------------------ обновления из репозитория
// Релизов нет: версия - WATCHDOG_VERSION в linux/src/watchdog/version.h,
// программа - linux/ygg-watchdog. Проверки: git-отпечаток (файл не испорчен
// по дороге), заголовок ELF и самопроверка - новая версия должна запуститься
// и назвать ту версию, что в исходнике (ловит «exe забыли закоммитить»).
const char* REPO_API = "https://api.github.com/repos/Yozmor/ygg-watchdog/contents/";
const char* REPO_PAGE = "https://github.com/Yozmor/ygg-watchdog";
const long long UPDATE_CHECK_INTERVAL_SECONDS = 24 * 3600;

struct RepoFile { bool ok = false; std::string url, sha, err; long long size = -1; };
RepoFile GetRepoFile(const std::string& path) {
    RepoFile f;
    net::HttpResult r = net::Get(std::string(REPO_API) + path, net::RequestOpts(), "Accept: application/vnd.github+json\r\n");
    json::Value v;
    if (!r.transportOk) { f.err = "GitHub недоступен (" + r.err + ")"; return f; }
    if (r.status == 404) { f.err = "в репозитории нет " + path; return f; }
    if (r.status != 200 || !json::Parse(r.body, v) || v["download_url"].str().empty()) { f.err = "GitHub ответил кодом " + std::to_string(r.status); return f; }
    f.ok = true;
    f.url = v["download_url"].str();
    f.sha = util::Lower(v["sha"].str());
    f.size = v["size"].i64(-1);
    return f;
}
std::string g_updateCheckError;
std::string CheckForUpdate() {   // пусто - новее нет (или не узнать: см. g_updateCheckError)
    g_updateCheckError.clear();
    RepoFile src = GetRepoFile("linux/src/watchdog/version.h");
    if (!src.ok) { g_updateCheckError = src.err; return ""; }
    net::HttpResult r = net::Get(src.url);
    size_t k = r.body.find("WATCHDOG_VERSION \"");
    if (!r.transportOk || r.status != 200 || k == std::string::npos) { g_updateCheckError = "не удалось прочитать версию в репозитории"; return ""; }
    size_t a = k + 18, b = r.body.find('"', a);
    std::string ver = r.body.substr(a, b == std::string::npos ? 0 : b - a);
    if (ver.empty() || !util::VersionNewer(CURRENT_VERSION, ver)) return "";
    return ver;
}
// Самопроверка файла программы: запустить с --selftest, он должен назвать версию
std::string SelfTestVersion(const std::string& exe) {
    util::ProcResult r = util::Run({ exe, "--selftest" }, 30000);
    if (!r.started || r.timedOut || r.code != 0) return "";
    return util::Trim(r.out);
}
bool DownloadRepoFile(const std::string& repoPath, std::string& data, std::string& err) {
    RepoFile f = GetRepoFile(repoPath);
    if (!f.ok) { err = f.err; return false; }
    net::HttpResult r = net::Get(f.url);
    if (!r.transportOk || r.status != 200 || !r.complete) { err = !r.transportOk ? "файл не скачался (" + r.err + ")" : "сервер ответил кодом " + std::to_string(r.status); return false; }
    if (f.size >= 0 && (long long)r.body.size() != f.size) { err = "файл скачался не целиком"; return false; }
    if (f.sha.size() == 40 && util::GitBlobSha1(r.body) != f.sha) { err = "отпечаток скачанного файла не совпал с репозиторием (повреждён по дороге)"; return false; }
    data = r.body;
    return true;
}
// Установка обновления. Возвращает текст ошибки или "" (успех).
std::string ApplyUpdate(std::string* installedTag = nullptr) {
    std::string tag = CheckForUpdate();
    if (tag.empty()) return g_updateCheckError.empty() ? "новых версий нет" : "не удалось проверить обновление: " + g_updateCheckError;
    std::string self = util::SelfExe(), dir = util::DirOf(self);
    std::string data, err;
    LogInfo("Скачиваю новую версию " + tag + " из репозитория...");
    if (!DownloadRepoFile("linux/ygg-watchdog", data, err)) return "не удалось скачать: " + err;
    if (!util::LooksLikeElf(data)) return "скачанный файл не похож на целую программу (нет заголовка ELF)";
    std::string fresh = dir + "ygg-watchdog.new";
    if (!util::WriteFile(fresh, data, 0755)) return "не удалось записать " + fresh + " (нужен root)";
    chmod(fresh.c_str(), 0755);
    std::string got = SelfTestVersion(fresh);
    if (got != tag) {
        unlink(fresh.c_str());
        return got.empty() ? "скачанная версия " + tag + " не прошла самопроверку (не запускается на этом компьютере)"
                           : "в репозитории номер версии " + tag + ", а linux/ygg-watchdog внутри " + got + " - похоже, свежую сборку забыли закоммитить";
    }
    std::string backup = self + ".backup";
    if (!util::CopyFile(self, backup)) { unlink(fresh.c_str()); return "не удалось сохранить текущую версию как " + backup; }
    if (rename(fresh.c_str(), self.c_str()) != 0) { unlink(fresh.c_str()); return "не удалось заменить " + self; }
    if (SelfTestVersion(self) != tag) {
        util::CopyFile(backup, self);
        util::WriteFile(UpdateFailedPath(), tag + "\n");
        return "новая версия на месте не прошла самопроверку - вернул прежнюю";
    }
    LogInfo("Обновлено до " + tag + ". Прежняя версия сохранена как " + backup);
    AppendEvent("ok", "ygg-watchdog обновлён до " + tag);
    if (installedTag) *installedTag = tag;
    return "";
}
void CheckForUpdateIfDue(WatchdogState& st) {
    if (util::FileExists(UpdateFailedPath())) {
        std::string tag = util::Trim(util::ReadFile(UpdateFailedPath()));
        unlink(UpdateFailedPath().c_str());
        std::string m = "Обновление ygg-watchdog" + (tag.empty() ? std::string() : " до " + tag) + " не прошло самопроверку - возвращена прежняя версия " + CURRENT_VERSION + ".";
        LogError(m); Notify("warn", m);
    }
    long long now = util::Now();
    if (st.updateLastCheckedAt != 0 && now - st.updateLastCheckedAt < UPDATE_CHECK_INTERVAL_SECONDS) return;
    st.updateLastCheckedAt = now;
    std::string tag = CheckForUpdate();
    if (!tag.empty() && st.updateNotifiedTag != tag) {
        st.updateNotifiedTag = tag;
        std::string m = "В репозитории новая версия ygg-watchdog: " + tag + " (у тебя " + CURRENT_VERSION + "). " + REPO_PAGE +
                        ". Обновиться: ygg-watchdog → «Управление» → [i] (или команда apply-update).";
        LogInfo(m); Notify("info", m);
    }
    ygg::UpdateInfo yu = ygg::CheckYggUpdate();
    if (yu.ok) {
        std::string cur = ygg::InstalledVersion(ygg::CtlPaths());
        if (!cur.empty() && util::VersionNewer(cur, yu.tag) && st.yggUpdateNotifiedTag != yu.tag) {
            st.yggUpdateNotifiedTag = yu.tag;
            std::string m = "Вышла новая версия Yggdrasil: " + yu.tag + " (установлена " + cur + "). " + (yu.page.empty() ? "" : "Скачать: " + yu.page);
            LogInfo(m); Notify("info", m);
        }
    }
}

// ------------------------------------------------------------------ одна проверка
std::string g_tickPhase;

void TickInner(TickBudget& budget) {
    g_tickPhase = "error";
    CleanOldLogEntries();
    LogInfo("=== Начало проверки ===");
    WatchdogState st = LoadState();
    if (!CheckBattery(st)) { SaveTickState(st); LogInfo("Аварийный режим по батарее активен - остальные проверки пропущены."); g_tickPhase = "battery"; return; }
    CheckForUpdateIfDue(st);
    SaveTickState(st);
    ConfigInfo cfg = LoadConfigInfo();
    CheckConfigHealth(st, cfg);
    SaveTickState(st);
    if (!EnsureYggdrasilService(st)) { SaveTickState(st); g_tickPhase = "service_down"; return; }
    if (st.internetDownSince != 0) {
        bool ok = CheckInternet(st);
        SaveTickState(st);
        if (!ok) { g_tickPhase = "no_internet"; return; }
    }
    long long now = util::Now();
    auto details = GetPeerDetails();
    if (!g_lastPeersOk) { LogError("yggdrasilctl не отвечает (служба запущена, но управление недоступно) - проверка пиров пропущена."); SaveTickState(st); return; }
    auto newFlaps = UpdatePeerTracks(st, details, now);
    MarkUnstable(details, st);
    for (const auto& d : details) {
        auto& o = g_observations[d.uri];
        o.checks++;
        if (d.up && !d.unstable) o.up++;
        if (newFlaps.count(d.uri)) o.flaps++;
    }
    std::vector<std::string> mainPeers;
    if (cfg.ok) { for (const auto& p : cfg.peers) if (std::find(mainPeers.begin(), mainPeers.end(), p) == mainPeers.end()) mainPeers.push_back(p); }
    else for (const auto& d : details) if (std::find(st.backupPeers.begin(), st.backupPeers.end(), d.uri) == st.backupPeers.end()) mainPeers.push_back(d.uri);
    auto aliveStable = [&](const std::string& u) { const PeerDetail* d = ygg::FindPeer(details, u); return d && d->up && !d->unstable; };
    {
        std::vector<std::string> keep;
        for (const auto& p : st.backupPeers) {
            if (aliveStable(p)) { keep.push_back(p); continue; }
            const PeerDetail* d = ygg::FindPeer(details, p);
            LogInfo(std::string(d && d->unstable ? "Резервный постоянно переподключается - убираю: " : "Резервный не работает - убираю: ") + p);
            RemovePeer(p);
            st.failedCandidates[p] = now;
        }
        st.backupPeers = keep;
    }
    std::set<std::string> usedHosts;
    int mainAlive = 0;
    for (const auto& p : mainPeers) {
        usedHosts.insert(util::HostFromUri(p));
        if (aliveStable(p)) ++mainAlive;
        else if (const PeerDetail* d = ygg::FindPeer(details, p)) if (d->unstable) LogInfo("Основной пир постоянно переподключается - не считаю его живым: " + p);
    }
    for (const auto& p : st.backupPeers) usedHosts.insert(util::HostFromUri(p));
    int alive = mainAlive + (int)st.backupPeers.size();
    int aliveAtStart = alive;
    bool wasLost = st.connLost;
    if (alive > MAX_ALIVE_PEERS && !st.backupPeers.empty()) {
        std::vector<std::pair<double, std::string>> byRtt;
        for (const auto& p : st.backupPeers) { const PeerDetail* d = ygg::FindPeer(details, p); byRtt.push_back({ d && d->rttMs >= 0 ? d->rttMs : 1e9, p }); }
        std::sort(byRtt.begin(), byRtt.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int extra = std::min(alive - MAX_ALIVE_PEERS, (int)byRtt.size());
        std::vector<std::string> removed;
        for (int i = 0; i < extra; ++i) {
            RemovePeer(byRtt[i].second);
            removed.push_back(byRtt[i].second);
            st.backupPeers.erase(std::remove(st.backupPeers.begin(), st.backupPeers.end(), byRtt[i].second), st.backupPeers.end());
        }
        alive -= extra;
        LogInfo("Живых пиров больше " + std::to_string(MAX_ALIVE_PEERS) + " - убраны лишние резервные: " + Join(removed, ", "));
        AppendEvent("info", "Убраны лишние резервные (живых пиров было больше " + std::to_string(MAX_ALIVE_PEERS) + "): " + Join(removed, ", "));
    }
    auto finishConnected = [&](const std::string& note) {
        if (wasLost && !note.empty()) { LogInfo(note); Notify("ok", note); }
        st.connLost = false; st.outageNotified = false; st.firewallNotified = false;
        g_tickPhase = mainAlive > 0 ? "ok" : "backup";
        CheckTunHealth(st, cfg);
        SaveTickState(st);
        RunDeviceChecks(st);
        SaveTickState(st);
    };
    if (alive >= MIN_ALIVE_PEERS) {
        st.topupRetryAt = 0;
        LogInfo("Проверка выполнена: живых пиров " + std::to_string(alive) + " (основных " + std::to_string(mainAlive) + ")");
        finishConnected("Связь через Yggdrasil снова есть: живых пиров " + std::to_string(alive) + ".");
        return;
    }
    if (alive > 0 && now < st.topupRetryAt) {
        LogInfo("Живых пиров " + std::to_string(alive) + " из нужных " + std::to_string(MIN_ALIVE_PEERS) + " - добор отложен до " +
                util::TimeText(st.topupRetryAt, "%H:%M") + " (в прошлый раз новых кандидатов не нашлось)");
        finishConnected("Связь через Yggdrasil снова есть: живых пиров " + std::to_string(alive) + ".");
        return;
    }
    LogInfo(alive == 0 ? "Живых пиров нет - ищу замену" : "Живых пиров " + std::to_string(alive) + " из нужных " + std::to_string(MIN_ALIVE_PEERS) + " - добираю резервные");
    if (!CheckInternet(st)) { SaveTickState(st); g_tickPhase = "no_internet"; return; }
    g_tickPhase = alive > 0 ? (mainAlive > 0 ? "ok" : "backup") : "down";
    bool exhausted = true;
    g_githubFailed = false;
    auto found = SearchBackups(st, mainPeers, usedHosts, MIN_ALIVE_PEERS - alive, budget, exhausted);
    if (g_githubFailed && found.empty()) CheckLocalVpn(st);
    alive += (int)found.size();
    if (!found.empty()) {
        std::vector<std::string> list;
        for (const auto& f : found) list.push_back(f.uri + " (" + SlugToRu(f.region) + (f.city.empty() ? "" : "/" + f.city) + ")");
        if (aliveAtStart == 0) {
            std::string why = mainPeers.empty() ? "основных пиров в конфиге нет" : "основные (" + Join(mainPeers, ", ") + ") не работают";
            Notify("warn", "Связи через Yggdrasil не было: " + why + ". Добавлены резервные: " + Join(list, ", "));
        } else {
            AppendEvent("info", "Живых пиров было " + std::to_string(aliveAtStart) + " из " + std::to_string(MIN_ALIVE_PEERS) + " - добавлены резервные: " + Join(list, ", "));
        }
    }
    if (alive >= MIN_ALIVE_PEERS) st.topupRetryAt = 0;
    else if (alive > 0 && exhausted) st.topupRetryAt = now + TOPUP_RETRY_SECONDS;
    if (alive > 0) { finishConnected(found.empty() ? "Связь через Yggdrasil снова есть: живых пиров " + std::to_string(alive) + "." : ""); return; }

    st.connLost = true;
    LogError("Не удалось найти рабочий резервный пир ни в одном из регионов поиска.");
    if (!st.outageNotified) {
        st.outageNotified = true;
        SaveTickState(st);
        Notify("alert", "Все пиры легли, замену найти не удалось. Дальше - крайний перебор всех стран.\n\nРазбор: " + DiagnoseNowText(mainPeers));
    }
    if (!RunApprovedVpnCommand("restart"))
        LogInfo("Если проблема в VPN - без одобренного VPN-скрипта (approve-vpn-command) сторож VPN не трогает. Проверь VPN-клиент вручную.");
    if (now - st.firewallCheckedAt >= 3600) {
        st.firewallCheckedAt = now;
        auto fw = CheckFirewallDiagnostics();
        if (fw) {
            LogError("Фаервол: " + *fw);
            if (!st.firewallNotified) { st.firewallNotified = true; Notify("alert", "Возможная причина: " + *fw); }
        }
    }
    SaveTickState(st);
    bool ok = CrisisScanStep(st, usedHosts, std::max(10LL, budget.Left() - 10));
    if (ok) { st.connLost = false; st.outageNotified = false; g_tickPhase = "backup"; }
    else g_tickPhase = "crisis";
    SaveTickState(st);
}

// true - проверка прошла; false - уже идёт другая (эта пропущена)
bool Tick() {
    util::FileLock lock(TickLockPath(), false);
    if (!lock.ok()) { LogInfo("Предыдущая проверка ещё идёт - эту пропускаю."); return false; }
    TickBudget budget;
    TickInner(budget);
    FlushPeerStats();
    if (budget.Elapsed() >= 60) LogInfo("Проверка заняла " + std::to_string(budget.Elapsed()) + " с");
    WatchdogState st = LoadState();
    st.lastTickAt = util::Now();
    st.lastPhase = g_tickPhase;
    UpdateDiagnosisState(st, g_tickPhase);
    SaveState(st);
    RecordHistory(g_tickPhase);
    return true;
}
bool TickRunning() {
    util::FileLock lock(TickLockPath(), false);
    return !lock.ok();
}
