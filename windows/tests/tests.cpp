// tests.cpp - проверки разборщиков и логики ygg-watchdog без сети и без Yggdrasil.
// Подключает сам ygg_watchdog.cpp (без его точки входа) и гоняет функции на
// заранее заданных данных. Сборка и запуск - run_tests.bat рядом.
// Пишет state.txt / watchdog.log в эту папку (tests\), рабочие файлы сторожа не трогает.
#define YGG_WATCHDOG_NO_MAIN
#include "../ygg_watchdog.cpp"

static int g_pass = 0, g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (ok) ++g_pass;
    else { ++g_fail; WriteOut(L"  ПРОВАЛ: " + what + L"\r\n"); }
}
static void Eq(const std::wstring& got, const std::wstring& want, const std::wstring& what) {
    Check(got == want, what + L": получено «" + got + L"», ожидалось «" + want + L"»");
}
static void Near(double got, double want, const std::wstring& what) {
    Check(std::fabs(got - want) < 0.01, what + L": получено " + std::to_wstring(got) + L", ожидалось " + std::to_wstring(want));
}
static void Section(const wchar_t* name) { WriteOut(std::wstring(L"- ") + name + L"\r\n"); }

static void TestUris() {
    Section(L"адреса пиров");
    Eq(HostFromUri(L"tls://[2001:db8::1]:443"), L"2001:db8::1", L"хост IPv6 в скобках");
    Check(PortFromUri(L"tls://[2001:db8::1]:443") == 443, L"порт IPv6");
    Eq(HostFromUri(L"quic://1.2.3.4:443?key=abc"), L"1.2.3.4", L"хост с ?key=");
    Eq(HostFromUri(L"tcp://Host.Example.org:80"), L"host.example.org", L"хост в нижнем регистре");
    Check(PortFromUri(L"tcp://Host.Example.org:80") == 80, L"порт DNS-имени");
    Eq(HostFromUri(L"мусор"), L"", L"не адрес");
    util::PeerUri u = util::ParseUri(L"wss://ygg.example.com:443/ws?key=1");
    Check(u.ok && u.scheme == L"wss" && u.host == L"ygg.example.com" && u.port == L"443" && u.query == L"key=1", L"ParseUri wss с путём");
    {
        in6_addr a;
        InetPtonW(AF_INET6, L"201:831c:922f:195a:45e9:bc59:ccc8:b93a", &a);
        Check(IsYggAddress(a), L"201:… - адрес Yggdrasil");
        InetPtonW(AF_INET6, L"300:1::1", &a);
        Check(IsYggAddress(a), L"300:… (подсеть) - адрес Yggdrasil");
        InetPtonW(AF_INET6, L"2001:db8::1", &a);
        Check(!IsYggAddress(a), L"2001:db8::1 - не Yggdrasil");
    }
    Eq(NormalizeIpv6(L"201-831c-922f-195a-45e9-bc59-ccc8-b93a.ipv6-literal.net"), L"201:831c:922f:195a:45e9:bc59:ccc8:b93a",
       L"адрес вида ipv6-literal.net");
}

static void TestVersions() {
    Section(L"версии");
    Check(VersionLessOrEqual(L"1.1.1", L"v1.1.1"), L"1.1.1 <= v1.1.1");
    Check(!VersionLessOrEqual(L"1.2.0", L"1.1.9"), L"1.2.0 > 1.1.9");
    Check(VersionLessOrEqual(L"1.1.1", L"1.1.10"), L"1.1.1 <= 1.1.10 (числа, не строки)");
    Check(util::VersionNewer(L"0.5.13", L"v0.5.14"), L"v0.5.14 новее 0.5.13");
    Check(!util::VersionNewer(L"0.5.14", L"0.5.14"), L"0.5.14 не новее себя");
}

static void TestPeersJson() {
    Section(L"getPeers -json");
    std::wstring sample =
        L"{\"peers\":[{\"remote\":\"tls://95.217.35.92:1337\",\"up\":true,\"latency\":42500000,\"uptime\":125.5,\"last_error\":\"\"},"
        L"{\"remote\":\"tls://ygg-msk-1.averyan.ru:8362\",\"up\":false,\"last_error\":\"context deadline exceeded\"}]}";
    bool ok = false;
    auto v = ParsePeersJson(sample, ok);
    Check(ok && v.size() == 2, L"два пира разобрано");
    if (v.size() == 2) {
        Check(v[0].up && std::fabs(v[0].rttMs - 42.5) < 0.01 && std::fabs(v[0].uptimeSec - 125.5) < 0.01, L"задержка в мс из наносекунд, uptime");
        Check(!v[1].up && v[1].rttMs < 0 && v[1].lastError == L"context deadline exceeded", L"мёртвый пир с ошибкой");
    }
    ParsePeersJson(L"Error: dial tcp 127.0.0.1:9002: connect refused", ok);
    Check(!ok, L"не JSON - ok=false");
}

static void TestClassify() {
    Section(L"разбор ошибок пиров");
    auto kind = [](const wchar_t* e) { PeerDiag pd; pd.error = e; ClassifyPeerError(pd); return pd.kind; };
    Eq(kind(L"dial tcp: lookup ygg.keenetic.pro: no such host"), L"dns", L"DNS");
    Eq(kind(L"EOF"), L"eof", L"EOF");
    Eq(kind(L"context deadline exceeded"), L"timeout", L"таймаут");
    Eq(kind(L"connectex: No connection could be made because the target machine actively refused it."), L"refused", L"отказ");
    Eq(kind(L""), L"waiting", L"пусто - ещё подключается");
}

static void TestFlaps() {
    Section(L"пир, который постоянно переподключается");
    WatchdogState st;
    auto tick = [&](long long t, bool up, double uptime) {
        PeerDetail d; d.uri = L"tls://a:1"; d.up = up; d.uptimeSec = uptime;
        std::vector<PeerDetail> v{ d };
        auto nf = UpdatePeerTracks(st, v, t);
        MarkUnstable(v, st);
        return std::make_pair((int)nf.size(), v[0].unstable);
    };
    tick(1000, true, 100);
    auto r = tick(1180, true, 280);
    Check(r.first == 0, L"uptime вырос на время между проверками - обрыва нет");
    r = tick(1360, true, 20);
    Check(r.first == 1 && !r.second, L"uptime сбросился - один обрыв, ещё не «нестабилен»");
    tick(1540, true, 10);
    r = tick(1720, true, 5);
    Check(r.second, L"три обрыва за полчаса - нестабилен");
    r = tick(3700, true, 2000);
    Check(!r.second, L"через полчаса без обрывов - снова стабилен");
    r = tick(3880, false, -1);
    Check(r.first == 1, L"был Up - стал Down: обрыв");
}

static void TestDiagnose() {
    Section(L"разбор «почему нет связи»");
    PeerDetail a; a.uri = L"tls://a:1"; a.up = true; a.unstable = true; a.flaps = 4;
    PeerDetail b = a; b.uri = L"tls://b:2";
    Diagnosis dg = Diagnose({ a.uri, b.uri }, { a, b }, 1);
    Check(!dg.anyUp && dg.verdict.find(L"отваливаются") != std::wstring::npos, L"все рвутся - свой вердикт");
    b.unstable = false;
    dg = Diagnose({ a.uri, b.uri }, { a, b }, 1);
    Check(dg.anyUp, L"один стабильный - связь есть");
    dg = Diagnose({ L"tls://c:3" }, {}, 1);
    Check(dg.peers.size() == 1 && dg.peers[0].kind == L"absent", L"пира нет в службе");
}

static void TestState() {
    Section(L"state.txt");
    WatchdogState s;
    s.backupPeers = { L"tls://a:1", L"quic://[2001:db8::1]:443?key=x" };
    s.regionalCountry = L"russia"; s.regionalCity = L"Moscow";
    s.homeRegion = L"finland";
    s.devices.push_back({ L"Kjele", L"201:831c::1" });
    s.deviceDownSince[L"Kjele"] = 12345;
    s.outageNotified = true;
    long long now = (long long)time(nullptr);
    s.failedCandidates[L"tls://bad:1"] = now;
    PeerTrack t; t.up = true; t.uptime = 50; t.at = now; t.flaps = { now - 10, now - 5 };
    s.peerTracks[L"tls://a:1"] = t;
    s.configError = L"строка 3: лишняя }";
    s.devicePeers[L"Kjele"] = { L"tls://ygg-msk-1.averyan.ru:8362", L"tls://[2001:db8::2]:443" };
    s.deviceTempPeers[L"Kjele"] = { L"tls://ygg-msk-1.averyan.ru:8362" };
    s.deviceWhy[L"Kjele"] = L"сеть не находит путь";
    SaveState(s);
    WatchdogState l = LoadState();
    Check(l.backupPeers == s.backupPeers, L"резервные пиры (IPv6, ?key=)");
    Check(l.regionalCountry == L"russia" && l.regionalCity == L"Moscow" && l.homeRegion == L"finland", L"регионы");
    Check(l.devices.size() == 1 && l.devices[0].host == L"201:831c::1" && l.deviceDownSince[L"Kjele"] == 12345, L"устройства");
    Check(l.outageNotified && l.failedCandidates.count(L"tls://bad:1"), L"флаги и неудачные кандидаты");
    Check(l.peerTracks.count(L"tls://a:1") && l.peerTracks[L"tls://a:1"].flaps.size() == 2, L"история обрывов");
    Eq(l.configError, s.configError, L"текст ошибки конфига");
    Check(l.devicePeers[L"Kjele"] == s.devicePeers[L"Kjele"] && l.deviceTempPeers[L"Kjele"].size() == 1 &&
          l.deviceWhy[L"Kjele"] == s.deviceWhy[L"Kjele"], L"общие и временные пиры устройства, причина");

    // старый формат: регион-режим и региональные пиры -> обычные резервные
    util::WriteFileAtomic(StatePath(), "mode=regional\r\nregional_peers=tls://x:1;tls://y:2\r\nbackup_peers=tls://x:1\r\n");
    l = LoadState();
    Check(l.backupPeers.size() == 2 && l.backupPeers[1] == L"tls://y:2", L"переход со старого формата");
    DeleteFileW(StatePath().c_str());
}

static void TestReliability() {
    Section(L"надёжность пиров");
    PeerStats ps;
    long long today = 20000;
    ps[{ today, L"tls://half:1" }] = { 20, 10, 0, 0, 0 };
    ps[{ today, L"tls://few:1" }] = { 3, 3, 0, 1, 1 };
    ps[{ today, L"tls://old:1" }] = { 10, 10, 0, 0, 0 };
    ps[{ today - 7, L"tls://old:1" }] = { 10, 0, 2, 0, 0 };
    PeerStats back = ParsePeerStats(FormatPeerStats(ps));
    Check(back.size() == ps.size(), L"запись и чтение peer_stats.txt");
    auto rel = ComputeReliability(back, today);
    Near(rel[L"tls://half:1"].score, 0.5, L"половина рабочих");
    Check(rel[L"tls://few:1"].score < 0, L"мало наблюдений - неизвестно");
    Near(rel[L"tls://old:1"].score, 10.0 / 15.0, L"неделя назад весит вдвое меньше");
    Check(rel[L"tls://old:1"].flaps == 2, L"обрывы суммируются");

    std::map<std::wstring, Reliability> r2;
    r2[L"a"].score = 0.95; r2[L"c"].score = 0.2;
    std::vector<std::wstring> order{ L"c", L"b", L"a", L"d" };
    OrderByReliability<std::wstring>(order, r2, [](const std::wstring& s) { return s; });
    Check((order == std::vector<std::wstring>{ L"a", L"b", L"d", L"c" }), L"надёжные вперёд, ненадёжные в конец, порядок остальных сохранён");
}

static void TestHistory() {
    Section(L"история и агрегаты задержек");
    std::map<std::wstring, std::vector<HistSample>> h;
    ParseHistoryLine(L"1000\tok\t1\ttls://a:1?key=x=y=U|12.5;tcp://[::1]:2=D|-1\r", h);
    Check(h.count(L"tls://a:1?key=x=y") && h[L"tls://a:1?key=x=y"][0].up && std::fabs(h[L"tls://a:1?key=x=y"][0].ms - 12.5) < 0.01,
          L"uri с '=' внутри");
    Check(h.count(L"tcp://[::1]:2") && !h[L"tcp://[::1]:2"][0].up, L"мёртвый пир");
    std::vector<HistSample> v;
    for (int i = 1; i <= 100; ++i) v.push_back({ 1000 + i, true, (double)i });
    v.push_back({ 2000, false, -1 });
    LatencyAgg a = Aggregate(v, 0);
    Check(a.n == 101 && a.up == 100, L"число проверок и рабочих");
    Near(a.avg, 50.5, L"средняя");
    Near(a.p95, 95, L"P95");
    Near(a.jitter, 28.866, L"разброс (стандартное отклонение)");
    Check(Aggregate(v, 5000).n == 0, L"пустое окно");
}

static void TestExeAndTask() {
    Section(L"exe и задача");
    Check(LooksLikeExe(util::SelfExe()), L"сам tests.exe - целый exe");
    std::wstring tmp = GetExeDir() + L"_test_fake.exe";
    util::WriteFileAtomic(tmp, std::string("MZ") + std::string(100, '\0'));
    Check(!LooksLikeExe(tmp), L"обрезанный файл с MZ - не exe");
    DeleteFileW(tmp.c_str());
    Eq(ygg::TaskTargetExe(L"C:\\a b\\ygg_guard.exe", L""), L"C:\\a b\\ygg_watchdog.exe", L"задача со стражем -> exe рядом");
    Eq(ygg::TaskTargetExe(L"C:\\x\\ygg_watchdog.exe", L"tick"), L"C:\\x\\ygg_watchdog.exe", L"задача напрямую");
    Eq(CheckExeIntegrity(GetExeDir() + L"_нет_такого.exe", false), L"файла нет", L"проверка целостности: нет файла");
    Check(CheckExeIntegrity(util::SelfExe(), false).empty(), L"проверка целостности: целый exe");
    // git-отпечаток: "hello\n" -> известное значение (git hash-object)
    Eq(GitBlobSha1Hex("hello\n"), L"ce013625030ba8dba906f756967f9e9ca394464a", L"git-отпечаток файла");
    Eq(ygg::GitBlobSha1("hello\n"), L"ce013625030ba8dba906f756967f9e9ca394464a", L"git-отпечаток (общий core.h)");
    Eq(ygg::ExeSelfTestVersion(GetExeDir() + L"_нет_такого.exe", L"selftest"), L"", L"самопроверка: нет файла - нет версии");
}

static void TestRoutes() {
    Section(L"маршруты до Telegram / GitHub");
    HttpRoute r{ L"proxy", L"127.0.0.1:3067", L"Karing (127.0.0.1:3067)" }, back;
    Check(RouteFromText(RouteToText(r), back) && back.kind == r.kind && back.proxy == r.proxy && back.label == r.label,
          L"запомненный маршрут читается обратно");
    Check(RouteFromText(L"direct||напрямую", back) && back.kind == L"direct", L"маршрут «напрямую»");
    Check(!RouteFromText(L"proxy||без адреса", back), L"прокси без адреса - не маршрут");
    Check(!RouteFromText(L"мусор", back), L"мусор - не маршрут");
    auto names = ProxyClientNames();
    Check(std::find(names.begin(), names.end(), L"karing") != names.end(), L"Karing во встроенном списке клиентов");

    HttpResult t;
    t.transportOk = true; t.status = 200; t.body = "{\"ok\":true,\"result\":{}}";
    Check(ReachedSite(L"api.telegram.org", t), L"ответ Telegram засчитан");
    t.status = 401; t.body = "{\"ok\":false,\"error_code\":401}";
    Check(ReachedSite(L"api.telegram.org", t), L"ошибка от самого Telegram - тоже до него дошли");
    t.status = 502; t.body = "Bad Gateway";
    Check(!ReachedSite(L"api.telegram.org", t), L"502 от прокси без выхода - не дошли");
    t.status = 200; t.body = "<html>заглушка</html>";
    Check(!ReachedSite(L"api.telegram.org", t), L"страница не от Telegram - не дошли");
    t.transportOk = false;
    Check(!ReachedSite(L"raw.githubusercontent.com", t), L"нет соединения - не дошли");
}

// notify.conf во временной папке tests\ (GetExeDir тестов), после проверки удаляется
static NotifyConfig ConfFrom(const std::string& text) {
    std::wstring p = GetExeDir() + L"notify.conf";
    { std::ofstream f(std::filesystem::path(p), std::ios::binary | std::ios::trunc); f << text; }
    NotifyConfig c = LoadNotifyConfig();
    DeleteFileW(p.c_str());
    return c;
}

static void TestNotifyConf() {
    Section(L"notify.conf");
    if (GetFileAttributesW((GetExeDir() + L"notify.conf").c_str()) != INVALID_FILE_ATTRIBUTES) {
        WriteOut(L"  пропущено: в папке тестов лежит свой notify.conf\r\n");
        return;
    }
    NotifyConfig c = ConfFrom("\xEF\xBB\xBF" "BOT_TOKEN = 123456:AAH-x_y \r\nCHAT_ID=\"987654321\"\r\nPROXY=\r\nPROXY_CLIENTS=\r\n");
    Check(c.valid && c.problem.empty(), L"BOM, пробелы и кавычки не мешают");
    Eq(c.botToken, L"123456:AAH-x_y", L"токен без пробелов");
    Eq(c.chatId, L"987654321", L"chat_id без кавычек");
    Check(c.proxyOverride.empty() && !c.proxySocks, L"пустой PROXY= - автопоиск");
    c = ConfFrom("BOT_TOKEN=123:abc\nCHAT_ID=-100200\n  PROXY = http://127.0.0.1:2080/\n");
    Check(c.valid && c.problem.empty(), L"группа с минусом");
    Eq(c.proxyOverride, L"127.0.0.1:2080", L"PROXY с http:// и / в конце");
    c = ConfFrom("BOT_TOKEN=123:abc\nCHAT_ID=5\nPROXY=socks5://127.0.0.1:10808\n");
    Check(c.valid && c.problem.empty() && c.proxySocks && c.proxyOverride.empty(), L"SOCKS в PROXY - пропуск, а не отказ");
    c = ConfFrom("BOT_TOKEN=abc def\nCHAT_ID=5\n");
    Check(!c.problem.empty(), L"токен без двоеточия - подсказка");
    c = ConfFrom("BOT_TOKEN=123:abc\nCHAT_ID=моё\n");
    Check(!c.problem.empty(), L"chat_id не число - подсказка");
    c = ConfFrom("BOT_TOKEN=\xC2\xA0" "123:ab\xE2\x80\x8B" "c\xC2\xA0\nCHAT_ID=5\xC2\xA0\n");
    Check(c.problem.empty(), L"неразрывный пробел и невидимый символ вычищаются");
    Eq(c.botToken, L"123:abc", L"токен после чистки");
    c = ConfFrom("BOT_TOKEN=123\xEF\xBC\x9A" "abc\nCHAT_ID=5\n");
    Eq(c.botToken, L"123:abc", L"полноширинное двоеточие исправляется");
    c = ConfFrom("BOT_TOKEN=123%3Aabc\nCHAT_ID=5\n");
    Eq(c.botToken, L"123:abc", L"%3A из ссылки исправляется");
    c = ConfFrom("BOT_TOKEN=987654321\nCHAT_ID=5\n");
    Check(c.problem.find(L"длина 9") != std::wstring::npos, L"без двоеточия - приметы токена в подсказке");
    c = ConfFrom("BOT_TOKEN=bot123:abc\nCHAT_ID=5\n");
    Eq(c.botToken, L"123:abc", L"bot из ссылки убирается");
    c = ConfFrom("BOT_TOKEN=123:ab\xC3\xA9" "c\nCHAT_ID=5\n");
    Check(c.problem.find(L"на месте 7") != std::wstring::npos, L"лишний символ назван с местом");
    c = ConfFrom("# BOT_TOKEN=123:abc\nCHAT_ID=5\n");
    Check(!c.valid, L"закомментированный токен - не настроен");
}

static void TestJson() {
    Section(L"json_lite");
    json::Value v;
    Check(json::Parse("{\"a\":[1,true,\"\\u043f\"],\"b\":{\"c\":null}}", v), L"разбор");
    Check(v[L"a"].size() == 3 && v[L"a"][1].boolean() && v[L"a"][2].str() == L"п" && v[L"b"][L"c"].isNull(), L"значения");
    json::Value w;
    Check(json::Parse(json::Dump(v, true), w) && w[L"a"][2].str() == L"п", L"запись и обратное чтение");
    Check(!json::Parse("2026/10/21 10:00:00 log line", v), L"строка лога - не JSON");
}

int wmain() {
    WriteOut(L"Тесты ygg-watchdog " + CURRENT_VERSION + L"\r\n");
    TestUris();
    TestVersions();
    TestPeersJson();
    TestClassify();
    TestFlaps();
    TestDiagnose();
    TestState();
    TestReliability();
    TestHistory();
    TestExeAndTask();
    TestJson();
    TestRoutes();
    TestNotifyConf();
    WriteOut(L"\r\nПройдено: " + std::to_wstring(g_pass) + L", провалено: " + std::to_wstring(g_fail) + L"\r\n");
    return g_fail == 0 ? 0 : 1;
}
