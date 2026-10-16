// ygg_watchdog_gui.cpp - окно сторожа (ygg-watchdog) для Windows.
//
// Собирается в тот же ygg_watchdog.exe, что и сам сторож: без параметров
// exe открывает это окно, с параметром - выполняет команду. Окно само
// ничего не чинит: все действия - запуском себя же с командой (как
// задача Планировщика), результат читается из вывода.
// Источники данных:
//   - ygg_watchdog.exe status-json  (состояние сторожа)
//   - events.log                    (журнал значимых событий)
//   - yggdrasilctl -json getPeers   (живые пиры с задержкой)
// Отрисовка - Direct2D (ui.h), без WebView и сторонних библиотек.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#include "ui.h"
#include "json_lite.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <taskschd.h>
#include <oleauto.h>
#include <functional>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <algorithm>

namespace gui {
using namespace ui;

static const wchar_t* YGGCTL_PATH = L"C:\\Program Files\\Yggdrasil\\yggdrasilctl.exe";
static const wchar_t* YGGCTL_ENDPOINT = L"-endpoint=tcp://127.0.0.1:9002";
static const wchar_t* TASK_NAME = L"YggWatchdogTick";

static const UINT WM_APP_DATA = WM_APP + 1;
static const UINT WM_APP_CMD_DONE = WM_APP + 2;  // команда ygg_watchdog.exe закончилась (lParam = std::string*)
static const UINT WM_APP_LIST = WM_APP + 3;      // пришёл список стран/городов (wParam 1/2)
static const UINT_PTR TIMER_CLOCK = 1;

// трей (код - ближе к концу файла)
bool TrayAutostartEnabled();
void SetTrayAutostart(bool on);
bool TrayPopupsEnabled();
void SetTrayPopups(bool on);
bool TrayRunning();
void StartTray();
static const UINT_PTR TIMER_ANIM = 2;

// ------------------------------------------------------------------
// Вспомогательное: путь, запуск процессов, чтение файлов
// ------------------------------------------------------------------
// Путь к самому себе: команды сторожа выполняет этот же exe
std::wstring SelfExe() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    size_t pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L".\\" : full.substr(0, pos + 1);
}

// Запуск без окна консоли, весь stdout -> строка UTF-8
std::string RunCapture(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs = 20000) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    std::string out;
    if (ok) {
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(hRead, chunk, sizeof(chunk), &n, nullptr) && n > 0) out.append(chunk, n);
        WaitForSingleObject(pi.hProcess, timeoutMs);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);
    return out;
}

std::string ReadFileBytes(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::wstring HhMm(long long ts, bool seconds = false) {
    if (ts <= 0) return L"--:--";
    time_t t = (time_t)ts;
    tm lt;
    localtime_s(&lt, &t);
    wchar_t b[16];
    wcsftime(b, 16, seconds ? L"%H:%M:%S" : L"%H:%M", &lt);
    return b;
}

std::wstring HostOf(const std::wstring& uri) {
    size_t a = uri.find(L"://");
    std::wstring rest = a == std::wstring::npos ? uri : uri.substr(a + 3);
    size_t colon = rest.rfind(L':');
    if (colon != std::wstring::npos) rest = rest.substr(0, colon);
    if (!rest.empty() && rest.front() == L'[') rest = rest.substr(1, rest.size() - 2);
    return rest;
}

// Короткая подпись пира для радара: первая часть имени хоста
std::wstring ShortName(const std::wstring& uri) {
    std::wstring h = HostOf(uri);
    // ygg.keenetic.pro -> KEENETIC: первая часть "ygg"/"peer"/... ни о чём не говорит
    std::vector<std::wstring> parts;
    size_t st = 0;
    while (true) {
        size_t d = h.find(L'.', st);
        parts.push_back(h.substr(st, d == std::wstring::npos ? std::wstring::npos : d - st));
        if (d == std::wstring::npos) break;
        st = d + 1;
    }
    std::wstring s = parts.empty() ? h : parts[0];
    bool numeric = !s.empty() && iswdigit(s[0]);
    static const wchar_t* generic[] = { L"ygg", L"yggdrasil", L"peer", L"peers", L"node", L"public", L"y" };
    for (auto g : generic) if (_wcsicmp(s.c_str(), g) == 0 && parts.size() > 2) { s = parts[1]; break; }
    if (numeric) s = h; // IP-адрес - показываем целиком
    if (s.size() > 14) s = s.substr(0, 14);
    for (auto& c : s) c = towupper(c);
    return s;
}

std::wstring CountryRu(const std::wstring& slug) {
    static const std::pair<const wchar_t*, const wchar_t*> t[] = {
        {L"armenia", L"Армения"}, {L"australia", L"Австралия"}, {L"austria", L"Австрия"}, {L"brazil", L"Бразилия"},
        {L"canada", L"Канада"}, {L"chile", L"Чили"}, {L"czechia", L"Чехия"}, {L"finland", L"Финляндия"},
        {L"france", L"Франция"}, {L"germany", L"Германия"}, {L"hong-kong", L"Гонконг"}, {L"hungary", L"Венгрия"},
        {L"india", L"Индия"}, {L"indonesia", L"Индонезия"}, {L"japan", L"Япония"}, {L"latvia", L"Латвия"},
        {L"luxembourg", L"Люксембург"}, {L"moldova", L"Молдова"}, {L"netherlands", L"Нидерланды"},
        {L"new-zealand", L"Новая Зеландия"}, {L"norway", L"Норвегия"}, {L"philippines", L"Филиппины"},
        {L"poland", L"Польша"}, {L"romania", L"Румыния"}, {L"russia", L"Россия"}, {L"saudi-arabia", L"Сауд. Аравия"},
        {L"singapore", L"Сингапур"}, {L"slovakia", L"Словакия"}, {L"south-africa", L"ЮАР"}, {L"spain", L"Испания"},
        {L"sweden", L"Швеция"}, {L"switzerland", L"Швейцария"}, {L"taiwan", L"Тайвань"}, {L"turkey", L"Турция"},
        {L"ukraine", L"Украина"}, {L"united-kingdom", L"Великобритания"}, {L"united-states", L"США"} };
    for (auto& p : t) if (slug == p.first) return p.second;
    return slug;
}

// ------------------------------------------------------------------
// Данные
// ------------------------------------------------------------------
struct EventItem { long long ts = 0; std::wstring level, text; };

// Сведения о задаче из самого Планировщика (COM): когда следующий запуск,
// идёт ли сейчас, чем кончился прошлый. Без них таймер мог только гадать
// «последняя проверка + 3 минуты».
struct TaskInfo {
    bool ok = false;
    int state = 0;               // TASK_STATE: 1 выключена, 2 в очереди, 3 готова, 4 выполняется
    long long nextRun = 0, lastRun = 0;
    long lastResult = 0;
    std::wstring exe;            // что задача запускает (первое действие «запуск программы»)
};

// Время из Планировщика (локальное, OLE DATE) -> unix
static long long OleDateToUnix(DATE d) {
    if (d < 1.0) return 0;
    SYSTEMTIME st;
    if (!VariantTimeToSystemTime(d, &st)) return 0;
    FILETIME lft, ft;
    if (!SystemTimeToFileTime(&st, &lft) || !LocalFileTimeToFileTime(&lft, &ft)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    long long t = (long long)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
    return t < 1000000000LL ? 0 : t;   // «ни разу» Планировщик отдаёт как 1999 год
}

// GUID-ы свои, чтобы не зависеть от taskschd.lib / libtaskschd.a
static const GUID kClsidTaskScheduler = { 0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };
static const GUID kIidTaskService     = { 0x2faba4c7, 0x4da9, 0x4013, { 0x96, 0x97, 0x20, 0xcc, 0x3f, 0xd4, 0x0f, 0x85 } };
static const GUID kIidExecAction      = { 0x4c3d624d, 0xfd6b, 0x49a3, { 0xb9, 0xb7, 0x09, 0xcb, 0x3c, 0xd3, 0xf0, 0x47 } };

// Вызывать в потоке, где уже сделан CoInitializeEx
TaskInfo QueryTask(const wchar_t* name) {
    TaskInfo ti;
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(kClsidTaskScheduler, nullptr, CLSCTX_INPROC_SERVER, kIidTaskService, (void**)&svc)) || !svc)
        return ti;
    VARIANT empty;
    VariantInit(&empty);
    if (SUCCEEDED(svc->Connect(empty, empty, empty, empty))) {
        BSTR rootName = SysAllocString(L"\\");
        ITaskFolder* root = nullptr;
        if (SUCCEEDED(svc->GetFolder(rootName, &root)) && root) {
            BSTR taskName = SysAllocString(name);
            IRegisteredTask* task = nullptr;
            if (SUCCEEDED(root->GetTask(taskName, &task)) && task) {
                ti.ok = true;
                TASK_STATE st;
                if (SUCCEEDED(task->get_State(&st))) ti.state = (int)st;
                DATE d = 0;
                if (SUCCEEDED(task->get_NextRunTime(&d))) ti.nextRun = OleDateToUnix(d);
                if (SUCCEEDED(task->get_LastRunTime(&d))) ti.lastRun = OleDateToUnix(d);
                LONG lr = 0;
                if (SUCCEEDED(task->get_LastTaskResult(&lr))) ti.lastResult = lr;
                ITaskDefinition* def = nullptr;
                if (SUCCEEDED(task->get_Definition(&def)) && def) {
                    IActionCollection* acts = nullptr;
                    if (SUCCEEDED(def->get_Actions(&acts)) && acts) {
                        IAction* act = nullptr;
                        if (SUCCEEDED(acts->get_Item(1, &act)) && act) {
                            IExecAction* ex = nullptr;
                            if (SUCCEEDED(act->QueryInterface(kIidExecAction, (void**)&ex)) && ex) {
                                BSTR path = nullptr;
                                if (SUCCEEDED(ex->get_Path(&path)) && path) {
                                    wchar_t buf[MAX_PATH * 2];
                                    ExpandEnvironmentStringsW(path, buf, MAX_PATH * 2);
                                    ti.exe = buf;
                                    if (ti.exe.size() >= 2 && ti.exe.front() == L'"') ti.exe = ti.exe.substr(1, ti.exe.size() - 2);
                                    SysFreeString(path);
                                }
                                ex->Release();
                            }
                            act->Release();
                        }
                        acts->Release();
                    }
                    def->Release();
                }
                task->Release();
            }
            SysFreeString(taskName);
            root->Release();
        }
        SysFreeString(rootName);
    }
    svc->Release();
    return ti;
}

// Код завершения прошлого запуска -> текст (пусто = всё нормально)
std::wstring TaskResultText(long code) {
    switch ((unsigned long)code) {
    case 0: case 0x41301: case 0x41303: return L"";          // успех / идёт сейчас / ещё не запускалась
    case 0x41306: return L"прервана (лимит времени или вручную)";
    case 0x800710E0: return L"запуск отклонён: прошлая проверка ещё шла";
    default: {
        wchar_t b[32];
        if ((unsigned long)code > 0xFFFF) swprintf(b, 32, L"ошибка 0x%08lX", (unsigned long)code);
        else swprintf(b, 32, L"код выхода %ld", code);
        return b;
    }
    }
}
struct LivePeer { std::wstring uri; bool up = false; double rttMs = -1; std::wstring lastError; };

struct Snapshot {
    bool statusOk = false;
    json::Value status;
    std::vector<EventItem> events;     // новые первыми
    std::vector<LivePeer> peers;
    bool yggOk = false;                // yggdrasilctl ответил
    std::wstring service = L"—";       // состояние службы Yggdrasil
    std::wstring selfAddress;          // адрес этого узла
    bool taskExists = false;           // задача YggWatchdogTick есть
    std::wstring taskExe;              // какой exe она запускает
    TaskInfo task;                     // данные Планировщика о задаче
    long long loadedAt = 0;
};

// Фоновый поток (чистый Win32 - без std::thread, чтобы собиралось любым
// MinGW, в том числе с моделью потоков win32): раз в 5 секунд или по
// запросу перечитывает данные и шлёт окну WM_APP_DATA.
class DataSource {
public:
    SRWLOCK lock = SRWLOCK_INIT;
    Snapshot snap;
    volatile LONG stop = 0;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE thread = nullptr;
    HWND notify = nullptr;

    void Start(HWND h) {
        notify = h;
        thread = CreateThread(nullptr, 0, &DataSource::ThreadProc, this, 0, nullptr);
    }
    void Stop() {
        InterlockedExchange(&stop, 1);
        SetEvent(wake);
        if (thread) { WaitForSingleObject(thread, 30000); CloseHandle(thread); thread = nullptr; }
    }
    void RefreshNow() { SetEvent(wake); }
    Snapshot Get() {
        AcquireSRWLockShared(&lock);
        Snapshot copy = snap;
        ReleaseSRWLockShared(&lock);
        return copy;
    }

private:
    static DWORD WINAPI ThreadProc(LPVOID p) { ((DataSource*)p)->Loop(); return 0; }

    void Loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // для QueryTask (COM Планировщика)
        while (!stop) {
            Snapshot s = Load();
            AcquireSRWLockExclusive(&lock);
            snap = s;
            ReleaseSRWLockExclusive(&lock);
            PostMessageW(notify, WM_APP_DATA, 0, 0);
            WaitForSingleObject(wake, 5000);
        }
        CoUninitialize();
    }

    Snapshot Load() {
        Snapshot s;
        s.loadedAt = (long long)time(nullptr);
        std::string st = RunCapture(SelfExe(), L"status-json");
        s.statusOk = json::Parse(st, s.status) && s.status.isObject();

        // events.log: unix<TAB>уровень<TAB>текст; берём хвост
        std::string ev = ReadFileBytes(ExeDir() + L"events.log");
        std::wstring evw = json::Parser::FromUtf8(ev);
        std::wstringstream ss(evw);
        std::wstring line;
        std::vector<EventItem> all;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            size_t t1 = line.find(L'\t');
            if (t1 == std::wstring::npos) continue;
            size_t t2 = line.find(L'\t', t1 + 1);
            if (t2 == std::wstring::npos) continue;
            EventItem e;
            try { e.ts = std::stoll(line.substr(0, t1)); } catch (...) { continue; }
            e.level = line.substr(t1 + 1, t2 - t1 - 1);
            e.text = line.substr(t2 + 1);
            all.push_back(e);
        }
        for (size_t i = all.size(); i > 0 && s.events.size() < 500; --i) s.events.push_back(all[i - 1]);

        // служба Yggdrasil
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenServiceW(scm, L"Yggdrasil", SERVICE_QUERY_STATUS);
            if (svc) {
                SERVICE_STATUS ss = {};
                if (QueryServiceStatus(svc, &ss))
                    s.service = ss.dwCurrentState == SERVICE_RUNNING ? L"работает" :
                                ss.dwCurrentState == SERVICE_STOPPED ? L"остановлена" :
                                ss.dwCurrentState == SERVICE_START_PENDING ? L"запускается" :
                                ss.dwCurrentState == SERVICE_STOP_PENDING ? L"останавливается" : L"меняет состояние";
                CloseServiceHandle(svc);
            } else {
                s.service = L"не найдена";
            }
            CloseServiceHandle(scm);
        }
        json::Value self;
        if (json::Parse(RunCapture(YGGCTL_PATH, std::wstring(YGGCTL_ENDPOINT) + L" -json getSelf", 5000), self))
            s.selfAddress = self[L"address"].str();

        // Задача Планировщика: путь берём у самого Планировщика (ниже, QueryTask);
        // XML в System32\\Tasks (UTF-16 или UTF-8) - только запасной вариант.
        std::string tx = ReadFileBytes(std::wstring(L"C:\\Windows\\System32\\Tasks\\") + TASK_NAME);
        if (tx.size() > 2) {
            s.taskExists = true;
            std::wstring xml;
            bool utf16 = ((unsigned char)tx[0] == 0xFF && (unsigned char)tx[1] == 0xFE) || tx[1] == 0;
            if (utf16) {
                size_t start = ((unsigned char)tx[0] == 0xFF) ? 2 : 0;
                for (size_t i = start; i + 1 < tx.size(); i += 2)
                    xml += (wchar_t)((unsigned char)tx[i] | ((unsigned char)tx[i + 1] << 8));
            } else xml = json::Parser::FromUtf8(tx);
            size_t a = xml.find(L"<Command>"), b = xml.find(L"</Command>");
            if (a != std::wstring::npos && b != std::wstring::npos && b > a) {
                s.taskExe = xml.substr(a + 9, b - a - 9);
                if (s.taskExe.size() >= 2 && s.taskExe.front() == L'"') s.taskExe = s.taskExe.substr(1, s.taskExe.size() - 2);
            }
        }

        s.task = QueryTask(TASK_NAME);
        if (s.task.ok) s.taskExists = true;
        if (!s.task.exe.empty()) s.taskExe = s.task.exe;

        // живые пиры
        std::string pj = RunCapture(YGGCTL_PATH, std::wstring(YGGCTL_ENDPOINT) + L" -json getPeers", 8000);
        json::Value pv;
        if (json::Parse(pj, pv) && pv[L"peers"].isArray()) {
            s.yggOk = true;
            const auto& arr = pv[L"peers"];
            for (size_t i = 0; i < arr.size(); ++i) {
                LivePeer p;
                p.uri = arr[i][L"remote"].str();
                p.up = arr[i][L"up"].boolean();
                double lat = arr[i][L"latency"].num(-1);
                p.rttMs = (p.up && lat >= 0) ? lat / 1e6 : -1;
                p.lastError = arr[i][L"last_error"].str();
                if (!p.uri.empty()) s.peers.push_back(p);
            }
        }
        return s;
    }
};

// ------------------------------------------------------------------
// Окно
// ------------------------------------------------------------------
enum Screen { SCR_MAIN, SCR_PEERS, SCR_REGION, SCR_DEVICES, SCR_LOG, SCR_CONTROL, SCR_CATALOG };

enum {
    BTN_CHECK = 1,
    NAV_FIRST = 10,                       // NAV_FIRST + Screen
    BTN_ADD_PEER = 20, BTN_DEL_PEER, BTN_RESTART_NOW, BTN_RESTART_LATER,
    BTN_REGION_ON = 30, BTN_REGION_OFF, BTN_HOME_SET, BTN_HOME_CLEAR,
    BTN_DEV_ADD = 40, BTN_DEV_DEL,
    BTN_TASK_INSTALL = 50, BTN_TASK_REMOVE, BTN_UPD_CHECK, BTN_UPD_APPLY, BTN_VPN_PICK, BTN_VPN_CLEAR, BTN_ANIM,
    BTN_TG_SAVE, BTN_TG_TEST,
    BTN_HEADER_TASK = 60,
    BTN_CAT_ADD = 70, BTN_CAT_REFRESH, BTN_CAT_VERIFY,
    BTN_TRAY_AUTO = 75, BTN_TRAY_POPUPS,
    BTN_CONFIRM_YES = 90, BTN_CONFIRM_NO = 91,
};

// Команды, чей результат надо особо обработать
enum { CMD_GENERIC = 0, CMD_ADD_PEER, CMD_ADD_DEVICE, CMD_TICK, CMD_REGION, CMD_VERIFY };

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// Ответ ygg_watchdog.exe -> понятные строки (без меток времени "[...] INFO:")
std::vector<std::wstring> OutputLines(const std::string& utf8) {
    std::vector<std::wstring> out;
    std::wstringstream ss(json::Parser::FromUtf8(utf8));
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && line[0] == L'[') {
            size_t close = line.find(L"] ");
            if (close != std::wstring::npos) line = line.substr(close + 2);
            for (const wchar_t* pre : { L"INFO: ", L"ERROR: ", L"DEBUG: " })
                if (line.rfind(pre, 0) == 0) { if (std::wstring(pre) == L"ERROR: ") line = L"Ошибка: " + line.substr(7); else line = line.substr(wcslen(pre)); }
        }
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

struct Job { HWND hwnd; std::wstring exe, args; int tag; UINT msg; DWORD timeout; };

DWORD WINAPI JobProc(LPVOID p) {
    Job* j = (Job*)p;
    std::string* out = new std::string(RunCapture(j->exe, j->args, j->timeout));
    PostMessageW(j->hwnd, j->msg, (WPARAM)j->tag, (LPARAM)out);
    delete j;
    return 0;
}

void StartJob(HWND hwnd, const std::wstring& exe, const std::wstring& args, int tag, UINT msg, DWORD timeout) {
    Job* j = new Job{ hwnd, exe, args, tag, msg, timeout };
    HANDLE th = CreateThread(nullptr, 0, JobProc, j, 0, nullptr);
    if (th) CloseHandle(th); else delete j;
}

bool PickScriptFile(HWND owner, std::wstring& path) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Скрипты и программы (*.bat;*.cmd;*.ps1;*.exe)\0*.bat;*.cmd;*.ps1;*.exe\0Все файлы\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Скрипт перезапуска VPN";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    path = buf;
    return true;
}

struct App {
    HWND hwnd = nullptr;
    Renderer r;
    DataSource data;
    Snapshot snap;
    int screen = SCR_MAIN;
    bool animate = true;
    float sweep = 0;                 // угол луча радара, градусы
    float radarScale = 0;            // мс на внешнем кольце (плавно догоняет цель)
    std::vector<HitBox> hits;
    int hoverId = 0, pressId = 0;
    long long checkRequestedAt = 0;  // идёт ручная проверка (tick)
    long long tickBeforeCheck = 0;
    std::wstring flash;              // сообщение в подвале
    unsigned flashColor = 0;
    long long flashUntil = 0;

    // фоновые команды
    bool busy = false;
    std::wstring busyLabel;

    // подтверждение опасных действий
    bool confirmOpen = false;
    std::wstring confirmText, confirmYes;
    std::function<void()> confirmAction;

    // виджеты
    TextBox tbPeer, tbCity, tbDevName, tbDevHost, tbTgToken, tbTgChat;
    ListBox lbMain, lbLive, lbCountries, lbCities, lbDevices, lbLog, lbCatCountries, lbCatCities, lbCatalog;
    TextBox* focusTb = nullptr;
    ListBox* focusLb = nullptr;

    // регион: справочники
    bool countriesLoading = false, citiesLoading = false;
    std::wstring citiesFor;
    std::vector<ListItem> citiesAll;

    std::vector<float> logHts;       // высоты строк журнала (с переносом)
    long long logHtsTop = -1;

    // каталог публичных пиров
    bool catalogLoading = false;
    std::wstring catalogFor;
    struct CatPeer { std::wstring uri, city, err; double ms = -1; };
    std::vector<CatPeer> catalog;
    struct Verified { bool up = false; double ms = -1; std::wstring text; };
    std::map<std::wstring, Verified> verified;   // итоги «Проверить пир»
    std::wstring verifyingUri;

    App() {
        tbPeer.id = 100; tbPeer.placeholder = L"tls://host:port";
        tbCity.id = 101; tbCity.placeholder = L"поиск города…";
        tbDevName.id = 102;
        tbDevHost.id = 103;
        tbTgToken.id = 104; tbTgToken.password = true;
        tbTgChat.id = 105;
        lbMain.id = 200; lbMain.emptyText = L"в конфиге нет пиров";
        lbLive.id = 201; lbLive.emptyText = L"yggdrasilctl не отвечает или пиров нет";
        lbCountries.id = 202; lbCountries.emptyText = L"загружаю список стран…";
        lbCities.id = 203; lbCities.emptyText = L"выбери страну слева";
        lbDevices.id = 204; lbDevices.emptyText = L"устройств нет - добавь справа";
        lbLog.id = 205; lbLog.emptyText = L"событий пока нет";
        lbCatCountries.id = 206; lbCatCountries.emptyText = L"загружаю список стран…";
        lbCatCities.id = 208; lbCatCities.emptyText = L"выбери страну слева";
        lbCatalog.id = 207; lbCatalog.emptyText = L"выбери страну слева";
    }

    // ---------- производные значения ----------
    std::wstring Phase() const { return snap.status[L"last_phase"].str(); }
    long long LastTick() const { return snap.status[L"last_tick_at"].i64(); }

    bool Stale() const { // сторож давно не проверял - задача, похоже, не работает
        long long lt = LastTick();
        return lt > 0 && (long long)time(nullptr) - lt > 10 * 60;
    }

    Theme CurrentTheme() const {
        if (!snap.statusOk) return ThemeGray();
        std::wstring p = Phase();
        // Зелёный - связь есть (через основные ИЛИ через резерв);
        // янтарный - связи нет и сторож ищет замену / перебирает
        if (p == L"ok" || p == L"backup") return ThemeGreen();
        if (p == L"crisis" || p == L"down") return ThemeAmber();
        if (p == L"no_internet" || p == L"battery") return ThemeRed();
        // Региональный режим - те же цвета: связь через регион есть - зелёный,
        // ищет регионального пира - янтарный. Голубой оставлен панели Yggdrasil.
        if (p == L"regional") return ThemeGreen();
        if (p == L"regional_search") return ThemeAmber();
        return ThemeGray();
    }

    std::vector<std::wstring> MainPeersUnique() const {
        std::vector<std::wstring> v;
        const auto& a = snap.status[L"main_peers"];
        for (size_t i = 0; i < a.size(); ++i) {
            std::wstring u = a[i].str();
            if (std::find(v.begin(), v.end(), u) == v.end()) v.push_back(u);
        }
        return v;
    }
    const LivePeer* FindLive(const std::wstring& uri) const {
        for (const auto& p : snap.peers) if (p.uri == uri) return &p;
        return nullptr;
    }
    // Разбор "почему нет связи" из status-json (поле diagnosis)
    std::wstring DiagPeerText(const std::wstring& uri) const {
        const auto& a = snap.status[L"diagnosis"][L"peers"];
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i][L"uri"].str() == uri && a[i][L"kind"].str() != L"up") return a[i][L"text"].str();
        return L"";
    }
    bool DiagHasProblem() const {
        const auto& d = snap.status[L"diagnosis"];
        if (d.isNull() || d[L"verdict"].str().empty()) return false;
        if (!d[L"any_up"].boolean()) return true;
        return !d[L"advice"].str().empty(); // связь есть, но часть основных мертва
    }

    bool InList(const wchar_t* key, const std::wstring& uri) const {
        const auto& a = snap.status[key];
        for (size_t i = 0; i < a.size(); ++i) if (a[i].str() == uri) return true;
        return false;
    }

    void DrawHeader(const Theme& th) {
        std::wstring ver = snap.statusOk ? snap.status[L"version"].str() : L"?";
        r.Text(L"YGG-WATCHDOG", 24, 14, 260, 40, Rgb(th.text), 26, true);
        std::wstring sub = L"//  v" + ver;
        if (snap.statusOk && snap.status[L"mode"].str() == L"regional") sub = L"//  v" + ver + L"  ·  режим: регион";
        r.Text(sub, 250, 18, 500, 30, Rgb(th.accent, 0.9f), 12);
        r.Line(24, 58, 290, 58, Rgb(th.accent, 0.8f), 2);
        r.Line(290, 58, 1016, 58, Rgb(th.accent, 0.25f), 1);

        long long now = (long long)time(nullptr);
        r.Text(HhMm(now, true), 760, 10, 256, 22, Rgb(th.muted), 12, false, Right);
        bool alive = snap.statusOk && LastTick() > 0 && !Stale();
        std::wstring task = std::wstring(L"TASK  ") + TASK_NAME;
        unsigned dot = alive ? th.accent : COL_ALERT;
        bool foreign = snap.taskExists && !snap.taskExe.empty() &&
                       _wcsicmp(snap.taskExe.c_str(), (SelfExe()).c_str()) != 0;
        if (!snap.taskExists) { task = L"задача не установлена"; dot = COL_ALERT; }
        else if (foreign) { task = L"задача запускает ygg-watchdog из другой папки"; dot = COL_WARN; }
        bool hov = hoverId == BTN_HEADER_TASK;
        r.Text(task, 600, 30, 400, 22, Rgb(foreign || !snap.taskExists ? dot : th.accent, hov ? 1.0f : 0.85f), 12, false, Right);
        r.Disc(1010, 41, 4, Rgb(dot));
        if (!confirmOpen) {
            float tw = r.TextWidth(task, 12);
            hits.push_back({ 1000 - tw, 30, tw + 16, 22, BTN_HEADER_TASK }); // клик -> экран «Управление»
        }
    }

    void Row(const Theme& th, float x, float y, float w, const std::wstring& k, const std::wstring& v, D2D1_COLOR_F vc) {
        r.Text(k, x, y, w, 22, Rgb(th.muted), 12);
        r.Text(v, x, y, w, 22, vc, 12, false, Right);
    }

    void BigTitle(const Theme& th, float x, float y, std::wstring l1, std::wstring l2) {
        r.Text(l1, x, y, 240, 40, Rgb(th.accent), 30, false);
        r.Text(l2, x, y + 38, 240, 32, Rgb(th.accent, 0.9f), 22, false);
    }

    void DrawStatePanel(const Theme& th, float x, float y, float w, float h) {
        r.Panel(x, y, w, h, th, L"СОСТОЯНИЕ");
        float cx = x + 16, cw = w - 32;
        const auto& st = snap.status;
        std::wstring ph = Phase();

        if (!snap.statusOk) {
            BigTitle(th, cx, y + 40, L"НЕТ", L"ДАННЫХ");
            r.TextWrapped(L"Команда status-json не ответила. Подробности - в watchdog.log рядом с программой.",
                          cx, y + 130, cw, Rgb(th.muted), 12);
            return;
        }
        if (ph == L"ok") BigTitle(th, cx, y + 40, L"ВСЁ", L"В ПОРЯДКЕ");
        else if (ph == L"backup") BigTitle(th, cx, y + 40, L"СВЯЗЬ ЕСТЬ", L"ЧЕРЕЗ РЕЗЕРВ");
        else if (ph == L"down") BigTitle(th, cx, y + 40, L"НЕТ ПИРОВ", L"ИЩУ ЗАМЕНУ");
        else if (ph == L"crisis") BigTitle(th, cx, y + 40, L"КРАЙНИЙ", L"ПЕРЕБОР");
        else if (ph == L"no_internet") BigTitle(th, cx, y + 40, L"НЕТ", L"ИНТЕРНЕТА");
        else if (ph == L"battery") {
            int pct = st[L"battery"].isNull() ? -1 : (int)st[L"battery"][L"percent"].num(-1);
            BigTitle(th, cx, y + 40, L"БАТАРЕЯ", pct >= 0 ? std::to_wstring(pct) + L"%" : L"");
        }
        else if (ph == L"regional") BigTitle(th, cx, y + 40, L"РЕГИОН", L"ЗАКРЕПЛЁН");
        else if (ph == L"regional_search") BigTitle(th, cx, y + 40, L"РЕГИОН", L"ИЩУ ПИР");
        else if (LastTick() == 0) BigTitle(th, cx, y + 40, L"ЕЩЁ НЕ", L"ПРОВЕРЯЛ");
        else BigTitle(th, cx, y + 40, L"ОШИБКА", L"СМОТРИ ЛОГ");

        r.Line(cx, y + 122, cx + cw, y + 122, Rgb(th.accent, 0.2f));

        float ry = y + 132;
        bool regional = st[L"mode"].str() == L"regional";
        Row(th, cx, ry, cw, L"режим", regional ? L"региональный" : L"обычный", Rgb(th.text)); ry += 26;

        if (regional) {
            const auto& rp = st[L"regional_peers"];
            int up = 0;
            for (size_t i = 0; i < rp.size(); ++i) { auto* lp = FindLive(rp[i].str()); if (lp && lp->up) ++up; }
            Row(th, cx, ry, cw, L"региональные", std::to_wstring(up) + L" / " + std::to_wstring(rp.size()),
                Rgb(up > 0 ? COL_OK : COL_ALERT)); ry += 26;
            Row(th, cx, ry, cw, L"регион", CountryRu(st[L"regional_country"].str()), Rgb(th.text)); ry += 26;
            Row(th, cx, ry, cw, L"основные", L"не проверяю", Rgb(th.muted)); ry += 26;
        } else {
            auto mains = MainPeersUnique();
            int up = 0;
            for (auto& u : mains) { auto* lp = FindLive(u); if (lp && lp->up) ++up; }
            Row(th, cx, ry, cw, L"основные пиры", std::to_wstring(up) + L" / " + std::to_wstring(mains.size()),
                Rgb(up > 0 ? COL_OK : COL_ALERT)); ry += 26;
            size_t nb = st[L"backup_peers"].size();
            if (nb > 0) {
                std::wstring reg = st[L"backup_region"].str();
                Row(th, cx, ry, cw, L"резервные", std::to_wstring(nb) + (reg.empty() ? L"" : L"  (" + CountryRu(reg) + L")"),
                    Rgb(COL_WARN)); ry += 26;
            }
            std::wstring egress = st[L"egress_region"].str(), home = st[L"home_region"].str();
            std::wstring search = !egress.empty() ? CountryRu(egress) + L" · выход трафика"
                                : !home.empty() ? CountryRu(home) + L" · домашний" : L"не определён";
            Row(th, cx, ry, cw, L"регион поиска", search, Rgb(th.text)); ry += 26;
        }

        long long downSince = st[L"internet_down_since"].i64();
        Row(th, cx, ry, cw, L"интернет", downSince ? L"нет с " + HhMm(downSince) : L"есть",
            Rgb(downSince ? COL_ALERT : COL_OK)); ry += 26;

        const auto& devs = st[L"devices"];
        if (devs.size() > 0) {
            int ok = 0;
            for (size_t i = 0; i < devs.size(); ++i) if (devs[i][L"down_since"].i64() == 0) ++ok;
            Row(th, cx, ry, cw, L"устройства", std::to_wstring(ok) + L" / " + std::to_wstring(devs.size()),
                Rgb(ok == (int)devs.size() ? COL_OK : COL_ALERT)); ry += 26;
        }
        if (!st[L"battery"].isNull()) {
            int pct = (int)st[L"battery"][L"percent"].num();
            bool ch = st[L"battery"][L"charging"].boolean();
            Row(th, cx, ry, cw, L"батарея", std::to_wstring(pct) + L"%  " + (ch ? L"заряд" : L"от батареи"),
                Rgb(pct <= 10 && !ch ? COL_ALERT : (ch ? COL_OK : th.text))); ry += 26;
        }

        // следующая проверка
        float ny = y + h - 154;
        r.Text(L"СЛЕДУЮЩАЯ ПРОВЕРКА", cx, ny, cw, 18, Rgb(th.accent, 0.85f), 11);
        long long now = (long long)time(nullptr);
        long long interval = st[L"tick_interval_sec"].i64(180);
        long long next = LastTick() > 0 ? LastTick() + interval : 0;
        std::wstring cd;
        float frac = 0;
        bool foreignTask = snap.taskExists && !snap.taskExe.empty() &&
                           _wcsicmp(snap.taskExe.c_str(), (SelfExe()).c_str()) != 0;
        const TaskInfo& ti = snap.task;
        unsigned cdColor = th.text;
        auto countdown = [&](long long target) {
            long long left = target - now;
            if (left < 0) left = 0;
            wchar_t b[16];
            swprintf(b, 16, L"%02lld:%02lld", left / 60, left % 60);
            cd = b;
            frac = 1.0f - (float)left / (float)interval;
            if (frac < 0) frac = 0;
        };
        if (checkRequestedAt) cd = L"идёт…";
        else if (!snap.taskExists) cd = L"нет задачи";
        else if (foreignTask) cd = L"не здесь";
        else if (ti.ok && ti.state == 1) { cd = L"выключена"; cdColor = COL_ALERT; }
        else if (ti.ok && ti.state == 4) {
            long long mins = ti.lastRun ? (now - ti.lastRun) / 60 : 0;
            if (mins >= 10) { cd = L"висит " + std::to_wstring(mins) + L" мин"; cdColor = COL_ALERT; }
            else { cd = L"проверяю…"; frac = 1; }
        }
        else if (ti.ok && ti.nextRun > 0) {
            if (ti.nextRun - now < -30) cd = L"скоро…";   // Планировщик запаздывает (сон, пробуждение)
            else countdown(ti.nextRun);
        }
        // Планировщик не ответил - по-старому, от последней проверки
        else if (next == 0) cd = L"—";
        else if (Stale()) cd = L"задача молчит";
        else if (next - now < -30) cd = L"скоро…";
        else countdown(next);
        r.Text(cd, cx, ny + 18, cw, 40, Rgb(cdColor), 26);
        r.Progress(cx, ny + 64, cw, 8, frac, th.accent);
        if (LastTick() > 0) {
            std::wstring last = L"последняя: " + HhMm(LastTick());
            std::wstring err = ti.ok ? TaskResultText(ti.lastResult) : L"";
            r.Text(last, cx, ny + 76, cw, 18, Rgb(th.muted), 11);
            if (!err.empty()) r.Text(L"запуск: " + err, cx, ny + 76, cw, 18, Rgb(COL_ALERT), 11, false, Right);
        }

        float bx = cx, by = y + h - 56, bw = cw, bh = 38;
        if (checkRequestedAt) r.Button(bx, by, bw, bh, L"ПРОВЕРЯЮ…", th.accent, true, false, false);
        else Btn(BTN_CHECK, bx, by, bw, bh, L"ПРОВЕРИТЬ СЕЙЧАС  [T]", th.accent, true);
    }

    // Радар: расстояние от центра = задержка (логарифмически), угол - от имени пира
    void DrawRadar(const Theme& th, float x, float y, float w, float h) {
        r.Panel(x, y, w, h, th, L"РАДАР ПИРОВ  //  расстояние = задержка");
        float cx = x + w / 2, cy = y + h / 2 + 12;
        float R = (std::min)(w, h) / 2 - 40;
        // Автомасштаб: внешнее кольцо = "круглое" значение чуть больше самой
        // большой задержки живых пиров. Шкала корневая (расстояние ~ sqrt),
        // чтобы 3, 8 и 15 мс не слипались у центра, а 300 и 900 мс не
        // упирались в край. Смена масштаба - плавная.
        static const double ladder[] = { 10, 20, 30, 50, 75, 100, 150, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 5000, 10000 };
        double maxRtt = 0;
        for (const auto& p : snap.peers) if (p.up && p.rttMs > maxRtt) maxRtt = p.rttMs;
        double target = ladder[sizeof(ladder) / sizeof(ladder[0]) - 1];
        for (double v : ladder) if (v >= maxRtt * 1.25 && v >= 30) { target = v; break; }
        if (radarScale <= 0) radarScale = (float)target;
        else radarScale = (float)exp(log(radarScale) + (log(target) - log(radarScale)) * 0.15);
        const float INNER = 0.14f; // свободный круг вокруг центра под его подпись
        auto distFor = [&](double ms) {
            double f = sqrt((std::max)(0.0, ms) / radarScale);
            if (f > 1.0) f = 1.0;
            return R * (INNER + (1 - INNER) * (float)f);
        };
        auto fmtMs = [](double v) {
            wchar_t b[32];
            if (v < 10) swprintf(b, 32, L"%.1f мс", v); else swprintf(b, 32, L"%.0f мс", v);
            return std::wstring(b);
        };
        for (int i = 1; i <= 3; ++i) r.Circle(cx, cy, R * (INNER + (1 - INNER) * i / 3.0f), Rgb(th.accent, i == 3 ? 0.30f : 0.22f));
        for (int a = 0; a < 360; a += 30) {
            float rad = a * 3.14159265f / 180;
            r.Line(cx, cy, cx + R * cosf(rad), cy + R * sinf(rad), Rgb(th.accent, 0.07f));
        }
        for (int i = 1; i <= 3; ++i) {  // подписи колец: f=i/3 -> мс = scale*f^2
            double f = i / 3.0;
            float rr = R * (INNER + (1 - INNER) * (float)f);
            r.Text(fmtMs(radarScale * f * f), cx + rr + 4, cy - 14, 70, 14, Rgb(th.muted, 0.75f), 9);
        }

        // луч
        if (animate) {
            // хвост луча - плавно гаснущие сектора, сам луч - яркая линия
            const int N = 12;
            for (int i = 0; i < N; ++i) {
                float a1 = sweep - i * 3.0f, a0 = a1 - 3.0f;
                r.Wedge(cx, cy, R, a0, a1, Rgb(th.accent, 0.16f * (1.0f - (float)i / N)));
            }
            float a = sweep * 3.14159265f / 180;
            r.Line(cx, cy, cx + R * cosf(a), cy + R * sinf(a), Rgb(th.accent, 0.9f), 2);
        }

        // центр - мы
        r.Disc(cx, cy, 5, Rgb(th.text));
        r.GlowCircle(cx, cy, 6, th.accent);

        if (!snap.yggOk) {
            r.Text(L"yggdrasilctl не отвечает", x, cy + R + 4, w, 20, Rgb(COL_ALERT), 11, false, Center);
            return;
        }
        for (const auto& p : snap.peers) {
            unsigned h32 = 2166136261u;
            for (wchar_t c : p.uri) { h32 ^= (unsigned)c; h32 *= 16777619u; }
            float ang = (h32 % 360) * 3.14159265f / 180;
            // живые - по задержке; мёртвые - крестиком на внешнем кольце
            float dist = (p.up && p.rttMs >= 0) ? distFor(p.rttMs) : R;
            float px = cx + dist * cosf(ang), py = cy + dist * sinf(ang);
            bool isBackup = InList(L"backup_peers", p.uri);
            std::wstring label = ShortName(p.uri);
            if (p.up) {
                unsigned col = isBackup ? COL_PINK : th.accent;
                r.GlowCircle(px, py, 6, col);
                r.Circle(px, py, 6, Rgb(col), 1.5f);
                r.Disc(px, py, 3, Rgb(col));
                wchar_t b[32];
                swprintf(b, 32, L"%.0f мс", p.rttMs);
                r.Text((isBackup ? L"РЕЗЕРВ " : L"") + label, px + 10, py - 14, 170, 14, Rgb(col), 10);
                r.Text(b, px + 10, py, 120, 14, Rgb(th.muted), 9);
            } else {
                float s = 5;
                r.Line(px - s, py - s, px + s, py + s, Rgb(COL_ALERT), 2);
                r.Line(px - s, py + s, px + s, py - s, Rgb(COL_ALERT), 2);
                r.Text(label, px + 10, py - 7, 170, 14, Rgb(COL_ALERT, 0.85f), 10);
            }
        }
    }

    void DrawJournal(const Theme& th, float x, float y, float w, float h) {
        r.Panel(x, y, w, h, th, L"ЖУРНАЛ");
        float ty = y + 40, bottom = y + h - 16, cw = w - 32;
        // Связи через основные нет - сверху разбор причины
        const auto& dg = snap.status[L"diagnosis"];
        std::wstring ph = Phase();
        if (DiagHasProblem() && !dg[L"any_up"].boolean() && ph != L"regional") {
            unsigned col = (ph == L"backup") ? COL_WARN : COL_ALERT;
            r.Text(ph == L"backup" ? L"ОСНОВНЫЕ ПИРЫ: ПОЧЕМУ НЕ РАБОТАЮТ" : L"ПОЧЕМУ НЕТ СВЯЗИ", x + 16, ty, cw, 16, Rgb(col), 10, true);
            ty += 18;
            ty += r.TextWrapped(dg[L"verdict"].str(), x + 16, ty, cw, Rgb(th.text), 11) + 4;
            std::wstring adv = dg[L"advice"].str();
            if (!adv.empty()) ty += r.TextWrapped(adv, x + 16, ty, cw, Rgb(th.muted), 11) + 4;
            r.Line(x + 16, ty + 4, x + 16 + cw, ty + 4, Rgb(th.accent, 0.2f));
            ty += 14;
        }
        if (snap.events.empty()) {
            r.TextWrapped(L"Событий пока нет. Сюда попадает всё, что сторож считает важным: резерв "
                          L"добавлен или убран, пропал интернет, перебор и т.д.", x + 16, ty, cw, Rgb(th.muted), 11);
            return;
        }
        for (const auto& e : snap.events) {
            unsigned col = e.level == L"ok" ? COL_OK : e.level == L"warn" ? COL_WARN :
                           e.level == L"alert" ? COL_ALERT : th.text;
            float th1 = r.TextWrapped(e.text, x + 16, 0, cw, Rgb(col), 11, false, false);
            if (ty + 16 + th1 > bottom) break;
            r.Text(HhMm(e.ts), x + 16, ty, cw, 14, Rgb(th.muted), 10, false, Left, false);
            r.TextWrapped(e.text, x + 16, ty + 15, cw, Rgb(col, 0.95f), 11);
            ty += 15 + th1 + 12;
        }
    }

    // ================== общий каркас ==================
    void Paint() {
        if (!r.EnsureTarget()) return;
        Theme th = CurrentTheme();
        r.BeginFrame(th.bg);
        hits.clear();
        r.Backdrop(th);
        DrawHeader(th);
        switch (screen) {
            case SCR_PEERS:   DrawPeersScreen(th); break;
            case SCR_REGION:  DrawRegionScreen(th); break;
            case SCR_DEVICES: DrawDevicesScreen(th); break;
            case SCR_LOG:     DrawLogScreen(th); break;
            case SCR_CONTROL: DrawControlScreen(th); break;
            case SCR_CATALOG: DrawCatalogScreen(th); break;
            default:
                DrawStatePanel(th, 24, 86, 268, 504);
                DrawRadar(th, 308, 86, 424, 504);
                DrawJournal(th, 748, 86, 268, 504);
        }
        DrawFooter(th);
        if (confirmOpen) DrawConfirm(th);
        r.EndFrame();
    }

    // Кнопка: рисует и регистрирует зону нажатия (если доступна)
    void Btn(int id, float x, float y, float w, float h, const std::wstring& label, unsigned color,
             bool filled, bool enabled = true) {
        bool blocked = !enabled || (busy && id != BTN_CONFIRM_NO) || (confirmOpen && id < BTN_CONFIRM_YES);
        if (blocked) {
            r.Button(x, y, w, h, label, 0x5A6268, false, false, false);
            return;
        }
        r.Button(x, y, w, h, label, color, filled, hoverId == id, pressId == id);
        hits.push_back({ x, y, w, h, id });
    }

    void DrawFooter(const Theme& th) {
        r.Line(24, 604, 1016, 604, Rgb(th.accent, 0.25f));
        struct Tab { int scr; const wchar_t* label; };
        static const Tab tabs[] = { { SCR_MAIN, L"[M] обзор" }, { SCR_PEERS, L"[P] пиры" }, { SCR_CATALOG, L"[C] каталог" }, { SCR_REGION, L"[R] регион" },
                                    { SCR_DEVICES, L"[D] устройства" }, { SCR_LOG, L"[L] журнал" }, { SCR_CONTROL, L"[N] управление" } };
        float x = 24;
        for (const auto& t : tabs) {
            float w = r.TextWidth(t.label, 12) + 4;
            bool active = screen == t.scr, hov = hoverId == NAV_FIRST + t.scr;
            r.Text(t.label, x, 610, w + 4, 22, Rgb(th.accent, active ? 1.0f : (hov ? 0.95f : 0.6f)), 12, active);
            if (active) r.Line(x, 632, x + w, 632, Rgb(th.accent), 2);
            if (!confirmOpen) hits.push_back({ x, 608, w, 26, NAV_FIRST + t.scr });
            x += w + 22;
        }
        // справа - что происходит / результат последнего действия
        long long now = (long long)time(nullptr);
        std::wstring msg;
        unsigned col = th.accent;
        if (busy) msg = L"выполняю: " + busyLabel + L"…";
        else if (!flash.empty() && now < flashUntil) { msg = flash; if (flashColor) col = flashColor; }
        if (!msg.empty()) {
            // длинное сообщение не наезжает на вкладки: обрезаем с конца, с многоточием
            float avail = 1016 - x;
            if (r.TextWidth(msg, 12) > avail) {
                while (msg.size() > 1 && r.TextWidth(msg + L"…", 12) > avail) msg.pop_back();
                msg += L"…";
            }
            r.Clip(x, 606, avail, 28);
            r.Text(msg, x, 610, avail, 22, Rgb(col), 12, false, Right);
            r.Unclip();
        }
    }

    void DrawConfirm(const Theme& th) {
        r.Fill(0, 0, 1040, 640, Rgb(0x000000, 0.65f));
        float w = 560, h = 210, x = (1040 - w) / 2, y = (640 - h) / 2;
        r.Fill(x, y, w, h, Rgb(th.bg, 0.97f));
        r.Brackets(x, y, w, h, Rgb(COL_WARN), 18, 2);
        r.Text(L"ПОДТВЕРДИ", x + 20, y + 14, w - 40, 20, Rgb(COL_WARN), 12);
        r.TextWrapped(confirmText, x + 20, y + 44, w - 40, Rgb(th.text), 13);
        Btn(BTN_CONFIRM_YES, x + 20, y + h - 56, 250, 38, confirmYes + L"  [Enter]", COL_WARN, true);
        Btn(BTN_CONFIRM_NO, x + w - 270, y + h - 56, 250, 38, L"ОТМЕНА  [Esc]", th.accent, false);
    }

    void Confirm(const std::wstring& text, const std::wstring& yesLabel, std::function<void()> action) {
        confirmText = text; confirmYes = yesLabel; confirmAction = action; confirmOpen = true;
        focusTb = nullptr;
        Redraw();
    }

    void Redraw() { InvalidateRect(hwnd, nullptr, FALSE); }

    void SetTb(TextBox* tb) {
        for (TextBox* t : { &tbPeer, &tbCity, &tbDevName, &tbDevHost, &tbTgToken, &tbTgChat }) t->focus = (t == tb);
        focusTb = tb;
        if (tb) focusLb = nullptr;
    }

    // Текстовое поле: рисует и регистрирует
    void Field(TextBox& tb, const Theme& th, float x, float y, float w, float h = 32) {
        tb.x = x; tb.y = y; tb.w = w; tb.h = h;
        r.DrawTextBox(tb, th);
    }
    void List(ListBox& lb, const Theme& th, float x, float y, float w, float h) {
        lb.x = x; lb.y = y; lb.w = w; lb.h = h;
        lb.Clamp();
        r.DrawList(lb, th, focusLb == &lb);
    }

    void Caption(const Theme& th, float x, float y, float w, const std::wstring& s) {
        r.Text(s, x, y, w, 18, Rgb(th.accent, 0.85f), 11);
    }
    void Note(const Theme& th, float x, float y, float w, const std::wstring& s) {
        r.TextWrapped(s, x, y, w, Rgb(th.muted), 11);
    }

    // ================== ПИРЫ ==================
    void DrawPeersScreen(const Theme& th) {
        r.Panel(24, 86, 620, 504, th, L"ОСНОВНЫЕ ПИРЫ  //  из yggdrasil.conf");
        std::vector<ListItem> items;
        const auto& mp = snap.status[L"main_peers"];
        for (size_t i = 0; i < mp.size(); ++i) {
            ListItem it;
            it.key = std::to_wstring(i + 1);
            it.left = std::to_wstring(i + 1) + L")  " + mp[i].str();
            const LivePeer* lp = FindLive(mp[i].str());
            if (lp && lp->up) { wchar_t b[32]; swprintf(b, 32, L"%.0f мс", lp->rttMs); it.right = b; it.rightColor = COL_OK; }
            else if (lp) {
                std::wstring why = DiagPeerText(mp[i].str());
                it.right = why.empty() ? L"нет связи" : why; it.rightColor = COL_ALERT;
            }
            else { it.right = L"не в работе"; }
            items.push_back(it);
        }
        lbMain.SetItems(items);
        List(lbMain, th, 40, 124, 588, 196);
        if (DiagHasProblem()) {
            const auto& dg = snap.status[L"diagnosis"];
            std::wstring t = dg[L"verdict"].str();
            if (!dg[L"advice"].str().empty()) t += L" " + dg[L"advice"].str();
            r.TextWrapped(t, 40, 328, 588, Rgb(dg[L"any_up"].boolean() ? COL_WARN : COL_ALERT), 11);
        } else {
            Note(th, 40, 328, 588, L"Порядок - как в конфиге. Изменения вступят в силу только после перезапуска службы Yggdrasil.");
        }
        Field(tbPeer, th, 40, 364, 420);
        Btn(BTN_ADD_PEER, 472, 364, 156, 32, L"ДОБАВИТЬ", th.accent, false, !tbPeer.Trimmed().empty());
        Btn(BTN_DEL_PEER, 40, 406, 260, 32, L"УДАЛИТЬ ВЫБРАННЫЙ", COL_ALERT, false, lbMain.Selected() != nullptr);
        Caption(th, 40, 456, 588, L"ПРИМЕНИТЬ ИЗМЕНЕНИЯ");
        Btn(BTN_RESTART_NOW, 40, 480, 290, 36, L"ПЕРЕЗАПУСТИТЬ СЛУЖБУ", COL_WARN, false);
        Btn(BTN_RESTART_LATER, 342, 480, 286, 36, L"ПЕРЕЗАПУСК ЧЕРЕЗ 15 С", th.accent, false);
        Note(th, 40, 528, 588, L"Если ты сидишь на этом компьютере через сам Yggdrasil по RDP - выбирай перезапуск через 15 с, чтобы команда успела уйти.");

        r.Panel(660, 86, 356, 504, th, L"СЕЙЧАС В РАБОТЕ  //  getPeers");
        std::vector<ListItem> live;
        for (const auto& p : snap.peers) {
            ListItem it;
            it.key = p.uri;
            it.left = ShortName(p.uri);
            bool isBackup = InList(L"backup_peers", p.uri), isReg = InList(L"regional_peers", p.uri);
            bool isMain = false;
            for (size_t i = 0; i < mp.size(); ++i) if (mp[i].str() == p.uri) isMain = true;
            std::wstring role = isBackup ? L"резерв" : isReg ? L"регион" : isMain ? L"основной" : L"прочий";
            if (p.up) { wchar_t b[48]; swprintf(b, 48, L"%ls  %.0f мс", role.c_str(), p.rttMs); it.right = b; it.rightColor = isBackup ? COL_PINK : COL_OK; }
            else { it.right = role + L"  нет"; it.rightColor = COL_ALERT; it.color = 0x9AA7B0; }
            live.push_back(it);
        }
        lbLive.SetItems(live);
        List(lbLive, th, 676, 124, 324, 300);
        const ListItem* sel = lbLive.Selected();
        if (sel) {
            const LivePeer* lp = FindLive(sel->key);
            std::wstring d = sel->key;
            if (lp && !lp->up && !lp->lastError.empty()) d += L"\nпоследняя ошибка: " + lp->lastError;
            r.TextWrapped(d, 676, 434, 324, Rgb(th.text), 11);
        } else {
            Note(th, 676, 434, 324, L"Выбери пир, чтобы увидеть полный адрес и последнюю ошибку.");
        }
    }

    // ================== КАТАЛОГ ==================
    // Публичные пиры из репозитория public-peers: страна -> город -> пиры.
    // Честность важнее красоты: открытый TCP-порт ещё не значит, что пир работает
    // (сервер может принять соединение и сразу оборвать). Поэтому:
    //   - пир, который уже есть в Yggdrasil (основной / резервный), показан по его
    //     настоящему состоянию из getPeers;
    //   - остальные - "порт открыт" (серым, не зелёным);
    //   - кнопка «Проверить пир» подключает его к Yggdrasil на пару секунд и
    //     показывает настоящий результат (зелёным только он).
    struct CatRow { const CatPeer* p; int rank; double ms; std::wstring right; unsigned color; bool main; };

    void DrawCatalogScreen(const Theme& th) {
        if (lbCountries.items.empty() && !countriesLoading) LoadCountries();
        r.Panel(24, 86, 220, 504, th, L"СТРАНА");
        List(lbCatCountries, th, 40, 124, 188, 450);

        // города - из заголовков файла страны
        r.Panel(256, 86, 220, 504, th, L"ГОРОД");
        {
            std::vector<ListItem> cities;
            ListItem all; all.left = L"— все —"; all.key = L""; all.color = 0x9AA7B0; all.right = std::to_wstring(catalog.size());
            cities.push_back(all);
            std::vector<std::wstring> order; std::map<std::wstring, int> cnt;
            for (const auto& p : catalog) { if (!cnt.count(p.city)) order.push_back(p.city); ++cnt[p.city]; }
            for (const auto& c : order) { ListItem it; it.key = c; it.left = c; it.right = std::to_wstring(cnt[c]); cities.push_back(it); }
            lbCatCities.SetItems(cities);
            if (lbCatCities.sel < 0 && !catalog.empty()) lbCatCities.sel = 0;
        }
        List(lbCatCities, th, 272, 124, 188, 450);

        std::wstring title = L"ПУБЛИЧНЫЕ ПИРЫ";
        if (const ListItem* c = lbCatCountries.Selected()) title += L"  //  " + c->left;
        r.Panel(488, 86, 528, 504, th, title);

        const auto& mp = snap.status[L"main_peers"];
        auto isMain = [&](const std::wstring& uri) {
            for (size_t i = 0; i < mp.size(); ++i) if (_wcsicmp(mp[i].str().c_str(), uri.c_str()) == 0) return true;
            return false;
        };
        const ListItem* cityItem = lbCatCities.Selected();
        std::wstring cityFilter = cityItem ? cityItem->key : L"";

        std::vector<CatRow> rows;
        int confirmed = 0, open = 0;
        for (const auto& p : catalog) {
            if (!cityFilter.empty() && p.city != cityFilter) continue;
            CatRow row{ &p, 3, 0, L"", th.muted, isMain(p.uri) };
            const LivePeer* lp = FindLive(p.uri);
            auto vr = verified.find(p.uri);
            if (lp) {                                   // уже в Yggdrasil: настоящее состояние
                if (lp->up) {
                    wchar_t b[48]; swprintf(b, 48, L"работает · %.0f мс", lp->rttMs);
                    row.right = b; row.color = COL_OK; row.rank = 0; row.ms = lp->rttMs; ++confirmed;
                } else {
                    std::wstring why = DiagPeerText(p.uri);
                    row.right = why.empty() ? L"не работает" : L"не работает · " + why; row.color = COL_ALERT; row.rank = 4;
                }
            } else if (vr != verified.end()) {          // проверен рукопожатием
                if (vr->second.up) {
                    wchar_t b[48]; swprintf(b, 48, L"проверен · %.0f мс", vr->second.ms);
                    row.right = b; row.color = COL_OK; row.rank = 0; row.ms = vr->second.ms; ++confirmed;
                } else {
                    row.right = L"не поднялся" + (vr->second.text.empty() ? L"" : L" · " + vr->second.text);
                    row.color = COL_ALERT; row.rank = 4;
                }
            } else if (p.ms >= 0) {
                wchar_t b[48]; swprintf(b, 48, L"порт открыт · %.0f мс", p.ms);
                row.right = b; row.rank = 1; row.ms = p.ms; ++open;
            } else if (p.err == L"udp") { row.right = L"UDP: порт не проверить"; row.rank = 2; }
            else if (p.err == L"dns") { row.right = L"имя не находится"; row.color = COL_ALERT; row.rank = 4; }
            else if (p.err == L"refused") { row.right = L"порт закрыт"; row.color = COL_ALERT; row.rank = 4; }
            else { row.right = L"не отвечает"; row.color = COL_ALERT; row.rank = 4; }
            rows.push_back(row);
        }
        std::stable_sort(rows.begin(), rows.end(), [](const CatRow& a, const CatRow& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            if (a.rank <= 1) return a.ms < b.ms;
            return false;
        });
        std::vector<ListItem> items;
        for (const auto& row : rows) {
            ListItem it;
            it.key = row.p->uri;
            it.left = (row.main ? L"● " : L"") + row.p->uri;
            it.right = row.right;
            it.rightColor = row.color;
            if (row.rank == 4) it.color = 0x9AA7B0;
            items.push_back(it);
        }
        lbCatalog.SetItems(items);
        List(lbCatalog, th, 504, 124, 496, 300);

        float y = 432;
        const ListItem* sel = lbCatalog.Selected();
        const CatPeer* cp = nullptr;
        if (sel) for (const auto& p : catalog) if (p.uri == sel->key) cp = &p;
        if (catalogLoading) {
            r.Text(L"загружаю пиры и проверяю порты…", 504, y, 496, 20, Rgb(th.accent), 11);
        } else if (cp) {
            r.Text(cp->uri + (cp->city.empty() ? L"" : L"   ·   " + cp->city), 504, y, 496, 20, Rgb(th.text), 11);
        } else if (!catalog.empty()) {
            r.Text(L"проверено работой: " + std::to_wstring(confirmed) + L"  ·  порт открыт: " + std::to_wstring(open) +
                   L"  ·  выбери пир", 504, y, 496, 20, Rgb(th.muted), 11);
        }
        y += 26;
        bool inYgg = cp && FindLive(cp->uri) != nullptr;
        bool canAdd = cp && !isMain(cp->uri);
        Btn(BTN_CAT_ADD, 504, y, 156, 34, L"ДОБАВИТЬ", th.accent, true, canAdd);
        Btn(BTN_CAT_VERIFY, 672, y, 156, 34, L"ПРОВЕРИТЬ ПИР", COL_WARN, false, cp && !inYgg);
        Btn(BTN_CAT_REFRESH, 840, y, 160, 34, L"ПЕРЕМЕРИТЬ ПОРТЫ", th.accent, false, !catalogFor.empty() && !catalogLoading);
        Note(th, 504, y + 44, 496,
             L"Серое «порт открыт» - это только TCP: сервер может принять соединение и сразу оборвать (как пир с EOF). "
             L"«Проверить пир» на несколько секунд подключает его к Yggdrasil и показывает настоящий результат. "
             L"● - пир уже в основных.");
    }

    void LoadCatalog(const std::wstring& slug) {
        catalogFor = slug;
        catalog.clear();
        lbCatalog.items.clear(); lbCatalog.sel = -1; lbCatalog.scroll = 0;
        lbCatCities.items.clear(); lbCatCities.sel = -1; lbCatCities.scroll = 0;
        lbCatalog.emptyText = L"загружаю и проверяю пиры…";
        catalogLoading = true;
        StartJob(hwnd, SelfExe(), L"catalog-json " + Quote(slug), 3, WM_APP_LIST, 90000);
    }

    void OnCatCountryPicked() {
        const ListItem* c = lbCatCountries.Selected();
        if (c && c->key != catalogFor) LoadCatalog(c->key);
    }

    // ================== РЕГИОН ==================
    void DrawRegionScreen(const Theme& th) {
        if (lbCountries.items.empty() && !countriesLoading) LoadCountries();
        r.Panel(24, 86, 300, 504, th, L"СТРАНА");
        List(lbCountries, th, 40, 124, 268, 450);

        r.Panel(340, 86, 340, 504, th, L"ГОРОД");
        Field(tbCity, th, 356, 124, 308, 30);
        List(lbCities, th, 356, 162, 308, 412);

        r.Panel(696, 86, 320, 504, th, L"ДЕЙСТВИЯ");
        const auto& st = snap.status;
        bool regional = st[L"mode"].str() == L"regional";
        float y = 120, x = 712, w = 288;
        auto row = [&](const std::wstring& k, const std::wstring& v, unsigned c) {
            r.Text(k, x, y, w, 20, Rgb(th.muted), 11);
            r.Text(v, x, y, w, 20, Rgb(c ? c : th.text), 11, false, Right);
            y += 22;
        };
        row(L"режим", regional ? L"региональный" : L"обычный", regional ? th.accent : 0);
        if (regional) row(L"регион", CountryRu(st[L"regional_country"].str()) + L" / " + st[L"regional_city"].str(), 0);
        std::wstring home = st[L"home_region"].str();
        row(L"домашний", home.empty() ? L"не задан" : CountryRu(home) + L" / " +
            (st[L"home_city"].str().empty() ? L"любой" : st[L"home_city"].str()), home.empty() ? th.muted : 0);
        const ListItem* c = lbCountries.Selected();
        const ListItem* city = lbCities.Selected();
        y += 6;
        r.Line(x, y, x + w, y, Rgb(th.accent, 0.2f));
        y += 8;
        row(L"выбрано", c ? c->left : L"—", 0);
        row(L"город", city ? (city->key.empty() ? L"любой" : city->left) : L"—", 0);
        bool cityChosen = c && city && !city->key.empty();
        y += 10;
        Btn(BTN_REGION_ON, x, y, w, 36, L"ВКЛЮЧИТЬ РЕГИОН", th.accent, true, cityChosen); y += 46;
        Btn(BTN_REGION_OFF, x, y, w, 32, L"СНЯТЬ РЕГИОН", COL_WARN, false, regional); y += 42;
        Btn(BTN_HOME_SET, x, y, w, 32, L"СДЕЛАТЬ ДОМАШНИМ", th.accent, false, c != nullptr && city != nullptr); y += 42;
        Btn(BTN_HOME_CLEAR, x, y, w, 32, L"СНЯТЬ ДОМАШНИЙ", th.accent, false, !home.empty()); y += 44;
        Note(th, x, y, w, L"Регион - на время поездки: пока включён, основные пиры не проверяются, "
                          L"сторож держит ближайших к городу. Домашний - запасной регион поиска резервных, когда GeoIP не ответил.");
    }

    void LoadCountries() {
        countriesLoading = true;
        lbCountries.emptyText = L"загружаю список стран…";
        StartJob(hwnd, SelfExe(), L"countries-json", 1, WM_APP_LIST, 60000);
    }
    void LoadCities(const std::wstring& slug) {
        citiesFor = slug;
        citiesAll.clear();
        lbCities.items.clear(); lbCities.sel = -1; lbCities.scroll = 0;
        lbCities.emptyText = L"загружаю города…";
        citiesLoading = true;
        StartJob(hwnd, SelfExe(), L"cities-json " + Quote(slug), 2, WM_APP_LIST, 30000);
    }
    void FilterCities() {
        std::wstring f = tbCity.Trimmed();
        for (auto& ch : f) ch = towlower(ch);
        std::vector<ListItem> v;
        ListItem any; any.left = L"— любой город —"; any.key = L""; any.color = 0x9AA7B0;
        if (f.empty()) v.push_back(any);
        for (const auto& it : citiesAll) {
            if (!f.empty()) {
                std::wstring l = it.left;
                for (auto& ch : l) ch = towlower(ch);
                if (l.find(f) == std::wstring::npos) continue;
            }
            v.push_back(it);
        }
        lbCities.SetItems(v);
        if (lbCities.sel < 0) lbCities.scroll = 0;
    }

    // ================== УСТРОЙСТВА ==================
    void DrawDevicesScreen(const Theme& th) {
        r.Panel(24, 86, 620, 504, th, L"УСТРОЙСТВА  //  пинг после подтверждённой связи с пирами");
        std::vector<ListItem> items;
        const auto& devs = snap.status[L"devices"];
        for (size_t i = 0; i < devs.size(); ++i) {
            ListItem it;
            it.key = devs[i][L"name"].str();
            it.left = devs[i][L"name"].str() + L"   " + devs[i][L"host"].str();
            long long ds = devs[i][L"down_since"].i64();
            if (ds) { it.right = L"недоступно с " + HhMm(ds); it.rightColor = COL_ALERT; }
            else { it.right = L"в сети"; it.rightColor = COL_OK; }
            items.push_back(it);
        }
        lbDevices.SetItems(items);
        List(lbDevices, th, 40, 124, 588, 300);
        Btn(BTN_DEV_DEL, 40, 436, 260, 32, L"УДАЛИТЬ ВЫБРАННОЕ", COL_ALERT, false, lbDevices.Selected() != nullptr);
        Note(th, 40, 484, 588, L"Если устройство не отвечает, сторож проверяет, не мешает ли нагрузка самому Yggdrasil "
                              L"на этом компьютере, и если мешает - перезапускает свою службу. Повтор - не чаще раза в час. "
                              L"Телефон не мониторится.");

        r.Panel(660, 86, 356, 504, th, L"ДОБАВИТЬ");
        Caption(th, 676, 124, 324, L"имя");
        Field(tbDevName, th, 676, 144, 324);
        Caption(th, 676, 188, 324, L"адрес Yggdrasil или IP");
        Field(tbDevHost, th, 676, 208, 324);
        Btn(BTN_DEV_ADD, 676, 256, 324, 36, L"ДОБАВИТЬ", th.accent, true,
            !tbDevName.Trimmed().empty() && !tbDevHost.Trimmed().empty());
        Note(th, 676, 304, 324, L"Если устройство с таким именем уже есть - у него просто обновится адрес.");
    }

    // ================== ЖУРНАЛ ==================
    void DrawLogScreen(const Theme& th) {
        r.Panel(24, 86, 992, 504, th, L"ЖУРНАЛ СОБЫТИЙ  //  events.log");
        std::vector<ListItem> items;
        for (const auto& e : snap.events) {
            ListItem it;
            time_t tt = (time_t)e.ts;
            tm lt;
            localtime_s(&lt, &tt);
            wchar_t b[32];
            wcsftime(b, 32, L"%d.%m %H:%M", &lt);
            it.key = std::to_wstring(e.ts) + L"|" + e.text;
            it.left = std::wstring(b) + L"   " + e.text;
            it.color = e.level == L"ok" ? COL_OK : e.level == L"warn" ? COL_WARN : e.level == L"alert" ? COL_ALERT : th.text;
            items.push_back(it);
        }
        lbLog.SetItems(items);
        // высота каждой строки - по переносу текста; считаем заново только когда журнал изменился
        {
            long long topTs = snap.events.empty() ? 0 : snap.events.front().ts;
            if (logHts.size() != items.size() || logHtsTop != topTs) {
                logHts.assign(items.size(), 0);
                for (size_t i = 0; i < items.size(); ++i)
                    logHts[i] = r.TextWrapped(items[i].left, 0, 0, 960 - 28, Rgb(th.text), 12, false, false) + 12;
                logHtsTop = topTs;
            }
            lbLog.hts = logHts;
        }
        List(lbLog, th, 40, 124, 960, 424);
        Note(th, 40, 556, 960, L"Новые события сверху, текст переносится целиком. "
                               L"Сюда пишется всё, что сторож шлёт в Telegram, даже если Telegram не настроен.");
    }

    // ================== УПРАВЛЕНИЕ ==================
    void DrawControlScreen(const Theme& th) {
        const auto& st = snap.status;
        auto line = [&](float x, float& y, float w, const std::wstring& k, const std::wstring& v, unsigned c) {
            r.Text(k, x, y, w, 20, Rgb(th.muted), 12);
            r.Text(v, x, y, w, 20, Rgb(c ? c : th.text), 12, false, Right);
            y += 24;
        };
        // --- задача
        r.Panel(24, 86, 488, 240, th, L"ЗАДАЧА ПЛАНИРОВЩИКА  //  " + std::wstring(TASK_NAME));
        float y = 120;
        bool foreign = snap.taskExists && !snap.taskExe.empty() &&
                       _wcsicmp(snap.taskExe.c_str(), (SelfExe()).c_str()) != 0;
        line(40, y, 456, L"состояние", !snap.taskExists ? L"не установлена" : foreign ? L"запускает другую папку" : L"работает отсюда",
             !snap.taskExists ? COL_ALERT : foreign ? COL_WARN : COL_OK);
        long long lt = LastTick(), nowc = (long long)time(nullptr);
        line(40, y, 456, L"последняя проверка", lt ? HhMm(lt) + L"  (" + std::to_wstring((nowc - lt) / 60) + L" мин назад)" : L"—", 0);
        const TaskInfo& ti = snap.task;
        if (ti.ok) {
            std::wstring nx; unsigned nc = 0;
            if (ti.state == 1) { nx = L"задача выключена"; nc = COL_ALERT; }
            else if (ti.state == 4) {
                long long mins = ti.lastRun ? (nowc - ti.lastRun) / 60 : 0;
                nx = mins >= 10 ? L"висит " + std::to_wstring(mins) + L" мин" : L"идёт сейчас";
                nc = mins >= 10 ? COL_ALERT : COL_OK;
            }
            else nx = ti.nextRun ? HhMm(ti.nextRun) : L"не запланирован";
            line(40, y, 456, L"следующий запуск", nx, nc);
            std::wstring res = TaskResultText(ti.lastResult);
            line(40, y, 456, L"прошлый запуск", ti.lastRun == 0 ? L"ещё не было" : res.empty() ? L"без ошибок" : res,
                 res.empty() ? 0 : COL_ALERT);
        }
        if (foreign) {
            r.Clip(40, y, 456, 20);
            r.Text(snap.taskExe, 40, y, 2000, 20, Rgb(th.muted), 10);
            r.Unclip();
            y += 22;
        }
        Btn(BTN_TASK_INSTALL, 40, 262, 220, 38, snap.taskExists ? L"ПЕРЕУСТАНОВИТЬ" : L"УСТАНОВИТЬ", th.accent, true);
        Btn(BTN_TASK_REMOVE, 276, 262, 220, 38, L"ОСТАНОВИТЬ", COL_ALERT, false, snap.taskExists);

        // --- служба
        r.Panel(528, 86, 488, 240, th, L"СЛУЖБА YGGDRASIL");
        y = 120;
        line(544, y, 456, L"состояние", snap.service, snap.service == L"работает" ? COL_OK : COL_ALERT);
        line(544, y, 456, L"узел", snap.selfAddress.empty() ? L"—" : snap.selfAddress, 0);
        Note(th, 544, y + 4, 456, L"Перезапуск нужен после правки основных пиров. Через 15 с - если ты сидишь на этом компьютере через сам Yggdrasil.");
        Btn(BTN_RESTART_NOW, 544, 262, 220, 38, L"ПЕРЕЗАПУСТИТЬ", COL_WARN, false);
        Btn(BTN_RESTART_LATER, 780, 262, 220, 38, L"ЧЕРЕЗ 15 С", th.accent, false);

        // --- обновления
        r.Panel(24, 342, 488, 248, th, L"ОБНОВЛЕНИЯ  //  github.com/Yozmor/ygg-watchdog");
        y = 376;
        line(40, y, 456, L"установлена", L"v" + st[L"version"].str(), 0);
        std::wstring upd = st[L"update_available"].str();
        line(40, y, 456, L"на GitHub", upd.empty() ? L"новее нет (по последней проверке)" : upd, upd.empty() ? th.muted : COL_WARN);
        Note(th, 40, y + 4, 456, L"Сторож сам проверяет раз в сутки и только сообщает. Ставится обновление только этой кнопкой.");
        // трей: строки-переключатели
        {
            float ty = 470;
            bool autoOn = TrayAutostartEnabled(), popOn = TrayPopupsEnabled(), running = TrayRunning();
            r.Line(40, ty - 6, 496, ty - 6, Rgb(th.accent, 0.2f));
            bool h1 = hoverId == BTN_TRAY_AUTO, h2 = hoverId == BTN_TRAY_POPUPS;
            r.Text(std::wstring(L"значок в трее") + (running ? L"" : L" (не запущен)"), 40, ty, 456, 20, Rgb(th.muted), 12);
            r.Text(autoOn ? L"запускать при входе: да" : L"запускать при входе: нет", 40, ty, 456, 20,
                   Rgb(th.accent, h1 ? 1.0f : 0.8f), 12, false, Right);
            if (!confirmOpen) hits.push_back({ 300, ty, 196, 20, BTN_TRAY_AUTO });
            ty += 24;
            r.Text(L"всплывающие окна", 40, ty, 456, 20, Rgb(th.muted), 12);
            r.Text(popOn ? L"включены" : L"выключены", 40, ty, 456, 20, Rgb(th.accent, h2 ? 1.0f : 0.8f), 12, false, Right);
            if (!confirmOpen) hits.push_back({ 380, ty, 116, 20, BTN_TRAY_POPUPS });
        }
        Btn(BTN_UPD_CHECK, 40, 526, 220, 38, L"ПРОВЕРИТЬ", th.accent, false);
        Btn(BTN_UPD_APPLY, 276, 526, 220, 38, L"УСТАНОВИТЬ", COL_WARN, false);

        // --- VPN и уведомления
        r.Panel(528, 342, 488, 248, th, L"VPN И УВЕДОМЛЕНИЯ");
        y = 376;
        bool vpn = st[L"vpn_script_approved"].boolean();
        line(544, y, 456, L"VPN-скрипт", vpn ? L"одобрен (сверка SHA-256)" : L"не задан", vpn ? COL_OK : th.muted);
        Btn(BTN_VPN_PICK, 544, 400, 220, 30, L"ВЫБРАТЬ СКРИПТ…", th.accent, false);
        Btn(BTN_VPN_CLEAR, 780, 400, 220, 30, L"СНЯТЬ ОДОБРЕНИЕ", COL_ALERT, false, vpn);

        // Telegram: значения только вводятся, обратно не показываются
        y = 442;
        bool tg = st[L"telegram_configured"].boolean();
        line(544, y, 456, L"Telegram", tg ? L"настроен" : L"не настроен", tg ? COL_OK : th.muted);
        Caption(th, 544, 466, 220, tg ? L"новый токен бота" : L"токен бота");
        Caption(th, 780, 466, 220, tg ? L"новый chat id" : L"chat id");
        Field(tbTgToken, th, 544, 484, 220, 30);
        Field(tbTgChat, th, 780, 484, 220, 30);
        bool anyTg = !tbTgToken.Trimmed().empty() || !tbTgChat.Trimmed().empty();
        if (!anyTg && tg) Btn(BTN_TG_SAVE, 544, 522, 220, 30, L"ОТКЛЮЧИТЬ", COL_ALERT, false);
        else Btn(BTN_TG_SAVE, 544, 522, 220, 30, L"СОХРАНИТЬ", th.accent, true, anyTg);
        Btn(BTN_TG_TEST, 780, 522, 220, 30, L"ПРОВЕРИТЬ", th.accent, false, tg);

        // анимация: строка-переключатель (и клавиша A)
        y = 562;
        bool hovA = hoverId == BTN_ANIM;
        r.Text(L"анимация радара", 544, y, 456, 20, Rgb(th.muted), 12);
        r.Text(std::wstring(animate ? L"включена" : L"выключена") + L"  [A]", 544, y, 456, 20,
               Rgb(th.accent, hovA ? 1.0f : 0.8f), 12, false, Right);
        if (!confirmOpen) hits.push_back({ 840, y, 160, 20, BTN_ANIM });
    }

    // ================== действия ==================
    void Flash(const std::wstring& s, unsigned color = 0) {
        flash = s; flashColor = color; flashUntil = (long long)time(nullptr) + 10;
    }

    std::wstring okMessage;          // короткое сообщение об успехе вместо последней строки вывода

    void RunCmd(const std::wstring& label, const std::wstring& args, int tag = CMD_GENERIC, DWORD timeout = 120000,
                const std::wstring& okMsg = L"") {
        if (busy) return;
        busy = true;
        busyLabel = label;
        okMessage = okMsg;
        StartJob(hwnd, SelfExe(), args, tag, WM_APP_CMD_DONE, timeout);
        Redraw();
    }

    void RequestCheck() {
        if (busy || checkRequestedAt) return;
        tickBeforeCheck = LastTick();
        checkRequestedAt = (long long)time(nullptr);
        RunCmd(L"проверка", L"tick", CMD_TICK, 10 * 60 * 1000);
    }

    void OnCmdDone(int tag, std::string* out) {
        busy = false;
        auto lines = OutputLines(*out);
        delete out;
        bool failed = false;
        for (auto& l : lines) if (l.rfind(L"Ошибка", 0) == 0) failed = true;
        std::wstring last = lines.empty() ? L"готово" : lines.back();
        if (failed) for (auto& l : lines) if (l.rfind(L"Ошибка", 0) == 0) { last = l; break; }
        if (tag == CMD_TICK) { checkRequestedAt = 0; last = L"Проверка выполнена."; }
        else if (!failed && !okMessage.empty()) last = okMessage;
        if (!failed && tag == CMD_ADD_PEER) tbPeer.Set(L"");
        if (!failed && tag == CMD_ADD_DEVICE) { tbDevName.Set(L""); tbDevHost.Set(L""); }
        if (!failed && tag == CMD_REGION) ClearRegionPick();
        if (tag == CMD_VERIFY) {
            // итог - JSON в последней строке вывода
            for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
                if (it->empty() || (*it)[0] != L'{') continue;
                json::Value v;
                int n8 = WideCharToMultiByte(CP_UTF8, 0, it->c_str(), (int)it->size(), nullptr, 0, nullptr, nullptr);
                std::string u8(n8, 0);
                if (n8 > 0) WideCharToMultiByte(CP_UTF8, 0, it->c_str(), (int)it->size(), &u8[0], n8, nullptr, nullptr);
                if (json::Parse(u8, v) && v.isObject()) {
                    Verified vr;
                    vr.up = v[L"up"].boolean();
                    vr.ms = v[L"ms"].num(-1);
                    vr.text = v[L"text"].str();
                    verified[v[L"uri"].str()] = vr;
                    last = vr.up ? L"Пир работает: Yggdrasil подключился к нему." : L"Пир не поднялся" + (vr.text.empty() ? L"." : L": " + vr.text + L".");
                    failed = !vr.up;
                }
                break;
            }
        }
        Flash(last, failed ? COL_ALERT : 0);
        data.RefreshNow();
        Redraw();
    }

    void OnList(int which, std::string* out) {
        json::Value v;
        bool ok = json::Parse(*out, v) && v.isArray();
        delete out;
        if (which == 3) {
            catalogLoading = false;
            catalog.clear();
            if (ok) for (size_t i = 0; i < v.size(); ++i) {
                CatPeer p;
                p.uri = v[i][L"uri"].str();
                p.city = v[i][L"city"].str();
                p.ms = v[i][L"tcp_ms"].num(-1);
                p.err = v[i][L"err"].str();
                if (!p.uri.empty()) catalog.push_back(p);
            }
            lbCatalog.emptyText = ok ? L"в этой стране пиров нет" : L"не удалось получить пиры (GitHub?)";
            Redraw();
            return;
        }
        if (which == 1) {
            countriesLoading = false;
            std::vector<ListItem> items;
            if (ok) for (size_t i = 0; i < v.size(); ++i) {
                ListItem it;
                it.key = v[i][L"slug"].str();
                it.left = v[i][L"name"].str();
                if (!v[i][L"supported"].boolean()) { it.right = L"нет координат"; it.color = 0x7A8288; }
                items.push_back(it);
            }
            lbCountries.SetItems(items);
            std::vector<ListItem> all = items;
            for (auto& it : all) { it.right.clear(); it.color = 0; }
            lbCatCountries.SetItems(all);
            lbCatCountries.emptyText = lbCountries.emptyText;
            lbCountries.emptyText = ok ? L"список пуст" : L"не удалось получить список стран (GitHub?)";
            lbCatCountries.emptyText = lbCountries.emptyText;
        } else {
            citiesLoading = false;
            citiesAll.clear();
            if (ok) for (size_t i = 0; i < v.size(); ++i) {
                ListItem it;
                it.key = v[i][L"name"].str();
                it.left = v[i][L"label"].str();
                citiesAll.push_back(it);
            }
            lbCities.emptyText = L"городов не найдено";
            FilterCities();
        }
        Redraw();
    }

    void OnButton(int id) {
        if (id >= NAV_FIRST && id < NAV_FIRST + 10) { GoTo(id - NAV_FIRST); return; }
        switch (id) {
        case BTN_CHECK: RequestCheck(); break;
        case BTN_HEADER_TASK: GoTo(SCR_CONTROL); break;
        case BTN_CONFIRM_YES: { confirmOpen = false; auto a = confirmAction; confirmAction = nullptr; if (a) a(); break; }
        case BTN_CONFIRM_NO: confirmOpen = false; confirmAction = nullptr; break;

        case BTN_ADD_PEER: {
            std::wstring uri = tbPeer.Trimmed();
            if (uri.find(L"://") == std::wstring::npos) { Flash(L"Адрес пира должен быть вида tls://host:port", COL_ALERT); break; }
            RunCmd(L"добавляю пир", L"add-main-peer " + Quote(uri), CMD_ADD_PEER);
            break;
        }
        case BTN_CAT_ADD: {
            const ListItem* s = lbCatalog.Selected();
            if (!s) break;
            RunCmd(L"добавляю пир", L"add-main-peer " + Quote(s->key), CMD_GENERIC, 120000,
                   L"Пир добавлен в основные. Перезапусти службу на вкладке «пиры».");
            break;
        }
        case BTN_CAT_VERIFY: {
            const ListItem* s = lbCatalog.Selected();
            if (!s) break;
            verifyingUri = s->key;
            RunCmd(L"проверяю пир (до 15 с)", L"verify-peer " + Quote(s->key), CMD_VERIFY, 60000);
            break;
        }
        case BTN_CAT_REFRESH:
            if (!catalogFor.empty()) LoadCatalog(catalogFor);
            break;
        case BTN_DEL_PEER: {
            const ListItem* s = lbMain.Selected();
            if (!s) break;
            std::wstring idx = s->key, uri = s->left;
            Confirm(L"Удалить из yggdrasil.conf пир\n" + uri + L"?\nУдалится только эта строка.", L"УДАЛИТЬ",
                    [this, idx] { RunCmd(L"удаляю пир", L"remove-main-peer " + idx); });
            break;
        }
        case BTN_RESTART_NOW:
            Confirm(L"Перезапустить службу Yggdrasil сейчас? Все соединения через Yggdrasil оборвутся. "
                    L"Если ты подключён к этому компьютеру через сам Yggdrasil - выбери «через 15 с».", L"ПЕРЕЗАПУСТИТЬ",
                    [this] { RunCmd(L"перезапуск службы", L"restart-yggdrasil"); });
            break;
        case BTN_RESTART_LATER: RunCmd(L"планирую перезапуск", L"restart-yggdrasil-delayed 15"); break;

        case BTN_REGION_ON: {
            const ListItem* c = lbCountries.Selected();
            const ListItem* city = lbCities.Selected();
            if (!c || !city || city->key.empty()) break;
            std::wstring slug = c->key, name = city->key, label = c->left + L" / " + city->left;
            Confirm(L"Включить региональный режим: " + label + L"?\nПока он включён, основные пиры не проверяются. "
                    L"Поиск ближайшего пира пойдёт в фоне (до пары минут).", L"ВКЛЮЧИТЬ",
                    [this, slug, name] { RunCmd(L"включаю регион", L"add-region " + Quote(slug) + L" " + Quote(name), CMD_REGION, 120000,
                                         L"Регион включён, ищу пир в фоне."); });
            break;
        }
        case BTN_REGION_OFF:
            Confirm(L"Снять региональный режим? Региональные пиры будут удалены, сторож вернётся к основным.", L"СНЯТЬ",
                    [this] { RunCmd(L"снимаю регион", L"remove-region", CMD_REGION, 120000, L"Регион снят."); });
            break;
        case BTN_HOME_SET: {
            const ListItem* c = lbCountries.Selected();
            const ListItem* city = lbCities.Selected();
            if (!c || !city) break;
            RunCmd(L"задаю домашний регион", L"set-home-region " + Quote(c->key) + (city->key.empty() ? L"" : L" " + Quote(city->key)), CMD_REGION,
                   120000, L"Домашний регион задан.");
            break;
        }
        case BTN_HOME_CLEAR: RunCmd(L"снимаю домашний регион", L"set-home-region clear", CMD_GENERIC, 120000, L"Домашний регион снят."); break;

        case BTN_DEV_ADD:
            RunCmd(L"добавляю устройство", L"add-device " + Quote(tbDevName.Trimmed()) + L" " + Quote(tbDevHost.Trimmed()), CMD_ADD_DEVICE);
            break;
        case BTN_DEV_DEL: {
            const ListItem* s = lbDevices.Selected();
            if (!s) break;
            std::wstring name = s->key;
            Confirm(L"Убрать устройство «" + name + L"» из мониторинга?", L"УБРАТЬ",
                    [this, name] { RunCmd(L"убираю устройство", L"remove-device " + Quote(name)); });
            break;
        }

        case BTN_TASK_INSTALL: {
            bool foreign = snap.taskExists && !snap.taskExe.empty() &&
                           _wcsicmp(snap.taskExe.c_str(), (SelfExe()).c_str()) != 0;
            if (foreign) {
                Confirm(L"Сейчас задача запускает другую копию:\n" + snap.taskExe +
                        L"\nПереустановка перенаправит автопроверки на ygg-watchdog из ЭТОЙ папки.", L"ПЕРЕНАПРАВИТЬ",
                        [this] { RunCmd(L"устанавливаю задачу", L"install-task"); });
            } else {
                RunCmd(L"устанавливаю задачу", L"install-task");
            }
            break;
        }
        case BTN_TASK_REMOVE:
            Confirm(L"Остановить автоматические проверки и удалить задачу " + std::wstring(TASK_NAME) +
                    L" из Планировщика? Сторож перестанет проверять пиры сам." +
                    (snap.taskExe.empty() ? L"" : L"\nСейчас она запускает: " + snap.taskExe), L"ОСТАНОВИТЬ",
                    [this] { RunCmd(L"удаляю задачу", L"uninstall-task"); });
            break;
        case BTN_UPD_CHECK: RunCmd(L"проверяю обновления", L"check-update"); break;
        case BTN_UPD_APPLY:
            Confirm(L"Скачать и установить новую версию ygg-watchdog с GitHub? Старая сохранится как ygg_watchdog.exe.backup. "
                    L"Файл заменится, когда окно будет закрыто.",
                    L"УСТАНОВИТЬ", [this] { RunCmd(L"обновляюсь", L"apply-update", CMD_GENERIC, 300000,
                                                  L"Обновление скачано. Закрой окно - файл заменится, потом открой снова."); });
            break;
        case BTN_VPN_PICK: {
            std::wstring path;
            if (PickScriptFile(hwnd, path)) {
                Confirm(L"Одобрить скрипт перезапуска VPN?\n" + path + L"\nСторож будет запускать его, когда пиры легли. "
                        L"Если файл потом изменится хоть на байт - запуск откажется.", L"ОДОБРИТЬ",
                        [this, path] { RunCmd(L"одобряю скрипт", L"approve-vpn-command " + Quote(path)); });
            }
            break;
        }
        case BTN_VPN_CLEAR: RunCmd(L"снимаю одобрение", L"clear-vpn-command"); break;
        case BTN_ANIM: animate = !animate; break;
        case BTN_TRAY_AUTO: {
            bool on = !TrayAutostartEnabled();
            SetTrayAutostart(on);
            if (on) StartTray();
            Flash(on ? L"Значок в трее будет запускаться при входе в Windows." : L"Значок в трее больше не запускается сам.");
            break;
        }
        case BTN_TRAY_POPUPS:
            SetTrayPopups(!TrayPopupsEnabled());
            Flash(TrayPopupsEnabled() ? L"Всплывающие окна включены." : L"Всплывающие окна выключены (значок остаётся).");
            break;
        case BTN_TG_SAVE: SaveTelegram(); break;
        case BTN_TG_TEST: RunCmd(L"отправляю тестовое сообщение", L"test-notify", CMD_GENERIC, 60000); break;
        }
        Redraw();
    }

    // После действий с регионом выбор в списках больше не нужен - сбрасываем,
    // чтобы «выбрано / город» не висели рядом с уже применённым состоянием.
    void ClearRegionPick() {
        lbCountries.sel = -1;
        citiesFor.clear();
        citiesAll.clear();
        lbCities.items.clear(); lbCities.sel = -1; lbCities.scroll = 0;
        lbCities.emptyText = L"выбери страну слева";
        tbCity.Set(L"");
    }

    // notify.conf пишет само окно (оно с правами администратора, в той же
    // папке, что ygg_watchdog.exe): токен не идёт через командную строку.
    // Пустое поле = оставить старое значение. Оба пусто при настроенном
    // Telegram = отключить (после подтверждения). Прочие строки (PROXY,
    // комментарии) сохраняются.
    void SaveTelegram() {
        std::wstring token = tbTgToken.Trimmed(), chat = tbTgChat.Trimmed();
        bool configured = snap.status[L"telegram_configured"].boolean();
        if (token.empty() && chat.empty()) {
            if (!configured) return;
            Confirm(L"Отключить уведомления в Telegram? Токен и chat id будут стёрты из notify.conf.", L"ОТКЛЮЧИТЬ",
                    [this] { WriteNotifyConf(L"", L"", true); });
            return;
        }
        if (!token.empty() && token.find(L':') == std::wstring::npos) {
            Flash(L"Токен бота выглядит как 123456:ABC… - проверь, что скопирован целиком", COL_ALERT); return;
        }
        if (!chat.empty()) {
            size_t i = chat[0] == L'-' ? 1 : 0;
            bool digits = i < chat.size();
            for (; i < chat.size(); ++i) if (!iswdigit(chat[i])) digits = false;
            if (!digits) { Flash(L"chat id - это число (у групп с минусом впереди)", COL_ALERT); return; }
        }
        WriteNotifyConf(token, chat, false);
    }

    void WriteNotifyConf(const std::wstring& token, const std::wstring& chat, bool clearBoth) {
        std::wstring path = ExeDir() + L"notify.conf";
        std::wstring old = json::Parser::FromUtf8(ReadFileBytes(path));
        std::wstring keepToken, keepChat, rest;
        std::wstringstream ss(old);
        std::wstring ln;
        while (std::getline(ss, ln)) {
            if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
            if (ln.rfind(L"BOT_TOKEN=", 0) == 0) keepToken = ln.substr(10);
            else if (ln.rfind(L"CHAT_ID=", 0) == 0) keepChat = ln.substr(8);
            else if (!ln.empty()) rest += ln + L"\r\n";
        }
        std::wstring t = clearBoth ? L"" : (token.empty() ? keepToken : token);
        std::wstring c = clearBoth ? L"" : (chat.empty() ? keepChat : chat);
        std::wstring out = rest;
        if (!t.empty()) out += L"BOT_TOKEN=" + t + L"\r\n";
        if (!c.empty()) out += L"CHAT_ID=" + c + L"\r\n";

        int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8(n, 0);
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), &utf8[0], n, nullptr, nullptr);
        std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        if (!f.is_open()) { Flash(L"Не удалось записать notify.conf", COL_ALERT); return; }
        f.write(utf8.data(), (std::streamsize)utf8.size());
        f.close();

        tbTgToken.Set(L""); tbTgChat.Set(L"");
        SetTb(nullptr);
        if (clearBoth) Flash(L"Telegram отключён.");
        else if (t.empty() || c.empty()) Flash(L"Сохранено, но не хватает " + std::wstring(t.empty() ? L"токена" : L"chat id") + L".", COL_WARN);
        else Flash(L"Telegram сохранён. Нажми «Проверить» - придёт тестовое сообщение.");
        data.RefreshNow();
    }

    void GoTo(int s) {
        screen = s;
        SetTb(nullptr);
        focusLb = nullptr;
        Redraw();
    }

    void OnData() {
        snap = data.Get();
        Redraw();
    }

    D2D1_POINT_2F Pt(LPARAM lp) { return r.ToDesign((short)LOWORD(lp), (short)HIWORD(lp)); }

    int HitAt(LPARAM lp) {
        D2D1_POINT_2F p = Pt(lp);
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) if (it->contains(p)) return it->id;
        return 0;
    }

    std::vector<TextBox*> ScreenFields() {
        if (screen == SCR_PEERS) return { &tbPeer };
        if (screen == SCR_REGION) return { &tbCity };
        if (screen == SCR_DEVICES) return { &tbDevName, &tbDevHost };
        if (screen == SCR_CONTROL) return { &tbTgToken, &tbTgChat };
        return {};
    }
    std::vector<ListBox*> ScreenLists() {
        if (screen == SCR_PEERS) return { &lbMain, &lbLive };
        if (screen == SCR_REGION) return { &lbCountries, &lbCities };
        if (screen == SCR_DEVICES) return { &lbDevices };
        if (screen == SCR_LOG) return { &lbLog };
        if (screen == SCR_CATALOG) return { &lbCatCountries, &lbCatCities, &lbCatalog };
        return {};
    }

    // Клик мимо кнопок: поля ввода и списки
    void OnFreeClick(LPARAM lp) {
        if (confirmOpen) return;
        D2D1_POINT_2F p = Pt(lp);
        for (TextBox* t : ScreenFields()) {
            if (t->Hit().contains(p)) {
                SetTb(t);
                // курсор - в конец (точное позиционирование мышью не нужно для коротких полей)
                t->caret = t->text.size();
                return;
            }
        }
        for (ListBox* l : ScreenLists()) {
            if (l->ClickAt(p)) {
                SetTb(nullptr);
                focusLb = l;
                if (l == &lbCountries) OnCountryPicked();
                if (l == &lbCatCountries) OnCatCountryPicked();
                if (l == &lbCatCities) { lbCatalog.sel = -1; lbCatalog.scroll = 0; }
                return;
            }
        }
        SetTb(nullptr);
    }

    void OnCountryPicked() {
        const ListItem* c = lbCountries.Selected();
        if (!c) return;
        if (c->right == L"нет координат") {
            citiesAll.clear(); lbCities.items.clear(); lbCities.sel = -1;
            lbCities.emptyText = L"для этой страны нет координат городов";
            return;
        }
        if (c->key != citiesFor) { tbCity.Set(L""); LoadCities(c->key); }
    }

    void OnWheel(WPARAM wp, LPARAM lp) {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        D2D1_POINT_2F p = r.ToDesign(pt.x, pt.y);
        for (ListBox* l : ScreenLists())
            if (p.x >= l->x && p.x <= l->x + l->w && p.y >= l->y && p.y <= l->y + l->h) { l->Wheel(GET_WHEEL_DELTA_WPARAM(wp)); break; }
        Redraw();
    }

    void SubmitField(TextBox* t) {
        if (t == &tbPeer && !tbPeer.Trimmed().empty()) OnButton(BTN_ADD_PEER);
        else if (t == &tbDevName) SetTb(&tbDevHost);
        else if (t == &tbDevHost && !tbDevName.Trimmed().empty() && !tbDevHost.Trimmed().empty()) OnButton(BTN_DEV_ADD);
        else if (t == &tbTgToken) SetTb(&tbTgChat);
        else if (t == &tbTgChat && (!tbTgToken.Trimmed().empty() || !tbTgChat.Trimmed().empty())) OnButton(BTN_TG_SAVE);
    }

    void OnKey(WPARAM vk) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (confirmOpen) {
            if (vk == VK_RETURN) OnButton(BTN_CONFIRM_YES);
            else if (vk == VK_ESCAPE) OnButton(BTN_CONFIRM_NO);
            return;
        }
        if (focusTb) {
            if (vk == VK_ESCAPE) SetTb(nullptr);
            else if (vk == VK_RETURN) SubmitField(focusTb);
            else if (vk == VK_TAB) {
                auto f = ScreenFields();
                for (size_t i = 0; i < f.size(); ++i) if (f[i] == focusTb) { SetTb(f[(i + 1) % f.size()]); break; }
            } else {
                focusTb->OnKey(vk, ctrl);
                if (focusTb == &tbCity) FilterCities();
            }
            Redraw();
            return;
        }
        if (focusLb && (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT)) {
            int d = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -focusLb->Visible() : focusLb->Visible();
            focusLb->Move(d);
            if (focusLb == &lbCountries) OnCountryPicked();
            if (focusLb == &lbCatCountries) OnCatCountryPicked();
            if (focusLb == &lbCatCities) { lbCatalog.sel = -1; lbCatalog.scroll = 0; }
            Redraw();
            return;
        }
        switch (vk) {
            case 'M': GoTo(SCR_MAIN); break;
            case 'P': GoTo(SCR_PEERS); break;
            case 'C': GoTo(SCR_CATALOG); break;
            case 'R': GoTo(SCR_REGION); break;
            case 'D': GoTo(SCR_DEVICES); break;
            case 'L': GoTo(SCR_LOG); break;
            case 'N': GoTo(SCR_CONTROL); break;
            case 'T': RequestCheck(); break;
            case 'A': animate = !animate; Flash(animate ? L"Анимация радара включена." : L"Анимация радара выключена."); break;
            case VK_ESCAPE: if (screen != SCR_MAIN) GoTo(SCR_MAIN); else ShowWindow(hwnd, SW_MINIMIZE); break;
        }
        Redraw();
    }

    void OnChar(wchar_t c) {
        if (!focusTb || confirmOpen) return;
        focusTb->OnChar(c);
        if (focusTb == &tbCity) FilterCities();
        Redraw();
    }
};

App g_app;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App& a = g_app;
    switch (msg) {
    case WM_CREATE:
        a.hwnd = hwnd;
        a.r.Init(hwnd);
        a.r.bgGray = LoadGrayPngResource(101);  // фон-древо, вшит в exe (gui.rc)
        a.data.Start(hwnd);
        SetTimer(hwnd, TIMER_CLOCK, 1000, nullptr);
        SetTimer(hwnd, TIMER_ANIM, 50, nullptr);
        return 0;
    case WM_SIZE:
        a.r.Resize();
        a.Redraw();
        return 0;
    case WM_DPICHANGED: {
        RECT* rc = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        a.r.Resize();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ANIM) {
            if (IsIconic(hwnd)) return 0;
            bool needs = (a.animate && a.screen == SCR_MAIN) || a.focusTb != nullptr; // луч / мигающий курсор
            if (!needs) return 0;
            if (a.animate && a.screen == SCR_MAIN) { a.sweep += 4.0f; if (a.sweep >= 360) a.sweep -= 360; }
        }
        a.Redraw();
        return 0;
    case WM_APP_DATA:
        a.OnData();
        return 0;
    case WM_APP_CMD_DONE:
        a.OnCmdDone((int)wp, (std::string*)lp);
        return 0;
    case WM_APP_LIST:
        a.OnList((int)wp, (std::string*)lp);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        a.Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        int id = a.HitAt(lp);
        if (id != a.hoverId) { a.hoverId = id; a.Redraw(); }
        SetCursor(LoadCursor(nullptr, id ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN:
        a.pressId = a.HitAt(lp);
        if (!a.pressId) a.OnFreeClick(lp);
        SetCapture(hwnd);
        a.Redraw();
        return 0;
    case WM_LBUTTONUP: {
        int id = a.HitAt(lp);
        ReleaseCapture();
        int pressed = a.pressId;
        a.pressId = 0;
        if (id && id == pressed) a.OnButton(id);
        a.Redraw();
        return 0;
    }
    case WM_MOUSEWHEEL:
        a.OnWheel(wp, lp);
        return 0;
    case WM_KEYDOWN:
        a.OnKey(wp);
        return 0;
    case WM_CHAR:
        a.OnChar((wchar_t)wp);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;
        break;
    case WM_DESTROY:
        a.data.Stop();
        a.r.DiscardTarget();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


// ==================================================================
// Трей: значок с цветом состояния, меню и свои всплывающие окна о
// новых событиях из events.log. Режим "ygg_watchdog.exe tray" - без
// прав администратора, только читает state.txt и events.log рядом с exe.
// Запуск при входе - HKCU\...\Run, настройки - HKCU\Software\Yozmor\ygg-watchdog.
// ==================================================================
static const wchar_t* TRAY_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* TRAY_RUN_VALUE = L"ygg-watchdog-tray";
static const wchar_t* TRAY_SETTINGS_KEY = L"Software\\Yozmor\\ygg-watchdog";
static const wchar_t* TRAY_MUTEX = L"Local\\YggWatchdogTray";

bool TrayAutostartEnabled() {
    wchar_t buf[1024]; DWORD sz = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, TRAY_RUN_KEY, TRAY_RUN_VALUE, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS;
}
void SetTrayAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, TRAY_RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + SelfExe() + L"\" tray";
        RegSetValueExW(k, TRAY_RUN_VALUE, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, TRAY_RUN_VALUE);
    }
    RegCloseKey(k);
}
bool TrayPopupsEnabled() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, TRAY_SETTINGS_KEY, L"TrayPopups", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return true;
    return v != 0;
}
void SetTrayPopups(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, TRAY_SETTINGS_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v = on ? 1 : 0;
    RegSetValueExW(k, L"TrayPopups", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}
bool TrayRunning() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, TRAY_MUTEX);
    if (h) { CloseHandle(h); return true; }
    return false;
}
void StartTray() {
    if (TrayRunning()) return;
    ShellExecuteW(nullptr, L"open", SelfExe().c_str(), L"tray", nullptr, SW_HIDE);
}

namespace tray {

static const UINT WM_TRAYICON = WM_APP + 20;
static const UINT_PTR TIMER_POLL = 1;
static const UINT_PTR TIMER_TOAST = 2;
enum { ID_OPEN = 1, ID_CHECK, ID_POPUPS, ID_AUTOSTART, ID_EXIT };

struct Toast { std::wstring level, text; long long ts = 0; };

struct TrayState {
    HINSTANCE hInst = nullptr;
    HWND msgWnd = nullptr, toastWnd = nullptr;
    NOTIFYICONDATAW nid = {};
    HICON icon = nullptr;
    unsigned iconColor = 0xFFFFFFFF;
    std::wstring phase;
    long long lastTick = 0;
    unsigned long long eventsOffset = 0;
    bool staleNotified = false;
    std::vector<Toast> queue;
    Toast cur;
    ULONGLONG shownAt = 0, showFor = 0;
    Renderer r;
    bool rInit = false;
    bool hoverClose = false;
    UINT taskbarCreated = 0;
};
TrayState g;

const float TW = 380, TH = 118; // размер всплывающего окна, DIP

unsigned PhaseColor(const std::wstring& p, bool stale) {
    if (stale || p.empty()) return 0x9AA7B0;
    if (p == L"ok" || p == L"backup" || p == L"regional") return COL_OK;
    if (p == L"down" || p == L"crisis" || p == L"regional_search") return COL_WARN;
    if (p == L"no_internet" || p == L"battery") return COL_ALERT;
    return 0x9AA7B0;
}
std::wstring PhaseText(const std::wstring& p, bool stale) {
    if (stale) return L"задача давно не проверяла";
    if (p == L"ok") return L"всё в порядке";
    if (p == L"backup") return L"связь через резерв";
    if (p == L"down") return L"нет пиров, ищу замену";
    if (p == L"crisis") return L"крайний перебор";
    if (p == L"no_internet") return L"нет интернета";
    if (p == L"battery") return L"батарея на исходе";
    if (p == L"regional") return L"регион закреплён";
    if (p == L"regional_search") return L"регион: ищу пир";
    if (p.empty()) return L"ещё не проверял";
    return L"ошибка, смотри лог";
}
bool Stale() { return g.lastTick > 0 && (long long)time(nullptr) - g.lastTick > 10 * 60; }

// Значок программы + цветная точка в правом нижнем углу
HICON MakeIcon(unsigned color) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color32 = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color32) { DeleteDC(dc); return nullptr; }
    HGDIOBJ old = SelectObject(dc, color32);
    memset(bits, 0, (size_t)sz * sz * 4);
    HICON base = (HICON)LoadImageW(g.hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, 0);
    if (base) { DrawIconEx(dc, 0, 0, base, sz, sz, 0, nullptr, DI_NORMAL); DestroyIcon(base); }
    GdiFlush();
    // точка: тёмная обводка + цвет, со сглаживанием
    BYTE* px = (BYTE*)bits;
    float rad = sz * 0.26f, cx = sz - rad - 0.5f, cy = sz - rad - 0.5f;
    BYTE cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
    for (int y = 0; y < sz; ++y) for (int x = 0; x < sz; ++x) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
        float outer = rad + 1.0f - d; if (outer <= 0) continue; if (outer > 1) outer = 1;
        float inner = rad - d; if (inner < 0) inner = 0; if (inner > 1) inner = 1;
        BYTE* p = px + ((size_t)y * sz + x) * 4;
        // предумноженная альфа: смешиваем поверх того, что уже есть
        float a = outer, ia = 1 - a;
        float r = cr * inner, gg = cg * inner, b = cb * inner; // обводка - чёрная
        p[0] = (BYTE)(b * a + p[0] * ia);
        p[1] = (BYTE)(gg * a + p[1] * ia);
        p[2] = (BYTE)(r * a + p[2] * ia);
        p[3] = (BYTE)(255 * a + p[3] * ia);
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE; ii.hbmColor = color32; ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color32); DeleteObject(mask);
    return ic;
}

void UpdateIcon() {
    bool stale = Stale();
    unsigned col = PhaseColor(g.phase, stale);
    std::wstring tip = L"ygg-watchdog: " + PhaseText(g.phase, stale);
    if (g.lastTick > 0) tip += L"\nпроверка в " + HhMm(g.lastTick);
    if (col != g.iconColor) {
        HICON ic = MakeIcon(col);
        if (ic) { if (g.icon) DestroyIcon(g.icon); g.icon = ic; g.iconColor = col; }
    }
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g.nid.hIcon = g.icon;
    wcsncpy_s(g.nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

void AddIcon() {
    g.nid = {};
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.msgWnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g.nid.uCallbackMessage = WM_TRAYICON;
    g.iconColor = 0xFFFFFFFF;
    if (g.icon) { DestroyIcon(g.icon); g.icon = nullptr; }
    g.icon = MakeIcon(0x9AA7B0);
    g.nid.hIcon = g.icon;
    wcsncpy_s(g.nid.szTip, L"ygg-watchdog", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g.nid);
    g.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g.nid);
}

void ReadState() {
    std::wstring t = json::Parser::FromUtf8(ReadFileBytes(ExeDir() + L"state.txt"));
    std::wstringstream ss(t);
    std::wstring ln;
    while (std::getline(ss, ln)) {
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (ln.rfind(L"last_phase=", 0) == 0) g.phase = ln.substr(11);
        else if (ln.rfind(L"last_tick_at=", 0) == 0) { try { g.lastTick = std::stoll(ln.substr(13)); } catch (...) {} }
    }
}

unsigned long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

// Новые строки events.log с прошлого раза
std::vector<Toast> ReadNewEvents() {
    std::vector<Toast> out;
    std::wstring path = ExeDir() + L"events.log";
    unsigned long long size = FileSize(path);
    if (size < g.eventsOffset) g.eventsOffset = 0;      // файл обрезали
    if (size == g.eventsOffset) return out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER off; off.QuadPart = (LONGLONG)g.eventsOffset;
    SetFilePointerEx(f, off, nullptr, FILE_BEGIN);
    std::string data((size_t)(size - g.eventsOffset), 0);
    DWORD got = 0;
    ReadFile(f, &data[0], (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    size_t lastNl = data.rfind('\n');
    if (lastNl == std::string::npos) return out;       // строка ещё дописывается
    data.resize(lastNl + 1);
    g.eventsOffset += data.size();
    std::wstringstream ss(json::Parser::FromUtf8(data));
    std::wstring ln;
    while (std::getline(ss, ln)) {
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        size_t t1 = ln.find(L'\t'); if (t1 == std::wstring::npos) continue;
        size_t t2 = ln.find(L'\t', t1 + 1); if (t2 == std::wstring::npos) continue;
        Toast t;
        try { t.ts = std::stoll(ln.substr(0, t1)); } catch (...) { continue; }
        t.level = ln.substr(t1 + 1, t2 - t1 - 1);
        t.text = ln.substr(t2 + 1);
        out.push_back(t);
    }
    return out;
}

void OpenMainWindow() {
    HWND w = FindWindowW(L"YggWatchdogGui", nullptr);
    if (w) {
        ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        if (GetForegroundWindow() == w) return;
    }
    ShellExecuteW(nullptr, L"open", SelfExe().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------- всплывающее окно ----------
void PlaceToast() {
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HMONITOR mon = MonitorFromRect(&wa, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn fn = (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (fn) fn(mon, 0, &dx, &dy);
    int w = MulDiv((int)TW, dx, 96), h = MulDiv((int)TH, dx, 96), m = MulDiv(16, dx, 96);
    SetWindowPos(g.toastWnd, HWND_TOPMOST, wa.right - w - m, wa.bottom - h - m, w, h, SWP_NOACTIVATE);
}

void ShowNextToast() {
    if (g.queue.empty()) {
        if (g.toastWnd) ShowWindow(g.toastWnd, SW_HIDE);
        KillTimer(g.msgWnd, TIMER_TOAST);
        return;
    }
    g.cur = g.queue.front();
    g.queue.erase(g.queue.begin());
    g.shownAt = GetTickCount64();
    g.showFor = g.cur.level == L"alert" ? 20000 : 8000;
    g.hoverClose = false;
    PlaceToast();
    if (g.rInit) g.r.Resize();
    ShowWindow(g.toastWnd, SW_SHOWNOACTIVATE);
    InvalidateRect(g.toastWnd, nullptr, FALSE);
    SetTimer(g.msgWnd, TIMER_TOAST, 50, nullptr);
}

void EnqueueToasts(const std::vector<Toast>& evs) {
    if (evs.empty() || !TrayPopupsEnabled()) return;
    bool idle = !g.toastWnd || !IsWindowVisible(g.toastWnd);
    if (evs.size() > 3) {
        // пачкой: самое важное + "и ещё N"
        Toast t = evs.back();
        for (const auto& e : evs) if (e.level == L"alert") t = e;
        t.text += L"  (и ещё " + std::to_wstring(evs.size() - 1) + L" событий - в журнале)";
        g.queue.push_back(t);
    } else {
        for (const auto& e : evs) g.queue.push_back(e);
    }
    if (idle) ShowNextToast();
}

void PaintToast() {
    if (!g.rInit) {
        g.r.designW = TW; g.r.designH = TH;
        g.rInit = g.r.Init(g.toastWnd);
        if (!g.rInit) return;
    }
    if (!g.r.EnsureTarget()) return;
    Renderer& r = g.r;
    const Toast& t = g.cur;
    Theme th = t.level == L"alert" ? ThemeRed() : t.level == L"warn" ? ThemeAmber() :
               t.level == L"ok" ? ThemeGreen() : ThemeGray();
    std::wstring head = t.level == L"alert" ? L"ТРЕВОГА" : t.level == L"warn" ? L"ВНИМАНИЕ" :
                        t.level == L"ok" ? L"ПОРЯДОК" : L"СОБЫТИЕ";
    r.BeginFrame(th.bg);
    r.Fill(0, 0, TW, TH, Rgb(th.bg));
    r.Brackets(1, 1, TW - 2, TH - 2, Rgb(th.accent), 14, 2);
    r.Disc(20, 21, 4, Rgb(th.accent));
    r.Text(L"YGG-WATCHDOG  ·  " + head, 32, 12, 260, 18, Rgb(th.accent), 11, true);
    r.Text(HhMm(t.ts), TW - 92, 12, 50, 18, Rgb(th.muted), 11, false, Right);
    r.Text(L"×", TW - 34, 8, 20, 22, Rgb(g.hoverClose ? th.text : th.muted), 15, false, Center);
    // текст: не влезает - обрезаем с многоточием
    std::wstring s = t.text;
    float avail = TH - 38 - 22;
    if (r.TextWrapped(s, 16, 0, TW - 32, Rgb(th.text), 12, false, false) > avail) {
        while (s.size() > 10 && r.TextWrapped(s + L"…", 16, 0, TW - 32, Rgb(th.text), 12, false, false) > avail) s.pop_back();
        s += L"…";
    }
    r.TextWrapped(s, 16, 36, TW - 32, Rgb(th.text), 12);
    float frac = g.showFor ? (float)(GetTickCount64() - g.shownAt) / (float)g.showFor : 1;
    if (frac > 1) frac = 1;
    float lx = 16, lw = TW - 32, ly = TH - 12;
    r.Line(lx, ly, lx + lw, ly, Rgb(th.accent, 0.15f), 2);
    r.Line(lx, ly, lx + lw * (1 - frac), ly, Rgb(th.accent, 0.8f), 2);
    r.EndFrame();
}

LRESULT CALLBACK ToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); PaintToast(); EndPaint(h, &ps); return 0; }
    case WM_SIZE: if (g.rInit) g.r.Resize(); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE;
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = g.r.ToDesign((short)LOWORD(lp), (short)HIWORD(lp));
        bool hc = p.x > TW - 40 && p.y < 32;
        if (hc != g.hoverClose) { g.hoverClose = hc; InvalidateRect(h, nullptr, FALSE); }
        g.shownAt = GetTickCount64();  // под мышью не исчезает
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g.hoverClose = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: {
        D2D1_POINT_2F p = g.r.ToDesign((short)LOWORD(lp), (short)HIWORD(lp));
        if (!(p.x > TW - 40 && p.y < 32)) OpenMainWindow();
        ShowNextToast();
        return 0;
    }
    case WM_RBUTTONUP: g.queue.clear(); ShowNextToast(); return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED: PlaceToast(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- меню ----------
void ShowMenu() {
    HMENU m = CreatePopupMenu();
    bool stale = Stale();
    std::wstring l1 = L"ygg-watchdog: " + PhaseText(g.phase, stale);
    std::wstring l2 = g.lastTick > 0 ? L"проверка в " + HhMm(g.lastTick) : L"проверок ещё не было";
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, l1.c_str());
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, l2.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Открыть окно");
    AppendMenuW(m, MF_STRING, ID_CHECK, L"Проверить сейчас");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (TrayPopupsEnabled() ? MF_CHECKED : 0), ID_POPUPS, L"Всплывающие окна");
    AppendMenuW(m, MF_STRING | (TrayAutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Запускать при входе в Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Убрать из трея");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g.msgWnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.msgWnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case ID_OPEN: OpenMainWindow(); break;
    case ID_CHECK:
        // tick нужны права администратора; результат придёт сюда же через events.log
        ShellExecuteW(nullptr, IsUserAnAdmin() ? L"open" : L"runas", SelfExe().c_str(), L"tick", nullptr, SW_HIDE);
        break;
    case ID_POPUPS: SetTrayPopups(!TrayPopupsEnabled()); break;
    case ID_AUTOSTART: SetTrayAutostart(!TrayAutostartEnabled()); break;
    case ID_EXIT: DestroyWindow(g.msgWnd); break;
    }
}

void Poll() {
    std::wstring oldPhase = g.phase;
    ReadState();
    bool stale = Stale();
    if (stale && !g.staleNotified) {
        g.staleNotified = true;
        Toast t; t.level = L"warn"; t.ts = (long long)time(nullptr);
        t.text = L"Автопроверка молчит больше 10 минут: задача Планировщика не запускается или висит. Открой окно → «Управление».";
        EnqueueToasts({ t });
    } else if (!stale) g.staleNotified = false;
    EnqueueToasts(ReadNewEvents());
    UpdateIcon();
}

LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && g.taskbarCreated) { AddIcon(); UpdateIcon(); return 0; } // Проводник перезапустился
    switch (msg) {
    case WM_TRAYICON:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP: case NIN_SELECT: case NIN_KEYSELECT: OpenMainWindow(); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: ShowMenu(); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_POLL) Poll();
        else if (wp == TIMER_TOAST) {
            if (GetTickCount64() - g.shownAt >= g.showFor) ShowNextToast();
            else if (g.toastWnd) InvalidateRect(g.toastWnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        if (g.icon) DestroyIcon(g.icon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int Run(HINSTANCE hInst) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, TRAY_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
    g.hInst = hInst;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MsgProc; wc.hInstance = hInst; wc.lpszClassName = L"YggWatchdogTrayMsg";
    RegisterClassExW(&wc);
    WNDCLASSEXW tc = { sizeof(tc) };
    tc.lpfnWndProc = ToastProc; tc.hInstance = hInst; tc.lpszClassName = L"YggWatchdogToast";
    tc.hCursor = LoadCursor(nullptr, IDC_HAND);
    RegisterClassExW(&tc);

    g.msgWnd = CreateWindowExW(0, wc.lpszClassName, L"ygg-watchdog tray", 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    g.toastWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, tc.lpszClassName, L"ygg-watchdog",
                                 WS_POPUP, 0, 0, (int)TW, (int)TH, nullptr, nullptr, hInst, nullptr);
    if (!g.msgWnd || !g.toastWnd) return 1;
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // старые события не показываем - только то, что появится после запуска
    g.eventsOffset = FileSize(ExeDir() + L"events.log");
    ReadState();
    AddIcon();
    UpdateIcon();
    SetTimer(g.msgWnd, TIMER_POLL, 5000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (mutex) CloseHandle(mutex);
    return 0;
}

} // namespace tray


bool IsElevated() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

int Run(HINSTANCE hInst, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // для WIC (загрузка фона)

    // Договорились: окна работают с правами администратора (перезапуск
    // службы, правка конфига, запуск задачи) - поднимаем права сразу.
    if (!IsElevated()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        ShellExecuteW(nullptr, L"runas", self, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }

    // Одно окно на систему: повторный запуск просто поднимает уже открытое
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"YggWatchdogGuiSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"YggWatchdogGui", nullptr);
        if (existing) { ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); }
        return 0;
    }

    // Трей: при первом открытии окна включаем автозапуск, дальше - как
    // выбрал пользователь; если включён, но не запущен - запускаем.
    {
        DWORD v = 0, sz = sizeof(v);
        if (RegGetValueW(HKEY_CURRENT_USER, TRAY_SETTINGS_KEY, L"TrayInit", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) {
            SetTrayAutostart(true);
            HKEY k;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, TRAY_SETTINGS_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
                DWORD one = 1;
                RegSetValueExW(k, L"TrayInit", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
                RegCloseKey(k);
            }
        }
        if (TrayAutostartEnabled()) StartTray();
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"YggWatchdogGui";
    RegisterClassExW(&wc);

    // 1040x640 DIP клиентской области с учётом DPI
    UINT dpi = GetDpiForSystem();
    RECT rc = { 0, 0, MulDiv(1040, dpi, 96), MulDiv(640, dpi, 96) };
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ygg-watchdog", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    BOOL dark = TRUE; // тёмный заголовок окна (Windows 10 20H1+ / 11)
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}

} // namespace gui
