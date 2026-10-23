// app.cpp - окно сторожа в терминале (по макетам): главное меню, пиры,
// статистика, разбор, каталог, регион, устройства, журнал, служба, управление.
// Подключается после core.cpp и cmds.cpp.
#include "../common/tui.h"

namespace app {
using namespace tui;
using P = tui::Canvas::Seg;
const Color G = pal::GREEN, GF = pal::GFRAME, GH = pal::GHL, TX = pal::TXT, DM = pal::DIM, DM2 = pal::DIM2,
            WH = pal::WHITE, OR = pal::ORANGE, RD = pal::RED;

// ------------------------------------------------------------------ фоновые задания
// Долгие действия запускают эту же программу с командой (как окно Windows-версии):
// экран не замирает, а итог показывается строкой внизу.
struct Job {
    std::string label;
    std::vector<std::string> args;
    std::atomic<bool> done{ false };
    util::ProcResult res;
    std::function<void(Job&)> onDone;
};
std::string CleanLogLine(std::string l) {
    if (!l.empty() && l[0] == '[') { size_t e = l.find("] "); if (e != std::string::npos) l = l.substr(e + 2); }
    for (const char* p : { "INFO: ", "ERROR: ", "DEBUG: " }) if (util::StartsWith(l, p)) l = l.substr(strlen(p));
    return util::Trim(l);
}
std::string LastMeaningful(const std::string& out) {
    auto lines = util::Lines(out);
    for (size_t i = lines.size(); i-- > 0;) {
        std::string l = CleanLogLine(lines[i]);
        if (!l.empty() && !util::StartsWith(l, "===")) return l;
    }
    return "";
}

// ------------------------------------------------------------------ снимок состояния (обновляется в фоне)
struct Snapshot {
    json::Value st;                    // status-json
    std::vector<ygg::PeerDetail> peers;
    bool peersOk = false;
    std::string peersErr;
    ygg::ServiceInfo svc;
    std::vector<std::string> events;   // последние строки events.log
    long long at = 0;
};

class App;
struct Screen {
    virtual ~Screen() {}
    virtual void Draw(Canvas& c, App& a) = 0;
    virtual bool Key(const tui::Key& k, App& a) = 0;   // true - клавиша обработана
    virtual void OnShow(App&) {}
};

// Модальный диалог: поля ввода или подтверждение
struct Modal {
    bool open = false;
    std::string title, text;
    std::vector<std::pair<std::string, LineEdit>> fields;   // подпись, ввод
    int focus = 0;
    bool danger = false;
    std::string okLabel = "Enter - да";
    std::function<void(const std::vector<std::string>&)> onOk;
};

class App {
public:
    Terminal term;
    std::mutex mu;
    Snapshot snap;
    std::vector<std::unique_ptr<Screen>> stack;
    std::vector<std::shared_ptr<Job>> jobs;
    Modal modal;
    std::string flash;
    Color flashColor = TX;
    long long flashAt = 0;
    std::atomic<bool> quit{ false };
    std::atomic<bool> refreshNow{ false };
    int spin = 0;

    void Flash(const std::string& s, Color c = TX) { flash = s; flashColor = c; flashAt = util::Now(); }
    void Push(Screen* s) { stack.emplace_back(s); s->OnShow(*this); }
    void Pop() { if (stack.size() > 1) stack.pop_back(); if (!stack.empty()) stack.back()->OnShow(*this); }
    Snapshot Snap() { std::lock_guard<std::mutex> lk(mu); return snap; }

    bool Busy(const std::string& label = "") {
        for (auto& j : jobs) if (!j->done && (label.empty() || j->label == label)) return true;
        return false;
    }
    // Запустить «ygg-watchdog <args>» в фоне
    void Run(const std::string& label, std::vector<std::string> args, int timeoutMs = 300000,
             std::function<void(Job&)> onDone = nullptr) {
        if (Busy(label)) { Flash("уже идёт: " + label, OR); return; }
        auto j = std::make_shared<Job>();
        j->label = label;
        j->onDone = onDone;
        args.insert(args.begin(), util::SelfExe());
        j->args = args;
        jobs.push_back(j);
        std::thread([j, timeoutMs] { j->res = util::Run(j->args, timeoutMs); j->done = true; }).detach();
    }
    // Функция в фоне (не процесс): для списков стран и т.п.
    void RunFn(const std::string& label, std::function<void()> fn) {
        auto j = std::make_shared<Job>();
        j->label = label;
        jobs.push_back(j);
        std::thread([j, fn] { fn(); j->done = true; }).detach();
    }
    void Confirm(const std::string& title, const std::string& text, std::function<void()> ok, bool danger = false) {
        modal = Modal();
        modal.open = true; modal.title = title; modal.text = text; modal.danger = danger;
        modal.onOk = [ok](const std::vector<std::string>&) { ok(); };
    }
    void Ask(const std::string& title, const std::string& text, std::vector<std::pair<std::string, std::string>> fields,
             std::function<void(const std::vector<std::string>&)> ok) {
        modal = Modal();
        modal.open = true; modal.title = title; modal.text = text;
        for (auto& f : fields) { LineEdit e; e.text = f.second; modal.fields.push_back({ f.first, e }); }
        modal.okLabel = "Enter - готово";
        modal.onOk = ok;
    }

    void Poll() {   // обновление снимка в фоне
        long long last = 0;
        while (!quit) {
            if (refreshNow.exchange(false) || util::Now() - last >= 5) {
                Snapshot s;
                json::Parse(StatusJson(), s.st);
                s.peers = ygg::GetPeers(&s.peersOk, &s.peersErr);
                WatchdogState ws = LoadState();
                MarkUnstable(s.peers, ws);
                s.svc = ygg::QueryService();
                auto lines = util::Lines(util::ReadFileTail(EventsPath(), 64 * 1024));
                size_t from = lines.size() > 300 ? lines.size() - 300 : 0;
                s.events.assign(lines.begin() + (long)from, lines.end());
                s.at = util::Now();
                { std::lock_guard<std::mutex> lk(mu); snap = std::move(s); }
                last = util::Now();
            }
            util::SleepMs(200);
        }
    }

    void DrawModal(Canvas& c) {
        int w = std::min(c.w - 6, 72);
        int lines = 0;
        std::vector<std::string> wrapped;
        for (const auto& para : util::Split(modal.text, '\n')) {
            std::string cur;
            for (const auto& word : util::Split(para, ' ')) {
                if (util::U8Len(cur) + util::U8Len(word) + 1 > (size_t)(w - 4)) { wrapped.push_back(cur); cur.clear(); }
                cur += (cur.empty() ? "" : " ") + word;
            }
            wrapped.push_back(cur);
        }
        lines = (int)wrapped.size();
        int h = 4 + lines + (int)modal.fields.size() * 2 + (modal.fields.empty() ? 0 : 1);
        int x = (c.w - w) / 2, y = std::max(1, (c.h - h) / 2);
        c.FillRect(x, y, x + w, y + h, Hex("#11161d"));
        Color fr = modal.danger ? OR : G;
        c.Box(x, y, w, h, modal.title, fr, fr);
        for (int i = 0; i < lines; ++i) c.Put(x + 2, y + 1 + i, wrapped[(size_t)i], TX, false, Color(), x + w - 2);
        int fy = y + 2 + lines;
        for (size_t i = 0; i < modal.fields.size(); ++i) {
            bool f = (int)i == modal.focus;
            c.Put(x + 2, fy, modal.fields[i].first, DM);
            std::string t = modal.fields[i].second.text;
            int maxw = w - 6;
            std::string shown = util::U8Len(t) > (size_t)maxw ? "…" + t.substr(t.size() - std::min(t.size(), (size_t)maxw * 2)) : t;
            shown = util::U8Len(shown) > (size_t)maxw ? util::U8Left(shown, (size_t)maxw) : shown;
            c.FillBg(x + 2, fy + 1, x + w - 2, f ? GH : Hex("#161b22"));
            int ex = c.Put(x + 3, fy + 1, shown, WH, false, Color(), x + w - 2);
            if (f) c.Put(ex, fy + 1, "▌", G);
            fy += 2;
        }
        c.Seq(x + 2, y + h - 1, { { " " + modal.okLabel, fr, true }, { "  Esc - отмена ", DM } });
    }
    bool ModalKey(const tui::Key& k) {
        if (k.k == K::Esc) { modal.open = false; return true; }
        if (k.k == K::Enter) {
            if (!modal.fields.empty() && modal.focus + 1 < (int)modal.fields.size()) { ++modal.focus; return true; }
            std::vector<std::string> vals;
            for (auto& f : modal.fields) vals.push_back(util::Trim(f.second.text));
            auto cb = modal.onOk;
            modal.open = false;
            if (cb) cb(vals);
            return true;
        }
        if (modal.fields.empty()) { if (k.is('y') || k.text == "д" || k.text == "Д") { auto cb = modal.onOk; modal.open = false; if (cb) cb({}); } return true; }
        if (k.k == K::Tab || k.k == K::Down) { modal.focus = (modal.focus + 1) % (int)modal.fields.size(); return true; }
        if (k.k == K::BackTab || k.k == K::Up) { modal.focus = (modal.focus + (int)modal.fields.size() - 1) % (int)modal.fields.size(); return true; }
        modal.fields[(size_t)modal.focus].second.Handle(k);
        return true;
    }

    void FinishJobs() {
        for (auto it = jobs.begin(); it != jobs.end();) {
            Job& j = **it;
            if (!j.done) { ++it; continue; }
            if (j.onDone) j.onDone(j);
            else if (!j.args.empty()) {
                std::string m = LastMeaningful(j.res.out);
                bool err = j.res.code != 0 || util::IContains(j.res.out, "ERROR:") || util::IContains(j.res.out, "Ошибка");
                Flash((m.empty() ? j.label + ": готово" : m), err ? OR : G);
            }
            refreshNow = true;
            it = jobs.erase(it);
        }
    }

    int Main();
};

// ------------------------------------------------------------------ общие элементы
void Header(Canvas& c, const std::string& title, const std::string& sub) {
    c.Seq(3, 1, { { "◢ ", G, true }, { title, G, true }, { "   " + sub, DM } });
}
void Footer(Canvas& c, App& a, const std::vector<std::pair<std::string, std::string>> items) {
    int y = c.h - 1;
    if (!a.flash.empty() && util::Now() - a.flashAt < 8) {
        c.Put(3, y, util::U8Fit(a.flash, (size_t)std::max(10, c.w - 6)), a.flashColor);
        return;
    }
    int x = 3;
    for (const auto& [k, v] : items) {
        if (x + (int)util::U8Len(k) + (int)util::U8Len(v) + 2 > c.w - 2) break;
        x = c.Put(x, y, k, G, true) + 1;
        x = c.Put(x, y, v, DM) + 3;
    }
    // индикатор фоновых заданий - справа
    std::string busy;
    for (auto& j : a.jobs) if (!j->done) { busy = j->label; break; }
    if (!busy.empty()) {
        static const char* sp[] = { "◐", "◓", "◑", "◒" };
        std::string t = std::string(sp[(a.spin / 3) % 4]) + " " + busy + "…";
        c.Put(std::max(x + 2, c.w - 2 - (int)util::U8Len(t)), y, t, OR);
    }
}
// Полоска задержки: чем быстрее пир, тем длиннее
Color RttColor(double ms) { return ms < 0 ? DM2 : ms < 120 ? G : ms < 250 ? OR : RD; }
void RttBar(Canvas& c, int x, int y, int w, double ms) {
    if (ms < 0) { c.Bar(x, y, w, 0, DM2); return; }
    c.Bar(x, y, w, std::max(0.08, 1.0 - std::min(ms, 400.0) / 400.0), RttColor(ms));
}
std::string RegionText(const std::string& slug, const std::string& city) {
    if (slug.empty()) return "—";
    std::string t = SlugToRu(slug);
    if (!city.empty()) {
        auto c = FindCity(LoadCityDatabase(), SlugToIso(slug), city);
        t += "/" + (c ? c->displayName() : city);
    }
    return t;
}
// Пословный перенос текста в ширину
std::vector<std::string> Wrap(const std::string& text, int w) {
    std::vector<std::string> out;
    for (const auto& para : util::SplitKeep(text, '\n')) {
        std::string cur;
        for (const auto& word : util::Split(para, ' ')) {
            if (!cur.empty() && (int)(util::U8Len(cur) + util::U8Len(word) + 1) > w) { out.push_back(cur); cur.clear(); }
            cur += (cur.empty() ? "" : " ") + word;
        }
        out.push_back(cur);
    }
    return out;
}
std::string EventIcon(const std::string& lvl) { return lvl == "ok" ? "●" : lvl == "warn" ? "◐" : lvl == "alert" ? "▲" : "·"; }
Color EventColor(const std::string& lvl) { return lvl == "ok" ? G : lvl == "warn" ? OR : lvl == "alert" ? RD : DM; }

// ------------------------------------------------------------------ экраны (объявления)
Screen* MakePeers(); Screen* MakeStats(); Screen* MakeDiag(); Screen* MakeCatalog(); Screen* MakeRegion();
Screen* MakeDevices(); Screen* MakeJournal(); Screen* MakeService(); Screen* MakeManage();

// ------------------------------------------------------------------ главное меню
struct MainScreen : Screen {
    int sel = 1;
    struct Item { const char* key; const char* name; const char* hint; Screen* (*make)(); };
    std::vector<Item> items = {
        { "1", "Проверить сейчас", "", nullptr },
        { "2", "Пиры", "основные, резервные, задержки", MakePeers },
        { "3", "Статистика", "надёжность и задержки", MakeStats },
        { "4", "Почему нет связи", "разбор ошибок пиров", MakeDiag },
        { "5", "Каталог публичных пиров", "проверить и добавить", MakeCatalog },
        { "6", "Регион", "поездка и домашний", MakeRegion },
        { "7", "Устройства", "мониторинг и диагностика", MakeDevices },
        { "8", "Журнал событий", "", MakeJournal },
        { "9", "Служба Yggdrasil", "перезапуск сейчас / с задержкой", MakeService },
        { "0", "Управление", "таймер, обновления, Telegram, VPN", MakeManage },
    };
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        const json::Value& st = s.st;
        int W = c.w - 4;
        c.Put(4, 1, "╲╲ ▗▄▄▄▖", G); c.Put(4, 2, "╱╱ ▝▀█▀▘", G);
        c.Put(14, 1, "YGG WATCHDOG", G, true); c.Put(28, 1, std::string("v") + CURRENT_VERSION, DM);
        c.Put(14, 2, "сторож пиров Yggdrasil", DM);
        if (c.w >= 78) {
            int rx = c.w - 32;
            c.Seq(rx, 1, { { "устройство ", DM }, { util::HostName(), TX } });
            const json::Value& tl = st["telegram_last"];
            if (!st["telegram_configured"].boolean()) c.Seq(rx, 2, { { "Telegram ", DM }, { "не настроен", DM } });
            else if (tl["at"].i64() == 0) c.Seq(rx, 2, { { "Telegram ", DM }, { "ещё не отправлял", DM } });
            else c.Seq(rx, 2, { { "Telegram ", DM }, { (tl["ok"].boolean() ? "✓ " : "✗ ") + util::TimeText(tl["at"].i64(), "%H:%M") +
                                  (tl["ok"].boolean() ? " " + util::U8Fit(tl["via"].str(), 14) : " не дошло"), tl["ok"].boolean() ? G : RD } });
        }
        // ---- состояние
        int y = 4;
        c.Box(2, y, W, 6, "состояние", GF, G);
        int alive = 0, total = 0;
        for (const auto& p : s.peers) { ++total; if (p.up && !p.unstable) ++alive; }
        int mainAlive = 0, mainN = (int)st["main_peers"].size();
        for (size_t i = 0; i < st["main_peers"].size(); ++i)
            if (const ygg::PeerDetail* d = ygg::FindPeer(s.peers, st["main_peers"][i].str())) if (d->up && !d->unstable) ++mainAlive;
        bool netOk = st["internet_down_since"].i64() == 0;
        long long next = st["timer"]["next"].i64(), last = st["last_tick_at"].i64(), now = util::Now();
        std::vector<P> l1;
        if (!s.svc.running()) l1 = { { "● ", RD }, { "служба Yggdrasil ", DM }, { s.svc.exists ? "остановлена" : "не найдена", RD } };
        else l1 = { { "● ", netOk ? G : RD }, { "сеть ", DM }, { netOk ? "жива" : "нет интернета", netOk ? TX : RD } };
        Color pc = alive >= 3 ? G : alive > 0 ? OR : RD;
        l1.push_back({ "   ● ", pc }); l1.push_back({ "пиры ", DM }); l1.push_back({ std::to_string(alive) + " живых", TX });
        if (c.w >= 86) l1.push_back({ " (" + std::to_string(mainAlive) + " осн. из " + std::to_string(mainN) + ")", DM });
        if (st["tick_running"].boolean()) { l1.push_back({ "   ◐ ", OR }); l1.push_back({ "идёт проверка", OR }); }
        else if (!st["timer"]["active"].boolean()) { l1.push_back({ "   ◷ ", RD }); l1.push_back({ "автопроверки выкл.", RD }); }
        else if (next > now) { l1.push_back({ "   ◷ ", OR }); l1.push_back({ "проверка через ", DM }); l1.push_back({ util::MinSec(next - now), TX }); }
        c.Seq(4, y + 1, l1, 2 + W - 1);
        std::string phase = st["last_phase"].str();
        std::string ph = phase == "ok" ? "обычный" : phase == "backup" ? "на резервных" : phase == "down" ? "пиров нет" : phase == "crisis" ? "крайний перебор" :
                         phase == "no_internet" ? "нет интернета" : phase == "battery" ? "батарея" : phase == "service_down" ? "служба стоит" : phase.empty() ? "—" : "ошибка";
        Color phc = phase == "ok" ? G : phase == "backup" ? OR : phase.empty() ? DM : RD;
        c.Seq(4, y + 2, { { "режим ", DM }, { ph, phc }, { "   выход трафика ", DM }, { RegionText(st["egress_region"].str(), ""), TX },
                          { "   поездка ", DM }, { RegionText(st["regional_country"].str(), st["regional_city"].str()), TX },
                          { "   дом ", DM }, { RegionText(st["home_region"].str(), st["home_city"].str()), TX } }, 2 + W - 1);
        std::vector<P> l3 = { { "устройства ", DM } };
        if (st["devices"].size() == 0) l3.push_back({ "не добавлены", DM });
        for (size_t i = 0; i < st["devices"].size() && i < 4; ++i) {
            const json::Value& d = st["devices"][i];
            long long ds = d["down_since"].i64();
            if (ds) { l3.push_back({ "◌ " + d["name"].str() + " ", OR }); l3.push_back({ "нет ответа " + util::Ago(now - ds) + "   ", OR }); }
            else l3.push_back({ "● " + d["name"].str() + "   ", G });
        }
        c.Seq(4, y + 3, l3, 2 + W - 1);
        // последнее событие или тревога
        if (st["config_problem"].boolean()) c.Seq(4, y + 4, { { "▲ ", RD }, { "yggdrasil.conf не читается: " + st["config_error"].str(), RD } }, 2 + W - 1);
        else if (!st["tun_problem"].str().empty()) c.Seq(4, y + 4, { { "▲ ", RD }, { st["tun_problem"].str(), RD } }, 2 + W - 1);
        else if (last && st["timer"]["active"].boolean() && now - last > 15 * 60 && !st["tick_running"].boolean())
            c.Seq(4, y + 4, { { "▲ ", RD }, { "автопроверки молчат " + util::Ago(now - last) + " - открой «Управление» [0]", RD } }, 2 + W - 1);
        else if (!s.events.empty()) {
            auto f = util::SplitKeep(s.events.back(), '\t');
            if (f.size() >= 3) c.Seq(4, y + 4, { { "последнее событие ", DM }, { util::TimeText(util::ToLL(f[0]), "%H:%M") + " ", DM }, { f[2], EventColor(f[1]) } }, 2 + W - 1);
        }
        // ---- меню
        int my = y + 7;
        int mh = std::min((int)items.size() + 2, c.h - my - 1);
        c.Box(2, my, W, mh, "меню", GF, G);
        for (int i = 0; i < (int)items.size() && i < mh - 2; ++i) {
            int r = my + 1 + i;
            bool on = i == sel;
            if (on) { c.FillBg(3, r, 2 + W - 1, GH); c.Put(4, r, "▸", G); }
            c.Put(6, r, std::string("[") + items[(size_t)i].key + "]", G, on);
            Color nc = on ? WH : (i == 8 ? OR : TX);
            if (i == 0 && a.Busy("проверка")) c.Put(10, r, "Проверка идёт…", OR, on);
            else c.Put(10, r, items[(size_t)i].name, nc, on);
            if (c.w >= 70) c.Put(40, r, items[(size_t)i].hint, DM, false, Color(), 2 + W - 1);
        }
        Footer(c, a, { { "↑↓", "выбор" }, { "Enter", "открыть" }, { "1-0", "быстрый пункт" }, { "t", "проверить" }, { "q", "выход" } });
    }
    void Open(int i, App& a) {
        if (i == 0) { a.Run("проверка", { "tick" }, 600000); a.Flash("Проверка запущена - обычно до пары минут.", G); return; }
        a.Push(items[(size_t)i].make());
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.k == K::Up) { sel = (sel + (int)items.size() - 1) % (int)items.size(); return true; }
        if (k.k == K::Down) { sel = (sel + 1) % (int)items.size(); return true; }
        if (k.k == K::Enter) { Open(sel, a); return true; }
        if (k.k == K::Char && k.text.size() == 1 && k.text[0] >= '0' && k.text[0] <= '9') {
            int i = k.text[0] == '0' ? 9 : k.text[0] - '1';
            sel = i;
            Open(i, a);
            return true;
        }
        if (k.is('t') || k.text == "е") { Open(0, a); return true; }
        if (k.is('q') || k.text == "й" || k.k == K::Esc) { a.quit = true; return true; }
        return false;
    }
};

// ------------------------------------------------------------------ пиры
struct PeersScreen : Screen {
    ListView lv;
    struct Row { std::string uri, role; const ygg::PeerDetail* d = nullptr; };
    std::vector<ygg::PeerDetail> keep;
    std::vector<Row> Rows(const Snapshot& s) {
        std::vector<Row> r;
        keep = s.peers;
        std::set<std::string> seen;
        auto add = [&](const std::string& uri, const std::string& role) {
            if (seen.count(util::Lower(uri))) return;
            seen.insert(util::Lower(uri));
            r.push_back({ uri, role, ygg::FindPeer(keep, uri) });
        };
        for (size_t i = 0; i < s.st["main_peers"].size(); ++i) add(s.st["main_peers"][i].str(), "основной");
        for (size_t i = 0; i < s.st["backup_peers"].size(); ++i) add(s.st["backup_peers"][i].str(), "резерв");
        for (const auto& p : keep) add(p.uri, p.inbound ? "входящий" : "добавлен");
        return r;
    }
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        auto rows = Rows(s);
        int alive = 0;
        for (const auto& r : rows) if (r.d && r.d->up && !r.d->unstable) ++alive;
        Header(c, "ПИРЫ", std::to_string(alive) + " живых из " + std::to_string(rows.size()) + " · держу 3-7 · обновляется каждые 5 с");
        int W = c.w - 4;
        int listH = std::max(3, std::min((int)rows.size(), c.h - 13));
        c.Box(2, 3, W, listH + 3, std::to_string(rows.size()) + " пиров", GF, G);
        int uriW = std::max(24, W - 52);
        c.Put(5, 4, "пир", DM); c.Put(7 + uriW, 4, "роль", DM); c.Put(18 + uriW, 4, "задержка", DM);
        if (c.w >= 90) c.Put(W - 12, 4, "на связи", DM);
        lv.Clamp((int)rows.size(), listH);
        for (int i = 0; i < listH && lv.top + i < (int)rows.size(); ++i) {
            const Row& r = rows[(size_t)(lv.top + i)];
            int y = 5 + i;
            bool on = lv.top + i == lv.sel;
            if (on) { c.FillBg(3, y, 2 + W - 1, GH); c.Put(3, y, "▸", G); }
            bool up = r.d && r.d->up;
            std::string dot = !r.d ? "○" : r.d->unstable ? "◌" : up ? "●" : "○";
            Color dc = !r.d ? DM2 : r.d->unstable ? OR : up ? G : RD;
            c.Put(5, y, dot, dc);
            c.Put(7, y, util::U8Fit(r.uri, (size_t)uriW - 1), on ? WH : TX, on);
            c.Put(7 + uriW, y, r.role, r.role == "резерв" ? OR : DM);
            double ms = r.d ? r.d->rttMs : -1;
            RttBar(c, 18 + uriW, y, 10, up ? ms : -1);
            c.Put(29 + uriW, y, up && ms >= 0 ? util::Fixed(ms, 0) + " мс" : up ? "—" : "не работает", up ? RttColor(ms) : RD);
            if (c.w >= 90 && up && r.d->uptimeSec >= 0) c.Put(W - 12, y, util::Ago((long long)r.d->uptimeSec), DM);
        }
        // подробности о выбранном
        int dy = 5 + listH + 2;
        if (lv.sel < (int)rows.size()) {
            const Row& r = rows[(size_t)lv.sel];
            std::string info;
            Color ic = DM;
            if (!r.d) info = "Yggdrasil не знает этот пир - служба ещё не перечитала конфиг (нужен перезапуск).";
            else if (r.d->unstable) { info = "Подключается и снова рвётся: " + std::to_string(r.d->flaps) + " раз за полчаса - живым не считается."; ic = OR; }
            else if (!r.d->up) { PeerDiag pd; pd.error = r.d->lastError; ClassifyPeerError(pd); info = "Не работает: " + pd.text + (r.d->lastError.empty() ? "" : "  (" + r.d->lastError + ")"); ic = RD; }
            else {
                info = "Работает" + (r.d->rttMs >= 0 ? ", задержка " + util::Fixed(r.d->rttMs, 0) + " мс" : std::string()) +
                       (r.d->rxBytes >= 0 ? ", принято " + util::FixedRu(r.d->rxBytes / 1048576.0, 1) + " МБ, отправлено " + util::FixedRu(r.d->txBytes / 1048576.0, 1) + " МБ" : "");
                ic = G;
            }
            int line = 0;
            for (const auto& l : Wrap(info, W - 4)) { if (line < 2) c.Put(4, dy + line, l, ic); ++line; }
        }
        Footer(c, a, { { "a", "добавить основной" }, { "d", "убрать" }, { "v", "проверить пир" }, { "r", "перезапуск через 15 с" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        Snapshot s = a.Snap();
        auto rows = Rows(s);
        if (lv.Handle(k, (int)rows.size(), std::max(3, std::min((int)rows.size(), a.term.canvas.h - 13)))) return true;
        std::string uri = lv.sel < (int)rows.size() ? rows[(size_t)lv.sel].uri : "";
        std::string role = lv.sel < (int)rows.size() ? rows[(size_t)lv.sel].role : "";
        if (k.is('a') || k.text == "ф") {
            a.Ask("Добавить основной пир", "Пир запишется в yggdrasil.conf (с проверкой и копией старого файла). Заработает после перезапуска службы.",
                  { { "адрес пира, например tls://host:443", "" } }, [&a](const std::vector<std::string>& v) {
                      if (!v.empty() && !v[0].empty()) a.Run("добавляю пир", { "add-main-peer", v[0] }, 60000);
                  });
            return true;
        }
        if ((k.is('d') || k.text == "в") && !uri.empty()) {
            if (role == "основной")
                a.Confirm("Убрать основной пир?", uri + "\n\nОн будет удалён из yggdrasil.conf (старый файл сохранится копией).",
                          [&a, uri] { a.Run("убираю пир", { "remove-main-peer", uri }, 60000); }, true);
            else a.Confirm("Отключить пир?", uri + "\n\nПир будет отключён до перезапуска службы (в конфиге его нет).",
                           [&a, uri] { a.Run("отключаю пир", { "disconnect-peer", uri }, 30000); }, true);
            return true;
        }
        if ((k.is('v') || k.text == "м") && !uri.empty()) {
            a.Run("проверяю пир", { "verify-peer", uri }, 60000, [&a, uri](Job& j) {
                json::Value v;
                if (!json::ParseLoose(util::Lines(j.res.out).empty() ? "" : util::Lines(j.res.out).back(), v)) { a.Flash("проверка не удалась", RD); return; }
                if (v["up"].boolean()) a.Flash("✓ " + uri + " поднялся" + (v["ms"].num(-1) >= 0 ? ", " + util::Fixed(v["ms"].num(), 0) + " мс" : ""), G);
                else a.Flash("✗ " + uri + ": " + v["text"].str(), RD);
            });
            return true;
        }
        if (k.is('r') || k.text == "к") {
            a.Confirm("Перезапустить Yggdrasil через 15 секунд?", "Новые основные пиры из конфига подхватятся только после перезапуска. "
                      "Если ты сидишь на этом компьютере по Yggdrasil, соединение на несколько секунд оборвётся.",
                      [&a] { a.Run("перезапуск", { "restart-yggdrasil-delayed", "15" }, 30000); });
            return true;
        }
        return false;
    }
};
Screen* MakePeers() { return new PeersScreen(); }

// ------------------------------------------------------------------ статистика
struct StatsScreen : Screen {
    int hours = 24;
    ListView lv;
    json::Value data;
    std::mutex mu;
    long long loadedAt = 0;
    int bins = 64;
    void Load(App& a) {
        int h = hours, b = bins;
        a.RunFn("статистика", [this, h, b] {
            json::Value v;
            json::Parse(StatsJson(h, b), v);
            // основные - сверху, потом резервные, потом остальные (недавно виденные)
            std::stable_sort(v.at("peers").arr.begin(), v.at("peers").arr.end(), [](const json::Value& x, const json::Value& y) {
                auto rank = [](const json::Value& p) { return p["main"].boolean() ? 0 : p["backup"].boolean() ? 1 : 2; };
                if (rank(x) != rank(y)) return rank(x) < rank(y);
                return x["last_seen"].i64() > y["last_seen"].i64();
            });
            // давно не виденные (за период) не показываем
            json::Value keep = json::MakeArray();
            for (auto& p : v.at("peers").arr) if (p["span"]["n"].i64() > 0 || p["main"].boolean() || p["backup"].boolean()) keep.arr.push_back(p);
            v.at("peers") = keep;
            std::lock_guard<std::mutex> lk(mu);
            data = v;
        });
        loadedAt = util::Now();
    }
    void OnShow(App& a) override { Load(a); }
    void Draw(Canvas& c, App& a) override {
        if (util::Now() - loadedAt > 60) Load(a);
        int W = c.w - 4;
        int want = std::max(20, W - 12);
        if (want != bins) { bins = want; Load(a); }
        json::Value v;
        { std::lock_guard<std::mutex> lk(mu); v = data; }
        const json::Value& peers = v["peers"];
        Header(c, "СТАТИСТИКА", "за " + std::to_string(hours) + " ч · " + std::to_string(v["checks"].i64()) + " проверок · с " +
               util::DateRu(util::Now() - hours * 3600LL));
        int tableH = std::max(3, std::min((int)peers.size(), c.h - 18));
        c.Box(2, 3, W, tableH + 3, "надёжность пиров", GF, G);
        int uriW = std::max(20, W - 48);
        c.Put(5, 4, "пир", DM); c.Put(7 + uriW, 4, "на связи", DM); c.Put(18 + uriW, 4, "обрывы", DM); c.Put(27 + uriW, 4, "ср / мин / макс", DM);
        lv.Clamp((int)peers.size(), tableH);
        for (int i = 0; i < tableH && lv.top + i < (int)peers.size(); ++i) {
            const json::Value& p = peers[(size_t)(lv.top + i)];
            int y = 5 + i;
            bool on = lv.top + i == lv.sel;
            const json::Value& sp = p["span"];
            double up = sp["up_pct"].num(-1);
            long long fl = p["flaps_span"].i64();
            bool bad = (up >= 0 && up < 95) || fl >= 3;
            Color rc = bad ? OR : TX;
            if (on) { c.FillBg(3, y, 2 + W - 1, GH); c.Put(3, y, "▸", G); }
            c.Put(5, y, p["last_up"].boolean() ? "●" : "◌", p["last_up"].boolean() ? (bad ? OR : G) : RD);
            c.Put(7, y, util::U8Fit(p["uri"].str(), (size_t)uriW - 1), on ? WH : TX, on);
            c.Put(7 + uriW, y, up >= 0 ? util::FixedRu(up, 1) + " %" : "—", rc);
            c.Put(18 + uriW, y, std::to_string(fl), rc);
            auto ms = [](double x) { return x >= 0 ? util::Fixed(x, 0) : std::string("—"); };
            c.Put(27 + uriW, y, ms(sp["avg_ms"].num(-1)) + " / " + ms(sp["min_ms"].num(-1)) + " / " + ms(sp["max_ms"].num(-1)) + " мс", rc, false, Color(), 2 + W - 1);
        }
        if (peers.size() == 0) c.Put(5, 5, a.Busy("статистика") ? "считаю…" : "истории ещё нет - она копится с каждой проверкой", DM);
        // график выбранного
        int gy = 3 + tableH + 4;
        int gh = c.h - gy - 2;
        if (lv.sel < (int)peers.size() && gh >= 6) {
            const json::Value& p = peers[(size_t)lv.sel];
            c.Box(2, gy, W, gh, "задержка: " + p["uri"].str(), GF, G);
            const json::Value& ser = p["series"];
            const json::Value& stt = p["state"];
            double mx = 0, mn = 1e9;
            for (size_t i = 0; i < ser.size(); ++i) { double x = ser[i].num(-1); if (x >= 0) { mx = std::max(mx, x); mn = std::min(mn, x); } }
            int rows = std::min(10, gh - 5);
            if (mx > 0 && rows >= 2) {
                // шкала от чуть ниже минимума до максимума: видно, как задержка «гуляет»
                double lo = std::max(0.0, mn - std::max(1.0, (mx - mn) * 0.25));
                if (mx - lo < 1) mx = lo + 1;
                c.Put(4, gy + 1, util::Fixed(mx, 0), DM);
                c.Put(4, gy + rows, util::Fixed(lo, 0), DM);
                double hot = std::max(mn * 2.0, 150.0);
                for (size_t i = 0; i < ser.size(); ++i) {
                    double x = ser[i].num(-1);
                    if (x < 0) continue;
                    double units = std::max(0.5, (x - lo) / (mx - lo) * rows * 8);
                    Color col = x > hot ? OR : G;
                    for (int r = 0; r < rows; ++r) {
                        double cell = units - (rows - 1 - r) * 8;
                        int xx = 9 + (int)i;
                        if (xx >= 2 + W - 1) break;
                        if (cell >= 8) c.Put(xx, gy + 1 + r, "█", col);
                        else if (cell > 0) c.Put(xx, gy + 1 + r, std::string(&"▁▂▃▄▅▆▇"[std::max(0, (int)cell - 1) * 3], 3), col);
                    }
                }
            } else c.Put(9, gy + 2, "задержка за период не измерялась", DM);
            c.Put(4, gy + rows + 1, "связь", DM);
            for (size_t i = 0; i < stt.size() && 9 + (int)i < 2 + W - 1; ++i) {
                long long s = stt[i].i64(-1);
                c.Put(9 + (int)i, gy + rows + 1, s < 0 ? "·" : "█", s < 0 ? DM2 : s ? G : RD);
            }
            int span = (int)ser.size();
            for (int k = 0; k < 4 && span > 0; ++k) {
                int xx = 9 + k * span / 4;
                c.Put(xx, gy + rows + 2, util::TimeText(util::Now() - hours * 3600LL + (long long)k * hours * 900, "%H:%M"), DM);
            }
            c.Put(9 + span - 6, gy + rows + 2, "сейчас", DM);
            const json::Value& sp = p["span"];
            c.Seq(4, gy + gh - 1, { { " на связи ", DM }, { sp["up_pct"].num(-1) >= 0 ? util::FixedRu(sp["up_pct"].num(), 1) + " %" : "—", TX },
                                    { "   P95 ", DM }, { sp["p95_ms"].num(-1) >= 0 ? util::Fixed(sp["p95_ms"].num(), 0) + " мс" : "—", TX },
                                    { "   разброс ", DM }, { sp["jitter_ms"].num(-1) >= 0 ? util::Fixed(sp["jitter_ms"].num(), 0) + " мс" : "—", TX },
                                    { "   надёжность 30 сут ", DM }, { p["reliability"].num(-1) >= 0 ? util::FixedRu(p["reliability"].num(), 1) + " % " : "мало данных ", TX } });
        }
        Footer(c, a, { { "↑↓", "пир" }, { "Tab", "6 / 24 / 48 ч" }, { "e", "выгрузить CSV" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        json::Value v;
        { std::lock_guard<std::mutex> lk(mu); v = data; }
        if (lv.Handle(k, (int)v["peers"].size(), std::max(3, std::min((int)v["peers"].size(), a.term.canvas.h - 18)))) return true;
        if (k.k == K::Tab) { hours = hours == 6 ? 24 : hours == 24 ? 48 : 6; Load(a); return true; }
        if (k.is('e') || k.text == "у") { a.Run("выгрузка", { "stats-csv" }, 60000); return true; }
        return false;
    }
};
Screen* MakeStats() { return new StatsScreen(); }

// ------------------------------------------------------------------ почему нет связи
struct DiagScreen : Screen {
    std::string fresh;   // свежий разбор (по [r])
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        const json::Value& d = s.st["diagnosis"];
        int W = c.w - 4;
        Header(c, "ПОЧЕМУ НЕТ СВЯЗИ", "разбор ошибок основных пиров");
        int y = 3;
        if (!fresh.empty()) {
            c.Box(2, y, W, c.h - y - 2, "разбор прямо сейчас", GF, G);
            int r = y + 1;
            for (const auto& l : Wrap(fresh, W - 4)) { if (r >= c.h - 3) break; c.Put(4, r++, l, util::StartsWith(l, "- ") ? OR : TX); }
        } else if (d["verdict"].str().empty()) {
            c.Put(4, y + 1, "Разбора ещё нет: он появляется после проверки. Нажми r - разобрать прямо сейчас.", DM);
        } else {
            int bh = (int)d["peers"].size() + 2;
            c.Box(2, y, W, bh, "основные пиры", GF, G);
            for (size_t i = 0; i < d["peers"].size(); ++i) {
                const json::Value& p = d["peers"][i];
                bool up = p["kind"].str() == "up";
                Color col = up ? G : p["kind"].str() == "unstable" || p["kind"].str() == "waiting" ? OR : RD;
                int uriW = std::max(20, W / 2 - 4);
                c.Put(4, y + 1 + (int)i, up ? "✓" : "✗", col, true);
                c.Put(6, y + 1 + (int)i, util::U8Fit(p["uri"].str(), (size_t)uriW), TX);
                c.Put(8 + uriW, y + 1 + (int)i, p["text"].str(), col, false, Color(), 2 + W - 1);
            }
            int r = y + bh + 1;
            for (const auto& l : Wrap(d["verdict"].str(), W - 4)) c.Put(4, r++, l, d["any_up"].boolean() ? G : OR, true);
            r++;
            for (const auto& l : Wrap(d["advice"].str(), W - 4)) c.Put(4, r++, l, TX);
            long long t = s.st["last_tick_at"].i64();
            if (t) c.Put(4, r + 1, "по итогам проверки в " + util::TimeText(t, "%H:%M"), DM);
        }
        Footer(c, a, { { "r", "разобрать сейчас" }, { "t", "проверить и починить" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.is('r') || k.text == "к") {
            a.Run("разбор", { "diagnose" }, 120000, [this, &a](Job& j) { fresh = util::Trim(j.res.out); if (fresh.empty()) a.Flash("разбор не удался", RD); });
            return true;
        }
        if (k.is('t') || k.text == "е") { a.Run("проверка", { "tick" }, 600000); return true; }
        return false;
    }
};
Screen* MakeDiag() { return new DiagScreen(); }

// ------------------------------------------------------------------ выбор страны (общий для региона и каталога)
struct CountryPane {
    std::vector<CountryEntry> all;
    bool loaded = false, loading = false;
    LineEdit filter;
    ListView lv;
    std::mutex mu;
    void Load(App& a) {
        if (loaded || loading) return;
        loading = true;
        a.RunFn("список стран", [this] {
            auto l = GetSortedCountryList();
            std::lock_guard<std::mutex> lk(mu);
            all = l; loaded = true; loading = false;
        });
    }
    std::vector<CountryEntry> Visible() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<CountryEntry> v;
        std::string f = util::U8Lower(filter.text);
        for (const auto& c : all) if (f.empty() || util::U8Lower(c.ru).find(f) != std::string::npos || c.slug.find(f) != std::string::npos) v.push_back(c);
        return v;
    }
    void Draw(Canvas& c, int x, int y, int w, int h, bool focus, const std::string& countLabel) {
        c.Box(x, y, w, h, "страна", focus ? G : GF, G);
        c.Seq(x + 2, y + 1, { { "/ ", G, true }, { filter.text.empty() && !focus ? "поиск…" : filter.text, filter.text.empty() ? DM2 : WH } });
        if (focus) c.Put(x + 4 + (int)util::U8Len(filter.text), y + 1, "▌", G);
        auto v = Visible();
        int rows = h - 4;
        lv.Clamp((int)v.size(), rows);
        if (v.empty()) c.Put(x + 2, y + 3, loaded ? "ничего не нашлось" : "загружаю с GitHub…", DM);
        for (int i = 0; i < rows && lv.top + i < (int)v.size(); ++i) {
            int r = y + 3 + i;
            bool on = lv.top + i == lv.sel;
            if (on) { c.FillBg(x + 1, r, x + w - 1, focus ? GH : Hex("#122a1d")); c.Put(x + 2, r, "▸", G); }
            const auto& e = v[(size_t)(lv.top + i)];
            c.Put(x + 4, r, util::U8Fit(e.ru, (size_t)w - 8), on ? WH : (e.supported ? TX : DM), on);
        }
        if (!countLabel.empty()) c.Put(x + 2, y + h - 1, " " + countLabel + " ", DM);
    }
    std::string Selected() { auto v = Visible(); return lv.sel < (int)v.size() ? v[(size_t)lv.sel].slug : ""; }
    bool Key(const tui::Key& k, int rows) {
        auto v = Visible();
        if (lv.Handle(k, (int)v.size(), rows)) return true;
        if (k.k == K::Char || k.k == K::Backspace) { filter.Handle(k); lv.sel = 0; lv.top = 0; return true; }
        return false;
    }
};

// ------------------------------------------------------------------ регион поездки / домашний
struct RegionScreen : Screen {
    CountryPane cp;
    int focus = 0;   // 0 - страна, 1 - город
    LineEdit cityFilter;
    ListView clv;
    std::string citySlug;
    std::vector<std::pair<City, double>> cities;   // город, расстояние до ближайшего пира (-1 - неизвестно)
    std::mutex mu;
    bool citiesLoading = false;
    void OnShow(App& a) override { cp.Load(a); }
    void LoadCities(App& a, const std::string& slug) {
        if (slug == citySlug) return;
        citySlug = slug;
        clv = ListView(); cityFilter.text.clear();
        { std::lock_guard<std::mutex> lk(mu); cities.clear(); }
        citiesLoading = true;
        a.RunFn("города", [this, slug] {
            std::string iso = SlugToIso(slug);
            auto list = GetCitiesForCountry(LoadCityDatabase(), iso);
            std::vector<std::pair<double, double>> peerPts;
            for (const auto& p : FetchCountryPeers(slug)) if (auto pc = FindCity(LoadCityDatabase(), iso, p.city)) peerPts.push_back({ pc->lat, pc->lon });
            std::vector<std::pair<City, double>> out;
            for (const auto& c : list) {
                double best = -1;
                for (const auto& pt : peerPts) { double d = HaversineKm(c.lat, c.lon, pt.first, pt.second); if (best < 0 || d < best) best = d; }
                out.push_back({ c, best });
            }
            // крупные города вперёд, остальные - по алфавиту
            std::stable_sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.first.population > y.first.population; });
            std::lock_guard<std::mutex> lk(mu);
            if (citySlug == slug) { cities = out; citiesLoading = false; }
        });
    }
    std::vector<std::pair<City, double>> VisibleCities() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<std::pair<City, double>> v;
        std::string f = util::U8Lower(cityFilter.text);
        for (const auto& c : cities)
            if (f.empty() || util::U8Lower(c.first.name).find(f) != std::string::npos || util::U8Lower(c.first.nameRu).find(f) != std::string::npos) v.push_back(c);
        return v;
    }
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        int W = c.w - 4;
        Header(c, "РЕГИОН", "пиры ищутся рядом с выбранным городом");
        c.Seq(4, 2, { { "поездка ", DM }, { RegionText(s.st["regional_country"].str(), s.st["regional_city"].str()), s.st["regional_country"].str().empty() ? DM : OR },
                      { "    дом ", DM }, { RegionText(s.st["home_region"].str(), s.st["home_city"].str()), TX },
                      { "    выход трафика ", DM }, { RegionText(s.st["egress_region"].str(), ""), TX } });
        int lw = std::max(24, W * 2 / 5), h = c.h - 9;
        std::string slug = cp.Selected();
        if (!slug.empty()) LoadCities(a, slug);
        cp.Draw(c, 2, 4, lw, h, focus == 0, "");
        int rx = 2 + lw + 1, rw = W - lw - 1;
        c.Box(rx, 4, rw, h, "город" + (slug.empty() ? std::string() : " · " + SlugToRu(slug)), focus == 1 ? G : GF, G);
        c.Seq(rx + 2, 5, { { "/ ", G, true }, { cityFilter.text.empty() && focus != 1 ? "начни печатать…" : cityFilter.text, cityFilter.text.empty() ? DM2 : WH } });
        if (focus == 1) c.Put(rx + 4 + (int)util::U8Len(cityFilter.text), 5, "▌", G);
        if (rw > 40) c.Put(rx + rw - 12, 5, "до пира", DM2);
        auto vc = VisibleCities();
        int rows = h - 4;
        clv.Clamp((int)vc.size() + 1, rows);
        for (int i = 0; i < rows && clv.top + i < (int)vc.size() + 1; ++i) {
            int idx = clv.top + i, r = 7 + i;
            bool on = idx == clv.sel;
            if (on) { c.FillBg(rx + 1, r, rx + rw - 1, focus == 1 ? GH : Hex("#122a1d")); c.Put(rx + 2, r, "▸", G); }
            if (idx == 0) { c.Put(rx + 4, r, "любой город страны", on ? WH : DM, on); continue; }
            const auto& cc = vc[(size_t)idx - 1];
            c.Put(rx + 4, r, util::U8Fit(cc.first.displayName(), (size_t)std::max(8, rw - 18)), on ? WH : TX, on);
            if (rw > 40 && cc.second >= 0) {
                long km = std::lround(cc.second);
                std::string d = km >= 1000 ? std::to_string(km / 1000) + " " + (km % 1000 < 100 ? km % 1000 < 10 ? "00" : "0" : "") + std::to_string(km % 1000) + " км"
                                           : std::to_string(km) + " км";
                c.Put(rx + rw - 12, r, d, cc.second > 500 ? OR : DM);
            }
        }
        if (citiesLoading && vc.empty()) c.Put(rx + 4, 8, "загружаю…", DM);
        // выбранное
        std::string choice = slug.empty() ? "" : SlugToRu(slug);
        if (clv.sel > 0 && clv.sel <= (int)vc.size()) choice += " / " + vc[(size_t)clv.sel - 1].first.displayName();
        c.Box(2, c.h - 5, W, 3, "", GF, G);
        if (!choice.empty())
            c.Seq(4, c.h - 4, { { "◐ ", OR }, { choice, TX }, { " — Enter: включить режим поездки, h: сделать домашним", DM } }, 2 + W - 1);
        Footer(c, a, { { "Tab", "страна↔город" }, { "Enter", "выбрать" }, { "h", "домашний" }, { "x", "снять поездку" }, { "Esc", "назад" } });
    }
    void Apply(App& a, bool home) {
        std::string slug = cp.Selected();
        if (slug.empty()) return;
        if (SlugToIso(slug).empty()) { a.Flash("для этой страны нет координат городов - выбери другую", OR); return; }
        auto vc = VisibleCities();
        std::string city = clv.sel > 0 && clv.sel <= (int)vc.size() ? vc[(size_t)clv.sel - 1].first.name : "";
        std::vector<std::string> args = { home ? "set-home-region" : "set-region", slug };
        if (!city.empty()) args.push_back(city);
        a.Run(home ? "домашний регион" : "регион поездки", args, 60000);
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.k == K::Tab || k.k == K::BackTab || (focus == 0 && k.k == K::Right) || (focus == 1 && k.k == K::Left)) { focus = 1 - focus; return true; }
        if (focus == 0) {
            if (k.k == K::Enter) { focus = 1; return true; }
            if (k.is('h') && cp.filter.text.empty()) { Apply(a, true); return true; }
            if (k.is('x') && cp.filter.text.empty()) { a.Run("снимаю поездку", { "remove-region" }, 30000); return true; }
            return cp.Key(k, a.term.canvas.h - 13);
        }
        auto vc = VisibleCities();
        if (clv.Handle(k, (int)vc.size() + 1, a.term.canvas.h - 13)) return true;
        if (k.k == K::Enter) {
            std::string choice = SlugToRu(cp.Selected()) + (clv.sel > 0 && clv.sel <= (int)vc.size() ? ", " + vc[(size_t)clv.sel - 1].first.displayName() : "");
            a.Confirm("Включить режим поездки?", choice + "\n\nКогда живых пиров станет меньше трёх, сторож будет искать их и здесь (после страны выхода трафика). "
                      "Основные пиры работают как обычно.", [this, &a] { Apply(a, false); });
            return true;
        }
        if (k.is('h') && cityFilter.text.empty()) { Apply(a, true); return true; }
        if (k.is('x') && cityFilter.text.empty()) { a.Run("снимаю поездку", { "remove-region" }, 30000); return true; }
        if (k.k == K::Char || k.k == K::Backspace) { cityFilter.Handle(k); clv = ListView(); clv.sel = vc.empty() ? 0 : 1; return true; }
        return false;
    }
};
Screen* MakeRegion() { return new RegionScreen(); }

// ------------------------------------------------------------------ каталог публичных пиров
struct CatalogScreen : Screen {
    CountryPane cp;
    int focus = 0;
    std::string slug;
    json::Value list;
    std::mutex mu;
    ListView lv;
    void OnShow(App& a) override { cp.Load(a); }
    void LoadList(App& a, const std::string& s) {
        slug = s;
        lv = ListView();
        { std::lock_guard<std::mutex> lk(mu); list = json::MakeArray(); }
        a.Run("проверяю порты", { "catalog-json", s }, 120000, [this, s](Job& j) {
            json::Value v;
            json::ParseLoose(j.res.out, v);
            std::lock_guard<std::mutex> lk(mu);
            if (slug == s) list = v;
        });
    }
    void Draw(Canvas& c, App& a) override {
        int W = c.w - 4;
        Header(c, "КАТАЛОГ", "публичные пиры из github.com/yggdrasil-network/public-peers");
        int lw = std::min(30, W / 3), h = c.h - 5;
        cp.Draw(c, 2, 3, lw, h, focus == 0, "");
        int rx = 2 + lw + 1, rw = W - lw - 1;
        json::Value v;
        { std::lock_guard<std::mutex> lk(mu); v = list; }
        c.Box(rx, 3, rw, h, slug.empty() ? "пиры" : SlugToRu(slug) + " · " + std::to_string(v.size()) + " пиров", focus == 1 ? G : GF, G);
        bool cityCol = rw >= 72;
        int uriW = std::max(20, rw - 4 - 18 - (cityCol ? 14 : 0));
        c.Put(rx + 4, 4, "пир", DM); c.Put(rx + 4 + uriW, 4, "порт", DM); c.Put(rx + 14 + uriW, 4, "надёжн.", DM);
        int rows = h - 3;
        lv.Clamp((int)v.size(), rows);
        if (slug.empty()) c.Put(rx + 2, 6, "выбери страну и нажми Enter", DM);
        else if (v.size() == 0) c.Put(rx + 2, 6, a.Busy("проверяю порты") ? "проверяю порты всех пиров (до 15 с)…" : "пиров нет", DM);
        for (int i = 0; i < rows && lv.top + i < (int)v.size(); ++i) {
            const json::Value& p = v[(size_t)(lv.top + i)];
            int r = 5 + i;
            bool on = lv.top + i == lv.sel;
            if (on) { c.FillBg(rx + 1, r, rx + rw - 1, focus == 1 ? GH : Hex("#122a1d")); c.Put(rx + 2, r, "▸", G); }
            double ms = p["tcp_ms"].num(-1);
            std::string err = p["err"].str();
            c.Put(rx + 4, r, util::U8Fit(p["uri"].str(), (size_t)uriW - 1), on ? WH : (ms >= 0 || err == "udp" ? TX : DM), on);
            c.Put(rx + 4 + uriW, r, ms >= 0 ? util::Fixed(ms, 0) + " мс" : err == "udp" ? "quic" : err == "dns" ? "нет DNS" : err == "refused" ? "закрыт" : "молчит",
                  ms >= 0 ? RttColor(ms) : err == "udp" ? DM : RD);
            double rel = p["reliability"].num(-1);
            c.Put(rx + 14 + uriW, r, rel >= 0 ? util::Fixed(rel, 0) + " %" : "—", rel >= 90 ? G : rel >= 0 && rel < 50 ? RD : DM);
            if (cityCol) c.Put(rx + 22 + uriW, r, util::U8Fit(p["city"].str(), (size_t)(rx + rw - 2 - (rx + 22 + uriW))), DM);
        }
        Footer(c, a, { { "Tab", "страна↔пиры" }, { "Enter", "проверить пир" }, { "a", "в основные" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.k == K::Tab || k.k == K::BackTab) { focus = 1 - focus; return true; }
        if (focus == 0) {
            if (k.k == K::Enter) { std::string s = cp.Selected(); if (!s.empty()) { LoadList(a, s); focus = 1; } return true; }
            return cp.Key(k, a.term.canvas.h - 9);
        }
        json::Value v;
        { std::lock_guard<std::mutex> lk(mu); v = list; }
        if (k.k == K::Left) { focus = 0; return true; }
        if (lv.Handle(k, (int)v.size(), a.term.canvas.h - 8)) return true;
        if (lv.sel >= (int)v.size()) return false;
        std::string uri = v[(size_t)lv.sel]["uri"].str();
        if (k.k == K::Enter) {
            a.Run("проверяю пир", { "verify-peer", uri }, 60000, [&a, uri](Job& j) {
                json::Value r;
                auto ls = util::Lines(j.res.out);
                if (ls.empty() || !json::ParseLoose(ls.back(), r)) { a.Flash("проверка не удалась", RD); return; }
                if (r["up"].boolean()) a.Flash("✓ " + uri + " поднялся" + (r["ms"].num(-1) >= 0 ? ", " + util::Fixed(r["ms"].num(), 0) + " мс" : "") + " - a: добавить в основные", G);
                else a.Flash("✗ " + uri + ": " + r["text"].str(), RD);
            });
            return true;
        }
        if (k.is('a') || k.text == "ф") {
            a.Confirm("Добавить в основные пиры?", uri + "\n\nПир запишется в yggdrasil.conf (с проверкой и копией старого файла). Заработает после перезапуска службы.",
                      [&a, uri] { a.Run("добавляю пир", { "add-main-peer", uri }, 60000); });
            return true;
        }
        return false;
    }
};
Screen* MakeCatalog() { return new CatalogScreen(); }

// ------------------------------------------------------------------ устройства
struct DevicesScreen : Screen {
    ListView lv;
    std::map<std::string, json::Value> diag;   // имя -> разбор (по Enter)
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        const json::Value& devs = s.st["devices"];
        int W = c.w - 4;
        Header(c, "УСТРОЙСТВА", "проверяются, когда связь с пирами в порядке");
        int n = (int)devs.size(), listH = std::max(2, std::min(n, c.h - 16));
        c.Box(2, 3, W, listH + 3, std::to_string(n) + (n == 1 ? " устройство" : n >= 2 && n <= 4 ? " устройства" : " устройств"), GF, G);
        int addrX = 20, stX = std::max(addrX + 30, W - 26);
        c.Put(5, 4, "имя", DM); c.Put(addrX, 4, "адрес", DM); c.Put(stX, 4, "состояние", DM);
        lv.Clamp(n, listH);
        if (n == 0) c.Put(5, 5, "Устройств нет. a - добавить (имя и адрес Yggdrasil второго компьютера).", DM);
        long long now = util::Now();
        for (int i = 0; i < listH && lv.top + i < n; ++i) {
            const json::Value& d = devs[(size_t)(lv.top + i)];
            int y = 5 + i;
            bool on = lv.top + i == lv.sel;
            if (on) { c.FillBg(3, y, 2 + W - 1, GH); c.Put(3, y, "▸", G); }
            long long ds = d["down_since"].i64();
            c.Put(5, y, ds ? "◌" : "●", ds ? OR : G);
            c.Put(7, y, util::U8Fit(d["name"].str(), 12), on ? WH : TX, on);
            c.Put(addrX, y, util::U8Fit(d["host"].str(), (size_t)(stX - addrX - 2)), DM);
            c.Put(stX, y, ds ? "нет ответа " + util::Ago(now - ds) : "на связи", ds ? OR : G, false, Color(), 2 + W - 1);
        }
        int dy = 3 + listH + 4;
        if (lv.sel < n) {
            const json::Value& d = devs[(size_t)lv.sel];
            std::string name = d["name"].str();
            int dh = c.h - dy - 2;
            if (dh >= 4) {
                c.Box(2, dy, W, dh, "диагностика: " + name, GF, G);
                int r = dy + 1;
                auto it = diag.find(name);
                if (a.Busy("разбор " + name)) c.Put(4, r, "разбираю: пинг, маршрут, путь в сети Yggdrasil…", OR);
                else if (it != diag.end()) {
                    for (size_t k = 0; k < it->second["steps"].size() && r < dy + dh - 3; ++k) {
                        bool ok = it->second["steps"][k]["ok"].boolean();
                        c.Put(4, r, ok ? "✓" : "✗", ok ? G : RD, true);
                        c.Put(6, r++, it->second["steps"][k]["text"].str(), TX, false, Color(), 2 + W - 1);
                    }
                    r++;
                    std::string kind = it->second["kind"].str();
                    auto lines = Wrap(it->second["text"].str(), W - 14);
                    for (size_t k = 0; k < lines.size() && r < dy + dh - 1; ++k) {
                        if (k == 0) c.Put(4, r, "вывод: ", kind == "ok" ? G : OR, true);
                        c.Put(11, r++, lines[k], TX);
                    }
                } else if (!d["why"].str().empty()) {
                    c.Put(4, r++, "последний разбор сторожа:", DM);
                    for (const auto& l : Wrap(d["why"].str(), W - 6)) if (r < dy + dh - 1) c.Put(4, r++, l, OR);
                } else c.Put(4, r, "Enter - разобрать прямо сейчас", DM);
                if (d["peers"].size() && dh >= 6)
                    c.Seq(4, dy + dh - 2, { { "общие пиры: ", DM }, { util::U8Fit(util::Join([&] { std::vector<std::string> v; for (size_t k = 0; k < d["peers"].size(); ++k) v.push_back(d["peers"][k].str()); return v; }(), " "), (size_t)W - 18), TX } });
            }
        }
        Footer(c, a, { { "Enter", "диагностика" }, { "a", "добавить" }, { "e", "изменить" }, { "d", "удалить" }, { "p", "пинг" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        Snapshot s = a.Snap();
        const json::Value& devs = s.st["devices"];
        int n = (int)devs.size();
        if (lv.Handle(k, n, std::max(2, std::min(n, a.term.canvas.h - 16)))) return true;
        auto ask = [&](const std::string& name, const std::string& host, const std::string& peers) {
            a.Ask(name.empty() ? "Добавить устройство" : "Изменить устройство",
                  "Общие пиры - те, к которым подключено и само устройство (через пробел, можно пусто). Если путь до устройства не находится, "
                  "сторож подключит их на время.", { { "имя", name }, { "адрес Yggdrasil (200::/7)", host }, { "общие пиры", peers } },
                  [&a](const std::vector<std::string>& v) {
                      if (v.size() < 3 || v[0].empty() || v[1].empty()) { a.Flash("нужны имя и адрес", OR); return; }
                      std::vector<std::string> args = { "add-device", v[0], v[1] };
                      if (v[2].empty()) args.push_back("--no-peers"); else args.push_back(v[2]);
                      a.Run("устройство", args, 30000);
                  });
        };
        if (k.is('a') || k.text == "ф") { ask("", "", ""); return true; }
        if (lv.sel >= n) return false;
        const json::Value& d = devs[(size_t)lv.sel];
        std::string name = d["name"].str();
        if (k.is('e') || k.text == "у") {
            std::vector<std::string> pv;
            for (size_t i = 0; i < d["peers"].size(); ++i) pv.push_back(d["peers"][i].str());
            ask(name, d["host"].str(), util::Join(pv, " "));
            return true;
        }
        if (k.is('d') || k.text == "в") { a.Confirm("Удалить устройство?", name + " (" + d["host"].str() + ")", [&a, name] { a.Run("удаляю", { "remove-device", name }, 30000); }, true); return true; }
        if (k.k == K::Enter) {
            a.Run("разбор " + name, { "diagnose-device-json", name }, 120000, [this, name](Job& j) {
                json::Value v;
                if (json::ParseLoose(j.res.out, v)) diag[name] = v;
            });
            return true;
        }
        if (k.is('p') || k.text == "з") {
            std::string host = d["host"].str();
            a.RunFn("пинг " + name, [&a, name, host] {
                net::PingResult r = net::Ping6(host, 2000);
                a.Flash(r.ok ? "✓ " + name + ": " + util::Fixed(r.ms, 0) + " мс" : "✗ " + name + ": нет ответа на ping", r.ok ? G : OR);
            });
            return true;
        }
        return false;
    }
};
Screen* MakeDevices() { return new DevicesScreen(); }

// ------------------------------------------------------------------ журнал событий
struct JournalScreen : Screen {
    int scroll = 0;   // 0 - внизу (самые новые)
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        int W = c.w - 4;
        Header(c, "ЖУРНАЛ СОБЫТИЙ", "то же, что уходит в Telegram · events.log");
        int h = c.h - 5;
        c.Box(2, 3, W, h, "", GF, G);
        // строки с переносом
        std::vector<std::pair<std::string, Color>> lines;
        std::vector<std::string> times;
        for (const auto& e : s.events) {
            auto f = util::SplitKeep(e, '\t');
            if (f.size() < 3) continue;
            auto wr = Wrap(f[2], W - 22);
            for (size_t i = 0; i < wr.size(); ++i) {
                lines.push_back({ wr[i], EventColor(f[1]) });
                times.push_back(i == 0 ? util::DateRu(util::ToLL(f[0])) + " " + EventIcon(f[1]) : "");
            }
        }
        int rows = h - 2;
        int maxScroll = std::max(0, (int)lines.size() - rows);
        scroll = std::max(0, std::min(scroll, maxScroll));
        int from = std::max(0, (int)lines.size() - rows - scroll);
        for (int i = 0; i < rows && from + i < (int)lines.size(); ++i) {
            c.Put(4, 4 + i, times[(size_t)(from + i)], DM);
            c.Put(21, 4 + i, lines[(size_t)(from + i)].first, lines[(size_t)(from + i)].second == DM ? TX : lines[(size_t)(from + i)].second, false, Color(), 2 + W - 1);
        }
        if (lines.empty()) c.Put(4, 4, "событий пока нет", DM);
        c.Put(4, 3 + h - 1, scroll ? " ▲ " + std::to_string(scroll) + " строк ниже " : " ▼ конец журнала ", DM2);
        Footer(c, a, { { "↑↓ PgUp/PgDn", "листать" }, { "End", "к новым" }, { "l", "лог проверок" }, { "Esc", "назад" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        int page = a.term.canvas.h - 8;
        if (k.k == K::Up) { ++scroll; return true; }
        if (k.k == K::Down) { --scroll; return true; }
        if (k.k == K::PgUp) { scroll += page; return true; }
        if (k.k == K::PgDn) { scroll -= page; return true; }
        if (k.k == K::End) { scroll = 0; return true; }
        if (k.k == K::Home) { scroll = 1 << 20; return true; }
        if (k.is('l') || k.text == "д") {
            a.term.Suspend();
            util::ProcResult r;
            pid_t pid = fork();
            if (pid == 0) { execlp("less", "less", "+G", "-R", LogPath().c_str(), (char*)nullptr); _exit(127); }
            int st = 0; waitpid(pid, &st, 0);
            a.term.Resume();
            return true;
        }
        return false;
    }
};
Screen* MakeJournal() { return new JournalScreen(); }

// ------------------------------------------------------------------ служба Yggdrasil
struct ServiceScreen : Screen {
    ygg::UpdateInfo yu;
    std::string installed;
    std::mutex mu;
    void OnShow(App& a) override {
        a.RunFn("версия Yggdrasil", [this] {
            std::string v = ygg::InstalledVersion(ygg::CtlPaths());
            std::lock_guard<std::mutex> lk(mu);
            installed = v;
        });
    }
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        const auto& sv = s.svc;
        int W = c.w - 4, half = W / 2;
        Header(c, "СЛУЖБА", "yggdrasil.service");
        c.Box(2, 3, half, 9, "состояние", GF, G);
        bool run = sv.running();
        c.Seq(4, 4, { { "● ", run ? G : RD }, { !sv.exists ? "служба не найдена" : run ? "работает" : sv.active == "failed" ? "упала" : "остановлена", run ? G : RD, true },
                      { sv.since ? "  с " + util::DateRu(sv.since) : "", DM } }, 2 + half - 1);
        c.Seq(4, 5, { { "автозапуск ", DM }, { !sv.exists ? "—" : sv.autoStart() ? "включён" : "выключен", sv.autoStart() ? TX : OR } }, 2 + half - 1);
        c.Seq(4, 6, { { "память ", DM }, { sv.memBytes >= 0 ? util::FixedRu(sv.memBytes / 1048576.0, 0) + " МБ" : "—", TX },
                      { "   PID ", DM }, { sv.pid ? std::to_string(sv.pid) : "—", TX } }, 2 + half - 1);
        c.Seq(4, 7, { { "конфиг ", DM }, { ygg::CtlPaths().conf, TX } }, 2 + half - 1);
        c.Seq(4, 9, { { "[r] ", OR, true }, { "перезапустить сейчас", OR } }, 2 + half - 1);
        c.Seq(4, 10, { { "[R] ", G, true }, { "через 15 с — если сидишь по Ygg", TX } }, 2 + half - 1);
        int rx = 2 + half + 1, rw = W - half - 1;
        c.Box(rx, 3, rw, 9, "действия", GF, G);
        c.Seq(rx + 2, 4, { { "[x] ", G, true }, { "остановить", TX } });
        c.Seq(rx + 2, 5, { { "[g] ", G, true }, { "запустить", TX } });
        c.Seq(rx + 2, 6, { { "[e] ", G, true }, { sv.autoStart() ? "выключить автозапуск" : "включить автозапуск", TX } });
        c.Seq(rx + 2, 7, { { "[j] ", G, true }, { "лог службы (journalctl)", TX } });
        c.Put(rx + 2, 9, "остановка обрывает все сессии,", DM);
        c.Put(rx + 2, 10, "спросит подтверждение", DM);
        c.Box(2, 13, W, 6, "Yggdrasil", GF, G);
        std::string inst;
        ygg::UpdateInfo u;
        { std::lock_guard<std::mutex> lk(mu); inst = installed; u = yu; }
        c.Seq(4, 14, { { "у тебя ", DM }, { inst.empty() ? "—" : inst, TX } });
        if (!u.checked) c.Seq(4, 15, { { "[u] ", G, true }, { "проверить новую версию на GitHub", TX } });
        else if (!u.ok) c.Seq(4, 15, { { "не удалось проверить: ", DM }, { u.err, OR } });
        else if (!inst.empty() && util::VersionNewer(inst, u.tag)) {
            c.Seq(4, 15, { { "вышла ", DM }, { u.tag, G, true }, { "  " + u.page, DM } }, 2 + W - 1);
            if (!u.debUrl.empty()) c.Seq(4, 16, { { "[i] ", G, true }, { "установить " + u.debName + " (apt, конфиг и ключ не трогаются)", G } }, 2 + W - 1);
        } else c.Seq(4, 15, { { "✓ ", G }, { "последняя версия (" + u.tag + ")", G } });
        Footer(c, a, { { "буква", "действие" }, { "Esc", "назад" }, { "q", "выход" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        if (k.is('r') || k.text == "к") { a.Confirm("Перезапустить Yggdrasil сейчас?", "Если ты подключён к этому компьютеру через Yggdrasil (SSH по адресу 200::/7), "
                                             "соединение оборвётся. Если не уверен - отмена и R (через 15 с).", [&a] { a.Run("перезапуск", { "restart-yggdrasil" }, 60000); }, true); return true; }
        if (k.is('R') || k.text == "К") { a.Run("перезапуск через 15 с", { "restart-yggdrasil-delayed", "15" }, 30000); return true; }
        if (k.is('x') || k.text == "ч") { a.Confirm("Остановить Yggdrasil?", "Все соединения через Yggdrasil оборвутся. Сторож не будет запускать службу, только если у неё выключен автозапуск.",
                                             [&a] { a.Run("остановка", { "service", "stop" }, 60000); }, true); return true; }
        if (k.is('g') || k.text == "п") { a.Run("запуск", { "service", "start" }, 60000); return true; }
        if (k.is('e') || k.text == "у") { a.Run("автозапуск", { "service", a.Snap().svc.autoStart() ? "disable" : "enable" }, 30000); return true; }
        if (k.is('j') || k.text == "о") {
            a.term.Suspend();
            pid_t pid = fork();
            if (pid == 0) { execlp("journalctl", "journalctl", "-u", "yggdrasil", "-e", "--no-hostname", (char*)nullptr); _exit(127); }
            int st = 0; waitpid(pid, &st, 0);
            a.term.Resume();
            return true;
        }
        if (k.is('u') || k.text == "г") { a.RunFn("проверка версии", [this] { auto u = ygg::CheckYggUpdate(); std::lock_guard<std::mutex> lk(mu); yu = u; }); return true; }
        if (k.is('i') || k.text == "ш") {
            ygg::UpdateInfo u;
            { std::lock_guard<std::mutex> lk(mu); u = yu; }
            if (u.debUrl.empty()) return true;
            a.Confirm("Установить Yggdrasil " + u.tag + "?", "Скачаю " + u.debName + " с GitHub и поставлю через apt. Служба перезапустится - соединения через Yggdrasil "
                      "на несколько секунд оборвутся. Конфиг и ключ останутся прежними.", [&a, u] { a.Run("установка Yggdrasil", { "install-ygg", u.debUrl }, 600000); }, true);
            return true;
        }
        return false;
    }
};
Screen* MakeService() { return new ServiceScreen(); }

// ------------------------------------------------------------------ управление
struct ManageScreen : Screen {
    std::string repoTag, repoErr;
    bool checked = false;
    std::mutex mu;
    void Draw(Canvas& c, App& a) override {
        Snapshot s = a.Snap();
        const json::Value& st = s.st;
        int W = c.w - 4, half = W / 2;
        Header(c, "УПРАВЛЕНИЕ", "");
        long long now = util::Now();
        // автопроверка
        c.Box(2, 3, half, 6, "автопроверка", GF, G);
        const json::Value& tm = st["timer"];
        if (!tm["installed"].boolean()) {
            c.Seq(4, 4, { { "○ ", RD }, { "таймер не установлен", RD } }, 2 + half - 1);
            c.Seq(4, 7, { { "[s] ", G, true }, { "установить (каждые 3 мин)", TX } }, 2 + half - 1);
        } else {
            bool on = tm["active"].boolean();
            c.Seq(4, 4, { { "● ", on ? G : RD }, { on ? "таймер включён" : "таймер выключен", on ? TX : RD } }, 2 + half - 1);
            if (on && tm["next"].i64() > now) c.Seq(4, 5, { { "каждые 3 мин, следующая ", DM }, { util::MinSec(tm["next"].i64() - now), TX } }, 2 + half - 1);
            long long last = st["last_tick_at"].i64();
            std::string ph = st["last_phase"].str();
            if (last) c.Seq(4, 6, { { "последняя ", DM }, { util::TimeText(last, "%H:%M") + " " + (ph == "ok" ? "✓ всё в порядке" : ph), ph == "ok" ? G : OR } }, 2 + half - 1);
            c.Seq(4, 7, { { "[s] ", G, true }, { on ? "выключить" : "включить", TX } }, 2 + half - 1);
        }
        // обновления
        int rx = 2 + half + 1, rw = W - half - 1;
        c.Box(rx, 3, rw, 6, "обновления", GF, G);
        std::string tag, err;
        bool chk;
        { std::lock_guard<std::mutex> lk(mu); tag = repoTag; err = repoErr; chk = checked; }
        if (tag.empty()) tag = st["update_available"].str();
        c.Seq(rx + 2, 4, { { "у тебя ", DM }, { CURRENT_VERSION, TX }, { "  в репо ", DM },
                           { !tag.empty() ? tag : chk ? (err.empty() ? std::string(CURRENT_VERSION) + " ✓" : "?") : "—", !tag.empty() ? G : DM } }, rx + rw - 1);
        if (!err.empty()) c.Put(rx + 2, 5, util::U8Fit(err, (size_t)rw - 4), OR);
        c.Seq(rx + 2, 6, { { "[u] ", G, true }, { "проверить", TX } });
        if (!tag.empty() && util::VersionNewer(CURRENT_VERSION, tag)) c.Seq(rx + 2, 7, { { "[i] ", G, true }, { "установить " + tag, G, true } });
        // Telegram
        c.Box(2, 10, half, 6, "Telegram", GF, G);
        const json::Value& tl = st["telegram_last"];
        if (st["telegram_configured"].boolean()) {
            std::string chat = st["telegram_chat"].str();
            c.Seq(4, 11, { { "● ", G }, { "настроен, chat " + (chat.size() > 4 ? chat.substr(0, 2) + "…" + chat.substr(chat.size() - 2) : chat), TX } }, 2 + half - 1);
            if (tl["at"].i64()) {
                c.Seq(4, 12, { { "последнее ", DM }, { util::TimeText(tl["at"].i64(), "%H:%M") + (tl["ok"].boolean() ? " доставлено" : " НЕ дошло"), tl["ok"].boolean() ? G : RD } }, 2 + half - 1);
                c.Seq(4, 13, { { tl["ok"].boolean() ? "путь " : "", DM }, { tl["ok"].boolean() ? tl["via"].str() : tl["err"].str(), tl["ok"].boolean() ? TX : RD } }, 2 + half - 1);
            }
        } else c.Seq(4, 11, { { "○ ", DM }, { "не настроен - уведомления выключены", DM } }, 2 + half - 1);
        c.Seq(4, 14, { { "[m] ", G, true }, { "тест   ", TX }, { "[n] ", G, true }, { "настроить", TX } }, 2 + half - 1);
        // VPN
        c.Box(rx, 10, rw, 6, "VPN-скрипт", GF, G);
        if (st["vpn_script_approved"].boolean()) {
            c.Seq(rx + 2, 11, { { "● ", G }, { "одобрен", TX } });
            c.Put(rx + 2, 12, util::U8Fit(st["vpn_script"].str(), (size_t)rw - 4), DM);
            c.Seq(rx + 2, 14, { { "[c] ", G, true }, { "снять одобрение", TX } });
        } else {
            c.Seq(rx + 2, 11, { { "○ ", DM }, { "не одобрен", DM } });
            c.Put(rx + 2, 12, "сторож VPN не трогает,", DM);
            c.Put(rx + 2, 13, "только сообщает", DM);
            c.Seq(rx + 2, 14, { { "[v] ", G, true }, { "одобрить свой скрипт…", TX } });
        }
        // конфиг
        c.Box(2, 17, W, 4, "конфиг Yggdrasil", GF, G);
        long long goodAt = st["config_good_at"].i64();
        if (st["config_problem"].boolean()) {
            c.Seq(4, 18, { { "▲ ", RD }, { "не читается: " + st["config_error"].str(), RD } }, 2 + W - 1);
            c.Seq(4, 19, { { "[r] ", G, true }, { st["config_restore_from"].str().empty() ? "рабочей копии нет - исправь вручную" : "вернуть рабочую копию " + st["config_restore_from"].str(), TX } }, 2 + W - 1);
        } else {
            c.Seq(4, 18, { { "● ", G }, { st["config_path"].str() + " в порядке", TX }, { goodAt ? "   рабочая копия от " + util::DateRu(goodAt) : "", DM } }, 2 + W - 1);
            c.Seq(4, 19, { { "[r] ", G, true }, { "вернуть рабочую копию (спросит подтверждение)", TX } }, 2 + W - 1);
        }
        Footer(c, a, { { "буква", "действие" }, { "Esc", "назад" }, { "q", "выход" } });
    }
    bool Key(const tui::Key& k, App& a) override {
        Snapshot s = a.Snap();
        const json::Value& st = s.st;
        if (k.is('s') || k.text == "ы") {
            if (!st["timer"]["installed"].boolean()) a.Run("установка таймера", { "install" }, 60000);
            else if (st["timer"]["active"].boolean())
                a.Confirm("Выключить автопроверки?", "Сторож перестанет проверять пиры сам. Ручная проверка (t) останется.", [&a] { a.Run("таймер", { "stop-timer" }, 30000); }, true);
            else a.Run("таймер", { "start-timer" }, 30000);
            return true;
        }
        if (k.is('u') || k.text == "г") {
            a.Run("проверка обновлений", { "check-update" }, 60000, [this, &a](Job& j) {
                std::string out = j.res.out;
                std::lock_guard<std::mutex> lk(mu);
                checked = true; repoErr.clear(); repoTag.clear();
                size_t p = out.find("новая версия: ");
                if (p != std::string::npos) { std::string rest = out.substr(p + strlen("новая версия: ")); repoTag = rest.substr(0, rest.find_first_of(" \n")); }
                else if (util::Contains(out, "Не удалось")) repoErr = LastMeaningful(out);
                a.Flash(LastMeaningful(out), repoErr.empty() ? G : OR);
            });
            return true;
        }
        if (k.is('i') || k.text == "ш") {
            std::string tag;
            { std::lock_guard<std::mutex> lk(mu); tag = repoTag; }
            if (tag.empty()) tag = st["update_available"].str();
            if (tag.empty() || !util::VersionNewer(CURRENT_VERSION, tag)) return true;
            a.Confirm("Установить ygg-watchdog " + tag + "?", "Скачаю linux/ygg-watchdog из репозитория, сверю отпечаток и проверю, что новая версия запускается. "
                      "Старая останется рядом как .backup и вернётся сама, если новая не запустится. Окно перезапустится.", [&a] {
                a.Run("обновление", { "apply-update" }, 300000, [&a](Job& j) {
                    if (j.res.code == 0 && util::Contains(j.res.out, "Обновлено")) {
                        a.term.Stop();
                        std::string self = util::SelfExe();
                        execl(self.c_str(), self.c_str(), (char*)nullptr);
                    }
                    a.Flash(LastMeaningful(j.res.out), j.res.code == 0 ? G : RD);
                });
            });
            return true;
        }
        if (k.is('m') || k.text == "ь") { a.Run("тест Telegram", { "test-notify" }, 90000); return true; }
        if (k.is('n') || k.text == "т") {
            NotifyConfig nc = LoadNotifyConfig();
            a.Ask("Telegram", "Токен - от @BotFather, chat_id - из https://api.telegram.org/bot<ТОКЕН>/getUpdates. Прокси можно не указывать: "
                  "сторож сам найдёт путь (напрямую, через прокси из окружения или VPN-клиента).",
                  { { "BOT_TOKEN", nc.botToken }, { "CHAT_ID", nc.chatId }, { "PROXY (необязательно)", nc.proxy } }, [&a](const std::vector<std::string>& v) {
                      if (v.size() < 3) return;
                      a.Run("настройка Telegram", { "set-notify", v[0], v[1], v[2] }, 30000);
                  });
            return true;
        }
        if ((k.is('v') || k.text == "м") && !st["vpn_script_approved"].boolean()) {
            a.Ask("Одобрить свой VPN-скрипт", "Сторож будет запускать его с аргументом restart, когда пиры легли. Запоминается SHA-256 файла: "
                  "если скрипт изменится хоть на байт - запуск откажется.", { { "путь к скрипту", "" } }, [&a](const std::vector<std::string>& v) {
                      if (!v.empty() && !v[0].empty()) a.Run("одобрение", { "approve-vpn-command", v[0] }, 30000);
                  });
            return true;
        }
        if ((k.is('c') || k.text == "с") && st["vpn_script_approved"].boolean()) { a.Run("снимаю одобрение", { "clear-vpn-command" }, 30000); return true; }
        if (k.is('r') || k.text == "к") {
            if (!st["config_problem"].boolean()) { a.Flash("Конфиг читается нормально - восстанавливать нечего.", G); return true; }
            a.Confirm("Вернуть рабочую копию конфига?", "Битый файл сохранится рядом как yggdrasil.conf.broken-дата, на его место встанет проверенная копия " +
                      st["config_restore_from"].str() + ".", [&a] { a.Run("восстановление конфига", { "restore-config" }, 60000); }, true);
            return true;
        }
        return false;
    }
};
Screen* MakeManage() { return new ManageScreen(); }

// ------------------------------------------------------------------ главный цикл
int App::Main() {
    g_quiet = true;
    LoadCityDatabase();
    ygg::CtlPaths();
    if (!term.Start()) { fprintf(stderr, "Окну нужен терминал. Команды без окна: ygg-watchdog help\n"); return 1; }
    std::thread poller([this] { Poll(); });
    stack.emplace_back(new MainScreen());
    long long lastDraw = 0;
    while (!quit) {
        FinishJobs();
        term.UpdateSize();
        Canvas& c = term.canvas;
        if (c.w < 60 || c.h < 20) {
            c.Put(1, 1, "Окно слишком маленькое: нужно хотя бы 80×24", OR);
        } else {
            stack.back()->Draw(c, *this);
            if (modal.open) DrawModal(c);
        }
        term.Present();
        lastDraw = util::Now();
        (void)lastDraw;
        tui::Key k = term.Read(Busy() ? 120 : 400);
        ++spin;
        if (k.k == K::None || k.k == K::Resize) continue;
        if (k.k == K::CtrlC) { quit = true; break; }
        if (modal.open) { ModalKey(k); continue; }
        if (stack.back()->Key(k, *this)) continue;
        if (k.k == K::Esc || k.k == K::Backspace) { if (stack.size() > 1) Pop(); else quit = true; continue; }
        if (k.is('q') || k.text == "й") { quit = true; continue; }
        if (k.is('t') || k.text == "е") { Run("проверка", { "tick" }, 600000); Flash("Проверка запущена - обычно до пары минут.", G); continue; }
    }
    quit = true;
    poller.join();
    term.Stop();
    return 0;
}

} // namespace app
