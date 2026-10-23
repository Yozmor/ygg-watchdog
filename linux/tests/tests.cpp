// tests.cpp - проверки разборщиков и логики ygg-watchdog (Linux) без сети и без Yggdrasil.
// Сборка и запуск: ./tests/run_tests.sh (пишет файлы только во временную папку).
#include "../src/watchdog/core.cpp"
#include "../src/watchdog/cmds.cpp"

static int g_pass = 0, g_fail = 0;
static void Check(bool ok, const std::string& what) {
    if (ok) ++g_pass;
    else { ++g_fail; printf("  ПРОВАЛ: %s\n", what.c_str()); }
}
static void Eq(const std::string& got, const std::string& want, const std::string& what) {
    Check(got == want, what + ": получено «" + got + "», ожидалось «" + want + "»");
}
static void Near(double got, double want, double eps, const std::string& what) {
    Check(std::fabs(got - want) <= eps, what + ": получено " + std::to_string(got) + ", ожидалось " + std::to_string(want));
}
static void Section(const char* n) { printf("- %s\n", n); }

static void TestStrings() {
    Section("строки и UTF-8");
    Eq(util::Trim("  a b \n"), "a b", "Trim");
    Check(util::Split("a;;b", ';').size() == 2, "Split выбрасывает пустые");
    Check(util::SplitKeep("a\t\tb", '\t').size() == 3, "SplitKeep сохраняет пустые");
    Eq(util::U8Fit("Владивосток", 5), "Влад…", "обрезка кириллицы");
    Check(util::U8Len("Москва") == 6, "длина кириллицы в символах");
    Eq(util::U8Lower("РоСсИя Ёж"), "россия ёж", "нижний регистр кириллицы");
    Eq(util::Ago(125), "2 мин", "Ago");
    Eq(util::MinSec(102), "1:42", "MinSec");
    Check(util::VersionNewer("1.9.9", "2.0.0") && !util::VersionNewer("2.0.0", "2.0.0") && util::VersionNewer("v1.2", "1.10"), "сравнение версий");
}
static void TestUri() {
    Section("адреса пиров");
    auto u = util::ParseUri("tls://[2001:db8::1]:443?key=abc");
    Check(u.ok && u.host == "2001:db8::1" && u.port == "443" && u.proto == "tls", "IPv6 в скобках");
    Eq(util::HostFromUri("tcp://Msk-1.Example.RU:8363"), "msk-1.example.ru", "хост в нижнем регистре");
    Check(util::PortFromUri("quic://1.2.3.4:443") == 443, "порт");
    Check(!util::ParseUri("мусор").ok, "мусор - не адрес");
}
static void TestJson() {
    Section("JSON");
    json::Value v;
    Check(json::Parse(R"({"a":[1,2.5,"текст"],"b":{"c":true,"d":null}})", v), "разбор");
    Eq(v["a"][2].str(), "текст", "\\u-последовательность в UTF-8");
    Check(v["a"][1].num() == 2.5 && v["b"]["c"].boolean() && v["b"]["d"].isNull(), "числа, bool, null");
    Check(v["нет"]["нет"].isNull(), "отсутствующий ключ - null, без падения");
    json::Value o = json::MakeObject();
    o.at("Peers") = json::MakeArray();
    o.at("Peers").arr.push_back(json::MakeString("tls://a:1"));
    o.at("IfName") = json::MakeString("auto");
    Eq(json::Dump(o), R"({"Peers":["tls://a:1"],"IfName":"auto"})", "порядок ключей сохраняется");
    Check(json::ParseLoose("Warning: x\n{\"peers\":[]}", v) && v["peers"].isArray(), "JSON после лишней строки");
}
static void TestPeersJson() {
    Section("getPeers -json");
    json::Value v;
    json::Parse(R"({"peers":[{"remote":"tls://de.example.net:443","up":true,"latency":48000000,"uptime":3600,"bytes_recvd":1048576},
                              {"remote":"tcp://x:1","up":false,"last_error":"dial tcp: lookup x: no such host"}]})", v);
    auto p = ygg::ParsePeers(v);
    Check(p.size() == 2, "два пира");
    Near(p[0].rttMs, 48, 0.01, "задержка из наносекунд");
    Check(!p[1].up && p[1].rttMs < 0, "у неживого пира задержки нет");
    PeerDiag pd; pd.error = p[1].lastError; ClassifyPeerError(pd);
    Eq(pd.kind, "dns", "ошибка lookup - это DNS");
    pd.error = "read tcp: connection reset by peer"; ClassifyPeerError(pd); Eq(pd.kind, "eof", "reset - обрыв");
    pd.error = "dial tcp 1.2.3.4:443: i/o timeout"; ClassifyPeerError(pd); Eq(pd.kind, "timeout", "таймаут");
}
static void TestMarkdown() {
    Section("список публичных пиров (markdown)");
    auto c = ParseCountryMarkdown("# Russia\n\n### Moscow\n* `tls://a.ru:443` - ok\n* `quic://b.ru:443`\n### Vladivostok\n`tcp://[2a01::1]:9000`\n`socks://x:1`\n");
    Check(c.size() == 3, "три пира (socks не берём)");
    Check(c.size() == 3 && c[0].city == "Moscow" && c[2].city == "Vladivostok", "города по заголовкам");
}
static void TestState() {
    Section("state.txt");
    WatchdogState s;
    s.backupPeers = { "tls://a:1", "tls://b:2" };
    s.regionalCountry = "russia"; s.regionalCity = "Vladivostok";
    s.devices = { { "ratatosk", "202:a2a5::1" } };
    s.devicePeers["ratatosk"] = { "tls://c:3" };
    s.deviceWhy["ratatosk"] = "спит\tтаб";
    PeerTrack t; t.up = true; t.uptime = 100; t.at = util::Now(); t.flaps = { util::Now() - 60 };
    s.peerTracks["tls://a:1"] = t;
    s.failedCandidates["tls://x:9"] = util::Now();
    WatchdogState r = ParseState(FormatState(s));
    Check(r.backupPeers == s.backupPeers, "резервные");
    Check(r.regionalCity == "Vladivostok" && r.devices.size() == 1 && r.devicePeers["ratatosk"].size() == 1, "регион и устройства");
    Eq(r.deviceWhy["ratatosk"], "спит таб", "табы в причине заменены пробелами");
    Check(r.peerTracks["tls://a:1"].flaps.size() == 1 && r.failedCandidates.count("tls://x:9"), "история пира и неудачные кандидаты");
    WatchdogState m = MigrateFromJson(R"({"backup_peers":["tls://a:1"],"regional_peers":["tls://r:2"],"regional_countries":["russia/Moscow"],
                                         "home_region":"germany","devices":{"server":"201:831c::1"}})");
    Check(m.backupPeers.size() == 2 && m.regionalCountry == "russia" && m.regionalCity == "Moscow" && m.homeRegion == "germany" &&
          m.devices.size() == 1 && m.devices[0].name == "server", "перенос из state.json Python-версии");
}
static void TestFlaps() {
    Section("пир, который постоянно переподключается");
    WatchdogState st;
    long long now = util::Now() - 2000;
    PeerDetail d; d.uri = "tls://a:1"; d.up = true; d.uptimeSec = 1000;
    UpdatePeerTracks(st, { d }, now);
    for (int i = 1; i <= 3; ++i) { d.uptimeSec = 30; UpdatePeerTracks(st, { d }, now + i * 180); }
    std::vector<PeerDetail> v = { d };
    MarkUnstable(v, st);
    Check(v[0].unstable && v[0].flaps == 3, "3 переподключения за полчаса - нестабилен");
}
static void TestStats() {
    Section("статистика и история");
    PeerStats ps;
    long long today = DayNumber(util::Now());
    ps[{ today, "tls://good:1" }] = { 100, 99, 0, 2, 2 };
    ps[{ today, "tls://bad:1" }] = { 100, 30, 9, 0, 0 };
    auto rel = ComputeReliability(ParsePeerStats(FormatPeerStats(ps)), today);
    Check(rel["tls://good:1"].score > 0.95 && rel["tls://bad:1"].score < 0.5, "надёжность");
    std::vector<PeerCandidate> c = { { "tls://bad:1", "" }, { "tls://new:1", "" }, { "tls://good:1", "" } };
    OrderByReliability<PeerCandidate>(c, rel, [](const PeerCandidate& x) { return x.uri; });
    Check(c[0].uri == "tls://good:1" && c[2].uri == "tls://bad:1", "надёжные вперёд, плохие в конец");
    std::map<std::string, std::vector<HistSample>> h;
    long long t = util::Now();
    ParseHistoryLine(std::to_string(t) + "\tok\t1\ttls://a:1?k=x=y=U|40.0;tls://b:2=D|-1", h);
    ParseHistoryLine(std::to_string(t + 1) + "\tok\t1\ttls://a:1?k=x=y=U|60.0", h);
    Check(h.count("tls://a:1?k=x=y") && h["tls://a:1?k=x=y"].size() == 2, "uri с '=' внутри");
    LatencyAgg a = Aggregate(h["tls://a:1?k=x=y"], 0);
    Near(a.avg, 50, 0.01, "средняя"); Near(a.min, 40, 0.01, "минимум"); Near(a.max, 60, 0.01, "максимум");
    Check(Aggregate(h["tls://b:2"], 0).up == 0, "неживой пир");
}
static void TestDiagnose() {
    Section("разбор «почему нет связи»");
    std::vector<PeerDetail> d(2);
    d[0].uri = "tls://a:1"; d[0].lastError = "i/o timeout";
    d[1].uri = "tls://b:1"; d[1].lastError = "connection timed out";
    Diagnosis dg = Diagnose({ "tls://a:1", "tls://b:1" }, d, 1);
    Check(util::Contains(dg.verdict, "не отвечают"), "все таймауты - режут соединения");
    d[0].up = true;
    dg = Diagnose({ "tls://a:1", "tls://b:1" }, d, 1);
    Check(dg.anyUp && util::Contains(dg.verdict, "1 из 2"), "связь есть, один мёртвый");
    dg = Diagnose({}, d, 1);
    Check(util::Contains(dg.verdict, "нет основных"), "пустой конфиг");
    Check(util::Contains(Diagnose({ "tls://z:1" }, {}, 0).verdict, "Нет интернета"), "нет интернета - не пиры");
}
static void TestCities() {
    Section("города и страны");
    const auto& db = LoadCityDatabase();
    Check(db.size() > 20000, "cities.dat загружен (" + std::to_string(db.size()) + ")");
    auto m = FindCity(db, "RU", "Moscow"), v = FindCity(db, "RU", "Vladivostok");
    Check(m && v, "Москва и Владивосток есть");
    if (m && v) Near(HaversineKm(m->lat, m->lon, v->lat, v->lon), 6420, 200, "расстояние Москва-Владивосток");
    Eq(SlugToIso("russia"), "RU", "slug->ISO"); Eq(IsoToSlug("DE"), "germany", "ISO->slug"); Eq(SlugToRu("united-states"), "США", "русское имя");
    auto c = ResolveCity("RU", "москва");
    Check(c && c->name == "Moscow", "город по русскому названию");
    Eq(ResolveCountryInput("Россия"), "russia", "страна по русскому названию");
}
static void TestBinary() {
    Section("файлы, хэши, ELF");
    Eq(util::GitBlobSha1("hello\n"), "ce013625030ba8dba906f756967f9e9ca394464a", "git-отпечаток (как git hash-object)");
    Eq(util::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
    Check(util::LooksLikeElf(util::ReadFile(util::SelfExe())), "сам tests - целый ELF");
    Check(!util::LooksLikeElf(std::string("\x7f" "ELF") + std::string(10, '\0')), "обрезанный ELF - не программа");
    std::string f = ProgDir() + "atomic.txt";
    Check(util::WriteFileAtomic(f, "один") && util::WriteFileAtomic(f, "два") && util::ReadFile(f) == "два", "атомарная запись");
}
static void TestNet() {
    Section("сеть: разбор без сети");
    auto p = net::ParseProxy("socks5://127.0.0.1:10808");
    Check(p.ok && p.socks && p.port == 10808, "socks5-прокси");
    p = net::ParseProxy("127.0.0.1:2080");
    Check(p.ok && !p.socks && p.host == "127.0.0.1", "голый адрес - HTTP-прокси");
    net::HttpResult r;
    net::ParseResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", r);
    Check(r.status == 200 && r.body == "hello world" && r.complete, "chunked-ответ");
    net::ParseResponse("HTTP/1.1 302 Found\r\nLocation: https://x/y\r\nContent-Length: 0\r\n\r\n", r);
    Check(r.status == 302 && r.location == "https://x/y", "редирект");
    net::ParseResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", r);
    Check(!r.complete, "обрыв тела замечен");
    net::Route rt;
    Check(net::RouteFromText(net::RouteToText({ "proxy", "socks5://127.0.0.1:1", "x" }), rt) && rt.proxy == "socks5://127.0.0.1:1", "маршрут туда-обратно");
    auto u = net::ParseUrl("http://ip-api.com/json/?fields=countryCode");
    Check(u.ok && !u.tls && u.port == 80 && u.path == "/json/?fields=countryCode", "http-ссылка");
}

int main() {
    g_quiet = true;
    char tmpl[] = "/tmp/ygg-watchdog-tests-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    g_dirOverride = dir + "/";
    util::CopyFile(util::SelfDir() + "../cities.dat", g_dirOverride + "cities.dat");
    if (!util::FileExists(g_dirOverride + "cities.dat")) util::CopyFile(util::SelfDir() + "cities.dat", g_dirOverride + "cities.dat");
    printf("Тесты ygg-watchdog %s (Linux)\n", CURRENT_VERSION);
    TestStrings(); TestUri(); TestJson(); TestPeersJson(); TestMarkdown(); TestState(); TestFlaps(); TestStats();
    TestDiagnose(); TestCities(); TestBinary(); TestNet();
    printf("\nИтог: %d пройдено, %d провалено\n", g_pass, g_fail);
    for (const auto& n : util::ListDir(dir)) unlink((dir + "/" + n).c_str());
    rmdir(dir.c_str());
    return g_fail ? 1 : 0;
}
