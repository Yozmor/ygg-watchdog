// cmds.cpp - команды сторожа (для systemd, окна, панели и консоли).
// Подключается после core.cpp.

// ------------------------------------------------------------------ основные пиры в конфиге
bool ModifyMainPeers(const std::function<void(std::vector<std::string>&)>& mutator, std::string* err = nullptr) {
    ygg::Paths p = ygg::CtlPaths();
    json::Value cfg;
    std::string e;
    if (!ygg::LoadConfig(p, cfg, e)) { if (err) *err = e; LogError("Не удалось прочитать конфиг: " + e); return false; }
    p.adminListen = ygg::FindAdminListen(p, cfg);
    std::vector<std::string> peers;
    for (size_t i = 0; i < cfg["Peers"].size(); ++i) if (cfg["Peers"][i].isString()) peers.push_back(cfg["Peers"][i].str());
    mutator(peers);
    json::Value arr = json::MakeArray();
    for (const auto& u : peers) arr.arr.push_back(json::MakeString(u));
    cfg.at("Peers") = arr;
    std::string backup;
    e = ygg::SaveConfig(p, cfg, &backup);
    if (!e.empty()) { if (err) *err = e; LogError("Не удалось сохранить конфиг: " + e); return false; }
    LogInfo("Конфиг сохранён. Копия: " + backup);
    return true;
}
void CmdListMainPeers() {
    auto peers = ReadMainPeers();
    Print("Основные пиры (" + std::to_string(peers.size()) + "):");
    for (size_t i = 0; i < peers.size(); ++i) Print("  " + std::to_string(i + 1) + ") " + peers[i]);
}
bool CmdAddMainPeer(const std::string& uri) {
    if (uri.find("://") == std::string::npos) { PrintError("пир должен быть вида tls://host:port"); return false; }
    auto cur = ReadMainPeers();
    if (std::find(cur.begin(), cur.end(), uri) != cur.end()) { PrintError("такой основной пир уже есть в конфиге: " + uri); return false; }
    if (!ModifyMainPeers([&](std::vector<std::string>& v) { v.push_back(uri); })) return false;
    LogInfo("Основной пир добавлен в конфиг: " + uri);
    Print("Изменения вступят в силу только после перезапуска службы Yggdrasil.");
    return true;
}
bool CmdRemoveMainPeer(const std::string& idxOrUri) {
    auto cur = ReadMainPeers();
    std::string target;
    if (util::IsNumber(idxOrUri)) {
        size_t i = (size_t)util::ToLL(idxOrUri);
        if (i < 1 || i > cur.size()) { PrintError("нет пира с номером " + idxOrUri); return false; }
        target = cur[i - 1];
    } else target = idxOrUri;
    bool removed = false;
    if (!ModifyMainPeers([&](std::vector<std::string>& v) {
            auto it = std::find(v.begin(), v.end(), target);
            if (it != v.end()) { v.erase(it); removed = true; }
        })) return false;
    if (!removed) { PrintError("такого пира в конфиге нет: " + target); return false; }
    LogInfo("Основной пир удалён из конфига: " + target);
    Print("Изменения вступят в силу только после перезапуска службы Yggdrasil.");
    return true;
}

// ------------------------------------------------------------------ регионы
std::string ResolveCountryInput(const std::string& in) {
    if (util::IsNumber(in)) {
        auto list = GetSortedCountryList();
        size_t i = (size_t)util::ToLL(in);
        return i >= 1 && i <= list.size() ? list[i - 1].slug : "";
    }
    std::string low = util::U8Lower(in);
    for (const auto& [s, ru] : COUNTRY_RU) if (util::U8Lower(ru) == low) return s;
    return util::Lower(in);
}
std::optional<City> ResolveCity(const std::string& iso, const std::string& in) {
    const auto& db = LoadCityDatabase();
    if (util::IsNumber(in)) {
        auto list = GetCitiesForCountry(db, iso);
        size_t i = (size_t)util::ToLL(in);
        if (i >= 1 && i <= list.size()) return list[i - 1];
        return std::nullopt;
    }
    if (auto c = FindCity(db, iso, in)) return c;
    std::string low = util::U8Lower(in);
    for (const auto& c : db) if (util::IEquals(c.countryCode, iso) && !c.nameRu.empty() && util::U8Lower(c.nameRu) == low) return c;
    return std::nullopt;
}
bool CmdSetRegion(const std::string& countryIn, const std::string& cityIn, bool home) {
    if (home && (countryIn.empty() || util::Lower(countryIn) == "clear")) {
        WatchdogState st = LoadState();
        st.homeRegion.clear(); st.homeCity.clear();
        SaveState(st);
        LogInfo("Домашний регион снят.");
        return true;
    }
    std::string slug = ResolveCountryInput(countryIn), iso = SlugToIso(slug);
    if (slug.empty() || iso.empty()) { PrintError("неизвестная страна: '" + countryIn + "'. Список стран - команда list-countries."); return false; }
    std::string cityName, cityLabel;
    if (!cityIn.empty()) {
        auto c = ResolveCity(iso, cityIn);
        if (!c) { PrintError("не удалось определить город по вводу '" + cityIn + "' (list-cities " + slug + ")."); return false; }
        cityName = c->name;
        cityLabel = c->displayName();
    }
    WatchdogState st = LoadState();
    std::string what = SlugToRu(slug) + (cityLabel.empty() ? " (любой город)" : ", " + cityLabel);
    if (home) {
        st.homeRegion = slug; st.homeCity = cityName;
        if (st.backupPeers.empty()) st.backupRegion.clear();
        SaveState(st);
        LogInfo("Домашний регион установлен: " + what);
    } else {
        st.regionalCountry = slug; st.regionalCity = cityName;
        SaveState(st);
        LogInfo("Регион поездки задан: " + what);
        AppendEvent("info", "Регион поездки: " + what);
    }
    return true;
}
void CmdRemoveRegion() {
    WatchdogState st = LoadState();
    if (st.regionalCountry.empty()) { Print("Регион поездки и так не задан."); return; }
    st.regionalCountry.clear(); st.regionalCity.clear();
    SaveState(st);
    LogInfo("Регион поездки снят");
    AppendEvent("info", "Регион поездки снят");
}
void CmdListCountries() {
    auto list = GetSortedCountryList();
    if (list.empty()) { PrintError("не удалось получить список стран - проверь интернет"); return; }
    Print("Доступные страны (" + std::to_string(list.size()) + "):");
    for (size_t i = 0; i < list.size(); ++i) Print("  " + std::to_string(i + 1) + ") " + list[i].ru + " [" + list[i].slug + "]" +
                                                    (list[i].supported ? "" : " (координаты не поддерживаются)"));
}
void CmdListCities(const std::string& in) {
    std::string slug = ResolveCountryInput(in), iso = SlugToIso(slug);
    if (iso.empty()) { PrintError("неизвестная страна: '" + in + "'"); return; }
    auto list = GetCitiesForCountry(LoadCityDatabase(), iso);
    Print("Города: " + SlugToRu(slug) + " (" + iso + "), всего " + std::to_string(list.size()) + ":");
    for (size_t i = 0; i < list.size(); ++i) Print("  " + std::to_string(i + 1) + ") " + list[i].displayLabel() + " (население " + std::to_string(list[i].population) + ")");
}

// ------------------------------------------------------------------ устройства
bool CmdAddDevice(const std::string& name, const std::string& host, const std::vector<std::string>& peers = {}, bool setPeers = false) {
    if (name.empty() || host.empty()) { PrintError("имя и адрес устройства не могут быть пустыми."); return false; }
    for (const auto& p : peers) if (p.find("://") == std::string::npos) { PrintError("пир должен быть вида tls://host:port: " + p); return false; }
    WatchdogState st = LoadState();
    bool found = false;
    std::string canon = name;
    for (auto& d : st.devices) if (util::IEquals(d.name, name)) { d.host = host; found = true; canon = d.name; break; }
    if (!found) st.devices.push_back({ name, host });
    if (setPeers) { if (peers.empty()) st.devicePeers.erase(canon); else st.devicePeers[canon] = peers; }
    SaveState(st);
    LogInfo(std::string(found ? "Устройство обновлено: " : "Устройство добавлено: ") + canon + " (" + host + ")" +
            (st.devicePeers.count(canon) ? ", общие пиры: " + Join(st.devicePeers[canon], " ") : ""));
    return true;
}
bool CmdRemoveDevice(const std::string& name) {
    WatchdogState st = LoadState();
    size_t before = st.devices.size();
    st.devices.erase(std::remove_if(st.devices.begin(), st.devices.end(), [&](const Device& d) { return util::IEquals(d.name, name); }), st.devices.end());
    if (st.devices.size() == before) { PrintError("устройство не найдено: " + name); return false; }
    st.deviceDownSince.erase(name); st.deviceEscalatedUntil.erase(name); st.deviceWhy.erase(name); st.devicePeers.erase(name);
    auto tp = st.deviceTempPeers.find(name);
    if (tp != st.deviceTempPeers.end()) {
        for (const auto& u : tp->second) if (std::find(st.backupPeers.begin(), st.backupPeers.end(), u) == st.backupPeers.end()) RemovePeer(u);
        st.deviceTempPeers.erase(tp);
    }
    SaveState(st);
    LogInfo("Устройство удалено: " + name);
    return true;
}
void CmdListDevices() {
    WatchdogState st = LoadState();
    if (st.devices.empty()) { Print("Устройств для мониторинга не добавлено."); return; }
    Print("Устройства (" + std::to_string(st.devices.size()) + "):");
    for (const auto& d : st.devices) {
        bool down = st.deviceDownSince.count(d.name) > 0;
        Print("  " + d.name + " (" + d.host + ")" + (down ? " - НЕДОСТУПНО: " + st.deviceWhy[d.name] : " - ок"));
        if (st.devicePeers.count(d.name)) Print("      общие пиры: " + Join(st.devicePeers[d.name], " "));
    }
}
// Разбор одного устройства в JSON (для окна)
std::string DeviceDiagJson(const std::string& name) {
    WatchdogState st = LoadState();
    for (const auto& d : st.devices) {
        if (!util::IEquals(d.name, name)) continue;
        bool ok = PingDeviceDebounced(d.host);
        DeviceDiag dg;
        if (ok) { dg.kind = "ok"; dg.text = "на связи"; }
        else dg = DiagnoseDevice(d.host);
        std::string j = "{\"name\":" + json::Quote(d.name) + ",\"kind\":" + json::Quote(dg.kind) + ",\"text\":" + json::Quote(dg.text) + ",\"steps\":[";
        for (size_t i = 0; i < dg.steps.size(); ++i) j += std::string(i ? "," : "") + "{\"ok\":" + (dg.steps[i].first ? "true" : "false") + ",\"text\":" + json::Quote(dg.steps[i].second) + "}";
        return j + "]}";
    }
    return "{\"name\":" + json::Quote(name) + ",\"kind\":\"none\",\"text\":\"устройство не найдено\",\"steps\":[]}";
}
void CmdDiagnoseDevice(const std::string& name) {
    WatchdogState st = LoadState();
    bool any = false;
    for (const auto& d : st.devices) {
        if (!name.empty() && !util::IEquals(d.name, name)) continue;
        any = true;
        if (PingDeviceDebounced(d.host)) { Print(d.name + ": на связи."); continue; }
        DeviceDiag dg = DiagnoseDevice(d.host);
        Print(d.name + ": " + (dg.kind == "ok" ? "на связи." : "недоступно - " + dg.text + "."));
        for (const auto& s : dg.steps) Print(std::string("   ") + (s.first ? "✓ " : "✗ ") + s.second);
    }
    if (!any) PrintError(name.empty() ? "устройств не добавлено." : "устройство не найдено: " + name);
}

// ------------------------------------------------------------------ проверка пира и каталог
struct VerifyResult { bool up = false; double ms = -1; std::string err, text; bool wasConfigured = false; };
VerifyResult VerifyPeerNow(const std::string& uri) {
    VerifyResult r;
    r.wasConfigured = ygg::FindPeer(GetPeerDetails(), uri) != nullptr;
    if (!r.wasConfigured) AddPeer(uri);
    auto start = std::chrono::steady_clock::now();
    int upSeen = 0;
    for (;;) {
        auto d = GetPeerDetails();
        if (const PeerDetail* p = ygg::FindPeer(d, uri)) {
            r.up = p->up; r.ms = p->rttMs; r.err = p->lastError;
            if (r.up && (++upSeen >= 2 || r.ms > 0)) break;
        }
        long long el = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (el >= HANDSHAKE_TIMEOUT_MS || (r.wasConfigured && !r.up) || (!r.up && !r.err.empty())) break;
        util::SleepMs(1000);
    }
    if (!r.wasConfigured) { RemovePeer(uri); g_handshakeResults.push_back({ uri, r.up }); FlushPeerStats(); }
    if (!r.up) { PeerDiag pd; pd.error = r.err; ClassifyPeerError(pd); r.text = pd.text; }
    return r;
}
std::string VerifyJson(const std::string& uri, const VerifyResult& r) {
    return "{\"uri\":" + json::Quote(uri) + ",\"up\":" + (r.up ? "true" : "false") + ",\"ms\":" + (r.ms >= 0 ? util::Fixed(r.ms, 1) : "-1") +
           ",\"error\":" + json::Quote(r.err) + ",\"text\":" + json::Quote(r.text) + ",\"was_configured\":" + (r.wasConfigured ? "true" : "false") + "}";
}
std::string CatalogJson(const std::string& slug) {
    auto cands = FetchCountryPeers(slug);
    std::vector<std::string> uris;
    for (const auto& c : cands) uris.push_back(c.uri);
    auto pr = net::ProbeMany(uris);
    auto rel = LoadReliability();
    std::vector<size_t> order(cands.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        double ma = pr[a].ms, mb = pr[b].ms;
        if ((ma >= 0) != (mb >= 0)) return ma >= 0;
        return ma >= 0 && ma < mb;
    });
    std::string j = "[";
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        auto it = rel.find(cands[i].uri);
        j += std::string(k ? "," : "") + "{\"uri\":" + json::Quote(cands[i].uri) + ",\"city\":" + json::Quote(cands[i].city) +
             ",\"tcp_ms\":" + (pr[i].ms >= 0 ? util::Fixed(pr[i].ms, 1) : "-1") + ",\"err\":" + json::Quote(pr[i].err) +
             ",\"reliability\":" + (it != rel.end() && it->second.score >= 0 ? util::Fixed(100 * it->second.score, 1) : "-1") + "}";
    }
    return j + "]";
}
std::string CountriesJson() {
    auto list = GetSortedCountryList();
    std::string j = "[";
    for (size_t i = 0; i < list.size(); ++i)
        j += std::string(i ? "," : "") + "{\"slug\":" + json::Quote(list[i].slug) + ",\"name\":" + json::Quote(list[i].ru) + ",\"supported\":" + (list[i].supported ? "true" : "false") + "}";
    return j + "]";
}
std::string CitiesJson(const std::string& slug) {
    std::string iso = SlugToIso(slug), j = "[";
    if (!iso.empty()) {
        auto list = GetCitiesForCountry(LoadCityDatabase(), iso);
        for (size_t i = 0; i < list.size(); ++i)
            j += std::string(i ? "," : "") + "{\"name\":" + json::Quote(list[i].name) + ",\"label\":" + json::Quote(list[i].displayLabel()) +
                 ",\"ru\":" + json::Quote(list[i].nameRu) + ",\"population\":" + std::to_string(list[i].population) +
                 ",\"lat\":" + util::Fixed(list[i].lat, 4) + ",\"lon\":" + util::Fixed(list[i].lon, 4) + "}";
    }
    return j + "]";
}

// ------------------------------------------------------------------ статистика
std::string AggJson(const LatencyAgg& a) {
    auto n1 = [](double v) { return v >= 0 ? util::Fixed(v, 1) : std::string("-1"); };
    return "{\"n\":" + std::to_string(a.n) + ",\"up\":" + std::to_string(a.up) + ",\"up_pct\":" + (a.n ? util::Fixed(100.0 * a.up / a.n, 1) : "-1") +
           ",\"avg_ms\":" + n1(a.avg) + ",\"p95_ms\":" + n1(a.p95) + ",\"jitter_ms\":" + n1(a.jitter) + ",\"min_ms\":" + n1(a.min) + ",\"max_ms\":" + n1(a.max) + "}";
}
// series: bins отрезков за hours часов, средняя задержка в каждом (-1 - нет данных);
// state: 1 - работал, 0 - не работал, -1 - не проверялся
std::string StatsJson(int hours = 24, int bins = 96) {
    auto hist = LoadHistory();
    auto rel = LoadReliability();
    WatchdogState st = LoadState();
    auto mains = ReadMainPeers(true);
    std::set<std::string> uris;
    for (const auto& kv : hist) uris.insert(kv.first);
    for (const auto& kv : rel) uris.insert(kv.first);
    long long now = util::Now(), span = hours * 3600LL, step = std::max(1LL, span / bins);
    long long checks = 0;
    for (const auto& l : util::Lines(util::ReadFile(HistoryPath()))) if (util::ToLL(l.substr(0, l.find('\t')), 0) >= now - span) ++checks;
    std::string j = "{\"now\":" + std::to_string(now) + ",\"hours\":" + std::to_string(hours) + ",\"checks\":" + std::to_string(checks) + ",\"peers\":[";
    bool first = true;
    for (const auto& uri : uris) {
        const auto& v = hist[uri];
        auto r = rel.count(uri) ? rel[uri] : Reliability();
        auto tr = st.peerTracks.find(uri);
        int flaps = tr == st.peerTracks.end() ? 0 : (int)tr->second.flaps.size();
        bool isMain = std::find(mains.begin(), mains.end(), uri) != mains.end();
        bool isBackup = std::find(st.backupPeers.begin(), st.backupPeers.end(), uri) != st.backupPeers.end();
        LatencyAgg span1 = Aggregate(v, now - span);
        long long flapsSpan = 0;
        for (const auto& [k, d] : ParsePeerStats(util::ReadFile(PeerStatsPath()))) if (k.second == uri && k.first >= DayNumber(now - span)) flapsSpan += d.flaps;
        j += std::string(first ? "" : ",") + "{\"uri\":" + json::Quote(uri) + ",\"main\":" + (isMain ? "true" : "false") + ",\"backup\":" + (isBackup ? "true" : "false") +
             ",\"last_seen\":" + std::to_string(v.empty() ? 0 : v.back().ts) + ",\"last_up\":" + (!v.empty() && v.back().up ? "true" : "false") +
             ",\"flaps_30min\":" + std::to_string(flaps) + ",\"flaps_span\":" + std::to_string(flapsSpan) +
             ",\"h1\":" + AggJson(Aggregate(v, now - 3600)) + ",\"d1\":" + AggJson(Aggregate(v, now - 86400)) +
             ",\"d7\":" + AggJson(Aggregate(v, now - 7 * 86400)) + ",\"span\":" + AggJson(span1) +
             ",\"reliability\":" + (r.score >= 0 ? util::Fixed(100 * r.score, 1) : "-1") + ",\"reliability_samples\":" + std::to_string(r.samples) +
             ",\"flaps_35d\":" + std::to_string(r.flaps) + ",\"tries\":" + std::to_string(r.tries) + ",\"tries_ok\":" + std::to_string(r.triesOk);
        first = false;
        std::string series = "[", state = "[";
        for (int k = 0; k < bins; ++k) {
            long long a = now - span + k * step, b = a + step;
            double sum = 0; int n = 0, seen = 0, up = 0;
            for (const auto& s : v) if (s.ts >= a && s.ts < b) { ++seen; if (s.up) { ++up; if (s.ms >= 0) { sum += s.ms; ++n; } } }
            series += std::string(k ? "," : "") + (n ? util::Fixed(sum / n, 1) : "-1");
            state += std::string(k ? "," : "") + (seen == 0 ? "-1" : up * 2 >= seen ? "1" : "0");
        }
        j += ",\"series\":" + series + "],\"state\":" + state + "]}";
    }
    return j + "]}";
}
void CmdStatsCsv() {
    json::Value v;
    json::Parse(StatsJson(24 * 7, 7), v);
    auto num = [](double x) { return x < 0 ? std::string() : util::FixedRu(x, 1); };
    std::string t = "пир;роль;работает сейчас";
    for (const char* w : { "1 ч", "24 ч", "7 сут" }) t += std::string(";доступность ") + w + ", %;средняя " + w + ", мс;P95 " + w + ", мс;разброс " + w + ", мс";
    t += ";надёжность 30 сут, %;наблюдений;обрывов 35 сут;пробных рукопожатий;удачных\r\n";
    for (size_t i = 0; i < v["peers"].size(); ++i) {
        const json::Value& p = v["peers"][i];
        t += util::Replace(p["uri"].str(), ";", " ") + ";" + (p["main"].boolean() ? "основной" : p["backup"].boolean() ? "резервный" : "-") + ";" +
             (p["last_seen"].i64() > util::Now() - 600 ? (p["last_up"].boolean() ? "да" : "нет") : "-");
        for (const char* w : { "h1", "d1", "d7" })
            t += ";" + num(p[w]["up_pct"].num(-1)) + ";" + num(p[w]["avg_ms"].num(-1)) + ";" + num(p[w]["p95_ms"].num(-1)) + ";" + num(p[w]["jitter_ms"].num(-1));
        t += ";" + num(p["reliability"].num(-1)) + ";" + p["reliability_samples"].text() + ";" + p["flaps_35d"].text() + ";" + p["tries"].text() + ";" + p["tries_ok"].text() + "\r\n";
    }
    std::string path = ProgDir() + "stats_export.csv";
    if (util::WriteFileAtomic(path, "\xEF\xBB\xBF" + t)) Print("Статистика выгружена: " + path);
    else PrintError("не удалось записать " + path);
}

// ------------------------------------------------------------------ systemd: установка таймера
const char* UNIT_DIR = "/etc/systemd/system/";
struct TimerInfo { bool installed = false, active = false, enabled = false; long long next = 0, last = 0; };
TimerInfo QueryTimer() {
    TimerInfo t;
    util::ProcResult r = util::Run({ "systemctl", "show", "ygg-watchdog.timer", "--timestamp=unix",
                                     "-p", "LoadState,ActiveState,UnitFileState,NextElapseUSecRealtime,LastTriggerUSec" }, 5000);
    std::map<std::string, std::string> kv;
    for (const auto& l : util::Lines(r.out)) { size_t e = l.find('='); if (e != std::string::npos) kv[l.substr(0, e)] = l.substr(e + 1); }
    t.installed = kv["LoadState"] == "loaded";
    t.active = kv["ActiveState"] == "active";
    t.enabled = kv["UnitFileState"] == "enabled";
    auto ts = [](const std::string& s) { return !s.empty() && s[0] == '@' ? util::ToLL(s.substr(1), 0) : 0LL; };
    t.next = ts(kv["NextElapseUSecRealtime"]);
    t.last = ts(kv["LastTriggerUSec"]);
    return t;
}
bool CmdInstall() {
    if (!util::IsRoot()) { PrintError("нужны права root: sudo " + util::SelfExe() + " install"); return false; }
    std::string self = util::SelfExe(), dir = util::DirOf(self);
    // старая Python-версия: свои юниты и команда
    for (const char* old : { "ygg-watchdog-tick.timer", "ygg-watchdog-tick.service" }) {
        if (util::FileExists(std::string(UNIT_DIR) + old)) {
            util::Run({ "systemctl", "disable", "--now", old }, 15000);
            unlink((std::string(UNIT_DIR) + old).c_str());
            Print("Убран старый юнит Python-версии: " + std::string(old));
        }
    }
    std::string svc =
        "[Unit]\n"
        "Description=ygg-watchdog: одна проверка пиров Yggdrasil\n"
        "After=network-online.target yggdrasil.service\n"
        "Wants=network-online.target\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        "WorkingDirectory=" + dir + "\n"
        "ExecStart=" + self + " tick\n"
        "TimeoutStartSec=10min\n"
        "Nice=5\n";
    // OnCalendar + Persistent: пропущенная во сне проверка - сразу после пробуждения
    std::string tmr =
        "[Unit]\n"
        "Description=ygg-watchdog: проверка пиров Yggdrasil каждые 3 минуты\n\n"
        "[Timer]\n"
        "OnBootSec=1min\n"
        "OnCalendar=*:0/3\n"
        "Persistent=true\n"
        "AccuracySec=5s\n"
        "Unit=ygg-watchdog.service\n\n"
        "[Install]\n"
        "WantedBy=timers.target\n";
    if (!util::WriteFileAtomic(std::string(UNIT_DIR) + "ygg-watchdog.service", svc, 0644) ||
        !util::WriteFileAtomic(std::string(UNIT_DIR) + "ygg-watchdog.timer", tmr, 0644)) { PrintError("не удалось записать юниты в " + std::string(UNIT_DIR)); return false; }
    // команда ygg-watchdog и путь для панели
    std::string link = "/usr/local/bin/ygg-watchdog";
    struct stat lst;
    if (lstat(link.c_str(), &lst) == 0 && (S_ISLNK(lst.st_mode) || S_ISREG(lst.st_mode))) unlink(link.c_str());
    if (symlink(self.c_str(), link.c_str()) != 0) Print("Не удалось создать команду " + link + " (" + strerror(errno) + ")");
    util::MkDirs("/etc/yozmor");
    util::WriteFileAtomic("/etc/yozmor/watchdog_path", self + "\n", 0644);
    util::Run({ "systemctl", "daemon-reload" }, 30000);
    util::ProcResult r = util::Run({ "systemctl", "enable", "--now", "ygg-watchdog.timer" }, 30000);
    if (r.code != 0) { PrintError("таймер не включился: " + util::Trim(r.out)); return false; }
    chmod(dir.c_str(), 0755);
    LogInfo("Таймер ygg-watchdog установлен: проверка каждые 3 минуты (и сразу после пробуждения). Программа: " + self);
    AppendEvent("ok", "Автопроверки включены: каждые 3 минуты.");
    return true;
}
bool CmdUninstall() {
    if (!util::IsRoot()) { PrintError("нужны права root"); return false; }
    util::Run({ "systemctl", "disable", "--now", "ygg-watchdog.timer" }, 30000);
    unlink((std::string(UNIT_DIR) + "ygg-watchdog.timer").c_str());
    unlink((std::string(UNIT_DIR) + "ygg-watchdog.service").c_str());
    util::Run({ "systemctl", "daemon-reload" }, 30000);
    char buf[4096];
    ssize_t n = readlink("/usr/local/bin/ygg-watchdog", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; if (std::string(buf) == util::SelfExe()) unlink("/usr/local/bin/ygg-watchdog"); }
    LogInfo("Таймер ygg-watchdog удалён. Файлы программы остались в " + util::SelfDir());
    return true;
}
bool CmdTimer(bool on) {
    util::ProcResult r = util::Run({ "systemctl", on ? "enable" : "disable", "--now", "ygg-watchdog.timer" }, 30000);
    if (r.code != 0) { PrintError(util::Trim(r.out).empty() ? "systemctl не справился" : util::Trim(r.out)); return false; }
    LogInfo(on ? "Автопроверки включены: каждые 3 минуты." : "Автопроверки выключены - ручная проверка по-прежнему работает.");
    AppendEvent(on ? "ok" : "warn", on ? "Автопроверки включены." : "Автопроверки выключены вручную.");
    return true;
}

// ------------------------------------------------------------------ status-json
std::string StatusJson() {
    WatchdogState st = LoadState();
    ConfigInfo ci = LoadConfigInfo();
    auto bat = ReadBattery();
    long long now = util::Now();
    TimerInfo tm = QueryTimer();
    auto b = [](bool x) { return std::string(x ? "true" : "false"); };
    auto arr = [](const std::vector<std::string>& v) { std::string r = "["; for (size_t i = 0; i < v.size(); ++i) r += (i ? "," : "") + json::Quote(v[i]); return r + "]"; };
    std::string j = "{";
    j += "\"version\":" + json::Quote(CURRENT_VERSION);
    j += ",\"device\":" + json::Quote(util::HostName());
    j += ",\"now\":" + std::to_string(now);
    j += ",\"last_tick_at\":" + std::to_string(st.lastTickAt);
    j += ",\"tick_interval_sec\":180";
    j += ",\"tick_running\":" + b(TickRunning());
    j += ",\"timer\":{\"installed\":" + b(tm.installed) + ",\"active\":" + b(tm.active) + ",\"enabled\":" + b(tm.enabled) +
         ",\"next\":" + std::to_string(tm.next) + ",\"last\":" + std::to_string(tm.last) + "}";
    j += ",\"last_phase\":" + json::Quote(st.lastPhase);
    j += ",\"mode\":" + json::Quote(st.regionalCountry.empty() ? "normal" : "regional");
    j += ",\"min_peers\":" + std::to_string(MIN_ALIVE_PEERS) + ",\"max_peers\":" + std::to_string(MAX_ALIVE_PEERS);
    std::vector<std::string> unstable;
    for (const auto& [u, tr] : st.peerTracks) if ((int)tr.flaps.size() >= FLAP_THRESHOLD) unstable.push_back(u);
    j += ",\"unstable_peers\":" + arr(unstable);
    j += ",\"service_problem\":" + b(st.serviceNotified);
    j += ",\"config_problem\":" + b(st.configNotified);
    j += ",\"config_error\":" + json::Quote(st.configError);
    j += ",\"config_restore_from\":" + json::Quote(st.configRestoreFrom.empty() ? "" : util::BaseName(st.configRestoreFrom));
    j += ",\"config_path\":" + json::Quote(ci.paths.conf);
    j += ",\"config_good_at\":" + std::to_string(util::FileMTime(GoodConfigPath(ci.paths)));
    j += ",\"tun_problem\":" + json::Quote(st.tunProblem);
    j += ",\"config_found\":" + b(ci.found);
    j += ",\"main_peers\":" + arr(ci.peers);
    j += ",\"backup_peers\":" + arr(st.backupPeers);
    j += ",\"backup_region\":" + json::Quote(st.backupRegion);
    j += ",\"regional_peers\":[]";
    j += ",\"regional_country\":" + json::Quote(st.regionalCountry);
    j += ",\"regional_city\":" + json::Quote(st.regionalCity);
    j += ",\"home_region\":" + json::Quote(st.homeRegion);
    j += ",\"home_city\":" + json::Quote(st.homeCity);
    j += ",\"egress_region\":" + json::Quote(st.egressRegion);
    j += ",\"internet_down_since\":" + std::to_string(st.internetDownSince);
    long long cooldown = st.crisisLastFullScanCompletedAt ? st.crisisLastFullScanCompletedAt + CRISIS_COOLDOWN_SECONDS : 0;
    j += ",\"crisis\":{\"countries_total\":" + std::to_string(st.crisisCountrySlugs.size()) + ",\"country_index\":" + std::to_string(st.crisisCountryIndex) +
         ",\"peer_index\":" + std::to_string(st.crisisPeerIndex) + ",\"current_country\":" +
         json::Quote(st.crisisCountryIndex < st.crisisCountrySlugs.size() ? st.crisisCountrySlugs[st.crisisCountryIndex] : "") +
         ",\"cooldown_until\":" + std::to_string(cooldown) + "}";
    j += ",\"devices\":[";
    for (size_t i = 0; i < st.devices.size(); ++i) {
        const auto& d = st.devices[i];
        auto dn = st.deviceDownSince.find(d.name);
        auto why = st.deviceWhy.find(d.name);
        auto pe = st.devicePeers.find(d.name);
        auto tp = st.deviceTempPeers.find(d.name);
        j += std::string(i ? "," : "") + "{\"name\":" + json::Quote(d.name) + ",\"host\":" + json::Quote(d.host) +
             ",\"down_since\":" + std::to_string(dn == st.deviceDownSince.end() ? 0 : dn->second) +
             ",\"why\":" + json::Quote(why == st.deviceWhy.end() ? "" : why->second) +
             ",\"peers\":" + arr(pe == st.devicePeers.end() ? std::vector<std::string>() : pe->second) +
             ",\"temp_peers\":" + arr(tp == st.deviceTempPeers.end() ? std::vector<std::string>() : tp->second) + "}";
    }
    j += "]";
    j += ",\"battery\":" + (bat ? "{\"percent\":" + std::to_string(bat->percent) + ",\"charging\":" + b(bat->charging) + "}" : std::string("null"));
    j += ",\"battery_critical_mode\":" + b(st.batteryCriticalMode);
    j += ",\"update_available\":" + json::Quote(st.updateNotifiedTag);
    j += ",\"ygg_update_available\":" + json::Quote(st.yggUpdateNotifiedTag);
    std::string vpn = ApprovedVpnPath();
    j += ",\"vpn_script_approved\":" + b(!vpn.empty());
    j += ",\"vpn_script\":" + json::Quote(vpn.substr(0, vpn.find('\t')));
    NotifyConfig nc = LoadNotifyConfig();
    j += ",\"telegram_configured\":" + b(nc.valid);
    j += ",\"telegram_chat\":" + json::Quote(nc.chatId);
    {
        auto f = util::SplitKeep(util::Trim(util::ReadFile(TgLastPath())), '\t');
        while (f.size() < 4) f.push_back("");
        j += ",\"telegram_last\":{\"at\":" + std::to_string(util::ToLL(f[0], 0)) + ",\"ok\":" + b(f[1] == "1") + ",\"via\":" + json::Quote(f[2]) + ",\"err\":" + json::Quote(f[3]) + "}";
    }
    j += ",\"diagnosis\":{\"verdict\":" + json::Quote(st.diagVerdict) + ",\"advice\":" + json::Quote(st.diagAdvice) + ",\"any_up\":" + b(st.diagAnyUp) + ",\"peers\":[";
    bool first = true;
    for (const auto& line : st.diagPeers) {
        auto f = util::SplitKeep(line, '\t');
        if (f.size() < 3) continue;
        j += std::string(first ? "" : ",") + "{\"uri\":" + json::Quote(f[0]) + ",\"kind\":" + json::Quote(f[1]) + ",\"text\":" + json::Quote(f[2]) +
             ",\"error\":" + json::Quote(f.size() > 3 ? f[3] : "") + "}";
        first = false;
    }
    j += "]}";
    j += ",\"files\":{\"log\":" + json::Quote(LogPath()) + ",\"events\":" + json::Quote(EventsPath()) + ",\"history\":" + json::Quote(HistoryPath()) +
         ",\"state\":" + json::Quote(StatePath()) + "}";
    return j + "}";
}

// ------------------------------------------------------------------ полный перебор по задержке (из Python-версии)
void CmdFindBestPeers(int count, int perCountry, bool add) {
    Print("Полный перебор по всем странам (по " + std::to_string(perCountry) + " кандидату на страну) - это займёт время, возможно десятки минут...");
    auto mains = ReadMainPeers(true);
    std::set<std::string> used;
    for (const auto& m : mains) used.insert(util::HostFromUri(m));
    std::vector<std::pair<double, std::string>> results;
    for (const auto& c : GetSortedCountryList()) {
        auto cands = FetchCountryPeers(c.slug);
        std::vector<std::string> uris;
        for (const auto& p : cands) if (!used.count(util::HostFromUri(p.uri))) uris.push_back(p.uri);
        auto pr = net::ProbeMany(uris);
        std::vector<std::pair<double, std::string>> open;
        for (size_t i = 0; i < uris.size(); ++i) if (pr[i].ms >= 0) open.push_back({ pr[i].ms, uris[i] });
        std::sort(open.begin(), open.end());
        int tried = 0;
        for (const auto& [tcp, uri] : open) {
            if (tried >= perCountry) break;
            ++tried;
            Print("  " + c.ru + ": " + uri + " ...");
            VerifyResult r = VerifyPeerNow(uri);
            if (r.up) { results.push_back({ r.ms >= 0 ? r.ms : 1e6, uri }); Print("    Up, " + (r.ms >= 0 ? util::Fixed(r.ms, 0) + " мс" : std::string("задержка не измерена"))); }
            else Print("    не поднялся: " + r.text);
        }
    }
    std::sort(results.begin(), results.end());
    if (results.empty()) { PrintError("ни один кандидат не поднялся ни в одной стране."); return; }
    Print("\nЛучшие пиры по реальной задержке:");
    for (size_t i = 0; i < results.size() && (int)i < count; ++i)
        Print("  " + results[i].second + " - " + (results[i].first < 1e6 ? util::Fixed(results[i].first, 0) + " мс" : std::string("не измерено")));
    if (add) {
        for (size_t i = 0; i < results.size() && (int)i < count; ++i) CmdAddMainPeer(results[i].second);
        Print("Не забудь перезапустить службу Yggdrasil, чтобы изменения вступили в силу.");
    }
}

// ------------------------------------------------------------------ справка и разбор команд
void PrintHelp() {
    Print(std::string("ygg-watchdog ") + CURRENT_VERSION + " - сторож пиров Yggdrasil\n\n"
          "  ygg-watchdog                      окно (интерфейс в терминале)\n"
          "  ygg-watchdog tick                 одна проверка (её запускает таймер раз в 3 минуты)\n"
          "  ygg-watchdog install | uninstall  включить / убрать таймер systemd и команду ygg-watchdog\n"
          "  ygg-watchdog start-timer | stop-timer\n"
          "  ygg-watchdog status | status-json | diagnose\n"
          "  ygg-watchdog list-main-peers | add-main-peer <uri> | remove-main-peer <номер|uri>\n"
          "  ygg-watchdog list-countries | list-cities <страна>\n"
          "  ygg-watchdog set-region <страна> [город] | remove-region\n"
          "  ygg-watchdog set-home-region <страна> [город] | set-home-region clear\n"
          "  ygg-watchdog list-devices | add-device <имя> <адрес> [пир ...] | remove-device <имя> | diagnose-device [имя]\n"
          "  ygg-watchdog verify-peer <uri>    подключить пир на пару секунд и показать результат\n"
          "  ygg-watchdog catalog-json <страна> | stats-json | stats-csv\n"
          "  ygg-watchdog restart-yggdrasil | restart-yggdrasil-delayed [секунд]\n"
          "  ygg-watchdog approve-vpn-command <путь> | clear-vpn-command\n"
          "  ygg-watchdog check-update | apply-update | test-notify | restore-config\n"
          "  ygg-watchdog find-best-peers [--count 3] [--per-country 1] [--add]\n"
          "  -v - подробный лог");
}
void CmdStatus() {
    json::Value v;
    json::Parse(StatusJson(), v);
    Print("ygg-watchdog " + v["version"].str() + " на " + v["device"].str());
    Print("Конфиг: " + v["config_path"].str() + (v["config_problem"].boolean() ? " - НЕ ЧИТАЕТСЯ: " + v["config_error"].str() : ""));
    long long last = v["last_tick_at"].i64();
    Print("Последняя проверка: " + (last ? util::TimeText(last) + " (" + util::Ago(util::Now() - last) + " назад), итог: " + v["last_phase"].str() : std::string("ещё не было")));
    Print(std::string("Таймер: ") + (v["timer"]["active"].boolean() ? "включён" : "ВЫКЛЮЧЕН") +
          (v["timer"]["next"].i64() ? ", следующая проверка " + util::TimeText(v["timer"]["next"].i64(), "%H:%M:%S") : ""));
    auto region = [](const std::string& slug, const std::string& city) {
        if (slug.empty()) return std::string("-");
        auto c = city.empty() ? std::nullopt : FindCity(LoadCityDatabase(), SlugToIso(slug), city);
        return SlugToRu(slug) + (city.empty() ? "" : ", " + (c ? c->displayName() : city));
    };
    Print("Регион поездки: " + region(v["regional_country"].str(), v["regional_city"].str()));
    Print("Домашний регион: " + region(v["home_region"].str(), v["home_city"].str()));
    Print("Выход трафика: " + (v["egress_region"].str().empty() ? std::string("не определён") : SlugToRu(v["egress_region"].str())));
    Print("\nОсновные пиры (" + std::to_string(v["main_peers"].size()) + "):");
    for (size_t i = 0; i < v["main_peers"].size(); ++i) Print("  " + v["main_peers"][i].str());
    Print("Резервные (" + std::to_string(v["backup_peers"].size()) + "):");
    for (size_t i = 0; i < v["backup_peers"].size(); ++i) Print("  " + v["backup_peers"][i].str());
    if (!v["diagnosis"]["verdict"].str().empty()) Print("\nРазбор: " + v["diagnosis"]["verdict"].str() + " " + v["diagnosis"]["advice"].str());
}
