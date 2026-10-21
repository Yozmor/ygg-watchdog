// core.h - работа с Yggdrasil: где что лежит, вызовы yggdrasilctl,
// чтение и сохранение конфига, фоновый сбор данных для окна.
#pragma once
#include "util.h"
#include <taskschd.h>
#include <oleauto.h>
#include <map>
#include <deque>
#include <algorithm>

namespace ygg {

static const wchar_t* SERVICE_NAME = L"Yggdrasil";

// ------------------------------------------------------------------
// Где Yggdrasil: берём из командной строки службы (-useconffile,
// -logto), иначе стандартные пути установщика.
// ------------------------------------------------------------------
struct Paths {
    std::wstring exe = L"C:\\Program Files\\Yggdrasil\\yggdrasil.exe";
    std::wstring ctl = L"C:\\Program Files\\Yggdrasil\\yggdrasilctl.exe";
    std::wstring conf = L"C:\\ProgramData\\Yggdrasil\\yggdrasil.conf";
    std::wstring log = L"C:\\ProgramData\\Yggdrasil\\yggdrasil.log";
    std::wstring adminListen;   // из конфига (AdminListen)
    std::wstring EndpointArg() const { return adminListen.empty() ? L"" : L"-endpoint=" + adminListen + L" "; }
};

inline Paths Discover() {
    Paths p;
    util::ServiceInfo si = util::QueryService(SERVICE_NAME);
    if (!si.binaryPath.empty()) {
        auto a = util::SplitArgs(si.binaryPath);
        if (!a.empty() && util::FileExists(a[0])) {
            p.exe = a[0];
            p.ctl = util::DirOf(a[0]) + L"yggdrasilctl.exe";
        }
        for (size_t i = 0; i + 1 < a.size(); ++i) {
            std::wstring k = util::Lower(a[i]);
            if (k == L"-useconffile" || k == L"--useconffile") p.conf = a[i + 1];
            if (k == L"-logto" || k == L"--logto") {
                std::wstring v = util::Lower(a[i + 1]);
                p.log = (v == L"stdout" || v == L"syslog") ? L"" : a[i + 1];
            }
        }
    }
    return p;
}

// Адрес управления (AdminListen). yggdrasil -normaliseconf его может не
// выдать, поэтому ищем по очереди: JSON конфига, сам файл (HJSON), лог.
inline std::wstring FindAdminListen(const Paths& p, const json::Value& cfg) {
    if (cfg.isObject() && cfg.obj)
        for (const auto& kv : *cfg.obj)
            if (util::Lower(kv.first) == L"adminlisten" && kv.second.isString() && !kv.second.str().empty()) return kv.second.str();
    std::wstring raw = util::FromUtf8(util::ReadFileBytes(p.conf));
    std::wstring low = util::Lower(raw);
    size_t k = low.find(L"adminlisten");
    while (k != std::wstring::npos) {
        size_t c = k + 11;
        while (c < raw.size() && (raw[c] == L'"' || raw[c] == L' ' || raw[c] == L'\t')) ++c;
        if (c < raw.size() && raw[c] == L':') {
            ++c;
            while (c < raw.size() && (raw[c] == L' ' || raw[c] == L'\t' || raw[c] == L'"')) ++c;
            size_t e = c;
            while (e < raw.size() && raw[e] != L'"' && raw[e] != L',' && raw[e] != L'\r' && raw[e] != L'\n' && raw[e] != L' ' && raw[e] != L'}') ++e;
            if (e > c) return raw.substr(c, e - c);
        }
        k = low.find(L"adminlisten", k + 11);
    }
    if (!p.log.empty()) {
        std::wstring tail = util::FromUtf8(util::ReadFileTail(p.log, 512 * 1024));
        const std::wstring mark = L"admin socket listening on ";
        size_t a = util::Lower(tail).rfind(mark);
        if (a != std::wstring::npos) {
            size_t b = a + mark.size(), e = b;
            while (e < tail.size() && !iswspace(tail[e])) ++e;
            std::wstring ep = tail.substr(b, e - b);
            if (!ep.empty()) return ep.find(L"://") == std::wstring::npos ? L"tcp://" + ep : ep;
        }
    }
    return L"";
}

// Конфиг в JSON (через сам yggdrasil, он понимает и HJSON с комментариями)
inline bool LoadConfig(const Paths& p, json::Value& out, std::wstring& err) {
    if (!util::FileExists(p.conf)) { err = L"не найден конфиг " + p.conf; return false; }
    util::ProcResult r = util::Run(p.exe, L"-useconffile \"" + p.conf + L"\" -normaliseconf -json", 15000);
    if (!r.started) { err = L"не запускается " + p.exe; return false; }
    json::Value v;
    if (r.code != 0 || !json::Parse(r.out, v) || !v.isObject()) {
        err = L"yggdrasil не принял конфиг: " + util::Trim(util::FromUtf8(r.out)).substr(0, 200);
        return false;
    }
    out = v;
    return true;
}

// Сохранить конфиг: проверка самим yggdrasil -> копия старого -> запись.
// Временный файл лежит в папке конфига (там же права, что у конфига:
// в нём закрытый ключ) и сразу удаляется. Возвращает текст ошибки или "".
inline std::wstring SaveConfig(const Paths& p, const json::Value& cfgIn, std::wstring* backupOut = nullptr) {
    // Поля, которые есть в файле, но -normaliseconf не выдал (AdminListen), не теряем
    json::Value cfg = json::Clone(cfgIn);
    if (cfg.obj && !cfg.obj->count(L"AdminListen") && !p.adminListen.empty() &&
        util::Lower(util::FromUtf8(util::ReadFileBytes(p.conf))).find(L"adminlisten") != std::wstring::npos)
        (*cfg.obj)[L"AdminListen"] = json::MakeString(p.adminListen);
    std::string text = json::Dump(cfg, true) + "\n";
    std::wstring tmp = p.conf + L".check";
    if (!util::WriteFileBytes(tmp, text)) return L"не удалось записать временный файл рядом с конфигом";
    util::ProcResult r = util::Run(p.exe, L"-useconffile \"" + tmp + L"\" -normaliseconf", 15000);
    DeleteFileW(tmp.c_str());
    if (!r.started) return L"не запускается " + p.exe;
    if (r.code != 0) return L"yggdrasil отклонил конфиг: " + util::Trim(util::FromUtf8(r.out)).substr(0, 200);

    std::wstring backup = p.conf + L".bak-" + util::TimeText(time(nullptr), L"%Y%m%d-%H%M%S");
    if (!CopyFileW(p.conf.c_str(), backup.c_str(), FALSE)) return L"не удалось сделать копию старого конфига";
    // атомарно: либо старый конфиг, либо новый целиком (никогда не обрезанный)
    if (!util::WriteFileAtomic(p.conf, text)) return L"не удалось записать конфиг (копия: " + backup + L")";
    if (backupOut) *backupOut = backup;
    return L"";
}

// Куда стучаться yggdrasilctl: путь из службы + AdminListen из конфига (или
// из лога). Считается один раз на процесс - файл конфига читается напрямую,
// без запуска yggdrasil -normaliseconf, поэтому это дёшево.
inline const Paths& CtlPaths() {
    static const Paths p = [] {     // инициализация статики потокобезопасна (C++11)
        Paths q = Discover();
        std::wstring al = FindAdminListen(q, json::Value());
        q.adminListen = (util::Lower(al) == L"none") ? L"" : al;
        if (!util::FileExists(q.ctl)) q.ctl = L"C:\\Program Files\\Yggdrasil\\yggdrasilctl.exe";
        return q;
    }();
    return p;
}

// Вызов yggdrasilctl -json. Ответ - JSON; если не JSON - err = текст.
inline bool Ctl(const Paths& p, const std::wstring& cmd, json::Value& out, std::wstring& err, DWORD timeout = 8000) {
    util::ProcResult r = util::Run(p.ctl, p.EndpointArg() + L"-json " + cmd, timeout);
    if (!r.started) { err = L"не запускается yggdrasilctl"; return false; }
    json::Value v;
    if (!json::Parse(r.out, v)) {
        err = util::Trim(util::FromUtf8(r.out));
        if (err.empty()) err = L"yggdrasilctl ничего не ответил";
        return false;
    }
    if (!v.isObject()) { err = util::Trim(util::FromUtf8(r.out)).substr(0, 200); return false; }
    if (!v[L"error"].str().empty()) { err = v[L"error"].str(); return false; }
    out = v;
    return true;
}

// ------------------------------------------------------------------
// Сторож (ygg-watchdog): найден ли, что говорит, когда следующая проверка
// ------------------------------------------------------------------
struct TaskInfo { bool ok = false; int state = 0; long long nextRun = 0, lastRun = 0; long lastResult = 0; std::wstring exe; };

inline long long OleDateToUnix(DATE d) {
    if (d < 1.0) return 0;
    SYSTEMTIME st;
    if (!VariantTimeToSystemTime(d, &st)) return 0;
    FILETIME lft, ft;
    if (!SystemTimeToFileTime(&st, &lft) || !LocalFileTimeToFileTime(&lft, &ft)) return 0;
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    long long t = (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
    return t < 1000000000LL ? 0 : t;
}

static const GUID kClsidTaskScheduler = { 0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };
static const GUID kIidTaskService     = { 0x2faba4c7, 0x4da9, 0x4013, { 0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85 } };
static const GUID kIidExecAction      = { 0x4c3d624d, 0xfd6b, 0x49a3, { 0xb9, 0xb7, 0x09, 0xcb, 0x3c, 0xd3, 0xf0, 0x47 } };

// Что на самом деле стережёт задача: если она запускает стража (ygg_guard.exe) -
// это ygg_watchdog.exe рядом с ним.
inline std::wstring TaskTargetExe(const std::wstring& path, const std::wstring& /*args*/) {
    std::wstring lp = util::Lower(path);
    const std::wstring guard = L"ygg_guard.exe";
    if (lp.size() >= guard.size() && lp.compare(lp.size() - guard.size(), guard.size(), guard) == 0)
        return util::DirOf(path) + L"ygg_watchdog.exe";
    return path;
}

inline TaskInfo QueryTask(const wchar_t* name) {
    TaskInfo ti;
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(kClsidTaskScheduler, nullptr, CLSCTX_INPROC_SERVER, kIidTaskService, (void**)&svc)) || !svc) return ti;
    VARIANT e; VariantInit(&e);
    if (SUCCEEDED(svc->Connect(e, e, e, e))) {
        BSTR rn = SysAllocString(L"\\");
        ITaskFolder* root = nullptr;
        if (SUCCEEDED(svc->GetFolder(rn, &root)) && root) {
            BSTR tn = SysAllocString(name);
            IRegisteredTask* t = nullptr;
            if (SUCCEEDED(root->GetTask(tn, &t)) && t) {
                ti.ok = true;
                TASK_STATE st; if (SUCCEEDED(t->get_State(&st))) ti.state = (int)st;
                DATE d = 0;
                if (SUCCEEDED(t->get_NextRunTime(&d))) ti.nextRun = OleDateToUnix(d);
                if (SUCCEEDED(t->get_LastRunTime(&d))) ti.lastRun = OleDateToUnix(d);
                LONG lr = 0; if (SUCCEEDED(t->get_LastTaskResult(&lr))) ti.lastResult = lr;
                // что задача запускает: первое действие «запуск программы»
                ITaskDefinition* def = nullptr;
                if (SUCCEEDED(t->get_Definition(&def)) && def) {
                    IActionCollection* acts = nullptr;
                    if (SUCCEEDED(def->get_Actions(&acts)) && acts) {
                        IAction* act = nullptr;
                        if (SUCCEEDED(acts->get_Item(1, &act)) && act) {
                            IExecAction* ex = nullptr;
                            if (SUCCEEDED(act->QueryInterface(kIidExecAction, (void**)&ex)) && ex) {
                                BSTR path = nullptr, args = nullptr;
                                if (SUCCEEDED(ex->get_Path(&path)) && path) {
                                    wchar_t buf[MAX_PATH * 2];
                                    ExpandEnvironmentStringsW(path, buf, MAX_PATH * 2);
                                    ti.exe = buf;
                                    if (ti.exe.size() >= 2 && ti.exe.front() == L'"') ti.exe = ti.exe.substr(1, ti.exe.size() - 2);
                                    SysFreeString(path);
                                    if (SUCCEEDED(ex->get_Arguments(&args)) && args) {
                                        ti.exe = TaskTargetExe(ti.exe, args);
                                        SysFreeString(args);
                                    }
                                }
                                ex->Release();
                            }
                            act->Release();
                        }
                        acts->Release();
                    }
                    def->Release();
                }
                t->Release();
            }
            SysFreeString(tn);
            root->Release();
        }
        SysFreeString(rn);
    }
    svc->Release();
    return ti;
}

// Путь к ygg_watchdog.exe - из задачи Планировщика YggWatchdogTick
// (через сам Планировщик; запасной путь - XML задачи в UTF-16 или UTF-8)
static const wchar_t* REG_WATCHDOG = L"SOFTWARE\\Yozmor\\ygg-watchdog";
static const wchar_t* REG_CONTROL  = L"SOFTWARE\\Yozmor\\ygg-control";

inline std::wstring FindWatchdogExe(const TaskInfo& ti) {
    // 1) сторож сам записывает свой путь в реестр при каждом запуске
    std::wstring reg = util::RegReadPath(REG_WATCHDOG);
    if (!reg.empty() && util::FileExists(reg)) return reg;
    // 2) путь, указанный вручную в панели
    std::wstring manual = util::RegReadPath(REG_CONTROL, L"WatchdogPath");
    if (!manual.empty() && util::FileExists(manual)) return manual;
    // 3) задача Планировщика
    if (!ti.exe.empty() && util::FileExists(ti.exe)) return ti.exe;
    std::string tx = util::ReadFileBytes(L"C:\\Windows\\System32\\Tasks\\YggWatchdogTick");
    if (tx.size() < 4) return L"";
    std::wstring xml;
    bool utf16 = ((unsigned char)tx[0] == 0xFF && (unsigned char)tx[1] == 0xFE) || tx[1] == 0;
    if (utf16) {
        size_t start = ((unsigned char)tx[0] == 0xFF) ? 2 : 0;
        for (size_t i = start; i + 1 < tx.size(); i += 2) xml += (wchar_t)((unsigned char)tx[i] | ((unsigned char)tx[i + 1] << 8));
    } else xml = util::FromUtf8(tx);
    size_t a = xml.find(L"<Command>"), b = xml.find(L"</Command>");
    if (a == std::wstring::npos || b == std::wstring::npos || b <= a) return L"";
    std::wstring exe = xml.substr(a + 9, b - a - 9);
    if (exe.size() >= 2 && exe.front() == L'"') exe = exe.substr(1, exe.size() - 2);
    size_t aa = xml.find(L"<Arguments>"), ab = xml.find(L"</Arguments>");
    if (aa != std::wstring::npos && ab != std::wstring::npos && ab > aa) exe = TaskTargetExe(exe, xml.substr(aa + 11, ab - aa - 11));
    return util::FileExists(exe) ? exe : L"";
}

// ------------------------------------------------------------------
// Обновления Yggdrasil: последний релиз на GitHub
// ------------------------------------------------------------------
struct UpdateInfo {
    bool checked = false, ok = false;
    std::wstring tag, page, msiUrl, msiName, err;
    long long at = 0;
};

// Версия самой панели - поднимать вручную при выпуске
static const wchar_t* PANEL_VERSION = L"1.0.0";

// Обновления самой панели: последний релиз Yozmor/ygg-control
inline UpdateInfo CheckPanelUpdate() {
    UpdateInfo u;
    u.checked = true;
    u.at = (long long)time(nullptr);
    DWORD status = 0;
    std::string body = util::HttpGet(L"https://api.github.com/repos/Yozmor/ygg-control/releases/latest", &status,
                                     4 * 1024 * 1024, L"Accept: application/vnd.github+json\r\n");
    json::Value v;
    if (status == 404) { u.err = L"на GitHub пока нет ни одного релиза панели"; return u; }
    if (body.empty() || !json::Parse(body, v) || v[L"tag_name"].str().empty()) {
        u.err = status ? L"GitHub ответил кодом " + std::to_wstring(status) : L"GitHub недоступен";
        return u;
    }
    u.ok = true;
    u.tag = v[L"tag_name"].str();
    u.page = v[L"html_url"].str();
    return u;
}

inline UpdateInfo CheckYggUpdate() {
    UpdateInfo u;
    u.checked = true;
    u.at = (long long)time(nullptr);
    DWORD status = 0;
    std::string body = util::HttpGet(L"https://api.github.com/repos/yggdrasil-network/yggdrasil-go/releases/latest", &status,
                                     4 * 1024 * 1024, L"Accept: application/vnd.github+json\r\n");
    json::Value v;
    if (body.empty() || !json::Parse(body, v) || v[L"tag_name"].str().empty()) {
        u.err = status ? L"GitHub ответил кодом " + std::to_wstring(status) : L"GitHub недоступен";
        return u;
    }
    u.ok = true;
    u.tag = v[L"tag_name"].str();
    u.page = v[L"html_url"].str();
    const auto& as = v[L"assets"];
    for (size_t i = 0; i < as.size(); ++i) {
        std::wstring n = util::Lower(as[i][L"name"].str());
        if (n.size() > 4 && n.substr(n.size() - 4) == L".msi" && (n.find(L"x64") != std::wstring::npos || n.find(L"amd64") != std::wstring::npos)) {
            u.msiName = as[i][L"name"].str();
            u.msiUrl = as[i][L"browser_download_url"].str();
        }
    }
    return u;
}

// ------------------------------------------------------------------
// Снимок всего, что знает окно. Собирается фоновым потоком.
// ------------------------------------------------------------------
struct TrafficPoint { long long t = 0; double rx = 0, tx = 0; };   // байт/с

struct Snapshot {
    Paths paths;
    bool ctlOk = false;          // yggdrasilctl отвечает
    std::wstring ctlError;
    json::Value self, peers, sessions, paths_, tree, multicast, tun, commands;
    long long treeAt = 0;
    util::ServiceInfo service;
    std::wstring version;        // yggdrasil -version
    // трафик
    std::deque<TrafficPoint> traffic;            // за 60 минут
    double totalRx = 0, totalTx = 0;
    std::map<std::wstring, std::deque<float>> sessionRate;   // ключ сессии -> история скорости
    // лог
    std::vector<std::wstring> logLines;
    unsigned long long logSize = 0;
    // сторож
    std::wstring wdExe;
    bool wdStatusOk = false;
    json::Value wdStatus;
    std::vector<std::wstring> wdEvents;   // хвост events.log сторожа: "unix\tуровень\tтекст"
    TaskInfo wdTask;
    long long loadedAt = 0;
};

static const UINT WM_APP_DATA = WM_APP + 1;

class DataSource {
public:
    SRWLOCK lock = SRWLOCK_INIT;
    Snapshot snap;
    volatile LONG stop = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE thread = nullptr;
    HWND notify = nullptr;
    volatile LONG wantConfigReload = 1;

    void Start(HWND h) { notify = h; thread = CreateThread(nullptr, 0, &DataSource::Proc, this, 0, nullptr); }
    void Stop() {
        InterlockedExchange(&stop, 1);
        SetEvent(wake);
        if (thread) { WaitForSingleObject(thread, 20000); CloseHandle(thread); thread = nullptr; }
    }
    void RefreshNow() { SetEvent(wake); }
    void ReloadPaths() { InterlockedExchange(&wantConfigReload, 1); SetEvent(wake); }
    Snapshot Get() { AcquireSRWLockShared(&lock); Snapshot c = snap; ReleaseSRWLockShared(&lock); return c; }

private:
    static DWORD WINAPI Proc(LPVOID p) { ((DataSource*)p)->Loop(); return 0; }

    void Loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Snapshot s;
        int tickNo = 0;
        double lastRx = -1, lastTx = -1; long long lastT = 0;
        std::map<std::wstring, std::pair<double, long long>> lastSess;
        while (!stop) {
            long long now = (long long)time(nullptr);
            s.loadedAt = now;

            if (InterlockedExchange(&wantConfigReload, 0)) {
                s.paths = Discover();
                json::Value cfg; std::wstring err;
                LoadConfig(s.paths, cfg, err);
                std::wstring al = FindAdminListen(s.paths, cfg);
                s.paths.adminListen = (util::Lower(al) == L"none") ? L"" : al;
                // yggdrasil -version: "Build name: ..." и "Build version: 0.5.13"
                util::ProcResult vr = util::Run(s.paths.exe, L"-version", 5000);
                s.version.clear();
                std::wstringstream vs(util::FromUtf8(vr.out));
                std::wstring vl;
                while (std::getline(vs, vl)) {
                    size_t c = util::Lower(vl).find(L"build version:");
                    if (c != std::wstring::npos) s.version = util::Trim(vl.substr(c + 14));
                }
                if (s.version.empty()) s.version = util::Trim(util::FromUtf8(vr.out)).substr(0, 40);
            }
            s.service = util::QueryService(SERVICE_NAME);

            std::wstring err;
            json::Value v;
            s.ctlOk = ygg::Ctl(s.paths, L"getSelf", v, err);
            s.ctlError = s.ctlOk ? L"" : err;
            if (s.ctlOk) {
                s.self = v;
                if (ygg::Ctl(s.paths, L"getPeers", v, err)) s.peers = v[L"peers"];
                if (ygg::Ctl(s.paths, L"getSessions", v, err)) s.sessions = v[L"sessions"];
                if (tickNo % 2 == 0) {
                    if (ygg::Ctl(s.paths, L"getPaths", v, err)) s.paths_ = v[L"paths"];
                    if (ygg::Ctl(s.paths, L"getTree", v, err)) { s.tree = v[L"tree"]; s.treeAt = now; }
                }
                if (tickNo % 5 == 0) {
                    if (ygg::Ctl(s.paths, L"getMulticastInterfaces", v, err)) s.multicast = v; else s.multicast = json::Value();
                    if (ygg::Ctl(s.paths, L"getTUN", v, err)) s.tun = v; else s.tun = json::Value();
                    if (ygg::Ctl(s.paths, L"list", v, err)) s.commands = v;
                }
                // трафик: сумма по пирам, скорость = разница / время
                double rx = 0, tx = 0;
                for (size_t i = 0; i < s.peers.size(); ++i) { rx += s.peers[i][L"bytes_recvd"].num(); tx += s.peers[i][L"bytes_sent"].num(); }
                s.totalRx = rx; s.totalTx = tx;
                if (lastT > 0 && now > lastT && rx >= lastRx && tx >= lastTx) {
                    TrafficPoint tp; tp.t = now;
                    tp.rx = (rx - lastRx) / (double)(now - lastT);
                    tp.tx = (tx - lastTx) / (double)(now - lastT);
                    s.traffic.push_back(tp);
                    while (!s.traffic.empty() && s.traffic.front().t < now - 3600) s.traffic.pop_front();
                }
                lastRx = rx; lastTx = tx; lastT = now;
                // скорость по сессиям (для графика в строке)
                std::map<std::wstring, std::pair<double, long long>> curSess;
                for (size_t i = 0; i < s.sessions.size(); ++i) {
                    std::wstring k = s.sessions[i][L"key"].str();
                    double b = s.sessions[i][L"bytes_recvd"].num() + s.sessions[i][L"bytes_sent"].num();
                    curSess[k] = { b, now };
                    auto it = lastSess.find(k);
                    if (it != lastSess.end() && now > it->second.second && b >= it->second.first) {
                        auto& h = s.sessionRate[k];
                        h.push_back((float)((b - it->second.first) / (double)(now - it->second.second)));
                        while (h.size() > 60) h.pop_front();
                    }
                }
                for (auto it = s.sessionRate.begin(); it != s.sessionRate.end();)
                    it = curSess.count(it->first) ? std::next(it) : s.sessionRate.erase(it);
                lastSess = curSess;
            }

            // лог: перечитываем хвост, только если файл изменился
            if (!s.paths.log.empty()) {
                WIN32_FILE_ATTRIBUTE_DATA fa;
                unsigned long long size = 0;
                if (GetFileAttributesExW(s.paths.log.c_str(), GetFileExInfoStandard, &fa))
                    size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                if (size != s.logSize) {
                    std::string tail = util::ReadFileTail(s.paths.log, 400 * 1024, &s.logSize);
                    std::wstring w = util::FromUtf8(tail);
                    s.logLines.clear();
                    std::wstringstream ss(w);
                    std::wstring line;
                    while (std::getline(ss, line)) {
                        if (!line.empty() && line.back() == L'\r') line.pop_back();
                        if (!line.empty()) s.logLines.push_back(line);
                    }
                    if (s.logLines.size() > 4000) s.logLines.erase(s.logLines.begin(), s.logLines.end() - 4000);
                }
            }

            // сторож
            if (tickNo % 2 == 0) {
                s.wdTask = QueryTask(L"YggWatchdogTick");
                s.wdExe = FindWatchdogExe(s.wdTask);
                s.wdStatusOk = false;
                if (!s.wdExe.empty()) {
                    util::ProcResult r = util::Run(s.wdExe, L"status-json", 15000);
                    json::Value st;
                    if (json::Parse(r.out, st) && st.isObject()) { s.wdStatus = st; s.wdStatusOk = true; }
                    s.wdEvents.clear();
                    std::wstringstream es(util::FromUtf8(util::ReadFileTail(util::DirOf(s.wdExe) + L"events.log", 16 * 1024)));
                    std::wstring el;
                    while (std::getline(es, el)) { if (!el.empty() && el.back() == L'\r') el.pop_back(); if (!el.empty()) s.wdEvents.push_back(el); }
                    if (s.wdEvents.size() > 12) s.wdEvents.erase(s.wdEvents.begin(), s.wdEvents.end() - 12);
                }
            }

            AcquireSRWLockExclusive(&lock);
            snap = s;
            ReleaseSRWLockExclusive(&lock);
            PostMessageW(notify, WM_APP_DATA, 0, 0);
            ++tickNo;
            WaitForSingleObject(wake, 3000);
        }
        CoUninitialize();
    }
};

} // namespace ygg
