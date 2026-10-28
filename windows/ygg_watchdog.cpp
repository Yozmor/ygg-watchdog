// ygg_watchdog.cpp
//
// Watchdog для Yggdrasil на Windows - полноценный .exe, без Python.
// Отдельный проект от yggdrasil_menu.cpp (панель ручного управления) -
// они никак не связаны и не должны зависеть друг от друга.
//
// Что делает:
//   - Читает основные пиры прямо из yggdrasil.conf (сам, без ручного
//     дублирования - именно так, как просили)
//   - Следит через yggdrasilctl, живы ли основные пиры
//   - Если ВСЕ основные легли - ищет резервного пира того же региона
//     через GitHub (public-peers), проверяет настоящим рукопожатием
//     (addPeer -> ждём Up -> либо оставляем, либо removePeer)
//   - Как только хоть один основной снова жив - убирает все резервные
//   - Пишет ПОДРОБНЫЙ лог: каждую проверку, каждый тест пира, каждую
//     ошибку - в файл рядом с самим exe
//   - Умеет сам зарегистрировать себя в Планировщике заданий Windows
//     (используя свой же реальный путь - никаких проблем с python.exe
//     и алиасами Windows Store, потому что мы теперь сами - .exe)
//
// Региональный режим (страна/город с подбором по координатам) -
// следующий этап, требует отдельной базы городов (GeoNames) - здесь
// пока не реализован, чтобы не подсовывать выдуманные данные.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // GetIpForwardTable2 и прочее из Vista+
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <regex>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <cmath>
#include <locale>
#include <codecvt>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <bcrypt.h>
#include <netfw.h>
#include "gui/core.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

// ------------------------------------------------------------------
// НАСТРОЙКИ
// ------------------------------------------------------------------
// Путь к yggdrasilctl и адрес управления (AdminListen) больше не зашиты в код:
// их находит ygg::CtlPaths() - по командной строке службы и по конфигу.

static const std::vector<std::wstring> CONFIG_CANDIDATES = {
    L"C:\\ProgramData\\Yggdrasil\\yggdrasil.conf",
    L"C:\\Program Files\\Yggdrasil\\yggdrasil.conf",
};

static const std::wstring GITHUB_API_HOST = L"api.github.com";
static const std::wstring GITHUB_RAW_HOST = L"raw.githubusercontent.com";
static const std::wstring GITHUB_TREE_PATH = L"/repos/yggdrasil-network/public-peers/git/trees/master?recursive=1";

static const int HANDSHAKE_TIMEOUT_MS = 15000;
static const int HANDSHAKE_POLL_INTERVAL_MS = 1500;

// ------------------------------------------------------------------
// Пути к файлам рядом с exe (не в ProgramData - договорились держать
// всё в одной папке)
// ------------------------------------------------------------------
std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    size_t pos = full.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L".\\" : full.substr(0, pos + 1);
}

std::wstring LogPath()   { return GetExeDir() + L"watchdog.log"; }
std::wstring StatePath() { return GetExeDir() + L"state.txt"; }

// ------------------------------------------------------------------
// Логирование - в файл (дозапись) и в консоль одновременно, с меткой
// времени. Каждая проверка пира, каждая ошибка - должны сюда попадать.
// ------------------------------------------------------------------
std::wstring NowTimestamp() {
    auto t = std::time(nullptr);
    std::tm tm;
    localtime_s(&tm, &t);
    std::wstringstream ss;
    ss << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S");
    return ss.str();
}

bool g_verboseMode = false;

// Вывод в консоль, а если stdout перенаправлен (окно сторожа запускает
// команды и читает их ответ через pipe) - UTF-8 через WriteFile.
// WriteConsoleW в pipe не пишет, поэтому без этого окно видело бы пустоту.
void WriteOut(const std::wstring& text) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(hOut, &mode)) {
        WriteConsoleW(hOut, text.c_str(), (DWORD)text.size(), &written, nullptr);
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], n, nullptr, nullptr);
    WriteFile(hOut, utf8.c_str(), (DWORD)utf8.size(), &written, nullptr);
}

void Log(const std::wstring& level, const std::wstring& message) {
    // DEBUG полностью скрыт по умолчанию (ни в консоль, ни в файл) -
    // только сама суть событий (что произошло, какие решения приняты).
    // Включается флагом -v для отладки, когда реально нужно видеть
    // сырой вывод getPeers и состояние каждого пира по отдельности.
    if (level == L"DEBUG" && !g_verboseMode) {
        return;
    }

    std::wstring line = L"[" + NowTimestamp() + L"] " + level + L": " + message;

    std::wstring withNewline = line + L"\r\n";
    WriteOut(withNewline);

    int size = WideCharToMultiByte(CP_UTF8, 0, withNewline.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, withNewline.c_str(), -1, &utf8[0], size, nullptr, nullptr);
    std::ofstream rawFile(std::filesystem::path(LogPath()), std::ios::app | std::ios::binary);
    if (rawFile.is_open()) {
        rawFile.write(utf8.c_str(), utf8.size() - 1); // без завершающего \0
    }
}

void LogInfo(const std::wstring& msg)  { Log(L"INFO", msg); }
void LogError(const std::wstring& msg) { Log(L"ERROR", msg); }
void LogDebug(const std::wstring& msg) { Log(L"DEBUG", msg); }

// Только на экран, БЕЗ записи в watchdog.log. Для списков, справок и
// ошибок ввода в ручных командах - чтобы лог оставался журналом работы
// сторожа (тики + результаты ручных действий), а не копией консоли.
void Print(const std::wstring& message) {
    WriteOut(message + L"\r\n");
}
void PrintError(const std::wstring& message) { Print(L"Ошибка: " + message); }

// ------------------------------------------------------------------
// Журнал значимых событий (events.log рядом с exe) - для окна сторожа.
// Одна строка = одно событие: unix-время <TAB> уровень <TAB> текст.
// Уровни: ok (восстановилось), warn (обходной путь: резерв и т.п.),
// alert (что-то легло), info (прочее). Пишется ВСЕГДА, даже если
// Telegram не настроен.
// ------------------------------------------------------------------
std::wstring EventsPath()  { return GetExeDir() + L"events.log"; }
std::wstring HistoryPath() { return GetExeDir() + L"history.log"; }

void AppendUtf8Line(const std::wstring& path, const std::wstring& line) {
    std::wstring withNewline = line + L"\r\n";
    int size = WideCharToMultiByte(CP_UTF8, 0, withNewline.c_str(), (int)withNewline.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, withNewline.c_str(), (int)withNewline.size(), &utf8[0], size, nullptr, nullptr);
    std::ofstream f(std::filesystem::path(path), std::ios::app | std::ios::binary);
    if (f.is_open()) f.write(utf8.c_str(), utf8.size());
}

void AppendEvent(const std::wstring& level, const std::wstring& message) {
    std::wstring clean = message;
    for (auto& c : clean) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    AppendUtf8Line(EventsPath(), std::to_wstring((long long)time(nullptr)) + L"\t" + level + L"\t" + clean);
}

// ------------------------------------------------------------------
// Чтение основных пиров прямо из yggdrasil.conf
// ------------------------------------------------------------------
std::optional<ygg::Paths> FindConfigPath() {
    ygg::Paths p = ygg::Discover();

    // 1. Штатный путь: тот, что нашла служба/реестр (или стандартный установщик)
    if (!p.conf.empty() && GetFileAttributesW(p.conf.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return p;
    }

    // 2. Известные пути установщика
    for (const auto& candidate : CONFIG_CANDIDATES) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            p.conf = candidate;
            // yggdrasil.exe нужен для LoadConfig/SaveConfig (валидация конфига) -
            // если Discover его не нашёл, достраиваем из соседней папки
            size_t pos = candidate.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                std::wstring dir = candidate.substr(0, pos + 1);
                std::wstring exe = dir + L"yggdrasil.exe";
                if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    p.exe = exe;
                    std::wstring ctl = dir + L"yggdrasilctl.exe";
                    if (GetFileAttributesW(ctl.c_str()) != INVALID_FILE_ATTRIBUTES) p.ctl = ctl;
                }
            }
            return p;
        }
    }
    return std::nullopt;
}

std::wstring ReadFileUtf8(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file.is_open()) return L"";
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string utf8 = buffer.str();

    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wide[0], size);
    return wide;
}

// Изменяет блок Peers: [...] в конфиге, вызывая mutator над списком URI
// и записывая результат обратно. Весь остальной конфиг (PrivateKey,
// MulticastInterfaces и т.д.) остаётся байт-в-байт нетронутым - меняется
// только содержимое самого блока Peers.
// quiet - для status-json и разбора: окно спрашивает их каждые 5 с, и ошибка
// чтения конфига не должна сыпаться в watchdog.log (о ней сообщает проверка).
std::vector<std::wstring> ReadMainPeers(const ygg::Paths& p, bool quiet = false) {
    std::vector<std::wstring> peers;
    json::Value cfg;
    std::wstring err;
    if (!ygg::LoadConfig(p, cfg, err)) {
        if (!quiet) LogError(L"Не удалось прочитать конфиг: " + err);
        return peers;
    }
    const json::Value& arr = cfg[L"Peers"];
    for (size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].isString()) peers.push_back(arr[i].str());
    }
    return peers;
}

bool ModifyMainPeers(const ygg::Paths& pIn, std::function<void(std::vector<std::wstring>&)> mutator) {
    ygg::Paths p = pIn;   // копия - ниже нам надо поменять p.adminListen

    json::Value cfg;
    std::wstring err;
    if (!ygg::LoadConfig(p, cfg, err)) {
        LogError(L"Не удалось прочитать конфиг: " + err);
        return false;
    }
    if (!cfg.isObject()) {
        LogError(L"Конфиг не является JSON-объектом");
        return false;
    }

    // SaveConfig умеет не потерять AdminListen, если -normaliseconf его не выдал,
    // но только когда p.adminListen уже заполнен. Заполняем заранее.
    p.adminListen = ygg::FindAdminListen(p, cfg);

    // Читаем текущий список
    std::vector<std::wstring> peers;
    const json::Value& arr = cfg[L"Peers"];
    for (size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].isString()) peers.push_back(arr[i].str());
    }

    // Мутируем (тот же mutator, что и раньше)
    mutator(peers);

    // Пишем обратно массивом строк
    json::Value newArr = json::MakeArray();
    for (const auto& uri : peers) newArr.arr->push_back(json::MakeString(uri));
    (*cfg.obj)[L"Peers"] = newArr;

    // Сохраняем - тот же SaveConfig, что у панели: проверка через yggdrasil -normaliseconf,
    // копия старого файла (.bak-YYYYMMDD-HHMMSS), потом запись
    std::wstring backup;
    err = ygg::SaveConfig(p, cfg, &backup);
    if (!err.empty()) {
        LogError(L"Не удалось сохранить конфиг: " + err);
        return false;
    }
    LogInfo(L"Конфиг сохранён. Копия: " + backup);
    return true;
}

void CmdListMainPeers() {
    auto configPath = FindConfigPath();
    if (!configPath.has_value()) {
        LogError(L"Не найден yggdrasil.conf");
        return;
    }
    auto peers = ReadMainPeers(*configPath);
    Print(L"Основные пиры (" + std::to_wstring(peers.size()) + L"):");
    for (size_t i = 0; i < peers.size(); ++i) {
        Print(L"  " + std::to_wstring(i + 1) + L") " + peers[i]);
    }
}

void CmdAddMainPeer(const std::wstring& uri) {
    auto configPath = FindConfigPath();
    if (!configPath.has_value()) {
        LogError(L"Не найден yggdrasil.conf");
        return;
    }
    auto existing = ReadMainPeers(*configPath);
    if (std::find(existing.begin(), existing.end(), uri) != existing.end()) {
        PrintError(L"такой основной пир уже есть в конфиге: " + uri);
        return;
    }
    bool ok = ModifyMainPeers(*configPath, [&](std::vector<std::wstring>& peers) {
        peers.push_back(uri);
    });
    if (ok) {
        LogInfo(L"Основной пир добавлен в конфиг: " + uri);
        Print(L"Изменения вступят в силу ТОЛЬКО после перезапуска службы Yggdrasil.");
    }
}

void CmdRemoveMainPeer(const std::wstring& indexStr) {
    auto configPath = FindConfigPath();
    if (!configPath.has_value()) {
        LogError(L"Не найден yggdrasil.conf");
        return;
    }
    auto currentPeers = ReadMainPeers(*configPath);

    size_t idx;
    try {
        idx = std::stoul(indexStr);
    } catch (...) {
        PrintError(L"нужен номер пира из списка (команда list-main-peers)");
        return;
    }
    if (idx < 1 || idx > currentPeers.size()) {
        PrintError(L"нет пира с номером " + indexStr);
        return;
    }
    std::wstring toRemove = currentPeers[idx - 1];

    // Удаляем ровно выбранную по номеру запись - если в конфиге есть
    // дубли одного адреса, второй экземпляр остаётся.
    bool ok = ModifyMainPeers(*configPath, [&](std::vector<std::wstring>& peers) {
        if (idx - 1 < peers.size() && peers[idx - 1] == toRemove) peers.erase(peers.begin() + (idx - 1));
    });
    if (ok) {
        LogInfo(L"Основной пир удалён из конфига: " + toRemove);
        Print(L"Изменения вступят в силу ТОЛЬКО после перезапуска службы Yggdrasil.");
    }
}


// ------------------------------------------------------------------
// Запуск yggdrasilctl с перехватом вывода
// ------------------------------------------------------------------
// Любой внешний процесс - с таймаутом: зависший yggdrasilctl / netsh / ping
// не должен вешать всю проверку (см. util::Run).
std::wstring RunProcessCapture(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs = 60000) {
    util::ProcResult r = util::Run(exe, args, timeoutMs);
    if (r.timedOut) LogError(L"Процесс не ответил за " + std::to_wstring(timeoutMs / 1000) + L" с и был остановлен: " + exe);
    return util::FromUtf8(r.out);
}

std::wstring RunYggCtl(const std::wstring& args) {
    const ygg::Paths& p = ygg::CtlPaths();
    return RunProcessCapture(p.ctl, p.EndpointArg() + args, 20000);
}

void AddPeer(const std::wstring& uri) {
    LogDebug(L"addPeer " + uri);
    RunYggCtl(L"addPeer uri=" + uri);
}

void RemovePeer(const std::wstring& uri) {
    LogDebug(L"removePeer " + uri);
    RunYggCtl(L"removePeer uri=" + uri);
}

// ------------------------------------------------------------------
// Пиры из getPeers в JSON (yggdrasilctl -json, Yggdrasil 0.5.x): массив
// "peers" из объектов; "latency" - в НАНОСЕКУНДАХ (есть только у Up-пиров),
// "uptime" - секунды с момента подключения. Разбор - json_lite, без regex.
// ------------------------------------------------------------------
struct PeerDetail {
    std::wstring uri;
    bool up = false;
    double rttMs = -1;      // -1 = неизвестно
    double uptimeSec = -1;  // -1 = неизвестно
    std::wstring lastError; // "last_error" из getPeers - почему пир не поднимается
    int flaps = 0;          // сколько раз переподключался за последние полчаса (из state)
    bool unstable = false;  // flaps >= порога: "Up", но держится плохо
};

bool g_lastPeersOk = false;   // ответил ли yggdrasilctl на последний getPeers

// Итоги настоящих рукопожатий за этот запуск (uri, поднялся ли) - для статистики
std::vector<std::pair<std::wstring, bool>> g_handshakeResults;

// Разбор ответа "yggdrasilctl -json getPeers" (без запуска - удобно для тестов)
std::vector<PeerDetail> ParsePeersJson(const std::wstring& out, bool& ok) {
    std::vector<PeerDetail> result;
    json::Value v;
    ok = json::Parse(util::ToUtf8(out), v) && v[L"peers"].isArray();
    if (!ok) return result;
    const json::Value& arr = v[L"peers"];
    for (size_t i = 0; i < arr.size(); ++i) {
        PeerDetail d;
        d.uri = arr[i][L"remote"].str();
        if (d.uri.empty()) continue;
        d.up = arr[i][L"up"].boolean();
        double lat = arr[i][L"latency"].num(-1);
        if (d.up && lat >= 0) d.rttMs = lat / 1e6;
        d.uptimeSec = arr[i][L"uptime"].num(-1);
        d.lastError = arr[i][L"last_error"].str();
        result.push_back(d);
    }
    return result;
}

std::vector<PeerDetail> GetPeerDetails() {
    std::wstring out = RunYggCtl(L"-json getPeers");
    LogDebug(L"Сырой вывод getPeers:\r\n" + out);
    return ParsePeersJson(out, g_lastPeersOk);
}

const PeerDetail* FindPeer(const std::vector<PeerDetail>& v, const std::wstring& uri) {
    for (const auto& d : v) if (_wcsicmp(d.uri.c_str(), uri.c_str()) == 0) return &d;
    return nullptr;
}

// Настоящая проверка: подключаем пир к Yggdrasil и ждём Up. Один вызов
// yggdrasilctl на шаг ожидания (раньше было два: текстовый getPeers + JSON).
bool VerifyPeerHandshake(const std::wstring& uri) {
    AddPeer(uri);
    auto start = std::chrono::steady_clock::now();
    bool success = false;
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < HANDSHAKE_TIMEOUT_MS) {
        auto details = GetPeerDetails();
        const PeerDetail* d = FindPeer(details, uri);
        if (d && d->up) { success = true; break; }
        // Yggdrasil уже написал ошибку подключения (обрыв, отказ, таймаут) - ждать
        // остаток времени нет смысла, пир не поднимется
        if (d && !d->lastError.empty()) break;
        Sleep(HANDSHAKE_POLL_INTERVAL_MS);
    }
    RemovePeer(uri);
    g_handshakeResults.push_back({ uri, success });
    return success;
}

// ------------------------------------------------------------------
// Диагностика: пропал ли интернет вообще (не только Yggdrasil-пиры) -
// TCP-подключение с коротким таймаутом к нескольким устойчивым
// публичным адресам, без DNS, без зависимости от Yggdrasil. Тот же
// принцип, что и в Linux-версии.
// ------------------------------------------------------------------
bool TryConnectTcp(const char* ip, int port, int timeoutMs) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    u_long mode = 1; // неблокирующий режим
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    connect(sock, (sockaddr*)&addr, sizeof(addr)); // ожидаемо вернёт WSAEWOULDBLOCK

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int result = select(0, nullptr, &writeSet, nullptr, &tv);
    bool connected = false;
    if (result > 0 && FD_ISSET(sock, &writeSet)) {
        int err = 0;
        int len = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
        connected = (err == 0);
    }

    closesocket(sock);
    return connected;
}

bool HasInternetConnectivity() {
    static bool wsaInitialized = false;
    if (!wsaInitialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        wsaInitialized = true;
    }

    const char* targets[] = { "1.1.1.1", "8.8.8.8", "9.9.9.9" };
    for (const char* ip : targets) {
        if (TryConnectTcp(ip, 443, 3000)) {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------
// WinHTTP - простой GET-запрос (HTTPS)
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Открывает WinHTTP-сессию с учётом системного прокси (тот, что видит
// браузер/большинство обычных программ через "Параметры -> Сеть ->
// Прокси" - у WinHTTP по умолчанию СВОЯ, независимая настройка,
// которая ничего не знает про VPN-клиенты вроде v2rayN/v2rayTun,
// настраивающие именно эту, классическую системную настройку).
// ------------------------------------------------------------------
std::vector<std::wstring> SplitBy(const std::wstring& s, wchar_t delim) {
    std::vector<std::wstring> parts;
    std::wstringstream ss(s);
    std::wstring item;
    while (std::getline(ss, item, delim)) {
        if (!item.empty()) parts.push_back(item);
    }
    return parts;
}

struct NotifyConfig {
    std::wstring botToken;
    std::wstring chatId;
    std::wstring proxyOverride; // необязательный ключ PROXY - в обход
                                 // чтения системной настройки текущего
                                 // пользователя (нужен для задачи
                                 // планировщика, которая работает от
                                 // имени SYSTEM и не видит HKCU обычного
                                 // пользователя)
    std::wstring proxyClients;  // PROXY_CLIENTS - свои VPN-клиенты для автопоиска (части имени exe через запятую)
    bool valid = false; // именно для Telegram - нужны оба BOT_TOKEN/CHAT_ID
    std::wstring problem;       // токен или chat_id заполнены, но выглядят неправильно - что не так
    bool proxySocks = false;    // в PROXY= вписан socks5://... - WinHTTP так не умеет, путь пропускается
};

// Токен и chat_id часто копируются с мусором: неразрывный пробел или
// невидимые символы из Telegram, "bot" из ссылки api.telegram.org/bot<токен>,
// угловые скобки из инструкции. Всё это убирается.
std::wstring CleanTelegramValue(const std::wstring& v, bool token) {
    std::wstring in = v;
    for (size_t p; (p = util::Lower(in).find(L"%3a")) != std::wstring::npos;) in.replace(p, 3, L":");   // из ссылки
    std::wstring out;
    for (wchar_t c : in) {
        // похожие на двоеточие: полноширинное, математическое, модификаторы
        if (c == 0xFF1A || c == 0x2236 || c == 0xA789 || c == 0x02D0 || c == 0x02F8 || c == 0xFE13 || c == 0xFE55 || c == 0x0589 || c == 0x05C3) c = L':';
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0 || c == 0xFEFF || c == 0x2060 ||
            (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || c == L'<' || c == L'>' || c == L'"' || c == L'\'')
            continue;
        out += c;
    }
    if (token && out.size() > 3 && util::Lower(out.substr(0, 3)) == L"bot" && iswdigit(out[3])) out.erase(0, 3);
    return out;
}

// Что не так с токеном (пусто - похож на настоящий): 123456789:AAH... - цифры, двоеточие, латиница/цифры/_/-
// Приметы токена без секретной части: длина, первые 3 знака, необычные символы
std::wstring TokenHints(const std::wstring& t) {
    std::wstring odd;
    std::set<wchar_t> seen;
    for (wchar_t c : t) {
        bool plain = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'-' || c == L':';
        if (plain || !seen.insert(c).second || seen.size() > 5) continue;
        wchar_t hex[8];
        swprintf(hex, 8, L"%04X", (unsigned)c);
        odd += (odd.empty() ? L"" : L" ") + std::wstring(L"«") + c + L"»U+" + hex;
    }
    return L"длина " + std::to_wstring(t.size()) + L", начало «" + t.substr(0, 3) + L"…»" +
           (odd.empty() ? L"" : L", необычные символы: " + odd);
}

std::wstring TelegramTokenProblem(const std::wstring& t) {
    size_t colon = t.find(L':');
    bool digitsOnly = !t.empty() && std::all_of(t.begin(), t.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (colon == std::wstring::npos && digitsOnly)
        return L"в BOT_TOKEN одни цифры (" + TokenHints(t) + L") - похоже, туда попал chat_id или только номер бота. "
               L"Токен от @BotFather выглядит как 123456789:AAH... - с двоеточием и буквами";
    if (colon == std::wstring::npos)
        return L"в BOT_TOKEN нет двоеточия (" + TokenHints(t) + L") - нужен вид 123456789:AAH..., около 46 знаков. "
               L"Скопируй токен у @BotFather заново целиком";
    if (colon == 0) return L"в BOT_TOKEN нет цифр перед двоеточием - нужен вид 123456789:AAH...";
    if (colon + 1 >= t.size()) return L"в BOT_TOKEN пусто после двоеточия - скопирован не целиком";
    for (size_t i = 0; i < t.size(); ++i) {
        wchar_t c = t[i];
        bool ok = i < colon ? (c >= L'0' && c <= L'9')
                            : ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'-');
        if (i == colon) ok = true;
        if (!ok) {
            wchar_t hex[8];
            swprintf(hex, 8, L"%04X", (unsigned)c);
            return L"в BOT_TOKEN лишний символ «" + std::wstring(1, c) + L"» (U+" + hex + L") на месте " + std::to_wstring(i + 1) +
                   L" - впиши токен заново в окне («Управление» → новый токен бота)";
        }
    }
    return L"";
}

NotifyConfig LoadNotifyConfig() {
    NotifyConfig config;
    std::wstring text = ReadFileUtf8(GetExeDir() + L"notify.conf");
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);   // метка BOM от Блокнота
    if (text.empty()) return config;

    for (auto line : SplitBy(text, L'\n')) {
        line = util::Trim(line);   // пробелы и \r по краям, отступы
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = util::Trim(line.substr(0, eq));   // "BOT_TOKEN = ..." тоже годится
        std::wstring value = util::Trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') && value.back() == value.front())
            value = util::Trim(value.substr(1, value.size() - 2));   // значение в кавычках
        if (key == L"BOT_TOKEN") config.botToken = CleanTelegramValue(value, true);
        else if (key == L"CHAT_ID") config.chatId = CleanTelegramValue(value, false);
        else if (key == L"PROXY") {
            size_t p = value.find(L"://");   // http://127.0.0.1:2080 -> 127.0.0.1:2080
            if (p != std::wstring::npos) {
                if (util::Lower(value.substr(0, p)).rfind(L"socks", 0) == 0) config.proxySocks = true;
                value = value.substr(p + 3);
            }
            while (!value.empty() && value.back() == L'/') value.pop_back();
            config.proxyOverride = config.proxySocks ? L"" : value;   // SOCKS WinHTTP не умеет - такой путь пропускается
        }
        else if (key == L"PROXY_CLIENTS") config.proxyClients = value;
    }
    config.valid = !config.botToken.empty() && !config.chatId.empty();
    if (config.valid) {
        bool chatOk = !config.chatId.empty();
        if (chatOk && config.chatId[0] == L'@') chatOk = config.chatId.size() > 1;
        else for (size_t i = 0; chatOk && i < config.chatId.size(); ++i)
            if (!(iswdigit(config.chatId[i]) || (i == 0 && config.chatId[i] == L'-'))) chatOk = false;
        std::wstring tp = TelegramTokenProblem(config.botToken);
        if (!tp.empty()) config.problem = tp;
        else if (!chatOk) config.problem = L"CHAT_ID в notify.conf выглядит неправильно: нужно число из getUpdates (у групп с минусом впереди)";
    }
    return config;
}


// ------------------------------------------------------------------
// Как достучаться до сайта (Telegram, GitHub): маршруты по очереди, пока
// какой-то не сработает.
//   1. тот, что сработал в прошлый раз для этого сайта (запомнен в реестре);
//   2. PROXY= из notify.conf;
//   3. подсказка от VPN-клиента: значение LocalProxy в HKLM\SOFTWARE\Yozmor
//      или первая строка файла %ProgramData%\Yozmor\local_proxy.txt
//      (например "127.0.0.1:2080") - так свой клиент может сообщить порт;
//   4. напрямую (работает, когда VPN-клиент в TUN-режиме);
//   5. системный прокси пользователя (есть у окна и трея, у SYSTEM - нет);
//   6. автопоиск: какие известные VPN-клиенты слушают порты на 127.0.0.1,
//      каждый порт проверяется запросом CONNECT к нужному сайту.
// Сработавший маршрут запоминается. WinHTTP умеет только HTTP-прокси
// (с CONNECT) - чистый SOCKS-порт не подойдёт.
// ------------------------------------------------------------------
static const wchar_t* REG_YOZMOR = L"SOFTWARE\\Yozmor";
static const wchar_t* REG_WATCHDOG_KEY = L"SOFTWARE\\Yozmor\\ygg-watchdog";

struct HttpRoute {
    std::wstring kind;    // direct / ie / proxy
    std::wstring proxy;   // "127.0.0.1:2080" для proxy
    std::wstring label;   // для людей: "напрямую", "Karing (127.0.0.1:3067)"
};

struct HttpResult {
    bool transportOk = false;   // сервер ответил (любым HTTP-кодом)
    DWORD status = 0;
    DWORD winErr = 0;           // код WinHTTP, если не дошли
    std::string body;
    bool readOk = true;         // тело дочитано без обрыва
    bool haveLen = false;
    unsigned long long expectedLen = 0, gotLen = 0;
    std::wstring via;           // каким маршрутом
};

using BodySink = std::function<bool(const char*, DWORD)>;

HttpResult HttpOnce(const HttpRoute& rt, const wchar_t* method, const std::wstring& host, const std::wstring& path,
                    const std::wstring& headers, const std::string* body, const BodySink& sink, DWORD recvTimeoutMs,
                    size_t maxBody = 64 * 1024 * 1024) {
    HttpResult r;
    r.via = rt.label;
    HINTERNET s = nullptr;
    const wchar_t* ua = L"ygg-watchdog/1.0";
    if (rt.kind == L"proxy") {
        s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NAMED_PROXY, rt.proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
    } else if (rt.kind == L"ie") {
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie = {};
        if (WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
            if (ie.lpszProxy) s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NAMED_PROXY, ie.lpszProxy,
                                              ie.lpszProxyBypass ? ie.lpszProxyBypass : WINHTTP_NO_PROXY_BYPASS, 0);
            else if (ie.lpszAutoConfigUrl || ie.fAutoDetect) s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (ie.lpszAutoConfigUrl) GlobalFree(ie.lpszAutoConfigUrl);
            if (ie.lpszProxy) GlobalFree(ie.lpszProxy);
            if (ie.lpszProxyBypass) GlobalFree(ie.lpszProxyBypass);
        }
        if (!s) { r.winErr = ERROR_NOT_FOUND; return r; }   // системного прокси нет - маршрута нет
    } else {
        s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!s) { r.winErr = GetLastError(); return r; }
    WinHttpSetTimeouts(s, 5000, 5000, 15000, (int)recvTimeoutMs);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (q) {
        std::wstring hdr = L"User-Agent: ygg-watchdog\r\n" + headers;
        BOOL sent = WinHttpSendRequest(q, hdr.c_str(), (DWORD)hdr.size(),
                                       body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA,
                                       body ? (DWORD)body->size() : 0, body ? (DWORD)body->size() : 0, 0);
        if (sent && WinHttpReceiveResponse(q, nullptr)) {
            r.transportOk = true;
            DWORD sz = sizeof(r.status);
            WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &sz, WINHTTP_NO_HEADER_INDEX);
            DWORD len = 0; sz = sizeof(len);
            r.haveLen = WinHttpQueryHeaders(q, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &len, &sz, WINHTTP_NO_HEADER_INDEX) != FALSE;
            r.expectedLen = len;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(q, &avail)) { r.readOk = false; break; }
                if (avail == 0) break;
                std::vector<char> buf(avail);
                DWORD got = 0;
                if (!WinHttpReadData(q, buf.data(), avail, &got)) { r.readOk = false; break; }
                r.gotLen += got;
                if (sink && r.status == 200) { if (!sink(buf.data(), got)) { r.readOk = false; break; } }
                else if (r.body.size() < maxBody) r.body.append(buf.data(), got);
            }
        } else {
            r.winErr = GetLastError();
        }
        WinHttpCloseHandle(q);
    } else {
        r.winErr = GetLastError();
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return r;
}

std::wstring RouteToText(const HttpRoute& r) { return r.kind + L"|" + r.proxy + L"|" + r.label; }
bool RouteFromText(const std::wstring& t, HttpRoute& r) {
    size_t a = t.find(L'|'), b = a == std::wstring::npos ? a : t.find(L'|', a + 1);
    if (b == std::wstring::npos) return false;
    r.kind = t.substr(0, a); r.proxy = t.substr(a + 1, b - a - 1); r.label = t.substr(b + 1);
    return r.kind == L"direct" || r.kind == L"ie" || (r.kind == L"proxy" && !r.proxy.empty());
}

// Подсказки от VPN-клиента: реестр и файл в ProgramData
std::vector<std::wstring> ProxyHints() {
    std::vector<std::wstring> v;
    std::wstring reg = util::Trim(util::RegReadPath(REG_YOZMOR, L"LocalProxy"));
    if (!reg.empty()) v.push_back(reg);
    wchar_t pd[MAX_PATH] = L"";
    ExpandEnvironmentStringsW(L"%ProgramData%\\Yozmor\\local_proxy.txt", pd, MAX_PATH);
    std::wstring f = util::FromUtf8(util::ReadFileBytes(pd));
    size_t nl = f.find_first_of(L"\r\n");
    f = util::Trim(nl == std::wstring::npos ? f : f.substr(0, nl));
    if (!f.empty() && std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
    for (auto& h : v) { size_t p = h.find(L"://"); if (p != std::wstring::npos) h = h.substr(p + 3); }   // http://127.0.0.1:2080 -> 127.0.0.1:2080
    return v;
}

// Встроенный список VPN-клиентов (части имени exe) + свои из PROXY_CLIENTS
std::vector<std::wstring> ProxyClientNames() {
    std::vector<std::wstring> v = { L"karing", L"sing-box", L"singbox", L"xray", L"v2ray", L"nekoray", L"nekobox", L"throne",
                                    L"hiddify", L"clash", L"mihomo", L"naive", L"happ", L"invisible" };
    for (const auto& x : SplitBy(LoadNotifyConfig().proxyClients, L',')) {
        std::wstring t = util::Lower(util::Trim(x));
        if (t.size() > 4 && t.compare(t.size() - 4, 4, L".exe") == 0) t.resize(t.size() - 4);
        if (!t.empty()) v.push_back(t);
    }
    return v;
}

// TCP-подключение к 127.0.0.1:port за 1 с, дальше блокирующий сокет с тайм-аутом 6 с
SOCKET ConnectLoopback(unsigned short port) {
    static util::WsaInit wsa;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr*)&a, sizeof(a));
    fd_set w; FD_ZERO(&w); FD_SET(s, &w);
    timeval tv = { 1, 0 };
    if (select(0, nullptr, &w, nullptr, &tv) <= 0) { closesocket(s); return INVALID_SOCKET; }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 6000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    return s;
}

// Пускает ли HTTP-прокси на 127.0.0.1:port к host:443 (ответ "HTTP/1.x 200" на CONNECT)
bool TestHttpConnectProxy(unsigned short port, const std::wstring& host) {
    SOCKET s = ConnectLoopback(port);
    if (s == INVALID_SOCKET) return false;
    bool ok = false;
    std::string h = util::ToUtf8(host);
    std::string req = "CONNECT " + h + ":443 HTTP/1.1\r\nHost: " + h + ":443\r\n\r\n";
    if (send(s, req.data(), (int)req.size(), 0) == (int)req.size()) {
        char buf[256];
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n >= 12) {
            buf[n] = 0;
            std::string resp(buf, n);
            ok = resp.compare(0, 7, "HTTP/1.") == 0 && resp.compare(9, 3, "200") == 0;
        }
    }
    closesocket(s);
    return ok;
}

// Порт - SOCKS5? (на приветствие 05 01 00 отвечает 05 xx) - только чтобы подсказать человеку
bool LooksLikeSocks5(unsigned short port) {
    SOCKET s = ConnectLoopback(port);
    if (s == INVALID_SOCKET) return false;
    const char hello[3] = { 5, 1, 0 };
    bool yes = false;
    if (send(s, hello, 3, 0) == 3) {
        char buf[8];
        int n = recv(s, buf, sizeof(buf), 0);
        yes = n >= 2 && buf[0] == 5;
    }
    closesocket(s);
    return yes;
}

// Ход перебора путей последнего запроса - для test-notify и строки «НЕ ушло» в окне
std::vector<std::wstring> g_routeTrace;

std::wstring WinHttpErrText(DWORD e) {
    switch (e) {
    case 12002: return L"тайм-аут (похоже на блокировку)";
    case 12007: return L"имя сайта не находится (DNS)";
    case 12029: return L"не подключиться";
    case 12030: return L"соединение оборвано";
    case 12152: return L"непонятный ответ сервера";
    case 12157: case 12175: return L"ошибка TLS (защищённое соединение не установилось)";
    case 12180: return L"автонастройка прокси не нашлась";
    case ERROR_NOT_FOUND: return L"не задан";
    default: return L"ошибка WinHTTP " + std::to_wstring(e);
    }
}

struct ProxyScan { std::vector<HttpRoute> routes; std::vector<std::wstring> notes; };

// Автопоиск: известные VPN-клиенты, слушающие 127.0.0.1 / 0.0.0.0, чей порт пускает к host
ProxyScan ScanLocalProxies(const std::wstring& host) {
    ProxyScan res;
    std::vector<HttpRoute>& out = res.routes;
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<BYTE> buf(size + 4096);
    size = (DWORD)buf.size();
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) {
        res.notes.push_back(L"автопоиск: не удалось прочитать список портов");
        return res;
    }
    auto* t = (MIB_TCPTABLE_OWNER_PID*)buf.data();
    auto names = ProxyClientNames();
    std::map<DWORD, std::wstring> procName;
    std::set<unsigned short> tried;
    for (DWORD i = 0; i < t->dwNumEntries && out.size() < 3 && tried.size() < 20; ++i) {
        const auto& row = t->table[i];
        DWORD addr = ntohl(row.dwLocalAddr);
        if (addr != INADDR_LOOPBACK && addr != INADDR_ANY) continue;
        unsigned short port = ntohs((u_short)row.dwLocalPort);
        if (tried.count(port)) continue;
        if (!procName.count(row.dwOwningPid)) {
            std::wstring n;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row.dwOwningPid);
            if (p) {
                wchar_t path[MAX_PATH * 2]; DWORD len = MAX_PATH * 2;
                if (QueryFullProcessImageNameW(p, 0, path, &len)) n = std::filesystem::path(path).filename().wstring();
                CloseHandle(p);
            }
            procName[row.dwOwningPid] = n;
        }
        std::wstring pn = procName[row.dwOwningPid], low = util::Lower(pn);
        bool known = false;
        for (const auto& k : names) if (!k.empty() && low.find(k) != std::wstring::npos) { known = true; break; }
        if (!known) continue;
        tried.insert(port);
        std::wstring a = L"127.0.0.1:" + std::to_wstring(port);
        std::wstring clean = pn.size() > 4 && util::Lower(pn.substr(pn.size() - 4)) == L".exe" ? pn.substr(0, pn.size() - 4) : pn;
        if (TestHttpConnectProxy(port, host)) out.push_back({ L"proxy", a, clean + L" (" + a + L")" });
        else if (LooksLikeSocks5(port)) res.notes.push_back(clean + L" (" + a + L") - SOCKS-порт, а нужен HTTP или mixed");
        else res.notes.push_back(clean + L" (" + a + L") - не пустил к " + host);
    }
    if (tried.empty()) res.notes.push_back(L"автопоиск: известные VPN-клиенты не слушают порты на 127.0.0.1");
    return res;
}

// Дошли ли до самого сайта, а не до ошибки прокси по дороге
bool ReachedSite(const std::wstring& host, const HttpResult& r) {
    if (!r.transportOk) return false;
    if (r.status == 407 || r.status == 502 || r.status == 503 || r.status == 504) return false;   // так отвечает прокси, у которого нет выхода
    if (util::Lower(host) == L"api.telegram.org") return r.body.find("\"ok\"") != std::string::npos;   // Telegram всегда отвечает JSON с "ok"
    return true;
}

// Сам запрос: маршруты по очереди. allowProxies=false - только напрямую
// (GeoIP: нужна страна, откуда выходят соединения самого Yggdrasil, а не прокси).
HttpResult HttpRequest(const wchar_t* method, const std::wstring& host, const std::wstring& path, const std::wstring& headers,
                       const std::string* body, const BodySink& sink = nullptr, DWORD recvTimeoutMs = 30000,
                       bool allowProxies = true, const std::function<void()>& beforeRetry = nullptr) {
    g_routeTrace.clear();
    std::vector<HttpRoute> routes;
    auto add = [&](const HttpRoute& r) {
        for (const auto& x : routes) if (x.kind == r.kind && x.proxy == r.proxy) return;
        routes.push_back(r);
    };
    std::wstring cacheKey = L"Route_" + util::Lower(host);
    HttpRoute cached;
    if (allowProxies && RouteFromText(util::RegReadPath(REG_WATCHDOG_KEY, cacheKey.c_str()), cached)) add(cached);
    if (allowProxies) {
        NotifyConfig nc = LoadNotifyConfig();
        std::wstring px = nc.proxyOverride;
        if (nc.proxySocks) g_routeTrace.push_back(L"PROXY из notify.conf - SOCKS, пропущен: нужен HTTP-порт (http или mixed)");
        if (!px.empty()) add({ L"proxy", px, L"PROXY из notify.conf (" + px + L")" });
        for (const auto& h : ProxyHints()) add({ L"proxy", h, L"прокси, указанный клиентом (" + h + L")" });
    }
    add({ L"direct", L"", L"напрямую" });
    if (allowProxies) add({ L"ie", L"", L"системный прокси" });

    HttpResult last;
    std::set<std::wstring> triedRoutes;
    auto attempt = [&](const HttpRoute& rt) {
        if (!triedRoutes.insert(rt.kind + L"|" + rt.proxy).second) return false;   // автопоиск нашёл уже проверенный порт
        if (beforeRetry) beforeRetry();
        HttpResult r = HttpOnce(rt, method, host, path, headers, body, sink, recvTimeoutMs);
        if (ReachedSite(host, r)) {
            if (allowProxies && RouteToText(rt) != RouteToText(cached))
                util::RegWriteString(REG_WATCHDOG_KEY, cacheKey.c_str(), RouteToText(rt));   // без прав (трей) - молча не запишется
            g_routeTrace.push_back(rt.label + L" - дошло");
            last = r;
            return true;
        }
        if (r.transportOk) {
            // ответ есть, но не от сайта: прокси без выхода в интернет и т.п. - пробуем дальше
            g_routeTrace.push_back(rt.label + L" - ответ HTTP " + std::to_wstring(r.status) + L" не от " + host + L" (прокси без выхода?)");
            if (!last.transportOk) last = r;
        } else {
            g_routeTrace.push_back(rt.label + L" - " + WinHttpErrText(r.winErr));
            if (r.winErr != ERROR_NOT_FOUND && !last.transportOk) last = r;
        }
        return false;
    };
    for (const auto& rt : routes) if (attempt(rt)) return last;
    if (allowProxies) {
        // автопоиск - один раз на сайт за запуск программы (сканирование портов не бесплатное)
        static std::map<std::wstring, ProxyScan> scanned;
        std::wstring key = util::Lower(host);
        if (!scanned.count(key)) scanned[key] = ScanLocalProxies(host);
        for (const auto& n : scanned[key].notes) g_routeTrace.push_back(n);
        for (const auto& rt : scanned[key].routes) if (attempt(rt)) return last;
    }
    // ни один путь не дошёл до сайта: ответ прокси без выхода - не успех
    if (last.transportOk && !ReachedSite(host, last)) { last.transportOk = false; last.winErr = 0; }
    return last;
}

std::wstring RouteTraceText() {
    std::wstring t;
    for (const auto& x : g_routeTrace) { if (!t.empty()) t += L"; "; t += x; }
    return t;
}

DWORD g_lastHttpStatus = 0;   // код ответа последнего HttpGetUtf8 (0 - не дошёл)

std::string HttpGetUtf8(const std::wstring& host, const std::wstring& path, bool allowProxies = true) {
    HttpResult r = HttpRequest(L"GET", host, path, L"", nullptr, nullptr, 30000, allowProxies);
    g_lastHttpStatus = r.status;
    if (!r.transportOk) {
        LogError(L"HTTP-запрос не удался: " + host + path + L" - " + RouteTraceText());
        return "";
    }
    if (r.status == 404) { LogDebug(L"HTTP 404 от " + host + path); return ""; }
    if (r.status != 200) {
        // 403/429 у api.github.com - обычно лимит запросов без авторизации (60 в час)
        LogError(L"HTTP " + std::to_wstring(r.status) + L" от " + host + path);
        return "";
    }
    return r.body;
}

// ------------------------------------------------------------------
// WinHTTP - простой POST-запрос (HTTPS), с телом. Используется для
// отправки уведомлений в Telegram.
// ------------------------------------------------------------------

// path через regex, не пишем полноценный JSON-парсер ради одного поля
// ------------------------------------------------------------------
struct CountryFile {
    std::wstring slug;
    std::wstring rawUrlPath; // путь на raw.githubusercontent.com
};

// ------------------------------------------------------------------
// Кэш базы пиров (папка peers_cache рядом с exe). Список стран и файлы
// стран меняются редко: свежий кэш (до 6 ч) используется без запроса к
// GitHub - это бережёт лимит api.github.com (60 запросов в час без
// авторизации). Если GitHub недоступен - берём кэш любой давности:
// старая база лучше, чем никакой, когда все пиры легли.
// ------------------------------------------------------------------
const long long PEER_CACHE_FRESH_SECONDS = 6 * 3600;

std::wstring PeerCacheDir() { return GetExeDir() + L"peers_cache\\"; }

// Возраст файла в секундах, -1 если файла нет
long long FileAgeSeconds(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return -1;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER a, b;
    a.LowPart = fad.ftLastWriteTime.dwLowDateTime; a.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    b.LowPart = nowFt.dwLowDateTime; b.HighPart = nowFt.dwHighDateTime;
    if (b.QuadPart < a.QuadPart) return 0;
    return (long long)((b.QuadPart - a.QuadPart) / 10000000ULL);
}

std::string ReadBytes(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void WriteBytes(const std::wstring& path, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(PeerCacheDir()), ec);
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (f.is_open()) f.write(data.c_str(), data.size());
}

// Получить через кэш: свежий кэш -> без сети; иначе сеть; при неудаче - старый кэш
std::string FetchCached(const std::wstring& cacheName, const std::wstring& host, const std::wstring& path,
                        const std::string& mustContain) {
    std::wstring cachePath = PeerCacheDir() + cacheName;
    long long age = FileAgeSeconds(cachePath);
    if (age >= 0 && age < PEER_CACHE_FRESH_SECONDS) {
        std::string cached = ReadBytes(cachePath);
        if (!cached.empty()) return cached;
    }
    std::string fresh = HttpGetUtf8(host, path);
    if (!fresh.empty() && (mustContain.empty() || fresh.find(mustContain) != std::string::npos)) {
        WriteBytes(cachePath, fresh);
        return fresh;
    }
    if (age >= 0) {
        std::string cached = ReadBytes(cachePath);
        if (!cached.empty()) {
            LogInfo(L"GitHub недоступен - использую сохранённую базу пиров (" + cacheName + L", возраст " +
                    std::to_wstring(age / 3600) + L" ч)");
            return cached;
        }
    }
    return "";
}

std::vector<CountryFile> ListCountryFiles() {
    std::string response = FetchCached(L"tree.json", GITHUB_API_HOST, GITHUB_TREE_PATH, "\"tree\"");
    std::vector<CountryFile> result;
    if (response.empty()) return result;

    // Ищем "path":"континент/страна.md","type":"blob"
    std::regex pathRe("\"path\"\\s*:\\s*\"([a-z\\-]+/[a-z0-9\\-]+)\\.md\"[^}]*\"type\"\\s*:\\s*\"blob\"");
    auto begin = std::sregex_iterator(response.begin(), response.end(), pathRe);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string fullPath = (*it)[1].str(); // "europe/russia"
        size_t slashPos = fullPath.find('/');
        std::string slug = fullPath.substr(slashPos + 1);

        CountryFile cf;
        cf.slug = std::wstring(slug.begin(), slug.end());
        std::string rawPath = "/yggdrasil-network/public-peers/master/" + fullPath + ".md";
        cf.rawUrlPath = std::wstring(rawPath.begin(), rawPath.end());
        result.push_back(cf);
    }
    return result;
}

struct PeerCandidate {
    std::wstring uri;
    std::wstring city;
};

// Хост пира: tls://host:443, tls://[2001:db8::1]:443, quic://1.2.3.4:443?key=...
// Разбор - util::ParseUri (тот же, что у панели): IPv6 в скобках понимает правильно.
std::wstring HostFromUri(const std::wstring& uri) {
    util::PeerUri u = util::ParseUri(uri);
    return u.ok ? util::Lower(u.host) : L"";
}

// Ищет, к какой стране относится хост уже настроенного пира - перебирая
// файлы стран по одному, пока не найдёт совпадение. Дорого (может
// скачать много файлов в худшем случае), но вызывается только один раз -
// результат сразу кэшируется в state.backup_region и больше не ищется
// заново, пока не сбросится вручную.


std::vector<PeerCandidate> FetchCountryPeers(const std::wstring& countrySlug) {
    std::vector<PeerCandidate> result;
    auto files = ListCountryFiles();

    std::wstring targetPath;
    for (const auto& f : files) {
        if (f.slug == countrySlug) {
            targetPath = f.rawUrlPath;
            break;
        }
    }
    if (targetPath.empty()) {
        if (files.empty()) {
            LogError(L"Нет списка стран: GitHub недоступен, а сохранённой базы пиров ещё нет");
        } else {
            LogError(L"Страна не найдена в репозитории: " + countrySlug);
        }
        return result;
    }

    std::string raw = FetchCached(countrySlug + L".md", GITHUB_RAW_HOST, targetPath, "://");

    int size = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    std::wstring text(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &text[0], size);

    std::wstring currentCity = L"Unknown";
    std::wregex headingRe(L"^\\s*#{1,6}\\s+(.+?)\\s*$");
    std::wregex uriLineRe(L"`((?:tcp|tls|quic|ws|wss)://[^`]+)`");

    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        std::wsmatch headingMatch;
        if (std::regex_match(line, headingMatch, headingRe)) {
            currentCity = headingMatch[1].str();
            continue;
        }
        auto begin = std::wsregex_iterator(line.begin(), line.end(), uriLineRe);
        auto end = std::wsregex_iterator();
        for (auto it = begin; it != end; ++it) {
            PeerCandidate pc;
            pc.uri = (*it)[1].str();
            pc.city = currentCity;
            result.push_back(pc);
        }
    }

    return result;
}

// ------------------------------------------------------------------
// GeoIP: определение egress-страны (страны, из которой реально видна
// исходящая связность - через VPN, если он держит маршрут по
// умолчанию, или напрямую, если нет). Несколько провайдеров по очереди -
// если один недоступен, молча пробуем следующего, без отдельного
// офлайн-фолбэка (сознательно не тащим базу MaxMind ради этого).
// ------------------------------------------------------------------
std::optional<std::wstring> ExtractJsonStringField(const std::string& json, const std::string& field) {
    std::regex re("\"" + field + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (std::regex_search(json, m, re)) {
        std::string v = m[1].str();
        return std::wstring(v.begin(), v.end());
    }
    return std::nullopt;
}

std::optional<std::wstring> DetectEgressCountryIso() {
    struct Provider { std::wstring host; std::wstring path; std::string field; };
    std::vector<Provider> providers = {
        { L"ip-api.com", L"/json/?fields=countryCode", "countryCode" },
        { L"ipinfo.io",  L"/json",  "country" },
        { L"ipapi.co",   L"/json/", "country" },
    };
    for (const auto& p : providers) {
        std::string body = HttpGetUtf8(p.host, p.path, false);   // только напрямую: страна выхода самого Yggdrasil
        if (body.empty()) continue;
        auto iso = ExtractJsonStringField(body, p.field);
        if (iso.has_value() && iso->size() == 2) {
            std::wstring upper = *iso;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);
            LogDebug(L"GeoIP (" + p.host + L") определил страну: " + upper);
            return upper;
        }
    }
    LogDebug(L"GeoIP: ни один провайдер не ответил");
    return std::nullopt;
}

std::wstring DeriveBackupRegion(const std::vector<std::wstring>& mainPeers) {
    auto files = ListCountryFiles();

    for (const auto& mainPeerUri : mainPeers) {
        std::wstring targetHost = HostFromUri(mainPeerUri);
        if (targetHost.empty()) continue;

        for (const auto& f : files) {
            auto peers = FetchCountryPeers(f.slug);
            for (const auto& p : peers) {
                if (HostFromUri(p.uri) == targetHost) {
                    LogDebug(L"Регион для " + mainPeerUri + L" определён: " + f.slug);
                    return f.slug;
                }
            }
        }
    }

    return L"";
}

// ------------------------------------------------------------------
// Состояние - свой минимальный текстовый формат (ключ=значение), не JSON.
// ------------------------------------------------------------------
// Устройство для мониторинга доступности (Level 2) - только ноутбуки,
// добавляются/именуются руками через меню (не телефон, не автоматически).
struct Device {
    std::wstring name;
    std::wstring host;
};

// Как вёл себя пир на прошлых проверках - чтобы заметить пир, который
// формально "Up", но постоянно переподключается (сессия рвётся каждые пару
// минут, трафик не успевает пойти).
struct PeerTrack {
    bool up = false;
    double uptime = -1;              // uptime на прошлой проверке, с
    long long at = 0;                // когда была прошлая проверка
    std::vector<long long> flaps;    // моменты замеченных переподключений
};

struct WatchdogState {
    std::vector<std::wstring> backupPeers;
    std::wstring backupRegion;        // где нашёлся последний резервный (для показа)
    // Регион поездки - теперь НАСТРОЙКА поиска, а не отдельный режим: основные пиры
    // работают как обычно, а замену сторож ищет в том числе здесь.
    std::wstring regionalCountry;     // slug, например "russia"
    std::wstring regionalCity;        // ascii-имя города из cities.dat (пусто = любой)
    long long internetDownSince = 0;  // unix-время, 0 = интернет не пропадал

    // Домашний регион (ручной, постоянный) и регион выхода трафика (GeoIP, кэш на час)
    std::wstring homeRegion;
    std::wstring homeCity;     // ascii-имя города из cities.dat (пусто = любой город страны)
    std::wstring egressRegion;
    long long egressRegionCheckedAt = 0;

    // Крайний перебор - возобновляемый курсор (индекс страны + индекс пира),
    // снимок списка стран на момент начала прохода.
    std::vector<std::wstring> crisisCountrySlugs;
    size_t crisisCountryIndex = 0;
    size_t crisisPeerIndex = 0;
    long long crisisLastFullScanCompletedAt = 0;

    // Устройства для мониторинга доступности и их текущее состояние
    std::vector<Device> devices;
    std::map<std::wstring, long long> deviceDownSince;
    std::map<std::wstring, long long> deviceEscalatedUntil;
    std::map<std::wstring, std::vector<std::wstring>> devicePeers;      // общие пиры устройства (задаёт пользователь)
    std::map<std::wstring, std::vector<std::wstring>> deviceTempPeers;  // какие из них сторож подключил временно
    std::map<std::wstring, std::wstring> deviceWhy;                     // последний разбор: почему недоступно

    // Аварийный режим по критическому заряду батареи
    bool batteryCriticalMode = false;
    int batteryCriticalEnteredPercent = -1;

    // Проверка обновлений с GitHub (только уведомление)
    long long updateLastCheckedAt = 0;
    std::wstring updateNotifiedTag;
    std::wstring yggUpdateNotifiedTag;   // про новую версию самого Yggdrasil

    // Итог последней проверки - для окна сторожа (status-json)
    long long lastTickAt = 0;
    bool outageNotified = false; // уже сообщили "все пиры легли, замены нет" в этом простое
    bool connLost = false;       // на прошлой проверке живых пиров не было вовсе
    std::wstring lastPhase; // ok / backup / down / crisis / no_internet / battery / service_down / error

    // Добор пиров до минимума: если в регионах больше нечего проверять - пауза
    long long topupRetryAt = 0;
    // Кандидаты, которые недавно не поднялись: не мучаем их каждую проверку
    std::map<std::wstring, long long> failedCandidates;
    // История переподключений пиров (см. PeerTrack)
    std::map<std::wstring, PeerTrack> peerTracks;

    // Чтобы не слать одно и то же каждые 3 минуты
    bool serviceNotified = false;     // служба остановлена / не запускается
    bool configNotified = false;      // yggdrasil.conf не читается
    std::wstring configError;         // что сказал yggdrasil про битый конфиг
    std::wstring configRestoreFrom;   // проверенная рабочая копия, которой можно заменить битый
    std::wstring tunProblem;          // о какой проблеме с адаптером уже сообщили
    long long firewallCheckedAt = 0;  // правила фаервола читаются не чаще раза в час
    bool firewallNotified = false;    // сообщали о блокирующем правиле в этом простое

    // Разбор "почему не работают основные пиры": считается в конце проверки и
    // кладётся сюда, а status-json только отдаёт готовое.
    std::wstring diagVerdict, diagAdvice;
    bool diagAnyUp = false;
    std::vector<std::wstring> diagPeers;   // "uri<TAB>kind<TAB>text<TAB>error"
};

// ------------------------------------------------------------------
// База городов (GeoNames cities15000, обработанная в компактный формат:
// country_code<TAB>name<TAB>lat<TAB>lon<TAB>population). Файл cities.dat
// должен лежать рядом с exe. Реальные данные, не выдуманные - взяты из
// официального публичного датасета geonames.org.
// ------------------------------------------------------------------

// POST (Telegram). err - почему не вышло, via - каким маршрутом ушло.
bool HttpPostUtf8(const std::wstring& host, const std::wstring& path, const std::string& body,
                  std::wstring* err = nullptr, std::wstring* via = nullptr) {
    // ответ на маленький POST приходит быстро: 15 с хватает, а висящий путь не съедает всю проверку
    HttpResult r = HttpRequest(L"POST", host, path, L"Content-Type: application/x-www-form-urlencoded\r\n", &body, nullptr, 15000);
    if (via) *via = r.via;
    if (!r.transportOk) {
        if (err) *err = L"не достучался до " + host + L": " + RouteTraceText();
        return false;
    }
    if (r.status == 200) return true;
    if (err) {
        json::Value v;
        std::wstring desc = json::Parse(r.body, v) ? v[L"description"].str() : L"";
        *err = L"сервер ответил HTTP " + std::to_wstring(r.status) + (desc.empty() ? L"" : L": " + desc) + L" (через " + r.via + L")";
        if (util::Lower(host) == L"api.telegram.org") {
            std::wstring d = util::Lower(desc);
            if (r.status == 401 || r.status == 404) *err += L" - неверный BOT_TOKEN: скопируй заново у @BotFather";
            else if (d.find(L"chat not found") != std::wstring::npos) *err += L" - неверный CHAT_ID или ты ещё не написал боту";
            else if (r.status == 403) *err += L" - бот заблокирован или с ним не начат диалог: открой бота и нажми «Старт»";
            else if (r.status == 429) *err += L" - слишком много сообщений, Telegram просит подождать";
        }
    }
    return false;
}

// Процентное кодирование UTF-8 байт для тела form-urlencoded запроса
std::string UrlEncodeUtf8(const std::wstring& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &utf8[0], size, nullptr, nullptr);
    if (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();

    const char* hexDigits = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else {
            encoded += '%';
            encoded += hexDigits[(c >> 4) & 0xF];
            encoded += hexDigits[c & 0xF];
        }
    }
    return encoded;
}

// ------------------------------------------------------------------
// Уведомления в Telegram - настраиваются через notify.conf рядом с
// exe (BOT_TOKEN=...\r\nCHAT_ID=...). Если файла нет или он неполный -
// уведомления просто молча выключены, ничего не ломается.
// ------------------------------------------------------------------
std::wstring GetDeviceName() {
    wchar_t buf[256];
    DWORD size = 256;
    if (GetComputerNameW(buf, &size)) {
        return std::wstring(buf);
    }
    return L"unknown";
}

std::wstring g_telegramLastVia, g_telegramLastErr;   // для test-notify
bool g_telegramForceEvent = false;                  // test-notify: в журнал всегда

// Ошибка Telegram - в журнал событий окна (вкладка «Журнал»), чтобы её было
// видно целиком. Та же ошибка повторяется не чаще раза в 6 ч.
void TelegramErrorEvent(const std::wstring& err) {
    long long now = (long long)time(nullptr), at = 0;
    try { at = std::stoll(util::RegReadPath(REG_WATCHDOG_KEY, L"TgEventAt")); } catch (...) {}
    std::wstring prev = util::RegReadPath(REG_WATCHDOG_KEY, L"TgEventErr");
    if (!g_telegramForceEvent && prev == err && now - at < 6 * 3600) return;
    AppendEvent(L"warn", L"Telegram: сообщение не ушло - " + err);
    util::RegWriteString(REG_WATCHDOG_KEY, L"TgEventAt", std::to_wstring(now));
    util::RegWriteString(REG_WATCHDOG_KEY, L"TgEventErr", err);
}

bool SendTelegramNotification(const std::wstring& message) {
    NotifyConfig config = LoadNotifyConfig();
    if (!config.valid) return false;
    if (!config.problem.empty()) {
        g_telegramLastVia.clear();
        g_telegramLastErr = config.problem;
        util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastAt", std::to_wstring((long long)time(nullptr)));
        util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastOk", L"0");
        util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastVia", L"");
        util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastErr", config.problem);
        LogError(L"Telegram: сообщение не отправлено - " + config.problem);
        TelegramErrorEvent(config.problem);
        return false;
    }

    std::wstring fullMessage = L"[" + GetDeviceName() + L"] " + message;
    std::string body = "chat_id=" + UrlEncodeUtf8(config.chatId) +
                        "&text=" + UrlEncodeUtf8(fullMessage);
    std::wstring path = L"/bot" + config.botToken + L"/sendMessage";

    std::wstring err, via;
    bool ok = HttpPostUtf8(L"api.telegram.org", path, body, &err, &via);
    g_telegramLastVia = via;
    g_telegramLastErr = err;
    // итог - в реестр, окно покажет «последняя отправка» (трей без прав - не запишет, это не страшно)
    util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastAt", std::to_wstring((long long)time(nullptr)));
    util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastOk", ok ? L"1" : L"0");
    util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastVia", ok ? via : L"");
    util::RegWriteString(REG_WATCHDOG_KEY, L"TgLastErr", ok ? L"" : err);
    if (!ok) { LogError(L"Telegram: сообщение не отправлено - " + err); TelegramErrorEvent(err); }
    else util::RegWriteString(REG_WATCHDOG_KEY, L"TgEventErr", L"");   // ушло - следующая ошибка снова попадёт в журнал
    return ok;
}

// Значимое событие: в журнал событий (для окна сторожа) и в Telegram.
void Notify(const std::wstring& level, const std::wstring& message) {
    AppendEvent(level, message);
    SendTelegramNotification(message);
}

struct City {
    std::wstring countryCode; // ISO 3166-1 alpha-2, например "RU"
    std::wstring name;        // ascii-имя (латиницей)
    double lat;
    double lon;
    long population;
    std::wstring nameRu;      // русское название, если нашлось (может быть пустым)

    // Имя для показа пользователю - русское, если есть, иначе латиницей
    std::wstring displayName() const {
        return nameRu.empty() ? name : nameRu;
    }

    // Для списков: латиница + русское в скобках, если оно есть
    std::wstring displayLabel() const {
        return nameRu.empty() ? name : (name + L" (" + nameRu + L")");
    }
};

// ------------------------------------------------------------------
// Статистика надёжности пиров (peer_stats.txt рядом с exe). Одна строка на
// пир и сутки: день<TAB>uri<TAB>проверок<TAB>рабочих<TAB>обрывов<TAB>рукопожатий<TAB>удачных.
//   проверок/рабочих - пир был в Yggdrasil во время проверки и работал
//                      ("Up" и не рвётся постоянно);
//   рукопожатий      - кандидат подключался на пробу (поиск замены, «Проверить пир»).
// Хранится 35 суток. Надёжность - доля удач с затуханием: вчерашние сутки
// весят почти как сегодняшние, неделя назад - вдвое меньше.
// ------------------------------------------------------------------
struct DayStat { long long checks = 0, up = 0, flaps = 0, tries = 0, triesOk = 0; };
using PeerStats = std::map<std::pair<long long, std::wstring>, DayStat>;   // (день, uri)

const long long STATS_KEEP_DAYS = 35;
const double STATS_HALF_LIFE_DAYS = 7.0;
const long long STATS_MIN_SAMPLES = 10;   // меньше наблюдений - надёжность "неизвестна"

std::wstring PeerStatsPath() { return GetExeDir() + L"peer_stats.txt"; }

long long DayNumber(long long unixTs) { return unixTs / 86400; }

PeerStats ParsePeerStats(const std::wstring& text) {
    PeerStats ps;
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> f;
        size_t pos = 0;
        for (;;) { size_t t = line.find(L'\t', pos); f.push_back(line.substr(pos, t == std::wstring::npos ? std::wstring::npos : t - pos)); if (t == std::wstring::npos) break; pos = t + 1; }
        if (f.size() < 7 || f[1].empty()) continue;
        try {
            DayStat d;
            d.checks = std::stoll(f[2]); d.up = std::stoll(f[3]); d.flaps = std::stoll(f[4]);
            d.tries = std::stoll(f[5]); d.triesOk = std::stoll(f[6]);
            ps[{ std::stoll(f[0]), f[1] }] = d;
        } catch (...) {}
    }
    return ps;
}

std::wstring FormatPeerStats(const PeerStats& ps) {
    std::wstring t;
    for (const auto& [k, d] : ps)
        t += std::to_wstring(k.first) + L"\t" + k.second + L"\t" + std::to_wstring(d.checks) + L"\t" + std::to_wstring(d.up) +
             L"\t" + std::to_wstring(d.flaps) + L"\t" + std::to_wstring(d.tries) + L"\t" + std::to_wstring(d.triesOk) + L"\r\n";
    return t;
}

// Файл пишут и проверка (от SYSTEM), и «Проверить пир» из окна - по очереди:
// замок - отдельный файл, открытый без совместного доступа.
struct FileLock {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit FileLock(const std::wstring& path, DWORD waitMs = 5000) {
        ULONGLONG until = GetTickCount64() + waitMs;
        for (;;) {
            h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (h != INVALID_HANDLE_VALUE || GetTickCount64() >= until) break;
            Sleep(100);
        }
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    ~FileLock() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

struct PeerObservation { long long checks = 0, up = 0, flaps = 0, tries = 0, triesOk = 0; };

void RecordPeerStats(const std::map<std::wstring, PeerObservation>& obs) {
    if (obs.empty()) return;
    FileLock lock(PeerStatsPath() + L".lock");
    if (!lock.ok()) { LogDebug(L"peer_stats.txt занят - статистика этой проверки пропущена"); return; }
    PeerStats ps = ParsePeerStats(ReadFileUtf8(PeerStatsPath()));
    long long today = DayNumber((long long)time(nullptr));
    for (auto it = ps.begin(); it != ps.end();) it = (today - it->first.first > STATS_KEEP_DAYS) ? ps.erase(it) : std::next(it);
    for (const auto& [uri, o] : obs) {
        DayStat& d = ps[{ today, uri }];
        d.checks += o.checks; d.up += o.up; d.flaps += o.flaps; d.tries += o.tries; d.triesOk += o.triesOk;
    }
    util::WriteFileAtomic(PeerStatsPath(), util::ToUtf8(FormatPeerStats(ps)));
}

struct Reliability {
    double score = -1;          // 0..1, -1 = данных мало
    long long samples = 0;      // наблюдений всего (проверки + рукопожатия)
    long long flaps = 0;        // обрывов за всё хранимое время
    long long tries = 0, triesOk = 0;   // пробных рукопожатий / удачных
};

std::map<std::wstring, Reliability> ComputeReliability(const PeerStats& ps, long long today) {
    std::map<std::wstring, std::pair<double, double>> acc;   // uri -> (взвешенные удачи, взвешенные попытки)
    std::map<std::wstring, Reliability> out;
    for (const auto& [k, d] : ps) {
        double age = (double)(today - k.first);
        if (age < 0) age = 0;
        double w = std::pow(0.5, age / STATS_HALF_LIFE_DAYS);
        auto& a = acc[k.second];
        a.first += w * (double)(d.up + d.triesOk);
        a.second += w * (double)(d.checks + d.tries);
        Reliability& r = out[k.second];
        r.samples += d.checks + d.tries;
        r.flaps += d.flaps;
        r.tries += d.tries;
        r.triesOk += d.triesOk;
    }
    for (auto& [uri, r] : out) {
        const auto& a = acc[uri];
        if (r.samples >= STATS_MIN_SAMPLES && a.second > 0) r.score = a.first / a.second;
    }
    return out;
}

std::map<std::wstring, Reliability> LoadReliability() {
    return ComputeReliability(ParsePeerStats(ReadFileUtf8(PeerStatsPath())), DayNumber((long long)time(nullptr)));
}

// Порядок кандидатов с учётом прошлого опыта: проверенно хорошие (>= 90%)
// вперёд, проверенно плохие (< 50%) в конец, остальные (и новые) - как были
// (по расстоянию до города). Внутри каждой группы порядок сохраняется.
template <class T>
void OrderByReliability(std::vector<T>& items, const std::map<std::wstring, Reliability>& rel,
                        const std::function<std::wstring(const T&)>& uriOf, int* goodOut = nullptr, int* badOut = nullptr) {
    auto group = [&](const T& x) {
        auto it = rel.find(uriOf(x));
        if (it == rel.end() || it->second.score < 0) return 1;
        return it->second.score >= 0.9 ? 0 : it->second.score < 0.5 ? 2 : 1;
    };
    std::stable_sort(items.begin(), items.end(), [&](const T& a, const T& b) { return group(a) < group(b); });
    if (goodOut) { *goodOut = 0; for (const auto& x : items) if (group(x) == 0) ++*goodOut; }
    if (badOut) { *badOut = 0; for (const auto& x : items) if (group(x) == 2) ++*badOut; }
}

// Загружается один раз на процесс (раньше - на каждый вызов: 23 тысячи строк
// при каждой сортировке кандидатов). UTF-8 разбирается через Windows API -
// std::codecvt_utf8 устарел и в C++20 удалён.
const std::vector<City>& LoadCityDatabase() {
    static std::vector<City> cities;
    static bool loaded = false;
    if (loaded) return cities;
    loaded = true;
    std::string raw = ReadBytes(GetExeDir() + L"cities.dat");
    if (raw.empty()) {
        LogError(L"Не найден cities.dat рядом с exe - поиск по городам (расстояние) работать не будет");
        return cities;
    }
    std::wstring text = util::FromUtf8(raw);
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::vector<std::wstring> parts = SplitBy(line, L'\t');
        if (parts.size() < 5) continue;
        City c;
        c.countryCode = parts[0];
        c.name = parts[1];
        try {
            c.lat = std::stod(parts[2]);
            c.lon = std::stod(parts[3]);
            c.population = std::stol(parts[4]);
        } catch (...) {
            continue;
        }
        // SplitBy пропускает пустые токены: нет русского названия - нет 6-го поля
        if (parts.size() >= 6) c.nameRu = parts[5];
        cities.push_back(c);
    }
    LogDebug(L"Загружено городов из cities.dat: " + std::to_wstring(cities.size()));
    return cities;
}

// Расстояние по формуле гаверсинуса (по поверхности Земли, км)
double HaversineDistanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0;
    const double toRad = 3.14159265358979323846 / 180.0;
    double dLat = (lat2 - lat1) * toRad;
    double dLon = (lon2 - lon1) * toRad;
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1 * toRad) * cos(lat2 * toRad) * sin(dLon / 2) * sin(dLon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

std::optional<City> FindCity(const std::vector<City>& db, const std::wstring& countryCode, const std::wstring& cityName) {
    for (const auto& c : db) {
        if (_wcsicmp(c.countryCode.c_str(), countryCode.c_str()) == 0 &&
            _wcsicmp(c.name.c_str(), cityName.c_str()) == 0) {
            return c;
        }
    }
    return std::nullopt;
}

// Соответствие slug'ов из репозитория GitHub (используются в путях к
// файлам стран) кодам стран ISO 3166-1 alpha-2 (используются в базе
// городов GeoNames). Только те 40 стран, что реально есть в репозитории
// yggdrasil-network/public-peers на момент написания - i2p/lokinet/tor
// намеренно не включены, это не страны, а отдельные оверлей-сети.
const std::vector<std::pair<std::wstring, std::wstring>> COUNTRY_SLUG_TO_ISO = {
    {L"armenia", L"AM"}, {L"australia", L"AU"}, {L"austria", L"AT"},
    {L"brazil", L"BR"}, {L"canada", L"CA"}, {L"chile", L"CL"},
    {L"czechia", L"CZ"}, {L"finland", L"FI"}, {L"france", L"FR"},
    {L"germany", L"DE"}, {L"hong-kong", L"HK"}, {L"hungary", L"HU"},
    {L"india", L"IN"}, {L"indonesia", L"ID"}, {L"japan", L"JP"},
    {L"latvia", L"LV"}, {L"luxembourg", L"LU"}, {L"moldova", L"MD"},
    {L"netherlands", L"NL"}, {L"new-zealand", L"NZ"}, {L"norway", L"NO"},
    {L"philippines", L"PH"}, {L"poland", L"PL"}, {L"romania", L"RO"},
    {L"russia", L"RU"}, {L"saudi-arabia", L"SA"}, {L"singapore", L"SG"},
    {L"slovakia", L"SK"}, {L"south-africa", L"ZA"}, {L"spain", L"ES"},
    {L"sweden", L"SE"}, {L"switzerland", L"CH"}, {L"taiwan", L"TW"},
    {L"turkey", L"TR"}, {L"ukraine", L"UA"}, {L"united-kingdom", L"GB"},
    {L"united-states", L"US"},
};

std::optional<std::wstring> SlugToIso(const std::wstring& slug) {
    for (const auto& [s, iso] : COUNTRY_SLUG_TO_ISO) {
        if (_wcsicmp(s.c_str(), slug.c_str()) == 0) return iso;
    }
    return std::nullopt;
}

// Обратное соответствие - код ISO (от GeoIP) -> slug репозитория
// (нужен для перевода результата GeoIP в регион для поиска пиров).
std::optional<std::wstring> IsoToSlug(const std::wstring& iso) {
    for (const auto& [slug, code] : COUNTRY_SLUG_TO_ISO) {
        if (_wcsicmp(code.c_str(), iso.c_str()) == 0) return slug;
    }
    return std::nullopt;
}

// Русские названия для тех же 40 стран - для отображения в списке
// выбора. Проверено вручную (названия стран однозначны, не как города).
const std::vector<std::pair<std::wstring, std::wstring>> COUNTRY_SLUG_TO_RU = {
    {L"armenia", L"Армения"}, {L"australia", L"Австралия"}, {L"austria", L"Австрия"},
    {L"brazil", L"Бразилия"}, {L"canada", L"Канада"}, {L"chile", L"Чили"},
    {L"czechia", L"Чехия"}, {L"finland", L"Финляндия"}, {L"france", L"Франция"},
    {L"germany", L"Германия"}, {L"hong-kong", L"Гонконг"}, {L"hungary", L"Венгрия"},
    {L"india", L"Индия"}, {L"indonesia", L"Индонезия"}, {L"japan", L"Япония"},
    {L"latvia", L"Латвия"}, {L"luxembourg", L"Люксембург"}, {L"moldova", L"Молдова"},
    {L"netherlands", L"Нидерланды"}, {L"new-zealand", L"Новая Зеландия"}, {L"norway", L"Норвегия"},
    {L"philippines", L"Филиппины"}, {L"poland", L"Польша"}, {L"romania", L"Румыния"},
    {L"russia", L"Россия"}, {L"saudi-arabia", L"Саудовская Аравия"}, {L"singapore", L"Сингапур"},
    {L"slovakia", L"Словакия"}, {L"south-africa", L"ЮАР"}, {L"spain", L"Испания"},
    {L"sweden", L"Швеция"}, {L"switzerland", L"Швейцария"}, {L"taiwan", L"Тайвань"},
    {L"turkey", L"Турция"}, {L"ukraine", L"Украина"}, {L"united-kingdom", L"Великобритания"},
    {L"united-states", L"США"},
};

std::wstring SlugToRussianName(const std::wstring& slug) {
    for (const auto& [s, ru] : COUNTRY_SLUG_TO_RU) {
        if (_wcsicmp(s.c_str(), slug.c_str()) == 0) return ru;
    }
    return slug; // не нашли перевод - показываем как есть (английский slug)
}



const int FLAP_THRESHOLD = 3;                       // переподключений за окно = "нестабилен"
const long long FLAP_WINDOW_SECONDS = 30 * 60;
const long long FAILED_CANDIDATE_TTL_SECONDS = 3600; // не поднявшийся кандидат - пауза на час

WatchdogState LoadState() {
    WatchdogState state;
    std::wstring text = ReadFileUtf8(StatePath());
    std::wstringstream ss(text);
    std::wstring line;
    bool oldRegionalMode = false;
    std::vector<std::wstring> oldRegionalPeers;
    auto num = [](const std::wstring& v, long long def = 0) { try { return std::stoll(v); } catch (...) { return def; } };
    long long now = (long long)time(nullptr);
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring value = line.substr(eq + 1);
        if (key == L"mode") oldRegionalMode = (value == L"regional");          // старый формат
        else if (key == L"regional_peers") oldRegionalPeers = SplitBy(value, L';'); // старый формат
        else if (key == L"backup_peers") state.backupPeers = SplitBy(value, L';');
        else if (key == L"backup_region") state.backupRegion = value;
        else if (key == L"regional_country") state.regionalCountry = value;
        else if (key == L"regional_city") state.regionalCity = value;
        else if (key == L"internet_down_since") state.internetDownSince = num(value);
        else if (key == L"home_region") state.homeRegion = value;
        else if (key == L"home_city") state.homeCity = value;
        else if (key == L"egress_region") state.egressRegion = value;
        else if (key == L"egress_region_checked_at") state.egressRegionCheckedAt = num(value);
        else if (key == L"crisis_country_slugs") state.crisisCountrySlugs = SplitBy(value, L';');
        else if (key == L"crisis_country_index") state.crisisCountryIndex = (size_t)num(value);
        else if (key == L"crisis_peer_index") state.crisisPeerIndex = (size_t)num(value);
        else if (key == L"crisis_last_full_scan_completed_at") state.crisisLastFullScanCompletedAt = num(value);
        else if (key == L"devices") {
            for (const auto& entry : SplitBy(value, L';')) {
                size_t comma = entry.find(L',');
                if (comma == std::wstring::npos) continue;
                Device d;
                d.name = entry.substr(0, comma);
                d.host = entry.substr(comma + 1);
                state.devices.push_back(d);
            }
        }
        else if (key == L"device_down_since" || key == L"device_escalated_until") {
            auto& m = key == L"device_down_since" ? state.deviceDownSince : state.deviceEscalatedUntil;
            for (const auto& entry : SplitBy(value, L';')) {
                size_t colon = entry.find_last_of(L':');
                if (colon == std::wstring::npos) continue;
                m[entry.substr(0, colon)] = num(entry.substr(colon + 1));
            }
        }
        else if (key == L"device_peers" || key == L"device_temp_peers" || key == L"device_why") {   // имя<TAB>значение
            size_t tab = value.find(L'\t');
            if (tab == std::wstring::npos || tab == 0) continue;
            std::wstring name = value.substr(0, tab), rest = value.substr(tab + 1);
            if (key == L"device_why") state.deviceWhy[name] = rest;
            else (key == L"device_peers" ? state.devicePeers : state.deviceTempPeers)[name] = SplitBy(rest, L' ');
        }
        else if (key == L"battery_critical_mode") state.batteryCriticalMode = (value == L"1");
        else if (key == L"battery_critical_entered_percent") state.batteryCriticalEnteredPercent = (int)num(value, -1);
        else if (key == L"update_last_checked_at") state.updateLastCheckedAt = num(value);
        else if (key == L"update_notified_tag") state.updateNotifiedTag = value;
        else if (key == L"ygg_update_notified_tag") state.yggUpdateNotifiedTag = value;
        else if (key == L"last_tick_at") state.lastTickAt = num(value);
        else if (key == L"last_phase") state.lastPhase = value;
        else if (key == L"outage_notified") state.outageNotified = (value == L"1");
        else if (key == L"conn_lost") state.connLost = (value == L"1");
        else if (key == L"topup_retry_at") state.topupRetryAt = num(value);
        else if (key == L"failed_candidate") {            // "unix|uri", одна строка на кандидата
            size_t bar = value.find(L'|');
            if (bar == std::wstring::npos) continue;
            long long t = num(value.substr(0, bar));
            if (now - t < FAILED_CANDIDATE_TTL_SECONDS) state.failedCandidates[value.substr(bar + 1)] = t;
        }
        else if (key == L"peer_track") {                  // uri<TAB>up<TAB>uptime<TAB>at<TAB>flap,flap,...
            std::vector<std::wstring> f;
            size_t pos = 0;
            for (;;) { size_t t = value.find(L'\t', pos); f.push_back(value.substr(pos, t == std::wstring::npos ? std::wstring::npos : t - pos)); if (t == std::wstring::npos) break; pos = t + 1; }
            if (f.size() < 4 || f[0].empty()) continue;
            PeerTrack tr;
            tr.up = f[1] == L"1";
            try { tr.uptime = std::stod(f[2]); } catch (...) {}
            tr.at = num(f[3]);
            if (f.size() >= 5) for (const auto& x : SplitBy(f[4], L',')) { long long t = num(x); if (now - t < FLAP_WINDOW_SECONDS) tr.flaps.push_back(t); }
            state.peerTracks[f[0]] = tr;
        }
        else if (key == L"service_notified") state.serviceNotified = (value == L"1");
        else if (key == L"config_notified") state.configNotified = (value == L"1");
        else if (key == L"config_error") state.configError = value;
        else if (key == L"config_restore_from") state.configRestoreFrom = value;
        else if (key == L"tun_problem") state.tunProblem = value;
        else if (key == L"firewall_checked_at") state.firewallCheckedAt = num(value);
        else if (key == L"firewall_notified") state.firewallNotified = (value == L"1");
        else if (key == L"diag_verdict") state.diagVerdict = value;
        else if (key == L"diag_advice") state.diagAdvice = value;
        else if (key == L"diag_any_up") state.diagAnyUp = (value == L"1");
        else if (key == L"diag_peer") state.diagPeers.push_back(value);
    }
    // Переход со старого формата: региональные пиры - это просто живые пиры,
    // добавленные сторожем; теперь они обычные резервные (логика 3-7 сама решит,
    // нужны ли они). Сам регион остаётся настройкой поиска.
    (void)oldRegionalMode;
    for (const auto& p : oldRegionalPeers)
        if (std::find(state.backupPeers.begin(), state.backupPeers.end(), p) == state.backupPeers.end())
            state.backupPeers.push_back(p);
    return state;
}

static std::wstring JoinBy(const std::vector<std::wstring>& v, const wchar_t* sep) {
    std::wstring r;
    for (size_t i = 0; i < v.size(); ++i) { if (i) r += sep; r += v[i]; }
    return r;
}

void SaveState(const WatchdogState& state) {
    std::wstring text;
    auto put = [&](const wchar_t* k, const std::wstring& v) { text += k; text += L"="; text += v; text += L"\r\n"; };
    auto b = [](bool x) { return std::wstring(x ? L"1" : L"0"); };
    put(L"backup_peers", JoinBy(state.backupPeers, L";"));
    put(L"backup_region", state.backupRegion);
    put(L"regional_country", state.regionalCountry);
    put(L"regional_city", state.regionalCity);
    put(L"internet_down_since", std::to_wstring(state.internetDownSince));
    put(L"home_region", state.homeRegion);
    put(L"home_city", state.homeCity);
    put(L"egress_region", state.egressRegion);
    put(L"egress_region_checked_at", std::to_wstring(state.egressRegionCheckedAt));
    put(L"crisis_country_slugs", JoinBy(state.crisisCountrySlugs, L";"));
    put(L"crisis_country_index", std::to_wstring(state.crisisCountryIndex));
    put(L"crisis_peer_index", std::to_wstring(state.crisisPeerIndex));
    put(L"crisis_last_full_scan_completed_at", std::to_wstring(state.crisisLastFullScanCompletedAt));
    std::vector<std::wstring> devs;
    for (const auto& d : state.devices) devs.push_back(d.name + L"," + d.host);
    put(L"devices", JoinBy(devs, L";"));
    std::vector<std::wstring> down, esc;
    for (const auto& [name, t] : state.deviceDownSince) down.push_back(name + L":" + std::to_wstring(t));
    for (const auto& [name, t] : state.deviceEscalatedUntil) esc.push_back(name + L":" + std::to_wstring(t));
    put(L"device_down_since", JoinBy(down, L";"));
    put(L"device_escalated_until", JoinBy(esc, L";"));
    for (const auto& [n, v] : state.devicePeers) if (!v.empty()) put(L"device_peers", n + L"\t" + JoinBy(v, L" "));
    for (const auto& [n, v] : state.deviceTempPeers) if (!v.empty()) put(L"device_temp_peers", n + L"\t" + JoinBy(v, L" "));
    for (const auto& [n, w] : state.deviceWhy) {
        std::wstring c = w;
        for (auto& ch : c) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (!c.empty()) put(L"device_why", n + L"\t" + c);
    }
    put(L"battery_critical_mode", b(state.batteryCriticalMode));
    put(L"battery_critical_entered_percent", std::to_wstring(state.batteryCriticalEnteredPercent));
    put(L"update_last_checked_at", std::to_wstring(state.updateLastCheckedAt));
    put(L"update_notified_tag", state.updateNotifiedTag);
    put(L"ygg_update_notified_tag", state.yggUpdateNotifiedTag);
    put(L"last_tick_at", std::to_wstring(state.lastTickAt));
    put(L"last_phase", state.lastPhase);
    put(L"outage_notified", b(state.outageNotified));
    put(L"conn_lost", b(state.connLost));
    put(L"topup_retry_at", std::to_wstring(state.topupRetryAt));
    for (const auto& [uri, t] : state.failedCandidates) put(L"failed_candidate", std::to_wstring(t) + L"|" + uri);
    for (const auto& [uri, tr] : state.peerTracks) {
        std::wstringstream up; up << std::fixed << std::setprecision(0) << tr.uptime;
        std::vector<std::wstring> fl;
        for (long long t : tr.flaps) fl.push_back(std::to_wstring(t));
        put(L"peer_track", uri + L"\t" + b(tr.up) + L"\t" + up.str() + L"\t" + std::to_wstring(tr.at) + L"\t" + JoinBy(fl, L","));
    }
    put(L"service_notified", b(state.serviceNotified));
    put(L"config_notified", b(state.configNotified));
    { std::wstring e = state.configError; for (auto& c : e) if (c == L'\r' || c == L'\n') c = L' '; put(L"config_error", e); }
    put(L"config_restore_from", state.configRestoreFrom);
    put(L"tun_problem", state.tunProblem);
    put(L"firewall_checked_at", std::to_wstring(state.firewallCheckedAt));
    put(L"firewall_notified", b(state.firewallNotified));
    put(L"diag_verdict", state.diagVerdict);
    put(L"diag_advice", state.diagAdvice);
    put(L"diag_any_up", b(state.diagAnyUp));
    for (const auto& dp : state.diagPeers) put(L"diag_peer", dp);

    // Атомарно: при обрыве питания посреди записи остаётся старый файл целиком,
    // а не обрубок (раньше это стирало резервные пиры, регионы и устройства).
    if (!util::WriteFileAtomic(StatePath(), util::ToUtf8(text)))
        LogError(L"Не удалось записать state.txt");
}

// Сохранение из проверки. Проверка идёт до пары минут, а за это время в окне
// могли снять регион, сменить домашний регион или устройства - и запись
// проверки затёрла бы эти правки старыми значениями (так "снятый регион"
// возвращался). Поэтому настройки пользователя берутся с диска, а не из памяти.
void SaveTickState(WatchdogState& state) {
    WatchdogState disk = LoadState();
    state.regionalCountry = disk.regionalCountry;
    state.regionalCity = disk.regionalCity;
    state.homeRegion = disk.homeRegion;
    state.homeCity = disk.homeCity;
    state.devices = disk.devices;
    state.devicePeers = disk.devicePeers;
    for (auto it = state.deviceDownSince.begin(); it != state.deviceDownSince.end();) {
        bool still = false;
        for (const auto& d : state.devices) if (d.name == it->first) still = true;
        it = still ? std::next(it) : state.deviceDownSince.erase(it);
    }
    SaveState(state);
}

// ------------------------------------------------------------------
// Регион для резервных/региональных пиров: сначала свежий egress
// (реальная исходящая связность - через VPN, если он держит маршрут
// по умолчанию), иначе домашний регион (ручной fallback), иначе -
// старая эвристика по стране самих основных пиров (крайний случай,
// если вообще ничего не настроено и GeoIP недоступен).
// ------------------------------------------------------------------
const long long EGRESS_REGION_REFRESH_SECONDS = 3600;

void RefreshEgressRegion(WatchdogState& state) {
    long long now = (long long)time(nullptr);
    if (state.egressRegionCheckedAt != 0 && (now - state.egressRegionCheckedAt) < EGRESS_REGION_REFRESH_SECONDS) {
        return; // кэш ещё свежий
    }
    state.egressRegionCheckedAt = now; // отмечаем попытку независимо от результата -
                                        // чтобы не долбить GeoIP-провайдеров каждый тик при недоступности
    auto iso = DetectEgressCountryIso();
    if (!iso.has_value()) {
        LogDebug(L"GeoIP недоступен - egress-регион оставлен как был: " +
                 (state.egressRegion.empty() ? L"(не определён)" : state.egressRegion));
        return;
    }
    auto slug = IsoToSlug(*iso);
    if (!slug.has_value()) {
        LogDebug(L"GeoIP определил страну " + *iso + L", но она не входит в поддерживаемые 40 стран");
        return;
    }
    if (state.egressRegion != *slug) {
        LogInfo(L"Egress-регион обновлён: " + (state.egressRegion.empty() ? L"(не было)" : state.egressRegion) +
                L" -> " + *slug);
    }
    state.egressRegion = *slug;
}


// ------------------------------------------------------------------
// Фаервол - программа правила НЕ меняет, только читает. Раньше читала через
// netsh и искала строки "Rule Name:" / "Action:" - на русской Windows netsh
// пишет "Имя правила:" / "Действие:", и проверка молча ничего не находила
// (а ещё netsh verbose идёт 5-15 с). Теперь - через COM-интерфейс фаервола
// (INetFwPolicy2): не зависит от языка системы и отвечает за доли секунды.
// Вызывается перед крайним перебором, не чаще раза в час.
// ------------------------------------------------------------------
static const GUID kClsidNetFwPolicy2 = { 0xE2B3C97F, 0x6AE1, 0x41AC, { 0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD } };
static const GUID kIidNetFwPolicy2  = { 0x98325047, 0xC671, 0x4174, { 0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86 } };
static const GUID kIidNetFwRule     = { 0xAF230D27, 0xBABA, 0x4E42, { 0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2 } };
static const GUID kIidEnumVariant   = { 0x00020404, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

std::optional<std::wstring> CheckFirewallDiagnostics() {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<std::wstring> blocking;
    bool defaultOutBlock = false, readOk = false;
    auto hasYgg = [](const std::wstring& s) { return util::Lower(s).find(L"ygg") != std::wstring::npos; };

    INetFwPolicy2* pol = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, kIidNetFwPolicy2, (void**)&pol)) && pol) {
        readOk = true;
        long profiles = 0;
        pol->get_CurrentProfileTypes(&profiles);
        for (NET_FW_PROFILE_TYPE2 pt : { NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC }) {
            if (!(profiles & pt)) continue;
            VARIANT_BOOL on = VARIANT_FALSE;
            NET_FW_ACTION act = NET_FW_ACTION_ALLOW;
            pol->get_FirewallEnabled(pt, &on);
            pol->get_DefaultOutboundAction(pt, &act);
            if (on == VARIANT_TRUE && act == NET_FW_ACTION_BLOCK) defaultOutBlock = true;
        }
        INetFwRules* rules = nullptr;
        if (SUCCEEDED(pol->get_Rules(&rules)) && rules) {
            IUnknown* unk = nullptr;
            if (SUCCEEDED(rules->get__NewEnum(&unk)) && unk) {
                IEnumVARIANT* en = nullptr;
                if (SUCCEEDED(unk->QueryInterface(kIidEnumVariant, (void**)&en)) && en) {
                    VARIANT var;
                    VariantInit(&var);
                    ULONG got = 0;
                    while (en->Next(1, &var, &got) == S_OK && got == 1) {
                        if (var.vt == VT_DISPATCH && var.pdispVal) {
                            INetFwRule* rule = nullptr;
                            if (SUCCEEDED(var.pdispVal->QueryInterface(kIidNetFwRule, (void**)&rule)) && rule) {
                                VARIANT_BOOL enabled = VARIANT_FALSE;
                                NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
                                BSTR name = nullptr, app = nullptr;
                                rule->get_Enabled(&enabled);
                                rule->get_Action(&action);
                                rule->get_Name(&name);
                                rule->get_ApplicationName(&app);
                                std::wstring n = name ? name : L"", a = app ? app : L"";
                                if (enabled == VARIANT_TRUE && action == NET_FW_ACTION_BLOCK && (hasYgg(n) || hasYgg(a)))
                                    blocking.push_back(n.empty() ? a : n);
                                if (name) SysFreeString(name);
                                if (app) SysFreeString(app);
                                rule->Release();
                            }
                        }
                        VariantClear(&var);
                    }
                    en->Release();
                }
                unk->Release();
            }
            rules->Release();
        }
        pol->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();

    if (!readOk) {
        LogDebug(L"Фаервол: не удалось прочитать настройки (служба брандмауэра выключена?)");
        return std::nullopt;
    }
    if (!blocking.empty())
        return L"в фаерволе Windows включено блокирующее правило про Yggdrasil/сторожа: «" + JoinBy(blocking, L"», «") +
               L"». Проверь его: wf.msc → правила для исходящих/входящих подключений.";
    if (defaultOutBlock)
        return L"фаервол Windows по умолчанию блокирует ВСЕ исходящие соединения, а разрешающего правила для "
               L"Yggdrasil может не быть. Проверь: wf.msc → свойства брандмауэра → исходящие подключения.";
    LogDebug(L"Фаервол: блокирующих правил про Yggdrasil не нашлось");
    return std::nullopt;
}

// ------------------------------------------------------------------
// Крайний перебор - последняя надежда, когда обычный поиск резервного
// пира в своём регионе не дал результата. Ограничен ПО ВРЕМЕНИ за
// один тик (не по числу пиров) и
// возобновляемый через курсор (индекс страны + индекс пира внутри неё) -
// следующий тик продолжает ровно с того места, где этот остановился.
// После полного прохода без результата - 12-часовой кулдаун. Если
// список стран на GitHub обновился посреди прохода - проход СНАЧАЛА
// доканчивается по старому снимку, новый подхватывается только на
// следующем проходе.
// ------------------------------------------------------------------
const long long CRISIS_SCAN_COOLDOWN_SECONDS = 12 * 3600;
const long long CRISIS_SCAN_TIME_BUDGET_SECONDS = 90;

bool CrisisScanCooldownActive(const WatchdogState& state) {
    if (state.crisisLastFullScanCompletedAt == 0) return false;
    long long elapsed = (long long)time(nullptr) - state.crisisLastFullScanCompletedAt;
    return elapsed < CRISIS_SCAN_COOLDOWN_SECONDS;
}

// true, если за этот шаг нашёлся и добавлен рабочий пир
bool CrisisScanStep(WatchdogState& state, const std::vector<std::wstring>& excludeHosts, long long budgetSec) {
    long long limitSec = (std::min)(budgetSec, CRISIS_SCAN_TIME_BUDGET_SECONDS);
    if (CrisisScanCooldownActive(state)) {
        LogDebug(L"Крайний перебор: кулдаун после последнего полного прохода ещё не закончился");
        return false;
    }

    if (state.crisisCountrySlugs.empty()) {
        auto files = ListCountryFiles();
        if (files.empty()) {
            LogError(L"Крайний перебор: не удалось получить список стран - повторю в следующий тик");
            return false;
        }
        for (const auto& f : files) state.crisisCountrySlugs.push_back(f.slug);
        state.crisisCountryIndex = 0;
        state.crisisPeerIndex = 0;
        LogInfo(L"=== Крайний перебор: начинаю полный перебор пиров по всем странам (" +
                std::to_wstring(state.crisisCountrySlugs.size()) + L") ===");
        Notify(L"warn", L"Крайний перебор: обычный поиск резервного пира не сработал. "
                                  L"Начинаю полный перебор пиров по всем странам в фоне.");
    }

    auto start = std::chrono::steady_clock::now();
    auto isExcluded = [&](const std::wstring& uri) {
        std::wstring host = HostFromUri(uri);
        for (const auto& h : excludeHosts) if (h == host) return true;
        return false;
    };

    while (state.crisisCountryIndex < state.crisisCountrySlugs.size()) {
        std::wstring slug = state.crisisCountrySlugs[state.crisisCountryIndex];
        auto peers = FetchCountryPeers(slug);

        while (state.crisisPeerIndex < peers.size()) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count() >= limitSec) {
                LogInfo(L"Крайний перебор: отведённое на этот тик время закончилось, продолжу со страны '" +
                        slug + L"', пир #" + std::to_wstring(state.crisisPeerIndex + 1));
                return false;
            }

            const auto& candidate = peers[state.crisisPeerIndex];
            if (!isExcluded(candidate.uri)) {
                LogInfo(L"Крайний перебор: проверяю " + candidate.uri + L" (" + slug + L"/" + candidate.city + L")");
                if (VerifyPeerHandshake(candidate.uri)) {
                    LogInfo(L"  Up - добавляю как резервного (крайний перебор)");
                    Notify(L"ok", L"Крайний перебор: найден рабочий пир - " + candidate.uri +
                                              L" (" + slug + L"/" + candidate.city + L")");
                    AddPeer(candidate.uri);
                    state.backupPeers.push_back(candidate.uri);
                    state.backupRegion = slug;
                    state.crisisPeerIndex++;
                    return true;
                }
            }
            state.crisisPeerIndex++;
        }

        state.crisisCountryIndex++;
        state.crisisPeerIndex = 0;
    }

    LogError(L"Крайний перебор: полный перебор всех стран завершён, рабочий пир не найден. "
             L"Программа НЕ трогает фаервол сама - только читает правила и сообщает, "
             L"остальное - руками.");
    Notify(L"alert", L"Крайний перебор: полный перебор всех пиров завершён без результата.");
    state.crisisLastFullScanCompletedAt = (long long)time(nullptr);
    state.crisisCountrySlugs.clear();
    state.crisisCountryIndex = 0;
    state.crisisPeerIndex = 0;
    return false;
}

// ------------------------------------------------------------------
// Мониторинг доступности устройств (Level 2) - только те устройства,
// что добавлены руками через меню (ноутбуки, НЕ телефон). Проверяются
// только после того, как основная связность (Level 1) уже подтверждена
// живой. При подтверждённой недоступности - эскалация ОДИН раз (с
// кулдауном на повторы): проверяем не загруженность машины вообще, а
// именно то, мешает ли нагрузка самому процессу/сокету Yggdrasil -
// через время отклика getSelf - и если да, перезапускаем службу.
// ------------------------------------------------------------------
// Адрес в виде для SMB "201-831c-...-b93a.ipv6-literal.net" (через дефисы) ->
// обычный IPv6 "201:831c:...:b93a". Иначе ping.exe его не поймёт.
std::wstring NormalizeIpv6(std::wstring a) {
    while (!a.empty() && (a.back() == L' ' || a.back() == L'\\')) a.pop_back();
    while (!a.empty() && (a.front() == L' ' || a.front() == L'\\')) a.erase(0, 1);
    static const std::wstring suf = L".ipv6-literal.net";
    if (a.size() > suf.size() && _wcsicmp(a.c_str() + a.size() - suf.size(), suf.c_str()) == 0) {
        a.resize(a.size() - suf.size());
        for (auto& c : a) { if (c == L'-') c = L':'; else if (c == L's') c = L'%'; }
    } else if (a.find(L':') == std::wstring::npos && a.find(L'-') != a.rfind(L'-')) {
        bool hex = true;
        for (wchar_t c : a) if (!(iswxdigit(c) || c == L'-')) { hex = false; break; }
        if (hex) for (auto& c : a) if (c == L'-') c = L':';
    }
    return a;
}

// Открыт ли TCP-порт на устройстве (connect с таймаутом, ничего не отправляем)
bool TcpPortOpen(const std::wstring& host, int port, int timeoutMs) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfoW* res = nullptr;
    if (GetAddrInfoW(host.c_str(), std::to_wstring(port).c_str(), &hints, &res) != 0 || !res) return false;
    bool ok = false;
    for (addrinfoW* ai = res; ai && !ok; ai = ai->ai_next) {
        SOCKET sock = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
        fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        if (select(0, nullptr, &ws, nullptr, &tv) > 0) {
            int e = 0; int len = sizeof(e);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&e, &len);
            ok = (e == 0);
        }
        closesocket(sock);
    }
    FreeAddrInfoW(res);
    return ok;
}

bool PingOnce(const std::wstring& hostIn, int timeoutMs) {
    std::wstring host = NormalizeIpv6(hostIn);
    // Windows по умолчанию не отвечает на пинг (фаервол режет входящий ICMP), хотя
    // SMB и RDP работают. Поэтому сначала пробуем порты SMB/RDP/SSH - любой ответ
    // значит "устройство на связи"; пинг - только если все порты молчат.
    for (int port : { 445, 3389, 22 })
        if (TcpPortOpen(host, port, timeoutMs)) return true;
    // IPv6 (адреса Yggdrasil) - через ICMP API: ping.exe для IPv6 не печатает
    // "TTL=", и разбор его вывода считал ответивший адрес недоступным.
    in6_addr a6;
    if (InetPtonW(AF_INET6, host.c_str(), &a6) == 1) return util::Ping6(host, (DWORD)timeoutMs) >= 0;
    std::wstring output = RunProcessCapture(L"ping.exe",
        L"-n 1 -w " + std::to_wstring(timeoutMs) + L" " + host, 15000);
    // ping.exe на успехе печатает "TTL=" в строке ответа (регистр и язык
    // локали могут отличаться для остального текста, но TTL= стабильно)
    std::wstring lower = output;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return lower.find(L"ttl=") != std::wstring::npos;
}

// Дебаунс - быстрая серия попыток ВНУТРИ одного вызова (не растягивая
// проверку на несколько 3-минутных тиков), чтобы не путать разовую
// заминку с реальной недоступностью.
bool PingDeviceDebounced(const std::wstring& host) {
    const int ATTEMPTS = 3;
    const int PAUSE_MS = 2000;
    for (int i = 0; i < ATTEMPTS; ++i) {
        if (PingOnce(host, 1000)) return true;
        if (i < ATTEMPTS - 1) Sleep(PAUSE_MS);
    }
    return false;
}

bool IsYggdrasilResponsive() {
    auto start = std::chrono::steady_clock::now();
    std::wstring out = RunYggCtl(L"getSelf");
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    const long long RESPONSIVENESS_THRESHOLD_MS = 5000;
    LogDebug(L"getSelf отозвался за " + std::to_wstring(elapsedMs) + L" мс");
    return !out.empty() && elapsedMs < RESPONSIVENESS_THRESHOLD_MS;
}

const long long DEVICE_ESCALATION_COOLDOWN_SECONDS = 3600;

// Определена ниже (после Tick()) - предварительное объявление, чтобы
// CheckDevices мог её вызвать при подтверждённой локальной перегрузке.
void RestartYggdrasilServiceNow();
bool RunApprovedVpnCommand(const std::wstring& args);   // ниже, в разделе про VPN

// ------------------------------------------------------------------
// Почему устройство недоступно. Проверки по порядку, первая найденная
// причина и есть ответ:
//   1. отвечает ли свой Yggdrasil (yggdrasilctl);
//   2. куда Windows отправит пакет: через адаптер Yggdrasil или через
//      чужой (обычно VPN-клиент в TUN-режиме перехватывает маршрут);
//   3. уходит ли пакет вообще: "общий сбой" ICMP = его режут прямо здесь
//      (VPN-клиент, фаервол, антивирус);
//   4. знает ли Yggdrasil путь / сессию до устройства (getPaths / getSessions):
//      нет - сеть его не находит (выключено, нет пиров, Yggdrasil там лежит);
//   5. путь есть, а ответа нет - причина на самом устройстве.
// Подробно - только для адресов Yggdrasil (200::/7).
// ------------------------------------------------------------------
struct DeviceDiag {
    std::wstring kind;   // ok / ygg_down / route / local_block / no_path / no_answer / other
    std::wstring text;   // по-человечески
    std::wstring vpn;    // замеченные адаптеры VPN-клиентов
};

bool IsYggAddress(const in6_addr& a) { return (a.s6_addr[0] & 0xFE) == 0x02; }

struct IcmpResult { bool ok = false; double ms = -1; DWORD err = 0; };

IcmpResult Ping6Detailed(const in6_addr& dst, DWORD timeoutMs) {
    IcmpResult r;
    sockaddr_in6 d = {}, s = {};
    d.sin6_family = AF_INET6; d.sin6_addr = dst;
    s.sin6_family = AF_INET6;
    HANDLE h = Icmp6CreateFile();
    if (h == INVALID_HANDLE_VALUE) { r.err = GetLastError(); return r; }
    char data[32] = "ygg-watchdog";
    std::vector<char> reply(sizeof(ICMPV6_ECHO_REPLY) + sizeof(data) + 64);
    DWORD n = Icmp6SendEcho2(h, nullptr, nullptr, nullptr, &s, &d, data, (WORD)sizeof(data), nullptr,
                             reply.data(), (DWORD)reply.size(), timeoutMs);
    if (n > 0) {
        auto* rp = (ICMPV6_ECHO_REPLY*)reply.data();
        if (rp->Status == IP_SUCCESS) { r.ok = true; r.ms = rp->RoundTripTime; }
        else r.err = rp->Status;
    } else {
        r.err = GetLastError();
    }
    IcmpCloseHandle(h);
    return r;
}

// Сетевые адаптеры: какой держит адрес Yggdrasil, и какие включённые адаптеры похожи на VPN-туннели
struct AdapterScan { ULONG yggIndex = 0; std::wstring yggName; bool yggUp = false; std::vector<std::wstring> vpn; std::map<ULONG, std::wstring> names; };

AdapterScan ScanAdapters(const in6_addr* yggAddr) {
    AdapterScan sc;
    ULONG size = 64 * 1024;
    std::vector<BYTE> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(size); rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size); }
    if (rc != NO_ERROR) return sc;
    static const wchar_t* vpnWords[] = { L"tun", L"wintun", L"wireguard", L"tap-windows", L"v2ray", L"sing", L"xray", L"outline",
                                         L"amnezia", L"hiddify", L"nekoray", L"clash", L"openvpn", L"vpn" };
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        std::wstring fname = a->FriendlyName ? a->FriendlyName : L"", desc = a->Description ? a->Description : L"";
        sc.names[a->Ipv6IfIndex] = fname;
        bool isYgg = false;
        if (yggAddr)
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
                if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET6 &&
                    memcmp(&((sockaddr_in6*)u->Address.lpSockaddr)->sin6_addr, yggAddr, sizeof(in6_addr)) == 0) isYgg = true;
        if (isYgg) { sc.yggIndex = a->Ipv6IfIndex; sc.yggName = fname; sc.yggUp = a->OperStatus == IfOperStatusUp; continue; }
        if (a->OperStatus != IfOperStatusUp) continue;
        std::wstring low = util::Lower(fname + L" " + desc);
        for (const wchar_t* w : vpnWords) if (low.find(w) != std::wstring::npos) { sc.vpn.push_back(fname); break; }
    }
    return sc;
}

// Знает ли Yggdrasil путь или сессию до адреса
bool YggKnowsPath(const in6_addr& target) {
    const std::pair<const wchar_t*, const wchar_t*> cmds[] = { { L"getPaths", L"paths" }, { L"getSessions", L"sessions" } };
    for (const auto& c : cmds) {
        json::Value v;
        std::wstring err;
        if (!ygg::Ctl(ygg::CtlPaths(), c.first, v, err)) continue;
        const auto& arr = v[c.second];
        for (size_t i = 0; i < arr.size(); ++i) {
            in6_addr a;
            if (InetPtonW(AF_INET6, arr[i][L"address"].str().c_str(), &a) == 1 && memcmp(&a, &target, sizeof(a)) == 0) return true;
        }
    }
    return false;
}

bool IsYggdrasilResponsive();   // ниже

DeviceDiag DiagnoseDevice(const std::wstring& hostIn) {
    DeviceDiag dg;
    std::wstring host = NormalizeIpv6(hostIn);
    in6_addr target;
    if (InetPtonW(AF_INET6, host.c_str(), &target) != 1 || !IsYggAddress(target)) {
        dg.kind = L"other";
        dg.text = L"не отвечает (подробный разбор - только для адресов Yggdrasil 200::/7)";
        return dg;
    }
    if (!IsYggdrasilResponsive()) {
        dg.kind = L"ygg_down";
        dg.text = L"Yggdrasil на этом компьютере не отвечает (служба зависла или перегружена)";
        return dg;
    }
    in6_addr self = {};
    bool haveSelf = false;
    {
        json::Value v; std::wstring err;
        if (ygg::Ctl(ygg::CtlPaths(), L"getSelf", v, err)) haveSelf = InetPtonW(AF_INET6, v[L"address"].str().c_str(), &self) == 1;
    }
    AdapterScan sc = ScanAdapters(haveSelf ? &self : nullptr);
    dg.vpn = JoinBy(sc.vpn, L", ");
    std::wstring vpnNote = sc.vpn.empty() ? L"" : L" Включены адаптеры VPN-клиентов: " + dg.vpn + L".";

    if (haveSelf && (sc.yggIndex == 0 || !sc.yggUp)) {
        dg.kind = L"route";
        dg.text = sc.yggIndex == 0 ? L"у адаптера Yggdrasil на этом компьютере нет его адреса - трафик Yggdrasil не идёт"
                                   : L"адаптер Yggdrasil «" + sc.yggName + L"» на этом компьютере выключен";
        return dg;
    }
    // куда Windows отправит пакет
    SOCKADDR_INET dst = {};
    dst.Ipv6.sin6_family = AF_INET6;
    dst.Ipv6.sin6_addr = target;
    MIB_IPFORWARD_ROW2 row = {};
    SOCKADDR_INET src = {};
    DWORD rr = GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &row, &src);
    if (rr != NO_ERROR) {
        dg.kind = L"route";
        dg.text = L"у Windows нет маршрута к адресу устройства (нет маршрута 200::/7 через адаптер Yggdrasil)." + vpnNote;
        return dg;
    }
    if (sc.yggIndex != 0 && row.InterfaceIndex != sc.yggIndex) {
        auto it = sc.names.find(row.InterfaceIndex);
        std::wstring via = it != sc.names.end() && !it->second.empty() ? it->second : L"адаптер №" + std::to_wstring(row.InterfaceIndex);
        dg.kind = L"route";
        dg.text = L"пакеты к устройству Windows отправляет через «" + via + L"», а не через адаптер Yggdrasil - "
                  L"маршрут перехвачен (обычно VPN-клиентом в TUN-режиме)." + vpnNote;
        return dg;
    }
    // уходит ли пакет
    IcmpResult ir = Ping6Detailed(target, 2000);
    if (!ir.ok) ir = Ping6Detailed(target, 2000);
    if (ir.ok) { dg.kind = L"ok"; dg.text = L"отвечает на ping"; return dg; }
    if (ir.err == IP_GENERAL_FAILURE || ir.err == ERROR_ACCESS_DENIED || ir.err == ERROR_NETWORK_ACCESS_DENIED) {
        dg.kind = L"local_block";
        dg.text = sc.vpn.empty()
            ? L"пакет к устройству блокируется прямо на этом компьютере («общий сбой» ICMP) - исходящий IPv6 режет фаервол или антивирус"
            : L"пакет к устройству блокируется прямо на этом компьютере («общий сбой» ICMP). Похоже, VPN-клиент в TUN-режиме ("
              + dg.vpn + L") режет весь исходящий IPv6 - нужен ygg-tun-fix или выключить TUN-режим";
        return dg;
    }
    // путь в сети Yggdrasil
    if (!YggKnowsPath(target)) {
        dg.kind = L"no_path";
        dg.text = L"сеть Yggdrasil не находит путь до устройства: оно выключено, Yggdrasil на нём не работает, "
                  L"или ваши пиры не связаны между собой";
        return dg;
    }
    dg.kind = L"no_answer";
    dg.text = L"путь до устройства в сети есть, но оно не отвечает - причина на самом устройстве "
              L"(завис Yggdrasil, система или фаервол там)";
    return dg;
}

struct DevPingJob { std::wstring host; bool ok = false; };
DWORD WINAPI DevPingThread(LPVOID p) {
    DevPingJob* j = (DevPingJob*)p;
    j->ok = PingDeviceDebounced(j->host);
    return 0;
}

std::vector<std::pair<std::wstring, std::wstring>> CheckDevices(WatchdogState& state) {
    std::vector<std::pair<std::wstring, std::wstring>> events; // {kind, message}
    if (state.devices.empty()) return events;

    long long now = (long long)time(nullptr);

    // Все устройства пингуются одновременно: раньше по очереди, до ~15 с на
    // каждое молчащее устройство. Память под задания не освобождается
    // намеренно - зависший поток не должен писать в освобождённое.
    auto* jobs = new std::vector<DevPingJob>(state.devices.size());
    std::vector<HANDLE> threads;
    for (size_t i = 0; i < state.devices.size(); ++i) {
        (*jobs)[i].host = state.devices[i].host;
        HANDLE h = CreateThread(nullptr, 256 * 1024, DevPingThread, &(*jobs)[i], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (h) threads.push_back(h); else DevPingThread(&(*jobs)[i]);
    }
    for (size_t i = 0; i < threads.size(); i += MAXIMUM_WAIT_OBJECTS) {
        DWORD n = (DWORD)(std::min)((size_t)MAXIMUM_WAIT_OBJECTS, threads.size() - i);
        WaitForMultipleObjects(n, &threads[i], TRUE, 40000);
    }
    for (HANDLE h : threads) CloseHandle(h);

    // какие пиры сейчас в Yggdrasil (для временных общих пиров)
    auto livePeers = GetPeerDetails();
    auto inYgg = [&](const std::wstring& uri) { return FindPeer(livePeers, uri) != nullptr; };
    auto removeTemp = [&](const std::wstring& name) {
        auto it = state.deviceTempPeers.find(name);
        if (it == state.deviceTempPeers.end()) return std::wstring();
        std::vector<std::wstring> removed;
        for (const auto& u : it->second)
            if (std::find(state.backupPeers.begin(), state.backupPeers.end(), u) == state.backupPeers.end()) { RemovePeer(u); removed.push_back(u); }
        state.deviceTempPeers.erase(it);
        return JoinBy(removed, L", ");
    };

    for (size_t di = 0; di < state.devices.size(); ++di) {
        const auto& dev = state.devices[di];
        bool reachable = (*jobs)[di].ok;
        auto downIt = state.deviceDownSince.find(dev.name);
        bool wasDown = (downIt != state.deviceDownSince.end());
        std::wstring who = L"Устройство '" + dev.name + L"' (" + dev.host + L")";

        if (reachable) {
            if (wasDown) {
                long long downtimeSec = now - downIt->second;
                state.deviceDownSince.erase(dev.name);
                state.deviceEscalatedUntil.erase(dev.name);
                state.deviceWhy.erase(dev.name);
                std::wstring removed = removeTemp(dev.name);
                events.push_back({ L"device_recovered",
                    who + L" снова на связи. Было недоступно " + std::to_wstring(downtimeSec / 60) + L" мин " +
                    std::to_wstring(downtimeSec % 60) + L" сек." +
                    (removed.empty() ? L"" : L" Временные общие пиры убраны: " + removed + L".") });
            }
            continue;
        }

        DeviceDiag dg = DiagnoseDevice(dev.host);
        if (dg.kind == L"ok") continue;   // пока разбирались - ответило; на следующей проверке будет "на связи"
        std::wstring prevWhy = state.deviceWhy[dev.name];
        state.deviceWhy[dev.name] = dg.text;

        if (!wasDown) {
            state.deviceDownSince[dev.name] = now;
            events.push_back({ L"device_unreachable", who + L" не отвечает: " + dg.text + L"." });
            continue; // меры - только если недоступность подтвердится на следующей проверке
        }
        if (dg.text != prevWhy)
            events.push_back({ L"device_unreachable", who + L" по-прежнему недоступно, причина другая: " + dg.text + L"." });

        auto escIt = state.deviceEscalatedUntil.find(dev.name);
        if (escIt != state.deviceEscalatedUntil.end() && now < escIt->second) continue;   // меры - не чаще раза в час
        state.deviceEscalatedUntil[dev.name] = now + DEVICE_ESCALATION_COOLDOWN_SECONDS;

        // ---- что можно сделать на этом компьютере
        std::wstring action;
        bool tried = false;
        if (dg.kind == L"ygg_down" || dg.kind == L"route") {
            action = dg.kind == L"ygg_down" ? L"перезапускаю службу Yggdrasil"
                                           : L"перезапускаю службу Yggdrasil - она заново создаст адаптер и маршрут 200::/7";
            if (dg.kind == L"route" && !dg.vpn.empty()) action += L" (если маршрут снова перехватит VPN-клиент - не поможет)";
            RestartYggdrasilServiceNow();
            Sleep(15000);
            tried = true;
        } else if (dg.kind == L"local_block") {
            if (RunApprovedVpnCommand(L"restart")) { action = L"запустил одобренный VPN-скрипт"; Sleep(10000); tried = true; }
            else action = L"в VPN-клиент и фаервол сторож не лезет - нужно поправить руками";
        } else if (dg.kind == L"no_path") {
            std::vector<std::wstring> add;
            auto pit = state.devicePeers.find(dev.name);
            if (pit != state.devicePeers.end())
                for (const auto& u : pit->second) if (!u.empty() && !inYgg(u)) add.push_back(u);
            if (!add.empty()) {
                for (const auto& u : add) AddPeer(u);
                auto& tmp = state.deviceTempPeers[dev.name];
                for (const auto& u : add) if (std::find(tmp.begin(), tmp.end(), u) == tmp.end()) tmp.push_back(u);
                action = L"временно подключил общие пиры с устройством: " + JoinBy(add, L", ");
                Sleep(15000);
                tried = true;
            } else if (pit == state.devicePeers.end() || pit->second.empty()) {
                action = L"общий пир для устройства не задан (вкладка «Устройства») - с ним маршрут нашёлся бы надёжнее";
            } else {
                action = L"общие пиры с устройством уже подключены - похоже, оно само выключено или без связи";
            }
        } else {
            action = L"с этого компьютера тут ничего не исправить";
        }
        LogInfo(who + L": " + dg.text + L" - " + action);
        std::wstring result;
        if (tried) result = PingDeviceDebounced(dev.host) ? L" Помогло: устройство отвечает." : L" Пока не помогло.";
        events.push_back({ tried ? L"device_action" : L"device_unreachable",
                           who + L" недоступно: " + dg.text + L". Что делаю: " + action + L"." + result });
    }

    return events;
}

// add-device <имя> <адрес> [общий пир ...]: пиры, к которым подключено и само
// устройство - сторож подключит их на время, если путь до устройства не
// находится. Без пиров у существующего устройства они не меняются;
// "--no-peers" - очистить.
void CmdAddDevice(const std::wstring& name, const std::wstring& host, const std::vector<std::wstring>& peers = {}, bool setPeers = false) {
    if (name.empty() || host.empty()) { PrintError(L"имя и адрес устройства не могут быть пустыми - устройство не добавлено."); return; }
    for (const auto& p : peers)
        if (p.find(L"://") == std::wstring::npos) { PrintError(L"пир должен быть вида tls://host:port: " + p); return; }
    WatchdogState state = LoadState();
    bool found = false;
    std::wstring canon = name;
    for (auto& d : state.devices) {
        if (_wcsicmp(d.name.c_str(), name.c_str()) == 0) { d.host = host; found = true; canon = d.name; break; }
    }
    if (!found) state.devices.push_back({ name, host });
    if (setPeers) {
        if (peers.empty()) state.devicePeers.erase(canon); else state.devicePeers[canon] = peers;
    }
    SaveState(state);
    std::wstring pl = state.devicePeers.count(canon) ? L", общие пиры: " + JoinBy(state.devicePeers[canon], L" ") : L"";
    LogInfo(std::wstring(found ? L"Устройство обновлено: " : L"Устройство добавлено: ") + canon + L" (" + host + L")" + pl);
}

// diagnose-device [имя]: разобрать недоступность прямо сейчас (без мер)
void CmdDiagnoseDevice(const std::wstring& name) {
    WatchdogState state = LoadState();
    bool any = false;
    for (const auto& d : state.devices) {
        if (!name.empty() && _wcsicmp(d.name.c_str(), name.c_str()) != 0) continue;
        any = true;
        if (PingDeviceDebounced(d.host)) { Print(d.name + L": на связи."); continue; }
        DeviceDiag dg = DiagnoseDevice(d.host);
        Print(d.name + L": " + (dg.kind == L"ok" ? L"на связи." : L"недоступно - " + dg.text + L"."));
    }
    if (!any) PrintError(name.empty() ? L"устройств не добавлено." : L"устройство не найдено: " + name);
}

void CmdRemoveDevice(const std::wstring& name) {
    WatchdogState state = LoadState();
    size_t before = state.devices.size();
    state.devices.erase(std::remove_if(state.devices.begin(), state.devices.end(),
        [&](const Device& d) { return _wcsicmp(d.name.c_str(), name.c_str()) == 0; }), state.devices.end());
    if (state.devices.size() == before) {
        PrintError(L"устройство не найдено: " + name);
        return;
    }
    state.deviceDownSince.erase(name);
    state.deviceEscalatedUntil.erase(name);
    state.deviceWhy.erase(name);
    state.devicePeers.erase(name);
    auto tp = state.deviceTempPeers.find(name);
    if (tp != state.deviceTempPeers.end()) {
        for (const auto& u : tp->second)
            if (std::find(state.backupPeers.begin(), state.backupPeers.end(), u) == state.backupPeers.end()) RemovePeer(u);
        state.deviceTempPeers.erase(tp);
    }
    SaveState(state);
    LogInfo(L"Устройство удалено: " + name);
}

void CmdListDevices() {
    WatchdogState state = LoadState();
    if (state.devices.empty()) {
        Print(L"Устройств для мониторинга не добавлено.");
        return;
    }
    Print(L"Устройства для мониторинга (" + std::to_wstring(state.devices.size()) + L"):");
    for (const auto& d : state.devices) {
        bool down = state.deviceDownSince.count(d.name) > 0;
        Print(L"  " + d.name + L" (" + d.host + L")" + (down ? L" - НЕДОСТУПНО: " + state.deviceWhy[d.name] : L" - ок"));
        if (state.devicePeers.count(d.name)) Print(L"      общие пиры: " + JoinBy(state.devicePeers[d.name], L" "));
    }
}

// ------------------------------------------------------------------
// Аварийный режим по критическому заряду батареи - при <=5% и НЕ на
// зарядке отключает ВСЕ остальные проверки до восстановления заряда.
// Логика одинаковая для Windows и Linux, здесь только чтение самого
// показателя - через GetSystemPowerStatus (на ПК без батареи просто
// возвращает "батареи нет", проверка тихо отключается).
// ------------------------------------------------------------------
const int BATTERY_CRITICAL_PERCENT = 5;

struct BatteryStatus {
    int percent;
    bool charging;
};

std::optional<BatteryStatus> ReadBatteryStatus() {
    SYSTEM_POWER_STATUS sps;
    if (!GetSystemPowerStatus(&sps)) return std::nullopt;
    if (sps.BatteryFlag == BATTERY_FLAG_NO_BATTERY || sps.BatteryFlag == BATTERY_FLAG_UNKNOWN) {
        return std::nullopt;
    }
    if (sps.BatteryLifePercent == 255) return std::nullopt; // неизвестно
    BatteryStatus bs;
    bs.percent = (int)sps.BatteryLifePercent;
    bs.charging = (sps.ACLineStatus == 1) || ((sps.BatteryFlag & BATTERY_FLAG_CHARGING) != 0);
    return bs;
}

// Возвращает false, если аварийный режим активен (все остальные
// проверки этого тика нужно пропустить).
bool CheckBattery(WatchdogState& state) {
    auto status = ReadBatteryStatus();
    if (!status.has_value()) return true; // нет батареи (десктоп) - проверка не применима

    if (state.batteryCriticalMode) {
        bool exited = status->charging ||
            (state.batteryCriticalEnteredPercent >= 0 && status->percent > state.batteryCriticalEnteredPercent);
        if (!exited) return false;
        state.batteryCriticalMode = false;
        state.batteryCriticalEnteredPercent = -1;
        std::wstring msg = L"Заряд " + std::to_wstring(status->percent) + L"% (зарядка: " +
            std::wstring(status->charging ? L"да" : L"нет") +
            L") - аварийный режим по батарее выключен, проверки возобновлены.";
        LogInfo(msg);
        Notify(L"ok", msg);
        return true;
    }

    if (status->percent <= BATTERY_CRITICAL_PERCENT && !status->charging) {
        state.batteryCriticalMode = true;
        state.batteryCriticalEnteredPercent = status->percent;
        std::wstring msg = L"Заряд " + std::to_wstring(status->percent) + L"% (не на зарядке) - "
            L"вхожу в аварийный режим по батарее, все проверки выключены до восстановления заряда.";
        LogInfo(msg);
        Notify(L"alert", msg);
        return false;
    }

    return true;
}

// ------------------------------------------------------------------
// VPN: продвинутый вариант - пользовательский скрипт, одобренный
// ОДИН раз руками (с правами администратора), защищённый привязкой к
// SHA-256 содержимого: перед КАЖДЫМ запуском хэш содержимого
// пересчитывается и сверяется - если скрипт изменился с момента
// одобрения хоть на байт, запуск отказывается. Базовый вариант (без
// одобренного скрипта) - в клиент не лезем вообще, только
// предупреждаем в логе/уведомлении.
// ------------------------------------------------------------------
std::wstring VpnApprovalPath() { return GetExeDir() + L"vpn_hook_approval.txt"; }

std::optional<std::wstring> Sha256HexOfFile(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::optional<std::wstring> result;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (!data.empty()) {
                BCryptHashData(hHash, data.data(), (ULONG)data.size(), 0);
            }
            unsigned char hash[32];
            if (BCryptFinishHash(hHash, hash, sizeof(hash), 0) == 0) {
                const wchar_t* hexDigits = L"0123456789abcdef";
                std::wstring hex;
                for (unsigned char b : hash) {
                    hex += hexDigits[(b >> 4) & 0xF];
                    hex += hexDigits[b & 0xF];
                }
                result = hex;
            }
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    return result;
}

bool ApproveVpnCommand(const std::wstring& scriptPath) {
    if (GetFileAttributesW(scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        PrintError(L"файл скрипта не найден: " + scriptPath);
        return false;
    }
    auto hash = Sha256HexOfFile(scriptPath);
    if (!hash.has_value()) {
        LogError(L"Не удалось посчитать SHA-256 скрипта: " + scriptPath);
        return false;
    }
    std::wstring text = L"path=" + scriptPath + L"\r\nsha256=" + *hash + L"\r\n";
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], size, nullptr, nullptr);
    std::ofstream out(std::filesystem::path(VpnApprovalPath()), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LogError(L"Не удалось сохранить одобрение VPN-скрипта");
        return false;
    }
    out.write(utf8.c_str(), utf8.size());
    LogInfo(L"VPN-скрипт одобрен: " + scriptPath + L" (SHA-256: " + *hash + L")");
    return true;
}

void ClearVpnApproval() {
    DeleteFileW(VpnApprovalPath().c_str());
    LogInfo(L"Одобрение VPN-скрипта снято.");
}

// Возвращает путь к одобренному скрипту, ТОЛЬКО если его содержимое
// сейчас совпадает с хэшем на момент одобрения - иначе nullopt.
std::optional<std::wstring> GetApprovedVpnCommand() {
    std::wstring text = ReadFileUtf8(VpnApprovalPath());
    if (text.empty()) return std::nullopt;

    std::wstring path, savedHash;
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == L"path") path = value;
        else if (key == L"sha256") savedHash = value;
    }
    if (path.empty() || savedHash.empty()) return std::nullopt;

    auto currentHash = Sha256HexOfFile(path);
    if (!currentHash.has_value() || *currentHash != savedHash) {
        LogError(L"VPN-скрипт изменился с момента одобрения (или недоступен) - отказываюсь запускать: " + path);
        return std::nullopt;
    }
    return path;
}

bool RunApprovedVpnCommand(const std::wstring& args) {
    auto approved = GetApprovedVpnCommand();
    if (!approved.has_value()) return false;
    LogInfo(L"Запускаю одобренный VPN-скрипт: " + *approved + L" " + args);
    RunProcessCapture(*approved, args);
    return true;
}

// Базовый вариант (без одобренного скрипта) - в сам VPN-клиент не
// лезем. Нет единого стандарта для Windows-VPN-клиентов (в отличие от
// systemd-юнитов на Linux), поэтому автоматического "угадывания" и
// перезапуска здесь нет - только явный лог-намёк.
void WarnVpnMayBeNeeded() {
    LogInfo(L"Если проблема в VPN (например, блокировка по IP/SNI пира) - без одобренного "
            L"VPN-скрипта (approve-vpn-command) программа сама VPN не трогает. Проверь VPN-клиент вручную.");
}

// ------------------------------------------------------------------
// Проверка обновлений на GitHub - ТОЛЬКО уведомление, ничего не
// скачивает сама. Реальная загрузка и применение - ТОЛЬКО по явной
// команде apply-update (её запускает пользователь руками - это и есть
// согласие, отдельный пароль не нужен).
// ------------------------------------------------------------------
const std::wstring CURRENT_VERSION = L"1.2.6"; // поднимать вручную при выпуске новой версии
const long long UPDATE_CHECK_INTERVAL_SECONDS = 24 * 3600;

std::vector<int> VersionTuple(const std::wstring& v) {
    std::wstring s = v;
    while (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s.erase(0, 1);
    std::vector<int> parts;
    for (const auto& part : SplitBy(s, L'.')) {
        std::wstring digits;
        for (wchar_t c : part) if (iswdigit(c)) digits += c;
        parts.push_back(digits.empty() ? 0 : std::stoi(digits));
    }
    return parts;
}

bool VersionLessOrEqual(const std::wstring& a, const std::wstring& b) {
    auto ta = VersionTuple(a), tb = VersionTuple(b);
    size_t n = (std::max)(ta.size(), tb.size());
    ta.resize(n, 0); tb.resize(n, 0);
    return ta <= tb;
}

// ------------------------------------------------------------------
// Обновления берутся прямо из репозитория (релизов нет, всё лежит в
// windows/ на GitHub): версия - из CURRENT_VERSION в windows/ygg_watchdog.cpp,
// программа - windows/ygg_watchdog.exe. Целостность скачанного проверяется
// по отпечатку git (SHA-1 "blob <размер>\0<содержимое>"), который GitHub
// отдаёт вместе с файлом: если файл пришёл обрезанным или подменённым по
// дороге, отпечаток не совпадёт.
// ------------------------------------------------------------------
static const wchar_t* REPO_API_CONTENTS = L"/repos/Yozmor/ygg-watchdog/contents/";
static const wchar_t* REPO_PAGE = L"https://github.com/Yozmor/ygg-watchdog";

struct RepoFile {
    bool ok = false;
    std::wstring downloadUrl, sha;   // sha - git-отпечаток файла (40 hex)
    long long size = -1;
    std::wstring err;
};

RepoFile GetRepoFile(const std::wstring& path) {
    RepoFile f;
    std::string body = HttpGetUtf8(GITHUB_API_HOST, REPO_API_CONTENTS + path);
    json::Value v;
    if (body.empty() || !json::Parse(body, v) || v[L"download_url"].str().empty()) {
        f.err = g_lastHttpStatus == 404 ? L"в репозитории нет " + path
              : g_lastHttpStatus ? L"GitHub ответил кодом " + std::to_wstring(g_lastHttpStatus)
              : L"GitHub недоступен";
        return f;
    }
    f.ok = true;
    f.downloadUrl = v[L"download_url"].str();
    f.sha = util::Lower(v[L"sha"].str());
    f.size = v[L"size"].i64(-1);
    return f;
}

bool SplitHttpsUrl(const std::wstring& url, std::wstring& host, std::wstring& path) {
    const std::wstring pre = L"https://";
    if (url.compare(0, pre.size(), pre) != 0) return false;
    size_t slash = url.find(L'/', pre.size());
    if (slash == std::wstring::npos) return false;
    host = url.substr(pre.size(), slash - pre.size());
    path = url.substr(slash);
    return !host.empty();
}

// Git-отпечаток содержимого (как у GitHub): SHA-1 от "blob <размер>\0" + данные
std::wstring GitBlobSha1Hex(const std::string& data) {
    std::string pre = "blob " + std::to_string(data.size());
    pre.push_back('\0');
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::wstring hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            BCryptHashData(h, (PUCHAR)pre.data(), (ULONG)pre.size(), 0);
            if (!data.empty()) BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0);
            unsigned char out[20];
            if (BCryptFinishHash(h, out, sizeof(out), 0) == 0) {
                const wchar_t* hx = L"0123456789abcdef";
                for (unsigned char b : out) { hex += hx[b >> 4]; hex += hx[b & 15]; }
            }
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

struct UpdateInfo {
    std::wstring tag;      // версия в репозитории
    std::wstring url;      // страница репозитория
};

std::wstring g_updateCheckError;   // почему не удалось узнать версию (для check-update)

std::optional<UpdateInfo> CheckForUpdate() {
    g_updateCheckError.clear();
    RepoFile src = GetRepoFile(L"windows/ygg_watchdog.cpp");
    if (!src.ok) { g_updateCheckError = src.err; return std::nullopt; }
    std::wstring host, path;
    if (!SplitHttpsUrl(src.downloadUrl, host, path)) { g_updateCheckError = L"странная ссылка на файл"; return std::nullopt; }
    std::string text = HttpGetUtf8(host, path);
    std::smatch m;
    std::regex verRe("CURRENT_VERSION\\s*=\\s*L\"([0-9.]+)\"");
    if (text.empty() || !std::regex_search(text, m, verRe)) { g_updateCheckError = L"не удалось прочитать версию в репозитории"; return std::nullopt; }
    std::string ver = m[1].str();
    std::wstring wver(ver.begin(), ver.end());
    if (VersionLessOrEqual(wver, CURRENT_VERSION)) return std::nullopt;
    return UpdateInfo{ wver, REPO_PAGE };
}

// Установленная версия Yggdrasil: "yggdrasil -version" -> "Build version: 0.5.14"
std::wstring InstalledYggVersion() {
    util::ProcResult r = util::Run(ygg::CtlPaths().exe, L"-version", 10000);
    std::wstringstream vs(util::FromUtf8(r.out));
    std::wstring line;
    while (std::getline(vs, line)) {
        size_t c = util::Lower(line).find(L"build version:");
        if (c != std::wstring::npos) return util::Trim(line.substr(c + 14));
    }
    return L"";
}

void CheckForUpdateIfDue(WatchdogState& state) {
    // Прошлое обновление не прошло самопроверку - скрипт вернул старую версию
    std::wstring failPath = GetExeDir() + L"update_failed.txt";
    if (util::FileExists(failPath)) {
        std::wstring tag = util::Trim(util::FromUtf8(ReadBytes(failPath)));
        DeleteFileW(failPath.c_str());
        std::wstring msg = L"Обновление ygg-watchdog" + (tag.empty() ? L"" : L" до " + tag) +
                           L" не прошло самопроверку - возвращена прежняя версия " + CURRENT_VERSION +
                           L". Новая лежит в репозитории битой или не запускается на этом компьютере.";
        LogError(msg);
        Notify(L"warn", msg);
    }
    long long now = (long long)time(nullptr);
    if (state.updateLastCheckedAt != 0 && (now - state.updateLastCheckedAt) < UPDATE_CHECK_INTERVAL_SECONDS) {
        return;
    }
    state.updateLastCheckedAt = now; // отмечаем попытку независимо от результата

    auto info = CheckForUpdate();
    if (!info.has_value() && g_updateCheckError.empty()) state.updateNotifiedTag.clear();   // в репозитории не новее - нечего показывать
    if (info.has_value() && state.updateNotifiedTag != info->tag) {
        state.updateNotifiedTag = info->tag;
        std::wstring msg = L"В репозитории новая версия ygg-watchdog: " + info->tag +
            L" (у тебя " + CURRENT_VERSION + L"). " + info->url +
            L". Обновиться - в окне: «Управление» → «Установить» (или команда apply-update).";
        LogInfo(msg);
        Notify(L"info", msg);
    }

    // Новая версия самого Yggdrasil - тоже только сообщение, ставишь сам
    ygg::UpdateInfo yu = ygg::CheckYggUpdate();
    if (yu.ok) {
        std::wstring cur = InstalledYggVersion();
        if (!cur.empty() && util::VersionNewer(cur, yu.tag) && state.yggUpdateNotifiedTag != yu.tag) {
            state.yggUpdateNotifiedTag = yu.tag;
            std::wstring msg = L"Вышла новая версия Yggdrasil: " + yu.tag + L" (установлена " + cur + L"). " +
                               (yu.page.empty() ? L"" : L"Скачать: " + yu.page);
            LogInfo(msg);
            Notify(L"info", msg);
        }
    }
}

// Скачивание файла: успех - только при HTTP 200 и если пришло ровно столько
// байт, сколько обещал сервер (раньше страница ошибки 404 или оборванная
// загрузка тоже считались "скачано").
bool DownloadFile(const std::wstring& url, const std::wstring& destPath) {
    std::wregex urlRe(L"^https://([^/]+)(/.*)$");
    std::wsmatch m;
    if (!std::regex_match(url, m, urlRe)) return false;
    std::wstring host = m[1].str(), path = m[2].str();

    std::ofstream out;
    auto reopen = [&]() {   // перед каждым маршрутом - файл заново
        if (out.is_open()) out.close();
        out.open(std::filesystem::path(destPath), std::ios::binary | std::ios::trunc);
    };
    BodySink sink = [&](const char* d, DWORD n) { out.write(d, n); return out.good(); };
    HttpResult r = HttpRequest(L"GET", host, path, L"", nullptr, sink, 60000, true, reopen);
    if (out.is_open()) out.close();
    if (!r.transportOk) { LogError(L"Скачивание: не достучался до " + host + L" (WinHTTP " + std::to_wstring(r.winErr) + L")"); return false; }
    if (r.status != 200) { LogError(L"Скачивание: сервер ответил HTTP " + std::to_wstring(r.status)); return false; }
    bool ok = r.readOk && r.gotLen > 0 && (!r.haveLen || r.gotLen == r.expectedLen);
    if (!ok) LogError(L"Скачивание оборвалось: получено " + std::to_wstring(r.gotLen) + L" байт" +
                      (r.haveLen ? L" из " + std::to_wstring(r.expectedLen) : L""));
    return ok;
}

// Похоже ли на целый exe: "MZ" в начале, по смещению из заголовка - "PE\0\0".
// Обрезанный файл тоже начинается с MZ, поэтому одного MZ мало.
bool LooksLikeExe(const std::wstring& path) {
    std::string d = ReadBytes(path);
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
    uint32_t peOff = (uint8_t)d[0x3C] | ((uint8_t)d[0x3D] << 8) | ((uint8_t)d[0x3E] << 16) | ((uint32_t)(uint8_t)d[0x3F] << 24);
    if ((size_t)peOff + 4 > d.size()) return false;
    return d.compare(peOff, 4, std::string("PE\0\0", 4)) == 0;
}

// Скачать файл из репозитория с проверкой: целый exe (если это exe) и git-отпечаток
// совпадает с тем, что сказал GitHub. err - почему не получилось.
bool DownloadRepoFile(const std::wstring& repoPath, const std::wstring& dest, std::wstring& err) {
    RepoFile f = GetRepoFile(repoPath);
    if (!f.ok) { err = f.err; return false; }
    if (!DownloadFile(f.downloadUrl, dest)) { err = L"файл не скачался"; return false; }
    std::string data = ReadBytes(dest);
    std::wstring got = GitBlobSha1Hex(data);
    if (f.sha.size() == 40 && got != f.sha) {
        DeleteFileW(dest.c_str());
        err = L"отпечаток скачанного файла не совпал с репозиторием (файл повреждён по дороге)";
        return false;
    }
    if (f.sha.size() != 40) LogInfo(L"GitHub не дал отпечаток " + repoPath + L" - проверены только заголовки");
    size_t dot = repoPath.rfind(L'.');
    if (dot != std::wstring::npos && util::Lower(repoPath.substr(dot)) == L".exe" && !LooksLikeExe(dest)) {
        DeleteFileW(dest.c_str());
        err = L"скачанный файл не похож на целый exe";
        return false;
    }
    return true;
}

// Применяет обновление: скачивает windows/ygg_watchdog.exe из репозитория,
// сверяет git-отпечаток и запускает его с selftest (должен назвать версию из
// исходника). Потом переименовывает текущий exe в .backup (работающий файл
// переименовать можно) и ставит новый на его место, ещё раз проверяет уже на
// месте - не прошёл, возвращает старый. Значок в трее перезапускается на
// новой версии. Запускать ТОЛЬКО по явной команде.
// Пусто - обновлено; иначе - почему нет (для окна и консоли).
std::wstring ApplyUpdate() {
    std::wstring lastErr;
    auto fail = [&](const std::wstring& m) { LogError(m); lastErr = m; };
    auto info = CheckForUpdate();
    if (!info.has_value()) {
        if (!g_updateCheckError.empty()) fail(L"Не удалось проверить обновление: " + g_updateCheckError);
        else LogInfo(L"Новых версий не найдено - обновляться не с чего.");
        return lastErr.empty() ? std::wstring(L"новой версии в репозитории нет") : lastErr;
    }

    // скрипт замены от прежних версий мог зависнуть в ожидании - без своего файла cmd завершается сам
    DeleteFileW((GetExeDir() + L"apply_update.cmd").c_str());

    wchar_t exePathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    std::wstring exePath = exePathBuf;
    std::wstring newPath = GetExeDir() + L"ygg_watchdog.new.exe";   // на .exe - чтобы запустить самопроверку
    std::wstring backupPath = exePath + L".backup";

    LogInfo(L"Скачиваю новую версию " + info->tag + L" из репозитория...");
    std::wstring dlErr;
    if (!DownloadRepoFile(L"windows/ygg_watchdog.exe", newPath, dlErr)) {
        fail(L"Не удалось скачать новую версию: " + dlErr);
        return lastErr;
    }

    if (!LooksLikeExe(newPath)) {
        fail(L"Скачанный файл не похож на целый .exe (нет заголовков MZ/PE) - отменяю обновление");
        DeleteFileW(newPath.c_str());
        return lastErr;
    }
    // Самопроверка до замены: новый exe запускается, создаёт графику и называет свою версию
    std::wstring gotVer = ygg::ExeSelfTestVersion(newPath, L"selftest");
    if (gotVer != info->tag) {
        fail(gotVer.empty()
            ? L"Скачанная версия " + info->tag + L" не прошла самопроверку (не запускается) - обновление отменено."
            : L"В репозитории номер версии " + info->tag + L", а windows/ygg_watchdog.exe внутри " + gotVer +
              L" - похоже, свежую сборку забыли закоммитить. Обновление отменено.");
        DeleteFileW(newPath.c_str());
        return lastErr;
    }

    // Работающий exe перезаписать нельзя, но переименовать можно: окно, значок
    // в трее и идущая проверка доработают на старом файле (.backup), а всё,
    // что запустится после, возьмёт новый. Ждать закрытия ничего не нужно
    // (раньше скрипт ждал всех копий и вечно висел на значке в трее).
    std::wstring bak = backupPath;
    if (util::FileExists(bak) && !DeleteFileW(bak.c_str()))
        bak = exePath + L".backup-" + std::to_wstring((long long)time(nullptr));   // старая копия ещё запущена - другое имя
    if (!MoveFileExW(exePath.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DWORD e = GetLastError();
        fail(L"Не удалось отодвинуть текущий exe в " + bak + L" (ошибка " + std::to_wstring(e) + L") - обновление отменено.");
        DeleteFileW(newPath.c_str());
        return lastErr;
    }
    if (!MoveFileExW(newPath.c_str(), exePath.c_str(), 0)) {
        DWORD e = GetLastError();
        MoveFileExW(bak.c_str(), exePath.c_str(), 0);   // вернуть как было
        fail(L"Не удалось поставить новый exe на место (ошибка " + std::to_wstring(e) + L") - оставлена прежняя версия.");
        DeleteFileW(newPath.c_str());
        return lastErr;
    }
    // самопроверка уже на месте; не прошла - старая возвращается
    std::wstring placed = ygg::ExeSelfTestVersion(exePath, L"selftest");
    if (placed != info->tag) {
        std::wstring broken = GetExeDir() + L"ygg_watchdog.failed.exe";
        MoveFileExW(exePath.c_str(), broken.c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(bak.c_str(), exePath.c_str(), 0);
        std::wstring msg = L"Обновление ygg-watchdog до " + info->tag + L" не прошло самопроверку на месте - возвращена прежняя версия " +
                           CURRENT_VERSION + L".";
        fail(msg);
        Notify(L"warn", msg);
        return lastErr;
    }
    LogInfo(L"Обновлено до " + info->tag + L" (прежняя версия сохранена как " + std::filesystem::path(bak).filename().wstring() + L").");

    // значок в трее работает на старом файле - перезапускаем на новом
    HWND trayWnd = FindWindowW(L"YggWatchdogTrayMsg", nullptr);
    if (trayWnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(trayWnd, &pid);
        HANDLE hp = pid ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr;
        PostMessageW(trayWnd, WM_CLOSE, 0, 0);   // штатно: значок убирается из трея
        if (hp) {
            if (WaitForSingleObject(hp, 5000) != WAIT_OBJECT_0) TerminateProcess(hp, 0);
            CloseHandle(hp);
        }
        // с обычными правами (не админ), как его запускает автозагрузка
        std::wstring cmd = L"runas.exe /trustlevel:0x20000 \"\\\"" + exePath + L"\\\" tray\"";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else LogError(L"Не удалось перезапустить значок в трее - открой ygg_watchdog.exe, он появится сам.");
    }
    return L"";
}

// ------------------------------------------------------------------
// Основная логика tick - правила 1 и 2 (резервные пиры). Региональный
// режим - следующий этап.
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Региональный режим: поиск ближайшего к целевому городу рабочего пира.
// Настоящий тест рукопожатием выполняется в ОТДЕЛЬНОМ фоновом процессе
// (AddRegionForeground спавнит его и сразу возвращает управление) -
// поэтому интерактивное меню не виснет на минуты, пока идёт проверка.
// ------------------------------------------------------------------
struct ScoredPeer {
    PeerCandidate peer;
    double distanceKm;
};

std::wstring ResolveCountryInput(const std::wstring& countryInput);   // ниже

std::vector<City> GetCitiesForCountry(const std::vector<City>& db, const std::wstring& iso) {
    std::vector<City> filtered;
    for (const auto& c : db) {
        if (_wcsicmp(c.countryCode.c_str(), iso.c_str()) == 0) {
            filtered.push_back(c);
        }
    }
    // Сортируем по английскому/латинскому названию всегда (единый ключ) -
    // русское название для большинства городов не найдено вручную
    // проверенным (только для ~50 крупных), поэтому сортировка по
    // отображаемому имени приводила бы к путанице: часть списка на
    // латинице сверху, часть на кириллице вперемешку. Так стабильнее.
    std::sort(filtered.begin(), filtered.end(), [](const City& a, const City& b) {
        return a.name < b.name;
    });
    return filtered;
}

struct CountryEntry {
    std::wstring slug;
    std::wstring ruName;
    bool supported;
};

// Общая логика: живой список стран с GitHub, отсортированный по русскому
// названию. И показ списка, и разрешение введённого номера используют
// ЭТУ ЖЕ функцию - чтобы порядок гарантированно совпадал.
std::vector<CountryEntry> GetSortedCountryList() {
    auto files = ListCountryFiles();
    std::vector<CountryEntry> entries;
    for (const auto& f : files) {
        // i2p, lokinet и tor - не страны, а пиры внутри анонимных сетей
        // (адреса вида .i2p / .onion): обычным TCP не достать и города у них нет
        if (f.slug == L"i2p" || f.slug == L"lokinet" || f.slug == L"tor") continue;
        CountryEntry e;
        e.slug = f.slug;
        e.ruName = SlugToRussianName(f.slug);
        e.supported = SlugToIso(f.slug).has_value();
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](const CountryEntry& a, const CountryEntry& b) {
        return a.ruName < b.ruName;
    });
    return entries;
}


// ------------------------------------------------------------------
// Показ реальных списков - страны и города конкретной страны, чтобы
// не гадать с транслитерацией/точным написанием при вводе региона.
// ------------------------------------------------------------------
void CmdListCountries() {
    Print(L"Загружаю список стран из репозитория GitHub...");
    auto entries = GetSortedCountryList();
    if (entries.empty()) {
        PrintError(L"Не удалось получить список стран - проверь интернет-соединение");
        return;
    }

    Print(L"Доступные страны (" + std::to_wstring(entries.size()) + L"):");
    for (size_t i = 0; i < entries.size(); ++i) {
        Print(L"  " + std::to_wstring(i + 1) + L") " + entries[i].ruName +
                (entries[i].supported ? L"" : L" (координаты пока не поддерживаются)"));
    }
    Print(L"Вводи НОМЕР страны из списка выше (не название).");
}

// Общая логика: города страны, отсортированные по убыванию населения,
// ограниченные limit штук. Используется и для показа списка, и для
// разрешения номера, который пользователь введёт - чтобы оба места
// гарантированно давали ОДИНАКОВЫЙ порядок (это и есть основа того,
// почему выбор по номеру вообще надёжен).

void CmdListCities(const std::wstring& countryInput) {
    std::wstring countrySlug = countryInput;
    try {
        size_t countryIdx = std::stoul(countryInput);
        auto countryList = GetSortedCountryList();
        if (countryIdx >= 1 && countryIdx <= countryList.size()) {
            countrySlug = countryList[countryIdx - 1].slug;
        }
    } catch (...) {
        // не число - используем countryInput как есть (уже slug)
    }

    auto isoOpt = SlugToIso(countrySlug);
    if (!isoOpt.has_value()) {
        PrintError(L"Неизвестная страна: '" + countrySlug + L"'. Список стран - команда list-countries.");
        return;
    }
    std::wstring iso = *isoOpt;

    const auto& db = LoadCityDatabase();
    auto cities = GetCitiesForCountry(db, iso);

    if (cities.empty()) {
        PrintError(L"Городов для страны '" + countrySlug + L"' (" + iso + L") не найдено в cities.dat");
        return;
    }

    Print(L"Города для '" + countrySlug + L"' (" + iso + L"), всего " +
            std::to_wstring(cities.size()) + L", по алфавиту:");
    for (size_t i = 0; i < cities.size(); ++i) {
        Print(L"  " + std::to_wstring(i + 1) + L") " + cities[i].displayLabel() +
                L" (население: " + std::to_wstring(cities[i].population) + L")");
    }
    Print(L"Вводи НОМЕР города из списка выше (не название) - например: 1");
}


// Номер из списка list-countries или slug -> slug (пусто, если номер вне списка)
std::wstring ResolveCountryInput(const std::wstring& countryInput) {
    try {
        size_t idx = std::stoul(countryInput);
        auto countryList = GetSortedCountryList();
        if (idx >= 1 && idx <= countryList.size()) return countryList[idx - 1].slug;
        return L"";
    } catch (...) {
        return countryInput; // не число - считаем, что это уже slug
    }
}

// countryInput == "" или "clear" - снять домашний регион.
// cityInput - номер из списка list-cities или точное название латиницей;
// пусто - любой город страны (как раньше, без учёта расстояния).
void CmdSetHomeRegion(const std::wstring& countryInput, const std::wstring& cityInput) {
    std::wstring lower = countryInput;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (countryInput.empty() || lower == L"clear") {
        WatchdogState state = LoadState();
        state.homeRegion.clear();
        state.homeCity.clear();
        SaveState(state);
        LogInfo(L"Домашний регион снят.");
        return;
    }

    std::wstring countrySlug = ResolveCountryInput(countryInput);
    auto isoOpt = SlugToIso(countrySlug);
    if (countrySlug.empty() || !isoOpt.has_value()) {
        PrintError(L"Неизвестная страна: '" + countryInput + L"'. Список стран - команда list-countries.");
        return;
    }

    std::wstring cityName;
    std::wstring cityLabel;
    if (!cityInput.empty()) {
        const auto& db = LoadCityDatabase();
        std::optional<City> city;
        try {
            size_t idx = std::stoul(cityInput);
            auto cities = GetCitiesForCountry(db, *isoOpt);
            if (idx >= 1 && idx <= cities.size()) city = cities[idx - 1];
        } catch (...) {
            city = FindCity(db, *isoOpt, cityInput);
        }
        if (!city.has_value()) {
            PrintError(L"Не удалось определить город по вводу '" + cityInput +
                     L"'. Используй номер из списка (команда list-cities " + countrySlug + L").");
            return;
        }
        cityName = city->name;
        cityLabel = city->displayName();
    }

    WatchdogState state = LoadState();
    state.homeRegion = countrySlug;
    state.homeCity = cityName;
    // Сбрасываем запомненный регион резервных - иначе при следующем падении
    // поиск пошёл бы по старому региону, а не по новому домашнему.
    if (state.backupPeers.empty()) state.backupRegion.clear();
    SaveState(state);
    LogInfo(L"Домашний регион установлен: " + SlugToRussianName(countrySlug) +
            (cityLabel.empty() ? L" (любой город)" : L", " + cityLabel));
}

// Кандидатов страны - по расстоянию до заданного города (ближайшие первыми).
// Пиры, чей город не найден в базе, уходят в конец очереди, но не выбрасываются.
void SortCandidatesByCity(std::vector<PeerCandidate>& candidates, const std::wstring& region, const std::wstring& cityName) {
    if (cityName.empty()) return;
    auto isoOpt = SlugToIso(region);
    if (!isoOpt.has_value()) return;
    const auto& db = LoadCityDatabase();
    auto target = FindCity(db, *isoOpt, cityName);
    if (!target.has_value()) {
        LogDebug(L"Город '" + cityName + L"' не найден в cities.dat - без сортировки");
        return;
    }
    std::vector<ScoredPeer> scored;
    for (const auto& p : candidates) {
        auto pc = FindCity(db, *isoOpt, p.city);
        double dist = pc.has_value() ? HaversineDistanceKm(target->lat, target->lon, pc->lat, pc->lon) : 1e9;
        scored.push_back({ p, dist });
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const ScoredPeer& a, const ScoredPeer& b) { return a.distanceKm < b.distanceKm; });
    candidates.clear();
    for (const auto& sp : scored) candidates.push_back(sp.peer);
    LogDebug(L"Кандидаты отсортированы по расстоянию до " + target->displayName());
}

// Регион поездки - настройка поиска, а не режим. Основные пиры работают как
// обычно; когда живых пиров меньше трёх, сторож ищет новые по очереди: выход
// трафика (GeoIP) -> регион поездки -> домашний -> страна основных пиров.
// Раньше add-region запускал фоновый процесс, который минуты спустя сохранял
// state.txt целиком - отсюда "снятый регион возвращается" и пачки региональных
// пиров. Теперь команда только записывает выбор, ничего не ищет сама.
void CmdSetRegion(const std::wstring& countryInput, const std::wstring& cityInput) {
    std::wstring countrySlug = ResolveCountryInput(countryInput);
    auto isoOpt = SlugToIso(countrySlug);
    if (countrySlug.empty() || !isoOpt.has_value()) {
        PrintError(L"Неизвестная страна: '" + countryInput + L"'. Список стран - команда list-countries.");
        return;
    }
    std::wstring cityName, cityLabel;
    if (!cityInput.empty()) {
        const auto& db = LoadCityDatabase();
        std::optional<City> city;
        try {
            size_t idx = std::stoul(cityInput);
            auto cities = GetCitiesForCountry(db, *isoOpt);
            if (idx >= 1 && idx <= cities.size()) city = cities[idx - 1];
        } catch (...) {
            city = FindCity(db, *isoOpt, cityInput);
        }
        if (!city.has_value()) {
            PrintError(L"Не удалось определить город по вводу '" + cityInput +
                       L"'. Используй номер из списка (команда list-cities " + countrySlug + L").");
            return;
        }
        cityName = city->name;
        cityLabel = city->displayName();
    }
    WatchdogState state = LoadState();
    state.regionalCountry = countrySlug;
    state.regionalCity = cityName;
    SaveState(state);
    LogInfo(L"Регион поездки задан: " + SlugToRussianName(countrySlug) +
            (cityLabel.empty() ? L" (любой город)" : L", " + cityLabel));
    AppendEvent(L"info", L"Регион поездки: " + SlugToRussianName(countrySlug) + (cityLabel.empty() ? L"" : L", " + cityLabel));
}

void RemoveRegion() {
    WatchdogState state = LoadState();
    if (state.regionalCountry.empty()) {
        Print(L"Регион поездки и так не задан.");
        return;
    }
    // Найденные раньше пиры не трогаем: это обычные резервные, лишние уберутся сами (больше 7)
    state.regionalCountry.clear();
    state.regionalCity.clear();
    SaveState(state);
    LogInfo(L"Регион поездки снят");
    AppendEvent(L"info", L"Регион поездки снят");
}

// ------------------------------------------------------------------
// Очистка старых записей лога - чтобы файл не рос бесконечно. Вызывается
// раз за тик (дёшево при небольшом файле, а раз DEBUG больше не пишется
// по умолчанию - файл и не должен разрастаться быстро).
// ------------------------------------------------------------------
const int LOG_RETENTION_DAYS = 14;

void CleanOldLogEntries() {
    std::wstring path = LogPath();
    std::wstring content = ReadFileUtf8(path);
    if (content.empty()) return;

    auto cutoff = std::chrono::system_clock::now() - std::chrono::hours(24 * LOG_RETENTION_DAYS);
    std::wregex tsRe(L"^\\[(\\d{4})-(\\d{2})-(\\d{2}) (\\d{2}):(\\d{2}):(\\d{2})\\]");

    auto rawLines = SplitBy(content, L'\n');
    std::vector<std::wstring> kept;
    bool anyDropped = false;

    for (auto line : rawLines) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        std::wsmatch m;
        if (std::regex_search(line, m, tsRe)) {
            std::tm tm = {};
            tm.tm_year = std::stoi(m[1]) - 1900;
            tm.tm_mon  = std::stoi(m[2]) - 1;
            tm.tm_mday = std::stoi(m[3]);
            tm.tm_hour = std::stoi(m[4]);
            tm.tm_min  = std::stoi(m[5]);
            tm.tm_sec  = std::stoi(m[6]);
            std::time_t t = std::mktime(&tm);
            auto lineTime = std::chrono::system_clock::from_time_t(t);
            if (lineTime >= cutoff) {
                kept.push_back(line);
            } else {
                anyDropped = true;
            }
        } else {
            kept.push_back(line); // строка без метки времени - оставляем на всякий случай
        }
    }

    if (!anyDropped) return; // нечего переписывать - не трогаем файл зря

    std::wstring newContent;
    for (const auto& l : kept) newContent += l + L"\r\n";

    int size = WideCharToMultiByte(CP_UTF8, 0, newContent.c_str(), (int)newContent.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, newContent.c_str(), (int)newContent.size(), &utf8[0], size, nullptr, nullptr);
    std::ofstream out(std::filesystem::path(path), std::ios::trunc | std::ios::binary);
    if (out.is_open()) {
        out.write(utf8.c_str(), utf8.size());
    }
}

// ------------------------------------------------------------------
// Возвращает true, если можно продолжать (есть интернет). Мутирует
// state.internetDownSince. При восстановлении сама формирует и
// отправляет ОДНО уведомление с обоими точными временами - тот же
// принцип, что и в Linux-версии.
// ------------------------------------------------------------------
bool CheckInternet(WatchdogState& state) {
    bool ok = HasInternetConnectivity();

    if (!ok) {
        if (state.internetDownSince == 0) {
            state.internetDownSince = (long long)time(nullptr);
            AppendEvent(L"alert", L"Пропал интернет целиком - замену пирам не ищу, жду восстановления.");
            LogInfo(L"Похоже, пропал интернет целиком (не только Yggdrasil-пиры). "
                    L"Поиск замены пирам не имеет смысла, пока нет сети вообще - жду восстановления.");
        }
        return false;
    }

    if (state.internetDownSince != 0) {
        time_t downTime = (time_t)state.internetDownSince;
        time_t upTime = time(nullptr);
        long long downtimeSeconds = (long long)(upTime - downTime);
        state.internetDownSince = 0;

        wchar_t downBuf[16];
        wchar_t upBuf[16];
        tm downTm, upTm;
        localtime_s(&downTm, &downTime);
        localtime_s(&upTm, &upTime);
        wcsftime(downBuf, 16, L"%H:%M:%S", &downTm);
        wcsftime(upBuf, 16, L"%H:%M:%S", &upTm);

        long long minutes = downtimeSeconds / 60;
        long long seconds = downtimeSeconds % 60;

        std::wstring message = L"Интернет пропадал с " + std::wstring(downBuf) +
                                L" до " + std::wstring(upBuf) + L" (простой " +
                                std::to_wstring(minutes) + L" мин " + std::to_wstring(seconds) + L" сек).";
        LogInfo(message);
        Notify(L"ok", message);
    }

    return true;
}

int PortFromUri(const std::wstring& uri) {
    util::PeerUri u = util::ParseUri(uri);
    if (!u.ok) return 0;
    try { return std::stoi(u.port); } catch (...) { return 0; }
}

struct ProbeJob {
    std::wstring host;
    int port = 0;
    double ms = -1;        // -1 = не подключился
    std::wstring err;      // dns / timeout / refused
};

DWORD WINAPI ProbeThread(LPVOID p) {
    ProbeJob* j = (ProbeJob*)p;
    addrinfoW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfoW* res = nullptr;
    if (GetAddrInfoW(j->host.c_str(), std::to_wstring(j->port).c_str(), &hints, &res) != 0 || !res) {
        j->err = L"dns";
        return 0;
    }
    j->err = L"timeout";
    for (addrinfoW* ai = res; ai; ai = ai->ai_next) {
        SOCKET sock = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
        fd_set ws, es;
        FD_ZERO(&ws); FD_SET(sock, &ws);
        FD_ZERO(&es); FD_SET(sock, &es);
        timeval tv{ 3, 0 };
        int rc = select(0, nullptr, &ws, &es, &tv);
        QueryPerformanceCounter(&t1);
        if (rc > 0 && FD_ISSET(sock, &ws)) {
            int e = 0; int len = sizeof(e);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&e, &len);
            if (e == 0) {
                j->ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
                j->err.clear();
                closesocket(sock);
                break;
            }
            j->err = L"refused";
        } else if (rc > 0) {
            j->err = L"refused";
        }
        closesocket(sock);
    }
    FreeAddrInfoW(res);
    return 0;
}

// Параллельно пробует TCP-порты всех кандидатов (до ~4 с на всех). Результат - по одному
// ProbeJob на кандидата в том же порядке. quic:// по TCP не проверить - err = "udp".
std::vector<ProbeJob> ProbePeers(const std::vector<PeerCandidate>& candidates) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    // Память намеренно не освобождается: если какой-то поток завис дольше ожидания,
    // он не должен писать в уже освобождённый вектор.
    std::vector<ProbeJob>* jobs = new std::vector<ProbeJob>(candidates.size());
    std::vector<HANDLE> threads;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::wstring& uri = candidates[i].uri;
        std::wstring proto = uri.substr(0, uri.find(L"://"));
        (*jobs)[i].host = HostFromUri(uri);
        (*jobs)[i].port = PortFromUri(uri);
        if (_wcsicmp(proto.c_str(), L"quic") == 0) { (*jobs)[i].err = L"udp"; continue; }
        if ((*jobs)[i].host.empty() || (*jobs)[i].port == 0) { (*jobs)[i].err = L"bad"; continue; }
        HANDLE h = CreateThread(nullptr, 64 * 1024, ProbeThread, &(*jobs)[i], STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (h) threads.push_back(h); else ProbeThread(&(*jobs)[i]);
    }
    for (size_t i = 0; i < threads.size(); i += MAXIMUM_WAIT_OBJECTS) {
        DWORD n = (DWORD)(std::min)((size_t)MAXIMUM_WAIT_OBJECTS, threads.size() - i);
        WaitForMultipleObjects(n, &threads[i], TRUE, 15000);
    }
    for (HANDLE h : threads) CloseHandle(h);
    return *jobs;
}

std::wstring g_tickPhase;

// Разбор "почему нет связи" (определён ниже, рядом с историей).
std::wstring DiagnoseNowText(const std::vector<std::wstring>& mainPeers);

// ------------------------------------------------------------------
// Общий бюджет одной проверки. Задача Планировщика - раз в 3 минуты; если
// проверка идёт дольше, следующая пропускает ход. Поэтому все долгие шаги
// (рукопожатия по 15 с, крайний перебор) смотрят, сколько времени осталось.
// ------------------------------------------------------------------
struct TickBudget {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    long long limitSec = 150;
    long long Elapsed() const {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    }
    long long Left() const { return limitSec - Elapsed(); }
};

const int MIN_ALIVE_PEERS = 3;     // меньше - сторож добирает резервные
const int MAX_ALIVE_PEERS = 7;     // больше - лишние резервные убираются (основные - никогда)
const long long TOPUP_RETRY_SECONDS = 30 * 60;

// ------------------------------------------------------------------
// Пир "Up", но постоянно переподключается. Признак - uptime из getPeers:
// между двумя проверками он должен вырасти на время между ними; если он
// меньше - соединение рвалось и поднималось заново. 3 таких раза за
// полчаса = "нестабилен": такой пир не считается живым.
// ------------------------------------------------------------------
std::set<std::wstring> UpdatePeerTracks(WatchdogState& st, const std::vector<PeerDetail>& details, long long now) {
    std::set<std::wstring> newFlaps;
    std::map<std::wstring, PeerTrack> next;
    for (const auto& d : details) {
        PeerTrack t;
        auto it = st.peerTracks.find(d.uri);
        if (it != st.peerTracks.end()) {
            const PeerTrack& prev = it->second;
            t.flaps = prev.flaps;
            long long gap = now - prev.at;
            if (prev.at > 0 && gap > 0 && gap < 3600 && prev.up) {
                if (!d.up) { t.flaps.push_back(now); newFlaps.insert(d.uri); }   // был Up - стал Down
                else if (d.uptimeSec >= 0 && prev.uptime >= 0 &&
                         d.uptimeSec + 30 < prev.uptime + (double)gap) { t.flaps.push_back(now); newFlaps.insert(d.uri); } // Up, но заново
            }
        }
        t.flaps.erase(std::remove_if(t.flaps.begin(), t.flaps.end(),
                                     [&](long long x) { return now - x >= FLAP_WINDOW_SECONDS; }), t.flaps.end());
        t.up = d.up;
        t.uptime = d.uptimeSec;
        t.at = now;
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

// ------------------------------------------------------------------
// Конфиг: основные пиры и имя TUN-интерфейса за один запуск yggdrasil
// ------------------------------------------------------------------
struct ConfigInfo {
    bool ok = false;
    bool found = false;     // файл конфига вообще нашёлся
    ygg::Paths paths;
    std::wstring err;
    std::vector<std::wstring> peers;
    std::wstring ifName;   // IfName: auto / none / имя
};

ConfigInfo LoadConfigInfo() {
    ConfigInfo ci;
    auto p = FindConfigPath();
    if (!p.has_value()) { ci.err = L"не найден yggdrasil.conf ни по одному известному пути"; return ci; }
    ci.found = true;
    ci.paths = *p;
    json::Value cfg;
    if (!ygg::LoadConfig(*p, cfg, ci.err)) return ci;
    ci.ok = true;
    const json::Value& arr = cfg[L"Peers"];
    for (size_t i = 0; i < arr.size(); ++i) if (arr[i].isString()) ci.peers.push_back(arr[i].str());
    ci.ifName = cfg[L"IfName"].str(L"auto");
    return ci;
}

// ------------------------------------------------------------------
// Рабочая копия конфига: yggdrasil.conf.good рядом с ним. Обновляется на
// каждой проверке, где конфиг читается нормально и изменился (в нём закрытый
// ключ - поэтому лежит там же, где сам конфиг, с теми же правами).
// Если конфиг сломался, сторож находит проверенную копию (.good, потом самую
// свежую .bak-…) и предлагает её в окне. Заменяет - только после твоего
// подтверждения (команда restore-config), сам не подкладывает.
// ------------------------------------------------------------------
std::wstring GoodConfigPath(const ygg::Paths& p) { return p.conf + L".good"; }

bool ConfigFileValid(const ygg::Paths& p, const std::wstring& file) {
    if (!util::FileExists(file)) return false;
    util::ProcResult r = util::Run(p.exe, L"-useconffile \"" + file + L"\" -normaliseconf", 15000);
    return r.started && !r.timedOut && r.code == 0;
}

// Проверенная копия для восстановления: .good, иначе самая свежая .bak-… (пусто - нет такой)
std::wstring FindConfigRestoreCandidate(const ygg::Paths& p) {
    std::wstring good = GoodConfigPath(p);
    if (ConfigFileValid(p, good)) return good;
    std::vector<std::pair<long long, std::wstring>> baks;
    std::error_code ec;
    std::filesystem::path conf(p.conf);
    std::wstring prefix = conf.filename().wstring() + L".bak-";
    for (const auto& e : std::filesystem::directory_iterator(conf.parent_path(), ec)) {
        std::wstring name = e.path().filename().wstring();
        if (name.rfind(prefix, 0) != 0 || !e.is_regular_file(ec)) continue;
        long long age = FileAgeSeconds(e.path().wstring());
        baks.push_back({ age < 0 ? (1LL << 60) : age, e.path().wstring() });
    }
    std::sort(baks.begin(), baks.end());            // самые свежие (младшие) - первыми
    for (size_t i = 0; i < baks.size() && i < 10; ++i)
        if (ConfigFileValid(p, baks[i].second)) return baks[i].second;
    return L"";
}

void CheckConfigHealth(WatchdogState& st, const ConfigInfo& ci) {
    if (ci.ok) {
        // рабочая копия - если конфиг поменялся с прошлого раза
        std::string cur = ReadBytes(ci.paths.conf);
        std::wstring good = GoodConfigPath(ci.paths);
        if (!cur.empty() && ReadBytes(good) != cur && !util::WriteFileAtomic(good, cur))
            LogError(L"Не удалось обновить рабочую копию конфига " + good);
        st.configError.clear();
        st.configRestoreFrom.clear();
        if (st.configNotified) {
            st.configNotified = false;
            Notify(L"ok", L"yggdrasil.conf снова читается нормально.");
        }
        return;
    }
    LogError(L"Конфиг не читается: " + ci.err);
    st.configError = ci.err;
    st.configRestoreFrom = ci.found ? FindConfigRestoreCandidate(ci.paths) : L"";
    if (!st.configNotified) {
        st.configNotified = true;
        std::wstring how = st.configRestoreFrom.empty()
            ? L"Проверенной рабочей копии рядом нет - исправь файл вручную."
            : L"Есть проверенная рабочая копия (" + std::filesystem::path(st.configRestoreFrom).filename().wstring() +
              L"): открой окно сторожа - на обзоре кнопка «Восстановить», заменит только после подтверждения.";
        Notify(L"alert", L"yggdrasil.conf не читается: " + ci.err + L". Служба работает со старыми настройками, пока её "
               L"не перезапустить; после перезапуска может не подняться. " + how);
    }
}

// restore-config: заменить битый конфиг проверенной копией. Битый файл не
// удаляется - сохраняется рядом как yggdrasil.conf.broken-дата.
void CmdRestoreConfig() {
    auto p = FindConfigPath();
    if (!p.has_value()) { PrintError(L"не найден yggdrasil.conf"); return; }
    json::Value tmp;
    std::wstring err;
    if (ygg::LoadConfig(*p, tmp, err)) { Print(L"Конфиг читается нормально - восстанавливать нечего."); return; }
    std::wstring from = FindConfigRestoreCandidate(*p);   // заново, не по state.txt
    if (from.empty()) { PrintError(L"рабочей копии конфига рядом нет (.good или .bak-…) - исправь файл вручную."); return; }
    std::string data = ReadBytes(from);
    if (data.empty()) { PrintError(L"не удалось прочитать " + from); return; }
    std::wstring broken = p->conf + L".broken-" + util::TimeText(time(nullptr), L"%Y%m%d-%H%M%S");
    if (util::FileExists(p->conf) && !CopyFileW(p->conf.c_str(), broken.c_str(), FALSE)) {
        PrintError(L"не удалось сохранить текущий файл как " + broken + L" - ничего не менял.");
        return;
    }
    if (!util::WriteFileAtomic(p->conf, data)) { PrintError(L"не удалось записать " + p->conf); return; }
    if (!ygg::LoadConfig(*p, tmp, err)) { PrintError(L"после замены конфиг всё равно не читается: " + err); return; }
    LogInfo(L"Конфиг восстановлен из " + from + L" (битый сохранён как " + broken + L")");
    AppendEvent(L"ok", L"yggdrasil.conf восстановлен из рабочей копии " + std::filesystem::path(from).filename().wstring());
    // Служба стоит (не поднялась из-за битого конфига) - запускаем. Работает -
    // она и так живёт со старыми, рабочими настройками: перезапуск не нужен.
    util::ServiceInfo si = util::QueryService(ygg::SERVICE_NAME);
    if (si.exists && si.state != SERVICE_RUNNING) {
        std::wstring e = util::ControlServiceNamed(ygg::SERVICE_NAME, L"start");
        Print(e.empty() ? L"Служба Yggdrasil запущена." : L"Служба Yggdrasil не запустилась: " + e);
    }
    WatchdogState st = LoadState();
    st.configError.clear();
    st.configRestoreFrom.clear();
    SaveState(st);
}

// ------------------------------------------------------------------
// Служба Yggdrasil. Остановлена, а тип запуска "Автоматически" - значит,
// должна работать: запускаем и сообщаем. "Вручную"/"Отключена" - похоже,
// её остановили намеренно: только сообщаем. В обоих случаях искать пиры при
// стоящей службе бессмысленно - проверка заканчивается.
// ------------------------------------------------------------------
bool EnsureYggdrasilService(WatchdogState& st) {
    util::ServiceInfo si = util::QueryService(ygg::SERVICE_NAME);
    if (!si.exists) return true;   // службы с таким именем нет (запущен иначе) - дальше решит yggdrasilctl
    if (si.state == SERVICE_START_PENDING) {
        for (int i = 0; i < 15 && si.state == SERVICE_START_PENDING; ++i) { Sleep(1000); si = util::QueryService(ygg::SERVICE_NAME); }
    }
    if (si.state == SERVICE_RUNNING) {
        if (st.serviceNotified) st.serviceNotified = false;
        return true;
    }
    if (si.startType != SERVICE_AUTO_START) {
        LogInfo(L"Служба Yggdrasil остановлена, тип запуска не «Автоматически» - не трогаю, проверка пиров пропущена.");
        if (!st.serviceNotified) {
            st.serviceNotified = true;
            Notify(L"warn", L"Служба Yggdrasil остановлена, и запускается она вручную - похоже, её остановили намеренно. "
                            L"Сторож её не запускает; пиры не проверяются, пока служба стоит.");
        }
        return false;
    }
    LogInfo(L"Служба Yggdrasil остановлена (тип запуска - автоматически) - запускаю");
    std::wstring err = util::ControlServiceNamed(ygg::SERVICE_NAME, L"start");
    if (!err.empty()) {
        LogError(L"Служба Yggdrasil не запускается: " + err);
        if (!st.serviceNotified) {
            st.serviceNotified = true;
            Notify(L"alert", L"Служба Yggdrasil остановлена и не запускается: " + err +
                             L". Посмотри её в services.msc и лог Yggdrasil.");
        }
        return false;
    }
    st.serviceNotified = false;
    Notify(L"warn", L"Служба Yggdrasil была остановлена - запустил её снова.");
    // Дать пирам подняться, иначе ниже все они выглядели бы мёртвыми
    for (int i = 0; i < 10; ++i) {
        Sleep(2000);
        bool any = false;
        for (const auto& d : GetPeerDetails()) if (d.up) any = true;
        if (any) break;
    }
    return true;
}

// ------------------------------------------------------------------
// Сетевой адаптер Yggdrasil (TUN). Пиры могут быть живы, а трафик на адреса
// 200::/7 всё равно не пойдёт: адаптер не создан, выключен или у него нет
// маршрута. Только проверка и сообщение (одно на каждую новую проблему).
// ------------------------------------------------------------------
void CheckTunHealth(WatchdogState& st, const ConfigInfo& ci) {
    if (!ci.ok || util::Lower(ci.ifName) == L"none") { st.tunProblem.clear(); return; } // TUN выключен в конфиге - норма
    json::Value tun, self;
    std::wstring err;
    if (!ygg::Ctl(ygg::CtlPaths(), L"getTUN", tun, err)) return;   // команда недоступна - не гадаем
    std::wstring problem;
    if (!tun[L"enabled"].boolean()) {
        problem = L"Сетевой адаптер Yggdrasil (TUN) не создан, хотя в конфиге он включён (IfName: " + ci.ifName +
                  L"). Пиры могут работать, но трафик на адреса Yggdrasil не идёт. Обычно помогает перезапуск службы; "
                  L"если нет - переустановка Yggdrasil (драйвер Wintun).";
    } else if (ygg::Ctl(ygg::CtlPaths(), L"getSelf", self, err)) {
        std::wstring addr = self[L"address"].str();
        in6_addr want;
        if (!addr.empty() && InetPtonW(AF_INET6, addr.c_str(), &want) == 1) {
            // ищем адаптер, у которого есть наш адрес Yggdrasil
            ULONG size = 32 * 1024;
            std::vector<BYTE> buf(size);
            ULONG rc = GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                            nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
            if (rc == ERROR_BUFFER_OVERFLOW) {
                buf.resize(size);
                rc = GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
            }
            if (rc == NO_ERROR) {
                IP_ADAPTER_ADDRESSES* found = nullptr;
                for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a && !found; a = a->Next)
                    for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
                        if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET6 &&
                            memcmp(&((sockaddr_in6*)u->Address.lpSockaddr)->sin6_addr, &want, sizeof(want)) == 0) { found = a; break; }
                std::wstring name = tun[L"name"].str();
                if (!found) {
                    problem = L"Ни у одного сетевого адаптера нет адреса Yggdrasil " + addr +
                              L" - трафик на адреса Yggdrasil не пойдёт. Перезапусти службу Yggdrasil.";
                } else if (found->OperStatus != IfOperStatusUp) {
                    problem = L"Сетевой адаптер Yggdrasil «" + std::wstring(found->FriendlyName ? found->FriendlyName : name.c_str()) +
                              L"» выключен - трафик Yggdrasil не идёт. Включи его в «Сетевых подключениях» или перезапусти службу.";
                } else {
                    // маршрут 200::/7 через этот адаптер
                    MIB_IPFORWARD_TABLE2* table = nullptr;
                    bool route = false;
                    if (GetIpForwardTable2(AF_INET6, &table) == NO_ERROR && table) {
                        for (ULONG i = 0; i < table->NumEntries; ++i) {
                            const MIB_IPFORWARD_ROW2& r = table->Table[i];
                            if (r.InterfaceIndex == found->Ipv6IfIndex && r.DestinationPrefix.PrefixLength == 7 &&
                                r.DestinationPrefix.Prefix.Ipv6.sin6_addr.s6_addr[0] == 0x02) { route = true; break; }
                        }
                        FreeMibTable(table);
                        if (!route)
                            problem = L"У адаптера Yggdrasil нет маршрута 200::/7 - трафик на адреса Yggdrasil уйдёт не туда. "
                                      L"Перезапусти службу Yggdrasil; если повторится - проверь, не меняет ли маршруты VPN-клиент.";
                    }
                }
            }
        }
    }
    if (problem.empty()) {
        if (!st.tunProblem.empty()) {
            st.tunProblem.clear();
            Notify(L"ok", L"Сетевой адаптер Yggdrasil снова в порядке.");
        }
        return;
    }
    LogError(problem);
    if (st.tunProblem != problem) {
        st.tunProblem = problem;
        Notify(L"alert", problem);
    }
}

// Устройства (Level 2) - только когда связь через пиры уже есть
void RunDeviceChecks(WatchdogState& state) {
    if (state.devices.empty()) return;
    auto deviceEvents = CheckDevices(state);
    for (const auto& [kind, msg] : deviceEvents) {
        LogInfo(msg);
        Notify(kind == L"device_recovered" ? L"ok" : kind == L"device_action" ? L"warn" : L"alert", msg);
    }
}

// ------------------------------------------------------------------
// Поиск резервных пиров по регионам, по очереди: выход трафика (GeoIP) ->
// регион поездки -> домашний -> страна основных пиров. Одна и та же страна
// дважды не проверяется. В каждой стране - сначала быстрая параллельная
// проба портов, потом настоящее рукопожатие только с отвечающими.
// ------------------------------------------------------------------
struct FoundPeer { std::wstring uri, city, region; };

std::vector<FoundPeer> SearchBackups(WatchdogState& state, const std::vector<std::wstring>& mainPeers,
                                     std::set<std::wstring>& usedHosts, int need, TickBudget& budget, bool& exhausted) {
    std::vector<FoundPeer> found;
    exhausted = true;
    std::vector<std::wstring> tried;
    long long now = (long long)time(nullptr);
    const auto reliability = LoadReliability();

    // На ходу: true - хватит (набрали или кончилось время)
    auto searchRegion = [&](const std::wstring& region, const std::wstring& why) -> bool {
        if (region.empty()) return false;
        for (const auto& t : tried) if (_wcsicmp(t.c_str(), region.c_str()) == 0) return false;
        tried.push_back(region);
        if (budget.Left() < 25) { exhausted = false; return true; }
        LogInfo(L"Ищу пиры: " + SlugToRussianName(region) + L" (" + why + L")");

        auto cands = FetchCountryPeers(region);
        std::wstring city = (_wcsicmp(region.c_str(), state.regionalCountry.c_str()) == 0) ? state.regionalCity
                          : (_wcsicmp(region.c_str(), state.homeRegion.c_str()) == 0) ? state.homeCity : L"";
        SortCandidatesByCity(cands, region, city);
        {
            int good = 0, bad = 0;
            OrderByReliability<PeerCandidate>(cands, reliability, [](const PeerCandidate& c) { return c.uri; }, &good, &bad);
            if (good || bad) LogInfo(L"  по прошлому опыту: " + std::to_wstring(good) + L" надёжных вперёд, " +
                                     std::to_wstring(bad) + L" ненадёжных в конец");
        }
        std::vector<PeerCandidate> fresh;
        for (const auto& c : cands) {
            if (std::find(mainPeers.begin(), mainPeers.end(), c.uri) != mainPeers.end()) continue;
            if (std::find(state.backupPeers.begin(), state.backupPeers.end(), c.uri) != state.backupPeers.end()) continue;
            if (usedHosts.count(HostFromUri(c.uri))) continue;          // тот же сервер другим протоколом - не запас
            auto f = state.failedCandidates.find(c.uri);
            if (f != state.failedCandidates.end() && now - f->second < FAILED_CANDIDATE_TTL_SECONDS) continue;
            fresh.push_back(c);
        }
        if (fresh.empty()) { LogInfo(L"  новых кандидатов нет"); return false; }

        // быстрая проба портов: у закрытых рукопожатие заведомо не пройдёт
        auto jobs = ProbePeers(fresh);
        std::vector<PeerCandidate> order;
        for (size_t i = 0; i < fresh.size(); ++i) if (jobs[i].ms >= 0) order.push_back(fresh[i]);
        size_t open = order.size();
        for (size_t i = 0; i < fresh.size(); ++i) if (jobs[i].err == L"udp") order.push_back(fresh[i]);
        LogInfo(L"  порт отвечает у " + std::to_wstring(open) + L" из " + std::to_wstring(fresh.size()));

        long long regionStart = budget.Elapsed();
        for (const auto& c : order) {
            if ((int)found.size() >= need) return true;
            if (budget.Left() < 20) { exhausted = false; return true; }
            if (budget.Elapsed() - regionStart > 60) { exhausted = false; return false; }  // дать шанс другим странам
            std::wstring host = HostFromUri(c.uri);
            if (usedHosts.count(host)) continue;
            LogInfo(L"  проверяю рукопожатие: " + c.uri + L" (" + c.city + L")");
            if (VerifyPeerHandshake(c.uri)) {
                LogInfo(L"    Up - добавляю как резервный");
                AddPeer(c.uri);
                state.backupPeers.push_back(c.uri);
                state.backupRegion = region;
                usedHosts.insert(host);
                found.push_back({ c.uri, c.city, region });
            } else {
                LogInfo(L"    не поднялся");
                state.failedCandidates[c.uri] = (long long)time(nullptr);
            }
        }
        return (int)found.size() >= need;
    };

    RefreshEgressRegion(state);
    if (searchRegion(state.egressRegion, L"выход трафика")) return found;
    if (searchRegion(state.regionalCountry, L"регион поездки")) return found;
    if (searchRegion(state.homeRegion, L"домашний регион")) return found;
    if ((int)found.size() < need && budget.Left() >= 60) {
        // страна основных пиров - дорого (перебор файлов стран), поэтому последней
        std::wstring derived = DeriveBackupRegion(mainPeers);
        if (searchRegion(derived, L"страна основных пиров")) return found;
    }
    if (tried.empty()) LogError(L"Регион для поиска не определён: GeoIP не ответил, регион поездки и домашний не заданы, "
                                L"страну основных пиров найти не удалось.");
    return found;
}

std::map<std::wstring, PeerObservation> g_observations;   // наблюдения этой проверки -> peer_stats.txt

// Наблюдения + итоги рукопожатий -> peer_stats.txt
void FlushPeerStats() {
    for (const auto& [uri, ok] : g_handshakeResults) {
        PeerObservation& o = g_observations[uri];
        o.tries++;
        if (ok) o.triesOk++;
    }
    g_handshakeResults.clear();
    RecordPeerStats(g_observations);
    g_observations.clear();
}

void TickInner(TickBudget& budget) {
    g_tickPhase = L"error";
    CleanOldLogEntries();
    LogInfo(L"=== Начало проверки ===");

    WatchdogState state = LoadState();

    // Аварийный режим по батарее отключает ВСЕ остальные проверки до
    // восстановления заряда - проверяется первым делом.
    if (!CheckBattery(state)) {
        SaveTickState(state);
        LogInfo(L"Аварийный режим по батарее активен - остальные проверки этого тика пропущены.");
        g_tickPhase = L"battery";
        return;
    }

    // Обновления - не чаще раза в сутки, только уведомление.
    CheckForUpdateIfDue(state);
    SaveTickState(state);

    // Конфиг - до службы: служба могла не подняться как раз из-за битого конфига
    ConfigInfo cfg = LoadConfigInfo();
    CheckConfigHealth(state, cfg);
    SaveTickState(state);

    // Служба стоит - пиры искать бессмысленно
    if (!EnsureYggdrasilService(state)) {
        SaveTickState(state);
        g_tickPhase = L"service_down";
        return;
    }

    // Если интернет уже пропадал - сначала проверяем его: иначе пиры могут сами
    // подняться раньше, чем мы заметим возврат сети, и сообщения о нём не будет.
    if (state.internetDownSince != 0) {
        bool internetOk = CheckInternet(state);
        SaveTickState(state);
        if (!internetOk) {
            g_tickPhase = L"no_internet";
            return;
        }
    }

    long long now = (long long)time(nullptr);
    auto details = GetPeerDetails();
    if (!g_lastPeersOk) {
        LogError(L"yggdrasilctl не отвечает (служба запущена, но управление недоступно) - проверка пиров пропущена.");
        SaveTickState(state);
        return;   // фаза error
    }
    auto newFlaps = UpdatePeerTracks(state, details, now);
    MarkUnstable(details, state);
    for (const auto& d : details) {
        PeerObservation& o = g_observations[d.uri];
        o.checks++;
        if (d.up && !d.unstable) o.up++;
        if (newFlaps.count(d.uri)) o.flaps++;
    }
    for (const auto& d : details)
        LogDebug(L"Пир: " + d.uri + L" -> " + (d.up ? L"Up" : L"Down") + (d.unstable ? L" (нестабилен)" : L""));

    // Основные: из конфига; если конфиг не читается - всё живое, кроме своих резервных
    std::vector<std::wstring> mainPeers;
    if (cfg.ok) {
        for (const auto& p : cfg.peers) if (std::find(mainPeers.begin(), mainPeers.end(), p) == mainPeers.end()) mainPeers.push_back(p);
    } else {
        for (const auto& d : details)
            if (std::find(state.backupPeers.begin(), state.backupPeers.end(), d.uri) == state.backupPeers.end()) mainPeers.push_back(d.uri);
    }

    auto aliveStable = [&](const std::wstring& uri) {
        const PeerDetail* d = FindPeer(details, uri);
        return d && d->up && !d->unstable;
    };

    // Резервные: мёртвые и нестабильные убираем (и не берём их снова час)
    {
        std::vector<std::wstring> keep;
        for (const auto& p : state.backupPeers) {
            if (aliveStable(p)) { keep.push_back(p); continue; }
            const PeerDetail* d = FindPeer(details, p);
            LogInfo(std::wstring(d && d->unstable ? L"Резервный постоянно переподключается - убираю: " : L"Резервный не работает - убираю: ") + p);
            RemovePeer(p);
            state.failedCandidates[p] = now;
        }
        state.backupPeers = keep;
    }

    std::set<std::wstring> usedHosts;
    int mainAlive = 0;
    std::vector<std::wstring> mainUnstable;
    for (const auto& p : mainPeers) {
        usedHosts.insert(HostFromUri(p));
        if (aliveStable(p)) ++mainAlive;
        else if (const PeerDetail* d = FindPeer(details, p)) if (d->unstable) mainUnstable.push_back(p);
    }
    for (const auto& p : state.backupPeers) usedHosts.insert(HostFromUri(p));
    int alive = mainAlive + (int)state.backupPeers.size();
    int aliveAtStart = alive;
    bool wasLost = state.connLost;
    for (const auto& p : mainUnstable)
        LogInfo(L"Основной пир постоянно переподключается - не считаю его живым: " + p);

    // Слишком много - лишние резервные (с самой большой задержкой) убираем
    if (alive > MAX_ALIVE_PEERS && !state.backupPeers.empty()) {
        std::vector<std::pair<double, std::wstring>> byRtt;
        for (const auto& p : state.backupPeers) {
            const PeerDetail* d = FindPeer(details, p);
            byRtt.push_back({ d && d->rttMs >= 0 ? d->rttMs : 1e9, p });
        }
        std::sort(byRtt.begin(), byRtt.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int extra = (std::min)(alive - MAX_ALIVE_PEERS, (int)byRtt.size());
        std::vector<std::wstring> removed;
        for (int i = 0; i < extra; ++i) {
            RemovePeer(byRtt[i].second);
            removed.push_back(byRtt[i].second);
            state.backupPeers.erase(std::remove(state.backupPeers.begin(), state.backupPeers.end(), byRtt[i].second), state.backupPeers.end());
        }
        alive -= extra;
        LogInfo(L"Живых пиров больше " + std::to_wstring(MAX_ALIVE_PEERS) + L" - убраны лишние резервные: " + JoinBy(removed, L", "));
        AppendEvent(L"info", L"Убраны лишние резервные (живых пиров было больше " + std::to_wstring(MAX_ALIVE_PEERS) + L"): " + JoinBy(removed, L", "));
    }

    // Связь есть: закончить проверку (адаптер, устройства) и выйти
    auto finishConnected = [&](const std::wstring& recoveredNote) {
        if (wasLost && !recoveredNote.empty()) { LogInfo(recoveredNote); Notify(L"ok", recoveredNote); }
        state.connLost = false;
        state.outageNotified = false;
        state.firewallNotified = false;
        g_tickPhase = mainAlive > 0 ? L"ok" : L"backup";
        CheckTunHealth(state, cfg);
        SaveTickState(state);
        RunDeviceChecks(state);
        SaveTickState(state);
    };

    if (alive >= MIN_ALIVE_PEERS) {
        state.topupRetryAt = 0;
        LogInfo(L"Проверка выполнена: живых пиров " + std::to_wstring(alive) + L" (основных " + std::to_wstring(mainAlive) + L")");
        finishConnected(L"Связь через Yggdrasil снова есть: живых пиров " + std::to_wstring(alive) + L".");
        return;
    }
    if (alive > 0 && now < state.topupRetryAt) {
        LogInfo(L"Живых пиров " + std::to_wstring(alive) + L" из нужных " + std::to_wstring(MIN_ALIVE_PEERS) +
                L" - добор отложен до " + util::TimeText(state.topupRetryAt, L"%H:%M") + L" (в прошлый раз новых кандидатов не нашлось)");
        finishConnected(L"Связь через Yggdrasil снова есть: живых пиров " + std::to_wstring(alive) + L".");
        return;
    }

    if (alive == 0) LogInfo(L"Живых пиров нет - ищу замену");
    else LogInfo(L"Живых пиров " + std::to_wstring(alive) + L" из нужных " + std::to_wstring(MIN_ALIVE_PEERS) + L" - добираю резервные");

    if (!CheckInternet(state)) {
        SaveTickState(state);
        g_tickPhase = L"no_internet";
        return;
    }
    g_tickPhase = alive > 0 ? (mainAlive > 0 ? L"ok" : L"backup") : L"down";

    bool exhausted = true;
    auto found = SearchBackups(state, mainPeers, usedHosts, MIN_ALIVE_PEERS - alive, budget, exhausted);
    alive += (int)found.size();
    if (!found.empty()) {
        std::wstring list;
        for (const auto& f : found) {
            if (!list.empty()) list += L", ";
            list += f.uri + L" (" + SlugToRussianName(f.region) + (f.city.empty() ? L"" : L"/" + f.city) + L")";
        }
        if (aliveAtStart == 0) {
            std::wstring why = mainPeers.empty() ? L"основных пиров в конфиге нет" : L"основные (" + JoinBy(mainPeers, L", ") + L") не работают";
            Notify(L"warn", L"Связи через Yggdrasil не было: " + why + L". Добавлены резервные: " + list);
        } else {
            AppendEvent(L"info", L"Живых пиров было " + std::to_wstring(aliveAtStart) + L" из " + std::to_wstring(MIN_ALIVE_PEERS) +
                                 L" - добавлены резервные: " + list);
        }
    }
    if (alive >= MIN_ALIVE_PEERS) state.topupRetryAt = 0;
    else if (alive > 0 && exhausted) state.topupRetryAt = now + TOPUP_RETRY_SECONDS;

    if (alive > 0) {
        finishConnected(found.empty() ? L"Связь через Yggdrasil снова есть: живых пиров " + std::to_wstring(alive) + L"." : L"");
        return;
    }

    // ---- живых пиров нет совсем, обычный поиск не помог ----
    state.connLost = true;
    LogError(L"Не удалось найти рабочий резервный пир ни в одном из регионов поиска.");
    if (!state.outageNotified) {
        state.outageNotified = true;
        SaveTickState(state);
        Notify(L"alert", L"Все пиры легли, замену найти не удалось. Дальше - крайний перебор всех стран.\n\nРазбор: " +
                         DiagnoseNowText(mainPeers));
    }

    // VPN: одобренный скрипт - пробуем перезапустить перед крайним перебором
    if (!RunApprovedVpnCommand(L"restart")) WarnVpnMayBeNeeded();

    // Фаервол - только читаем, не чаще раза в час, сообщаем один раз за простой
    if (now - state.firewallCheckedAt >= 3600) {
        state.firewallCheckedAt = now;
        auto firewallNote = CheckFirewallDiagnostics();
        if (firewallNote.has_value()) {
            LogError(L"Фаервол: " + *firewallNote);
            if (!state.firewallNotified) {
                state.firewallNotified = true;
                Notify(L"alert", L"Возможная причина: " + *firewallNote);
            }
        }
    }
    SaveTickState(state);

    // Крайняя мера - перебор ВСЕХ пиров по всем странам (по остатку времени)
    std::vector<std::wstring> excludeHosts(usedHosts.begin(), usedHosts.end());
    bool foundCrisis = CrisisScanStep(state, excludeHosts, (std::max)(10LL, budget.Left() - 10));
    if (foundCrisis) {
        state.connLost = false;
        state.outageNotified = false;
        g_tickPhase = L"backup";
    } else {
        g_tickPhase = L"crisis";
    }
    SaveTickState(state);
}

// Обёртка: сама проверка + запись итога (время, фаза) в state.txt и
// строки истории в history.log - для окна сторожа.
void RecordHistory(const std::wstring& phase);

void UpdateDiagnosisState(WatchdogState& st, const std::wstring& phase);   // ниже, рядом с Diagnose
void RecordOwnHash();                                                     // ниже, рядом со стражем
void EnsureGuardHealthy();

void Tick() {
    // Одна проверка за раз: окно, Планировщик и консоль могут запустить её одновременно.
    // Вторая не должна лезть в те же пиры и state.txt - просто пропускает ход.
    // !lockH: мьютекс создала проверка от SYSTEM, и нам в нём отказано (ACCESS_DENIED) -
    // это тоже значит "проверка уже идёт"; раньше такой случай запускал вторую параллельно.
    HANDLE lockH = CreateMutexW(nullptr, TRUE, L"Global\\YggWatchdogTickLock");
    if (!lockH || GetLastError() == ERROR_ALREADY_EXISTS) {
        LogInfo(L"Предыдущая проверка ещё идёт - эту пропускаю.");
        if (lockH) CloseHandle(lockH);
        return;
    }
    RecordOwnHash();
    EnsureGuardHealthy();
    TickBudget budget;
    TickInner(budget);
    FlushPeerStats();
    if (budget.Elapsed() >= 60) LogInfo(L"Проверка заняла " + std::to_wstring(budget.Elapsed()) + L" с");
    else LogDebug(L"Проверка заняла " + std::to_wstring(budget.Elapsed()) + L" с");
    WatchdogState st = LoadState();
    st.lastTickAt = (long long)time(nullptr);
    st.lastPhase = g_tickPhase;
    // Диагностика - в самом конце, когда поиск замены уже закончен
    UpdateDiagnosisState(st, g_tickPhase);
    SaveState(st);
    RecordHistory(g_tickPhase);
    if (lockH) { ReleaseMutex(lockH); CloseHandle(lockH); }
}


// ------------------------------------------------------------------
// ------------------------------------------------------------------
// История для окна сторожа: одна строка на тик в history.log -
//   unix-время <TAB> фаза <TAB> интернет(1/0) <TAB> пир=U|мс;пир=D|-1;...
// Задержка - из getPeers в JSON. Храним ~48 часов.
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Разбор "почему нет связи": по last_error каждого основного пира
// (что сказал сам Yggdrasil) плюс пара быстрых проверок - есть ли
// интернет, работает ли DNS. Только чтение, ничего не меняет.
// ------------------------------------------------------------------
struct PeerDiag {
    std::wstring uri;
    std::wstring kind;   // up / unstable / dns / eof / timeout / refused / unreachable / tls / waiting / absent / other
    std::wstring text;   // по-человечески
    std::wstring error;  // как есть от Yggdrasil
};
struct Diagnosis {
    std::vector<PeerDiag> peers;
    std::wstring verdict;  // одна строка: что случилось
    std::wstring advice;   // одна строка: что делать
    bool anyUp = false;
};

bool DnsWorks() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    const char* names[] = { "github.com", "cloudflare.com" };
    for (const char* n : names) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(n, "443", &hints, &res) == 0) { freeaddrinfo(res); WSACleanup(); return true; }
    }
    WSACleanup();
    return false;
}

static bool HasCI(const std::wstring& hay, const wchar_t* needle) {
    std::wstring h = hay, n = needle;
    for (auto& c : h) c = towlower(c);
    for (auto& c : n) c = towlower(c);
    return h.find(n) != std::wstring::npos;
}

static void ClassifyPeerError(PeerDiag& pd) {
    const std::wstring& e = pd.error;
    if (e.empty())                                   { pd.kind = L"waiting";     pd.text = L"ещё подключается, ошибки пока нет"; }
    else if (HasCI(e, L"no such host") || HasCI(e, L"lookup"))
                                                     { pd.kind = L"dns";         pd.text = L"имя сервера не находится (DNS)"; }
    else if (HasCI(e, L"EOF") || HasCI(e, L"reset by peer") || HasCI(e, L"forcibly closed") || HasCI(e, L"wsarecv"))
                                                     { pd.kind = L"eof";         pd.text = L"сервер обрывает соединение (EOF)"; }
    else if (HasCI(e, L"timeout") || HasCI(e, L"timed out") || HasCI(e, L"deadline"))
                                                     { pd.kind = L"timeout";     pd.text = L"сервер не отвечает (таймаут)"; }
    else if (HasCI(e, L"refused") || HasCI(e, L"actively refused"))
                                                     { pd.kind = L"refused";     pd.text = L"порт закрыт (соединение отклонено)"; }
    else if (HasCI(e, L"unreachable") || HasCI(e, L"no route"))
                                                     { pd.kind = L"unreachable"; pd.text = L"нет маршрута до сервера"; }
    else if (HasCI(e, L"tls") || HasCI(e, L"certificate") || HasCI(e, L"handshake"))
                                                     { pd.kind = L"tls";         pd.text = L"не прошло рукопожатие (TLS / ключ)"; }
    else                                             { pd.kind = L"other";       pd.text = L"ошибка: " + e; }
}

// internet: 1 - точно есть, 0 - точно нет, -1 - проверить самому
Diagnosis Diagnose(const std::vector<std::wstring>& mainPeers, const std::vector<PeerDetail>& details, int internet = -1) {
    Diagnosis dg;
    int nDns = 0, nEof = 0, nTimeout = 0, nRefused = 0, nUnreach = 0, nTls = 0, nOther = 0, nWait = 0, nAbsent = 0, nUnstable = 0;
    for (const auto& uri : mainPeers) {
        PeerDiag pd;
        pd.uri = uri;
        const PeerDetail* found = nullptr;
        for (const auto& d : details) if (_wcsicmp(d.uri.c_str(), uri.c_str()) == 0) { found = &d; break; }
        if (!found) { pd.kind = L"absent"; pd.text = L"Yggdrasil не знает этот пир (служба не перечитала конфиг?)"; ++nAbsent; }
        else if (found->up && found->unstable) {
            pd.kind = L"unstable";
            pd.text = L"подключается и снова рвётся (" + std::to_wstring(found->flaps) + L" раз за полчаса)";
            ++nUnstable;
        }
        else if (found->up) { pd.kind = L"up"; pd.text = L"работает"; dg.anyUp = true; }
        else {
            pd.error = found->lastError;
            ClassifyPeerError(pd);
            if (pd.kind == L"dns") ++nDns; else if (pd.kind == L"eof") ++nEof; else if (pd.kind == L"timeout") ++nTimeout;
            else if (pd.kind == L"refused") ++nRefused; else if (pd.kind == L"unreachable") ++nUnreach;
            else if (pd.kind == L"tls") ++nTls; else if (pd.kind == L"waiting") ++nWait; else ++nOther;
        }
        dg.peers.push_back(pd);
    }

    if (mainPeers.empty()) {
        dg.verdict = L"В конфиге нет основных пиров.";
        dg.advice = L"Добавь хотя бы два пира на вкладке «пиры».";
        return dg;
    }
    if (dg.anyUp) {
        int dead = nDns + nEof + nTimeout + nRefused + nUnreach + nTls + nOther + nAbsent + nUnstable;
        dg.verdict = dead == 0 ? L"Связь есть, все основные пиры работают."
                               : L"Связь есть, но " + std::to_wstring(dead) + L" из " + std::to_wstring(mainPeers.size()) + L" основных пиров не работают.";
        dg.advice = dead == 0 ? L"" : L"Пока держит хотя бы один пир, всё в порядке. Мёртвые лучше заменить.";
        return dg;
    }
    bool netOk = internet < 0 ? HasInternetConnectivity() : internet == 1;
    if (!netOk) {
        dg.verdict = L"Нет интернета вообще - дело не в пирах.";
        dg.advice = L"Проверь Wi-Fi или кабель, роутер, VPN. Сторож сам заметит, когда сеть вернётся.";
        return dg;
    }
    int total = (int)mainPeers.size();
    if (nDns > 0) {
        bool dnsOk = DnsWorks();
        if (!dnsOk) {
            dg.verdict = L"Не работает DNS на этом компьютере: имена серверов не находятся.";
            dg.advice = L"Проверь DNS в настройках сети или VPN. Пиры с IP-адресом вместо имени работают и без DNS.";
            return dg;
        }
        if (nDns == total) {
            dg.verdict = L"DNS работает, но имён всех основных пиров больше нет - сервера, похоже, закрылись.";
            dg.advice = L"Замени основные пиры: выбери новые в каталоге (вкладка «пиры»).";
            return dg;
        }
    }
    if (nUnstable == total) {
        dg.verdict = L"Пиры подключаются и тут же отваливаются: соединение рвётся каждые несколько минут, трафик не успевает пойти. "
                     L"Похоже, провайдер обрывает долгие соединения с этими серверами (так бывает с зарубежными хостингами).";
        dg.advice = L"Нужен пир поближе, лучше в своей стране - сторож добирает резервные сам; или подключение через VPN.";
        return dg;
    }
    if (nTimeout + nUnreach == total) {
        dg.verdict = L"Сервера пиров не отвечают: похоже, провайдер или фаервол режет эти соединения.";
        dg.advice = L"Попробуй пиры на портах 443/80, пиры с tls:// или quic://, либо через VPN.";
        return dg;
    }
    if (nEof == total) {
        dg.verdict = L"Сервера принимают соединение и сразу обрывают его.";
        dg.advice = L"Так бывает при DPI-блокировке или перегрузке сервера. Попробуй tls:// пиры или другие сервера.";
        return dg;
    }
    if (nRefused == total) {
        dg.verdict = L"Сервера живы, но порты пиров закрыты - Yggdrasil на них больше не работает.";
        dg.advice = L"Замени основные пиры.";
        return dg;
    }
    if (nWait == total) {
        dg.verdict = L"Yggdrasil только что запущен или переподключается - ошибок пока нет.";
        dg.advice = L"Подожди пару минут.";
        return dg;
    }
    if (nAbsent == total) {
        dg.verdict = L"Пиров из конфига нет в работающем Yggdrasil.";
        dg.advice = L"Перезапусти службу Yggdrasil, чтобы она перечитала конфиг.";
        return dg;
    }
    std::wstring parts;
    auto add = [&](int n, const wchar_t* what) { if (n) { if (!parts.empty()) parts += L", "; parts += std::to_wstring(n) + L" - " + what; } };
    add(nDns, L"имя не находится"); add(nEof, L"обрыв"); add(nTimeout, L"таймаут"); add(nRefused, L"порт закрыт");
    add(nUnreach, L"нет маршрута"); add(nTls, L"TLS"); add(nOther, L"другое"); add(nWait, L"подключается"); add(nAbsent, L"нет в службе");
    add(nUnstable, L"рвётся");
    dg.verdict = L"Все основные пиры не работают, причины разные: " + parts + L".";
    dg.advice = L"Замени мёртвые пиры на живые из каталога.";
    return dg;
}

std::wstring DiagnosisShort(const Diagnosis& dg);

// Кладёт разбор в state (или очищает, если основные пиры в порядке или не проверяются)
// Разбор считается всегда, когда пиры вообще проверялись: при 3-7 пирах "всё
// зелёное" не значит, что все основные живы - окно покажет, какие мертвы.
void UpdateDiagnosisState(WatchdogState& st, const std::wstring& phase) {
    st.diagVerdict.clear(); st.diagAdvice.clear(); st.diagPeers.clear(); st.diagAnyUp = false;
    bool checked = phase == L"ok" || phase == L"backup" || phase == L"down" || phase == L"crisis" || phase == L"no_internet";
    if (!checked) return;
    std::vector<std::wstring> mp;
    auto cfg = FindConfigPath();
    if (cfg.has_value()) mp = ReadMainPeers(*cfg, true);
    auto details = GetPeerDetails();
    MarkUnstable(details, st);
    Diagnosis dg = Diagnose(mp, details, phase == L"no_internet" ? 0 : 1);
    auto clean = [](std::wstring t) { for (auto& c : t) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' '; return t; };
    st.diagVerdict = clean(dg.verdict);
    st.diagAdvice = clean(dg.advice);
    st.diagAnyUp = dg.anyUp;
    for (const auto& p : dg.peers)
        st.diagPeers.push_back(clean(p.uri) + L"\t" + clean(p.kind) + L"\t" + clean(p.text) + L"\t" + clean(p.error));
}

std::wstring DiagnoseNowText(const std::vector<std::wstring>& mainPeers) {
    auto details = GetPeerDetails();
    MarkUnstable(details, LoadState());
    return DiagnosisShort(Diagnose(mainPeers, details));
}

std::wstring DiagnosisShort(const Diagnosis& dg) {
    std::wstring s = dg.verdict;
    for (const auto& p : dg.peers) if (p.kind != L"up") s += L"\n- " + HostFromUri(p.uri) + L": " + p.text;
    if (!dg.advice.empty()) s += L"\n" + dg.advice;
    return s;
}

// История - 8 суток (для недельных цифр в stats-json). Обрезается не чаще
// раза в сутки: смотрим только время первой строки, весь файл не читаем.
const long long HISTORY_RETENTION_SECONDS = 8 * 86400;

void PruneHistoryIfLarge() {
    std::string head;
    {
        std::ifstream f(std::filesystem::path(HistoryPath()), std::ios::binary);
        if (!f.is_open()) return;
        char buf[32] = {};
        f.read(buf, sizeof(buf) - 1);
        head.assign(buf, (size_t)f.gcount());
    }
    long long firstTs = 0;
    try { firstTs = std::stoll(head.substr(0, head.find('\t'))); } catch (...) { return; }
    long long cutoff = (long long)time(nullptr) - HISTORY_RETENTION_SECONDS;
    if (firstTs >= cutoff - 86400) return;     // старше срока меньше чем на сутки - потерпит
    std::wstring content = ReadFileUtf8(HistoryPath());
    std::wstring kept;
    for (auto line : SplitBy(content, L'\n')) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        long long ts = 0;
        try { ts = std::stoll(line.substr(0, line.find(L'\t'))); } catch (...) { continue; }
        if (ts >= cutoff) kept += line + L"\r\n";
    }
    util::WriteFileAtomic(HistoryPath(), util::ToUtf8(kept));
}

// ------------------------------------------------------------------
// Агрегаты задержек из history.log: за час / сутки / неделю по каждому пиру -
// сколько проверок, доля рабочих, средняя задержка, P95 (95% проверок были
// быстрее этого), разброс (стандартное отклонение - "дрожание" задержки).
// ------------------------------------------------------------------
struct HistSample { long long ts = 0; bool up = false; double ms = -1; };

// Строка history.log: ts<TAB>фаза<TAB>интернет<TAB>uri=U|мс;uri=D|-1;...
// В uri бывает '=' (?key=...), поэтому граница - последнее "=U|" / "=D|".
void ParseHistoryLine(const std::wstring& lineIn, std::map<std::wstring, std::vector<HistSample>>& out) {
    std::wstring line = lineIn;
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    size_t t1 = line.find(L'\t');
    if (t1 == std::wstring::npos) return;
    long long ts = 0;
    try { ts = std::stoll(line.substr(0, t1)); } catch (...) { return; }
    size_t t2 = line.find(L'\t', t1 + 1), t3 = t2 == std::wstring::npos ? t2 : line.find(L'\t', t2 + 1);
    if (t3 == std::wstring::npos) return;
    for (const auto& item : SplitBy(line.substr(t3 + 1), L';')) {
        size_t a = item.rfind(L"=U|"), b = item.rfind(L"=D|");
        size_t m = (a == std::wstring::npos) ? b : (b == std::wstring::npos ? a : (std::max)(a, b));
        if (m == std::wstring::npos || m == 0) continue;
        HistSample s;
        s.ts = ts;
        s.up = item[m + 1] == L'U';
        try { s.ms = std::stod(item.substr(m + 3)); } catch (...) { s.ms = -1; }
        out[item.substr(0, m)].push_back(s);
    }
}

struct LatencyAgg { long long n = 0, up = 0, rttN = 0; double avg = -1, p95 = -1, jitter = -1; };

LatencyAgg Aggregate(const std::vector<HistSample>& v, long long fromTs) {
    LatencyAgg a;
    std::vector<double> r;
    for (const auto& s : v) {
        if (s.ts < fromTs) continue;
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
    size_t rank = (size_t)std::ceil(0.95 * (double)r.size());   // P95 "ближайшим рангом"
    a.p95 = r[(std::max)((size_t)1, rank) - 1];
    return a;
}

std::wstring Num1(double v) { std::wstringstream s; s << std::fixed << std::setprecision(1) << v; return s.str(); }

std::wstring AggJson(const LatencyAgg& a) {
    return L"{\"n\":" + std::to_wstring(a.n) + L",\"up\":" + std::to_wstring(a.up) +
           L",\"up_pct\":" + (a.n ? Num1(100.0 * (double)a.up / (double)a.n) : std::wstring(L"-1")) +
           L",\"avg_ms\":" + (a.avg >= 0 ? Num1(a.avg) : std::wstring(L"-1")) +
           L",\"p95_ms\":" + (a.p95 >= 0 ? Num1(a.p95) : std::wstring(L"-1")) +
           L",\"jitter_ms\":" + (a.jitter >= 0 ? Num1(a.jitter) : std::wstring(L"-1")) + L"}";
}

void RecordHistory(const std::wstring& phase) {
    WatchdogState st = LoadState();
    bool internet = (st.internetDownSince == 0) && phase != L"no_internet";
    std::wstring peers;
    if (phase != L"battery") {
        for (const auto& d : GetPeerDetails()) {
            if (!peers.empty()) peers += L";";
            std::wstringstream rtt;
            rtt << std::fixed << std::setprecision(1) << d.rttMs;
            peers += d.uri + L"=" + (d.up ? L"U|" : L"D|") + (d.rttMs < 0 ? std::wstring(L"-1") : rtt.str());
        }
    }
    AppendUtf8Line(HistoryPath(), std::to_wstring((long long)time(nullptr)) + L"\t" + phase + L"\t" +
                   (internet ? L"1" : L"0") + L"\t" + peers);
    PruneHistoryIfLarge();
}

// ------------------------------------------------------------------
// status-json: всё, что знает сторож, одним JSON - для окна сторожа и
// панели Иггдрасиля. Только чтение, ничего не меняет и не пишет в лог.
// ------------------------------------------------------------------
std::wstring JsonEscape(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        switch (c) {
            case L'"': r += L"\\\""; break;
            case L'\\': r += L"\\\\"; break;
            case L'\n': r += L"\\n"; break;
            case L'\r': r += L"\\r"; break;
            case L'\t': r += L"\\t"; break;
            default:
                if (c < 0x20) { const wchar_t* hx = L"0123456789abcdef"; r += L"\\u00"; r += hx[(c >> 4) & 0xF]; r += hx[c & 0xF]; }
                else r += c;
        }
    }
    return r;
}
std::wstring JStr(const std::wstring& s) { return L"\"" + JsonEscape(s) + L"\""; }
std::wstring JArr(const std::vector<std::wstring>& v) {
    std::wstring r = L"[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) r += L","; r += JStr(v[i]); }
    return r + L"]";
}

void CmdStatusJson() {
    WatchdogState st = LoadState();
    std::vector<std::wstring> mainPeers;
    auto cfg = FindConfigPath();
    if (cfg.has_value()) mainPeers = ReadMainPeers(*cfg, true);
    auto bat = ReadBatteryStatus();
    bool vpnApproved = !ReadFileUtf8(VpnApprovalPath()).empty();
    bool notifyOn = LoadNotifyConfig().valid;
    long long now = (long long)time(nullptr);
    long long cooldownUntil = st.crisisLastFullScanCompletedAt ? st.crisisLastFullScanCompletedAt + CRISIS_SCAN_COOLDOWN_SECONDS : 0;

    std::wstring j = L"{";
    j += L"\"version\":" + JStr(CURRENT_VERSION);
    j += L",\"device\":" + JStr(GetDeviceName());
    j += L",\"now\":" + std::to_wstring(now);
    j += L",\"last_tick_at\":" + std::to_wstring(st.lastTickAt);
    j += L",\"tick_interval_sec\":180";
    j += L",\"last_phase\":" + JStr(st.lastPhase);
    // "mode" оставлен для совместимости с панелью: regional = задан регион поездки
    j += L",\"mode\":" + JStr(st.regionalCountry.empty() ? L"normal" : L"regional");
    j += L",\"min_peers\":" + std::to_wstring(MIN_ALIVE_PEERS) + L",\"max_peers\":" + std::to_wstring(MAX_ALIVE_PEERS);
    {
        std::vector<std::wstring> unstable;
        for (const auto& [uri, tr] : st.peerTracks) if ((int)tr.flaps.size() >= FLAP_THRESHOLD) unstable.push_back(uri);
        j += L",\"unstable_peers\":" + JArr(unstable);
    }
    j += L",\"service_problem\":" + std::wstring(st.serviceNotified ? L"true" : L"false");
    j += L",\"config_problem\":" + std::wstring(st.configNotified ? L"true" : L"false");
    j += L",\"config_error\":" + JStr(st.configError);
    j += L",\"config_restore_from\":" + JStr(st.configRestoreFrom.empty() ? L"" : std::filesystem::path(st.configRestoreFrom).filename().wstring());
    j += L",\"tun_problem\":" + JStr(st.tunProblem);
    j += L",\"config_found\":" + std::wstring(cfg.has_value() ? L"true" : L"false");
    j += L",\"main_peers\":" + JArr(mainPeers);
    j += L",\"backup_peers\":" + JArr(st.backupPeers);
    j += L",\"backup_region\":" + JStr(st.backupRegion);
    j += L",\"regional_peers\":[]";   // старое поле: региональных пиров больше нет
    j += L",\"regional_country\":" + JStr(st.regionalCountry);
    j += L",\"regional_city\":" + JStr(st.regionalCity);
    j += L",\"home_region\":" + JStr(st.homeRegion);
    j += L",\"home_city\":" + JStr(st.homeCity);
    j += L",\"egress_region\":" + JStr(st.egressRegion);
    j += L",\"internet_down_since\":" + std::to_wstring(st.internetDownSince);
    j += L",\"crisis\":{\"countries_total\":" + std::to_wstring(st.crisisCountrySlugs.size()) +
         L",\"country_index\":" + std::to_wstring(st.crisisCountryIndex) +
         L",\"peer_index\":" + std::to_wstring(st.crisisPeerIndex) +
         L",\"current_country\":" + JStr(st.crisisCountryIndex < st.crisisCountrySlugs.size() ? st.crisisCountrySlugs[st.crisisCountryIndex] : L"") +
         L",\"cooldown_until\":" + std::to_wstring(cooldownUntil) + L"}";
    j += L",\"devices\":[";
    for (size_t i = 0; i < st.devices.size(); ++i) {
        const auto& d = st.devices[i];
        auto it = st.deviceDownSince.find(d.name);
        if (i) j += L",";
        auto why = st.deviceWhy.find(d.name);
        auto pe = st.devicePeers.find(d.name);
        auto tp = st.deviceTempPeers.find(d.name);
        j += L"{\"name\":" + JStr(d.name) + L",\"host\":" + JStr(d.host) +
             L",\"down_since\":" + std::to_wstring(it == st.deviceDownSince.end() ? 0 : it->second) +
             L",\"why\":" + JStr(why == st.deviceWhy.end() ? L"" : why->second) +
             L",\"peers\":" + JArr(pe == st.devicePeers.end() ? std::vector<std::wstring>() : pe->second) +
             L",\"temp_peers\":" + JArr(tp == st.deviceTempPeers.end() ? std::vector<std::wstring>() : tp->second) + L"}";
    }
    j += L"]";
    j += L",\"battery\":";
    if (bat.has_value()) {
        j += L"{\"percent\":" + std::to_wstring(bat->percent) + L",\"charging\":" + (bat->charging ? L"true" : L"false") + L"}";
    } else {
        j += L"null";
    }
    j += L",\"battery_critical_mode\":" + std::wstring(st.batteryCriticalMode ? L"true" : L"false");
    j += L",\"update_available\":" + JStr(!st.updateNotifiedTag.empty() && !VersionLessOrEqual(st.updateNotifiedTag, CURRENT_VERSION)
                                            ? st.updateNotifiedTag : L"");
    j += L",\"vpn_script_approved\":" + std::wstring(vpnApproved ? L"true" : L"false");
    j += L",\"telegram_configured\":" + std::wstring(notifyOn ? L"true" : L"false");
    {
        long long at = 0;
        try { at = std::stoll(util::RegReadPath(REG_WATCHDOG_KEY, L"TgLastAt")); } catch (...) {}
        j += L",\"telegram_last\":{\"at\":" + std::to_wstring(at) +
             L",\"ok\":" + (util::RegReadPath(REG_WATCHDOG_KEY, L"TgLastOk") == L"1" ? L"true" : L"false") +
             L",\"via\":" + JStr(util::RegReadPath(REG_WATCHDOG_KEY, L"TgLastVia")) +
             L",\"err\":" + JStr(util::RegReadPath(REG_WATCHDOG_KEY, L"TgLastErr")) + L"}";
    }
    {   // разбор - готовый, из state (считается в конце проверки)
        j += L",\"diagnosis\":{\"verdict\":" + JStr(st.diagVerdict) + L",\"advice\":" + JStr(st.diagAdvice) +
             L",\"any_up\":" + std::wstring(st.diagAnyUp ? L"true" : L"false") + L",\"peers\":[";
        bool first = true;
        for (const auto& line : st.diagPeers) {
            std::vector<std::wstring> f;
            size_t pos = 0;
            for (int k = 0; k < 3; ++k) { size_t t = line.find(L'\t', pos); if (t == std::wstring::npos) break; f.push_back(line.substr(pos, t - pos)); pos = t + 1; }
            if (f.size() < 3) continue;
            if (!first) j += L",";
            first = false;
            j += L"{\"uri\":" + JStr(f[0]) + L",\"kind\":" + JStr(f[1]) + L",\"text\":" + JStr(f[2]) +
                 L",\"error\":" + JStr(line.substr(pos)) + L"}";
        }
        j += L"]}";
    }
    j += L",\"files\":{\"log\":" + JStr(LogPath()) + L",\"events\":" + JStr(EventsPath()) +
         L",\"history\":" + JStr(HistoryPath()) + L",\"state\":" + JStr(StatePath()) + L"}";
    j += L"}";

    WriteOut(j + L"\n");
}

// Списки для окна сторожа (выбор региона) - JSON, только чтение.
void CmdCountriesJson() {
    auto entries = GetSortedCountryList();
    std::wstring j = L"[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i) j += L",";
        j += L"{\"slug\":" + JStr(entries[i].slug) + L",\"name\":" + JStr(entries[i].ruName) +
             L",\"supported\":" + (entries[i].supported ? L"true" : L"false") + L"}";
    }
    WriteOut(j + L"]\n");
}


// ------------------------------------------------------------------
// catalog-json <страна>: публичные пиры страны из репозитория с
// задержкой TCP-подключения (время connect до host:port). Это не пинг
// Yggdrasil, а грубая оценка "жив ли сервер и далеко ли он". quic://
// работает по UDP - его так не проверить. Только чтение.
// ------------------------------------------------------------------
void CmdCatalogJson(const std::wstring& countrySlug) {
    auto candidates = FetchCountryPeers(countrySlug);
    std::vector<ProbeJob> jobs = ProbePeers(candidates);
    auto rel = LoadReliability();

    std::vector<size_t> order(candidates.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        double ma = jobs[a].ms, mb = jobs[b].ms;
        if ((ma >= 0) != (mb >= 0)) return ma >= 0;
        if (ma >= 0) return ma < mb;
        return false;
    });

    std::wstring j = L"[";
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        if (k) j += L",";
        std::wstringstream ms;
        ms << std::fixed << std::setprecision(1) << jobs[i].ms;
        j += L"{\"uri\":" + JStr(candidates[i].uri) + L",\"city\":" + JStr(candidates[i].city) +
             L",\"tcp_ms\":" + (jobs[i].ms >= 0 ? ms.str() : std::wstring(L"-1")) +
             L",\"err\":" + JStr(jobs[i].err) +
             L",\"reliability\":" + (rel.count(candidates[i].uri) && rel[candidates[i].uri].score >= 0
                                        ? Num1(100.0 * rel[candidates[i].uri].score) : std::wstring(L"-1")) + L"}";
    }
    WriteOut(j + L"]\n");
}


// ------------------------------------------------------------------
// verify-peer <uri>: настоящая проверка пира. Открытый TCP-порт ещё ничего
// не значит (сервер может принять соединение и сразу его оборвать), поэтому
// пир на несколько секунд подключается к самому Yggdrasil, и смотрим, что
// скажет getPeers: поднялся ли он, какая задержка, какая ошибка. Если пир
// уже в списке (основной / резервный) - ничего не добавляем, берём как есть.
// Результат - одна строка JSON в конце вывода.
// ------------------------------------------------------------------
void CmdVerifyPeer(const std::wstring& uri) {
    auto find = [&](const std::vector<PeerDetail>& v) -> const PeerDetail* {
        for (const auto& d : v) if (_wcsicmp(d.uri.c_str(), uri.c_str()) == 0) return &d;
        return nullptr;
    };
    bool alreadyThere = find(GetPeerDetails()) != nullptr;
    bool up = false;
    double ms = -1;
    std::wstring err;
    if (!alreadyThere) AddPeer(uri);
    auto start = std::chrono::steady_clock::now();
    int upSeen = 0;
    for (;;) {
        auto details = GetPeerDetails();
        const PeerDetail* d = find(details);
        if (d) {
            up = d->up;
            ms = d->rttMs;
            err = d->lastError;
            if (up && ++upSeen >= 2) break;      // дали ещё одну секунду, чтобы появилась задержка
            if (up && ms > 0) break;
        }
        long long el = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (el >= HANDSHAKE_TIMEOUT_MS || (alreadyThere && !up) || (!up && !err.empty())) break;
        Sleep(1000);
    }
    if (!alreadyThere) {
        RemovePeer(uri);
        g_handshakeResults.push_back({ uri, up });
        FlushPeerStats();
    }
    std::wstringstream msS;
    msS << std::fixed << std::setprecision(1) << ms;
    PeerDiag pd;
    pd.error = err;
    if (!up) ClassifyPeerError(pd);
    WriteOut(L"{\"uri\":" + JStr(uri) + L",\"up\":" + (up ? L"true" : L"false") + L",\"ms\":" + (ms >= 0 ? msS.str() : std::wstring(L"-1")) +
             L",\"error\":" + JStr(err) + L",\"text\":" + JStr(up ? L"" : pd.text) +
             L",\"was_configured\":" + (alreadyThere ? L"true" : L"false") + L"}\n");
}

// stats-json: по каждому пиру из истории и статистики - задержки за час /
// сутки / неделю и надёжность за месяц. Только чтение. Для окна (экран
// статистики) и для выгрузки.
void CmdStatsJson() {
    std::map<std::wstring, std::vector<HistSample>> hist;
    {
        std::wstringstream ss(ReadFileUtf8(HistoryPath()));
        std::wstring line;
        while (std::getline(ss, line)) ParseHistoryLine(line, hist);
    }
    auto rel = LoadReliability();
    WatchdogState st = LoadState();
    std::set<std::wstring> uris;
    for (const auto& kv : hist) uris.insert(kv.first);
    for (const auto& kv : rel) uris.insert(kv.first);
    long long now = (long long)time(nullptr);
    std::wstring j = L"{\"now\":" + std::to_wstring(now) + L",\"peers\":[";
    bool first = true;
    for (const auto& uri : uris) {
        if (!first) j += L",";
        first = false;
        const auto& v = hist[uri];
        bool lastUp = !v.empty() && v.back().up;
        auto r = rel.count(uri) ? rel[uri] : Reliability();
        auto tr = st.peerTracks.find(uri);
        int flapsNow = tr == st.peerTracks.end() ? 0 : (int)tr->second.flaps.size();
        bool isBackup = std::find(st.backupPeers.begin(), st.backupPeers.end(), uri) != st.backupPeers.end();
        j += L"{\"uri\":" + JStr(uri) +
             L",\"last_seen\":" + std::to_wstring(v.empty() ? 0 : v.back().ts) +
             L",\"last_up\":" + (lastUp ? L"true" : L"false") +
             L",\"backup\":" + (isBackup ? L"true" : L"false") +
             L",\"flaps_30min\":" + std::to_wstring(flapsNow) +
             L",\"h1\":" + AggJson(Aggregate(v, now - 3600)) +
             L",\"d1\":" + AggJson(Aggregate(v, now - 86400)) +
             L",\"d7\":" + AggJson(Aggregate(v, now - 7 * 86400)) +
             L",\"reliability\":" + (r.score >= 0 ? Num1(100.0 * r.score) : std::wstring(L"-1")) +
             L",\"reliability_samples\":" + std::to_wstring(r.samples) +
             L",\"flaps_35d\":" + std::to_wstring(r.flaps) +
             L",\"tries\":" + std::to_wstring(r.tries) + L",\"tries_ok\":" + std::to_wstring(r.triesOk);
        // для графика: 24 часа по 15 минут, средняя задержка в каждом отрезке (-1 = нет данных / не работал)
        j += L",\"series24\":[";
        for (int k = 0; k < 96; ++k) {
            long long a = now - 86400 + (long long)k * 900, b = a + 900;
            double sum = 0; int n = 0;
            for (const auto& s : v) if (s.ts >= a && s.ts < b && s.up && s.ms >= 0) { sum += s.ms; ++n; }
            if (k) j += L",";
            j += n ? Num1(sum / n) : std::wstring(L"-1");
        }
        j += L"]}";
    }
    WriteOut(j + L"]}\n");
}

// stats-csv: та же статистика в таблицу stats_export.csv рядом с exe (для Excel:
// UTF-8 с BOM и ";" между колонками - так её правильно открывает русский Excel).
void CmdStatsCsv() {
    std::map<std::wstring, std::vector<HistSample>> hist;
    {
        std::wstringstream ss(ReadFileUtf8(HistoryPath()));
        std::wstring line;
        while (std::getline(ss, line)) ParseHistoryLine(line, hist);
    }
    auto rel = LoadReliability();
    WatchdogState st = LoadState();
    std::vector<std::wstring> mains;
    if (auto cfg = FindConfigPath()) mains = ReadMainPeers(*cfg, true);
    std::set<std::wstring> uris;
    for (const auto& kv : hist) uris.insert(kv.first);
    for (const auto& kv : rel) uris.insert(kv.first);
    long long now = (long long)time(nullptr);
    auto num = [](double v) { if (v < 0) return std::wstring(); std::wstring s = Num1(v); for (auto& c : s) if (c == L'.') c = L','; return s; };
    auto cell = [](std::wstring s) { for (auto& c : s) if (c == L';' || c == L'\r' || c == L'\n') c = L' '; return s; };
    std::wstring t = L"пир;роль;работает сейчас";
    for (const wchar_t* w : { L"1 ч", L"24 ч", L"7 сут" })
        t += std::wstring(L";доступность ") + w + L", %;средняя " + w + L", мс;P95 " + w + L", мс;разброс " + w + L", мс";
    t += L";надёжность 30 сут, %;наблюдений;обрывов 35 сут;пробных рукопожатий;удачных\r\n";
    for (const auto& uri : uris) {
        const auto& v = hist[uri];
        bool isMain = std::find(mains.begin(), mains.end(), uri) != mains.end();
        bool isBackup = std::find(st.backupPeers.begin(), st.backupPeers.end(), uri) != st.backupPeers.end();
        t += cell(uri) + L";" + (isMain ? L"основной" : isBackup ? L"резервный" : L"-") + L";" +
             (!v.empty() && v.back().ts > now - 600 ? (v.back().up ? L"да" : L"нет") : L"-");
        for (long long win : { 3600LL, 86400LL, 7 * 86400LL }) {
            LatencyAgg a = Aggregate(v, now - win);
            t += L";" + (a.n ? num(100.0 * (double)a.up / (double)a.n) : L"") + L";" + num(a.avg) + L";" + num(a.p95) + L";" + num(a.jitter);
        }
        Reliability r = rel.count(uri) ? rel[uri] : Reliability();
        t += L";" + num(r.score >= 0 ? 100.0 * r.score : -1) + L";" + std::to_wstring(r.samples) + L";" + std::to_wstring(r.flaps) +
             L";" + std::to_wstring(r.tries) + L";" + std::to_wstring(r.triesOk) + L"\r\n";
    }
    std::wstring path = GetExeDir() + L"stats_export.csv";
    if (util::WriteFileAtomic(path, "\xEF\xBB\xBF" + util::ToUtf8(t))) Print(L"Статистика выгружена: " + path);
    else PrintError(L"не удалось записать " + path);
}

void CmdCitiesJson(const std::wstring& countrySlug) {
    std::wstring j = L"[";
    auto iso = SlugToIso(countrySlug);
    if (iso.has_value()) {
        const auto& db = LoadCityDatabase();
        auto cities = GetCitiesForCountry(db, *iso);
        for (size_t i = 0; i < cities.size(); ++i) {
            if (i) j += L",";
            j += L"{\"name\":" + JStr(cities[i].name) + L",\"label\":" + JStr(cities[i].displayLabel()) +
                 L",\"population\":" + std::to_wstring(cities[i].population) + L"}";
        }
    }
    WriteOut(j + L"]\n");
}

// ------------------------------------------------------------------
// Самостоятельная регистрация в Планировщике заданий - используем
// СВОЙ реальный путь, никакого python.exe и связанных с ним проблем
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Страж (ygg_guard.exe, исходник ygg_guard.cpp). Целостность exe не может
// проверить сам exe: повреждённый файл просто не запустится. Поэтому задача
// Планировщика запускает отдельную маленькую программу-стража: она проверяет
// ygg_watchdog.exe, запускает проверку, а если exe повреждён - скачивает
// целый из репозитория. А сторож на каждой проверке смотрит, цел ли страж,
// и при нужде так же достаёт его из репозитория: они страхуют друг друга.
// ------------------------------------------------------------------
static const wchar_t* REG_WD_KEY = L"SOFTWARE\\Yozmor\\ygg-watchdog";

std::wstring GuardExePath() { return GetExeDir() + L"ygg_guard.exe"; }
std::wstring WatchdogExePath() { return GetExeDir() + L"ygg_watchdog.exe"; }

// Цел ли файл программы: пусто - цел, иначе - что не так. checkHash: если файл
// не пересобирали (размер и время те же, что записал сам сторож), его SHA-256
// тоже должен совпасть - иначе он испорчен на диске.
std::wstring CheckExeIntegrity(const std::wstring& path, bool checkHash) {
    if (!util::FileExists(path)) return L"файла нет";
    if (!LooksLikeExe(path)) return L"нет заголовков MZ/PE (файл обрезан или затёрт)";
    if (!checkHash) return L"";
    std::wstring kSha = util::RegReadPath(REG_WD_KEY, L"ExeSha256"), kSize = util::RegReadPath(REG_WD_KEY, L"ExeSize"),
                 kTime = util::RegReadPath(REG_WD_KEY, L"ExeTime");
    if (kSha.empty() || kSize.empty() || kTime.empty()) return L"";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return L"";
    unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    unsigned long long ft = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    if (std::to_wstring(size) != kSize || std::to_wstring(ft) != kTime) return L"";   // пересобран / обновлён - это нормально
    auto h = Sha256HexOfFile(path);
    if (h.has_value() && *h != util::Lower(kSha))
        return L"содержимое изменилось без пересборки (SHA-256 не совпадает) - похоже на повреждение на диске";
    return L"";
}

void InstallScheduledTask();

// Скачать целого стража из репозитория вместо битого. Пусто - получилось.
std::wstring RepairGuardFromRepo(const std::wstring& why) {
    std::wstring tmp = GuardExePath() + L".download", err;
    if (!DownloadRepoFile(L"windows/ygg_guard.exe", tmp, err)) return L"скачать целый не удалось: " + err;
    long long now = (long long)time(nullptr);
    if (util::FileExists(GuardExePath()))
        MoveFileExW(GuardExePath().c_str(), (GuardExePath() + L".broken-" + util::TimeText(now, L"%Y%m%d-%H%M%S")).c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!MoveFileExW(tmp.c_str(), GuardExePath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return L"заменить его не удалось";
    }
    Notify(L"warn", L"Страж ygg_guard.exe был повреждён (" + why + L") - скачан целый из репозитория.");
    return L"";
}

// Сторож на каждой проверке смотрит, цел ли страж. Если задача из-за битого
// стража работала напрямую (без проверки целостности exe), а страж снова цел -
// задача возвращается на стража.
void EnsureGuardHealthy() {
    bool usesGuard = util::RegReadPath(REG_WD_KEY, L"TaskUsesGuard") == L"1";
    bool fallback = util::RegReadPath(REG_WD_KEY, L"TaskDirectFallback") == L"1";
    if (!usesGuard && !fallback) return;   // задачи нет или стража не было никогда
    std::wstring why = CheckExeIntegrity(GuardExePath(), false);
    if (!why.empty()) {
        long long now = (long long)time(nullptr), last = 0;
        try { last = std::stoll(util::RegReadPath(REG_WD_KEY, L"GuardSelfRepairAt")); } catch (...) {}
        if (now - last < 3600) return;                     // пробовали меньше часа назад
        util::RegWriteString(REG_WD_KEY, L"GuardSelfRepairAt", std::to_wstring(now));
        std::wstring err = RepairGuardFromRepo(why);
        if (!err.empty()) {
            Notify(L"alert", L"Страж ygg_guard.exe повреждён (" + why + L"), " + err +
                             L". Проверки идут напрямую, без проверки целостности ygg_watchdog.exe - собери заново (run_ygg_watchdog.bat).");
            return;
        }
    }
    if (fallback) {
        LogInfo(L"Страж снова цел - возвращаю задачу на него.");
        InstallScheduledTask();
    }
}

// Отпечаток своего exe для стража: размер, время записи и SHA-256. Пишется,
// только когда файл новый (пересобран / обновлён). Если позже содержимое
// изменится при тех же размере и времени - это повреждение на диске, и страж
// его заметит.
void RecordOwnHash() {
    const std::wstring key = L"SOFTWARE\\Yozmor\\ygg-watchdog";
    std::wstring self = util::SelfExe();
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(self.c_str(), GetFileExInfoStandard, &fa)) return;
    unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    unsigned long long ft = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    std::wstring sSize = std::to_wstring(size), sTime = std::to_wstring(ft);
    if (util::RegReadPath(key, L"ExeSize") == sSize && util::RegReadPath(key, L"ExeTime") == sTime &&
        !util::RegReadPath(key, L"ExeSha256").empty()) return;
    auto h = Sha256HexOfFile(self);
    if (!h.has_value()) return;
    util::RegWriteString(key, L"ExeSha256", *h);
    util::RegWriteString(key, L"ExeSize", sSize);
    util::RegWriteString(key, L"ExeTime", sTime);
    LogDebug(L"Отпечаток exe обновлён: " + *h);
}

// ------------------------------------------------------------------
// Перезапуск службы Yggdrasil - нужен после правки основных пиров
// (файл конфига читается только при старте). ВАЖНО: если подключён к
// этому компьютеру ЧЕРЕЗ САМ Yggdrasil (например RDP по Yggdrasil-
// адресу) - обычный перезапуск оборвёт твою же сессию. Для этого
// случая есть отложенный вариант через одноразовую задачу планировщика -
// команда успевает уйти по ещё живому каналу, а сам перезапуск
// происходит уже через несколько секунд, после того как ты успел
// отключиться/переключиться.
// ------------------------------------------------------------------
static const wchar_t* YGG_SERVICE_NAME = L"Yggdrasil";

void RunSimpleCommand(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        LogError(L"Не удалось запустить команду: " + cmd);
    }
}

void RestartYggdrasilServiceNow() {
    Print(L"Перезапускаю службу Yggdrasil немедленно...");
    Print(L"Внимание: если подключён к этому компьютеру ЧЕРЕЗ Yggdrasil - "
          L"соединение сейчас оборвётся. Если это твой единственный канал - "
          L"используй отложенный перезапуск вместо этого.");
    std::wstring err = util::ControlServiceNamed(YGG_SERVICE_NAME, L"restart");   // с таймаутом 20 + 20 с
    if (err.empty()) LogInfo(L"Служба Yggdrasil перезапущена.");
    else LogError(L"Перезапуск службы Yggdrasil не удался: " + err);
}

void RestartYggdrasilServiceDelayed(int delaySeconds) {
    auto future = std::chrono::system_clock::now() + std::chrono::seconds(delaySeconds);
    std::time_t t = std::chrono::system_clock::to_time_t(future);
    std::tm tm;
    localtime_s(&tm, &t);
    wchar_t timeBuf[16];
    swprintf(timeBuf, 16, L"%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::wstring taskName = L"YggRestartOnce";
    std::wstring innerCmd = L"net stop \"" + std::wstring(YGG_SERVICE_NAME) +
                             L"\" & net start \"" + std::wstring(YGG_SERVICE_NAME) + L"\"";
    std::wstring cmd = L"schtasks /Create /TN \"" + taskName + L"\" /TR \"cmd.exe /c \\\"" +
                        innerCmd + L"\\\"\" /SC ONCE /ST " + std::wstring(timeBuf) + L" /RL HIGHEST /F";

    RunSimpleCommand(cmd);
    LogInfo(L"Перезапуск службы запланирован через " + std::to_wstring(delaySeconds) +
            L" секунд (в " + std::wstring(timeBuf) + L"). Успей отключиться/переключиться, "
            L"если сидишь по Yggdrasil.");
}

// ------------------------------------------------------------------
// Задача Планировщика - через COM-интерфейс самого Планировщика (как его
// читает окно), без schtasks и PowerShell: все настройки ставятся сразу.
//   - от SYSTEM с наивысшими правами, без окон (сессия 0);
//   - каждые 3 минуты, бессрочно;
//   - StartWhenAvailable: пропущенный во сне запуск - сразу при пробуждении;
//   - не пропускать и не прерывать при работе от батареи (по умолчанию
//     Планировщик делает и то, и другое - был реальный простой ~90 минут);
//   - лимит одного запуска 10 минут, второй экземпляр не запускать.
// ------------------------------------------------------------------
struct ComReleaser { IUnknown* p; ~ComReleaser() { if (p) p->Release(); } };

static std::wstring HrText(const wchar_t* step, HRESULT hr) {
    wchar_t b[24];
    swprintf(b, 24, L"0x%08lX", (unsigned long)hr);
    return std::wstring(step) + L" (" + b + L")";
}

bool RegisterWatchdogTask(const std::wstring& exe, const std::wstring& args, std::wstring& err, const std::wstring& workDir = L"") {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    ITaskService* svc = nullptr; ITaskFolder* root = nullptr; ITaskDefinition* def = nullptr;
    IRegistrationInfo* info = nullptr; IPrincipal* pr = nullptr; ITaskSettings* ts = nullptr;
    ITriggerCollection* trs = nullptr; ITrigger* tr = nullptr; IRepetitionPattern* rep = nullptr;
    IActionCollection* acts = nullptr; IAction* act = nullptr; IExecAction* ex = nullptr; IRegisteredTask* reg = nullptr;
    VARIANT empty; VariantInit(&empty);
    auto B = [](const std::wstring& s) { return SysAllocString(s.c_str()); };
    do {
        HRESULT hr = CoCreateInstance(ygg::kClsidTaskScheduler, nullptr, CLSCTX_INPROC_SERVER, ygg::kIidTaskService, (void**)&svc);
        if (FAILED(hr)) { err = HrText(L"Планировщик недоступен", hr); break; }
        if (FAILED(hr = svc->Connect(empty, empty, empty, empty))) { err = HrText(L"нет связи с Планировщиком", hr); break; }
        BSTR rootName = B(L"\\");
        hr = svc->GetFolder(rootName, &root);
        SysFreeString(rootName);
        if (FAILED(hr)) { err = HrText(L"нет корневой папки задач", hr); break; }
        if (FAILED(hr = svc->NewTask(0, &def))) { err = HrText(L"не создаётся задача", hr); break; }

        if (SUCCEEDED(def->get_RegistrationInfo(&info))) {
            BSTR a = B(L"ygg-watchdog"), d = B(L"Проверка пиров Yggdrasil раз в 3 минуты (ygg-watchdog)");
            info->put_Author(a); info->put_Description(d);
            SysFreeString(a); SysFreeString(d);
        }
        if (FAILED(hr = def->get_Principal(&pr))) { err = HrText(L"нет учётной записи задачи", hr); break; }
        { BSTR u = B(L"SYSTEM"); pr->put_UserId(u); SysFreeString(u); }
        pr->put_LogonType(TASK_LOGON_SERVICE_ACCOUNT);
        pr->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

        if (FAILED(hr = def->get_Settings(&ts))) { err = HrText(L"нет настроек задачи", hr); break; }
        ts->put_Enabled(VARIANT_TRUE);
        ts->put_StartWhenAvailable(VARIANT_TRUE);
        ts->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        ts->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        ts->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        { BSTR lim = B(L"PT10M"); ts->put_ExecutionTimeLimit(lim); SysFreeString(lim); }

        if (FAILED(hr = def->get_Triggers(&trs)) || FAILED(hr = trs->Create(TASK_TRIGGER_TIME, &tr))) { err = HrText(L"не создаётся расписание", hr); break; }
        {
            time_t now = time(nullptr);
            tm lt; localtime_s(&lt, &now);
            wchar_t sb[32];
            wcsftime(sb, 32, L"%Y-%m-%dT%H:%M:%S", &lt);
            BSTR start = B(sb);
            tr->put_StartBoundary(start);
            SysFreeString(start);
        }
        if (FAILED(hr = tr->get_Repetition(&rep))) { err = HrText(L"не задаётся повтор", hr); break; }
        { BSTR iv = B(L"PT3M"); rep->put_Interval(iv); SysFreeString(iv); }   // длительность не задана = бессрочно

        if (FAILED(hr = def->get_Actions(&acts)) || FAILED(hr = acts->Create(TASK_ACTION_EXEC, &act)) ||
            FAILED(hr = act->QueryInterface(ygg::kIidExecAction, (void**)&ex))) { err = HrText(L"не создаётся действие", hr); break; }
        {
            BSTR p = B(exe), a = B(args), wd = B(workDir.empty() ? util::DirOf(exe) : workDir);
            ex->put_Path(p); ex->put_Arguments(a); ex->put_WorkingDirectory(wd);
            SysFreeString(p); SysFreeString(a); SysFreeString(wd);
        }

        VARIANT user; VariantInit(&user);
        user.vt = VT_BSTR; user.bstrVal = B(L"SYSTEM");
        BSTR name = B(L"YggWatchdogTick");
        hr = root->RegisterTaskDefinition(name, def, TASK_CREATE_OR_UPDATE, user, empty, TASK_LOGON_SERVICE_ACCOUNT, empty, &reg);
        SysFreeString(name);
        VariantClear(&user);
        if (FAILED(hr)) { err = HrText(L"Планировщик не принял задачу", hr); break; }
        ok = true;
    } while (false);
    IUnknown* all[] = { reg, ex, act, acts, rep, tr, trs, ts, pr, info, def, root, svc };
    for (IUnknown* p : all) if (p) p->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

bool DeleteWatchdogTask(std::wstring& err) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    ITaskService* svc = nullptr; ITaskFolder* root = nullptr;
    VARIANT empty; VariantInit(&empty);
    HRESULT hr = CoCreateInstance(ygg::kClsidTaskScheduler, nullptr, CLSCTX_INPROC_SERVER, ygg::kIidTaskService, (void**)&svc);
    if (SUCCEEDED(hr)) hr = svc->Connect(empty, empty, empty, empty);
    if (SUCCEEDED(hr)) { BSTR rn = SysAllocString(L"\\"); hr = svc->GetFolder(rn, &root); SysFreeString(rn); }
    if (SUCCEEDED(hr)) { BSTR n = SysAllocString(L"YggWatchdogTick"); hr = root->DeleteTask(n, 0); SysFreeString(n); }
    ok = SUCCEEDED(hr);
    if (!ok) err = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? L"задачи и так нет" : HrText(L"не удалось", hr);
    if (root) root->Release();
    if (svc) svc->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}

void InstallScheduledTask() {
    // Страж битый или пропал - сначала пробуем достать целого из репозитория
    std::wstring why = CheckExeIntegrity(GuardExePath(), false);
    if (!why.empty()) {
        std::wstring e = RepairGuardFromRepo(why);
        if (!e.empty()) LogError(L"Страж ygg_guard.exe: " + why + L", " + e);
    }
    bool guard = CheckExeIntegrity(GuardExePath(), false).empty();
    std::wstring err;
    bool ok;
    if (guard) {
        // cmd /c ""страж" || "сторож" tick": страж не стартовал (битый файл) - cmd
        // запускает сторожа напрямую, а тот на этой же проверке чинит стража
        wchar_t sys[MAX_PATH] = L"";
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring cmd = std::wstring(sys) + L"\\cmd.exe";
        std::wstring args = L"/c \"\"" + GuardExePath() + L"\" || \"" + WatchdogExePath() + L"\" tick\"";
        ok = RegisterWatchdogTask(cmd, args, err, GetExeDir());
    } else {
        ok = RegisterWatchdogTask(WatchdogExePath(), L"tick", err);
    }
    if (!ok) {
        LogError(L"Задача не установлена: " + err);
        return;
    }
    util::RegWriteString(REG_WD_KEY, L"TaskUsesGuard", guard ? L"1" : L"0");
    util::RegWriteString(REG_WD_KEY, L"TaskDirectFallback", guard ? L"0" : L"1");
    if (guard) LogInfo(L"Задача YggWatchdogTick установлена: каждые 3 минуты, через стража (ygg_guard.exe); "
                       L"если страж не запустится - сторож напрямую.");
    else LogInfo(L"Задача YggWatchdogTick установлена: каждые 3 минуты, напрямую - целого стража ygg_guard.exe нет "
                 L"и скачать не вышло. Как только он появится, сторож сам вернёт задачу на стража.");
}

// Удаляет задачу: автоматические проверки прекращаются до следующей установки.
void UninstallScheduledTask() {
    std::wstring err;
    if (DeleteWatchdogTask(err)) {
        util::RegWriteString(REG_WD_KEY, L"TaskUsesGuard", L"0");
        util::RegWriteString(REG_WD_KEY, L"TaskDirectFallback", L"0");
        LogInfo(L"Задача удалена. Автоматические проверки остановлены - чтобы включить снова, установи задачу ещё раз.");
    } else {
        LogError(L"Задача не удалена: " + err);
    }
}


// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------
// ------------------------------------------------------------------
// Проверка и запрос прав администратора. Нужны для schtasks /RL HIGHEST
// (создать задачу с повышенными правами может только уже повышенный
// процесс) и для доступа к админ-сокету yggdrasilctl. При запуске из
// Планировщика (там задача и так помечена RL HIGHEST) процесс уже
// элевирован, поэтому повторный запрос тут не сработает - проверка
// IsElevated() в начале как раз это отличает и не тратит время зря.
// ------------------------------------------------------------------
bool IsElevated() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

// Консольные команды (tick, install-task, status-json и т.д.). Окно -
// в gui/ygg_watchdog_gui.cpp, точка входа wWinMain - в конце файла.
int WatchdogMain(int argc, wchar_t* argv[]) {
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-v" || a == L"--verbose") {
            g_verboseMode = true;
        } else {
            args.push_back(a);
        }
    }

    // Права администратора нужны для планировщика, службы и конфига.
    // status-json и справочники только читают - им права не нужны.
    // Окно и задача Планировщика и так работают с правами; из обычной
    // консоли перезапуск с UAC увёл бы вывод в никуда (exe оконный), поэтому
    // просто говорим, что нужна консоль администратора.
    bool readOnlyCommand = !args.empty() &&
        (args[0] == L"status-json" || args[0] == L"countries-json" || args[0] == L"cities-json" ||
         args[0] == L"diagnose" || args[0] == L"catalog-json" || args[0] == L"stats-json");
    if (!readOnlyCommand && !IsElevated()) {
        PrintError(L"нужны права администратора: запусти команду из консоли «от имени администратора».");
        return 1;
    }

    if (args.empty()) return 0;

    std::wstring command = args[0];

    if (command == L"tick") {
        Tick();
    } else if (command == L"install-task") {
        InstallScheduledTask();
    } else if (command == L"repair-task") {
        // из трея, когда автопроверки молчат: задача заново + сразу одна проверка
        InstallScheduledTask();
        AppendEvent(L"ok", L"Задача автопроверок восстановлена (по твоему подтверждению).");
        Tick();
    } else if (command == L"uninstall-task") {
        UninstallScheduledTask();
    } else if (command == L"add-region" || command == L"set-region") {
        if (args.size() < 2) {
            PrintError(L"использование: add-region <страна> [город]");
            return 1;
        }
        CmdSetRegion(args[1], args.size() >= 3 ? args[2] : L"");
    } else if (command == L"restore-config") {
        CmdRestoreConfig();
    } else if (command == L"remove-region") {
        RemoveRegion();
    } else if (command == L"list-countries") {
        CmdListCountries();
    } else if (command == L"list-cities") {
        if (args.size() < 2) {
            LogError(L"Использование: list-cities <страна>");
            return 1;
        }
        CmdListCities(args[1]);
    } else if (command == L"list-main-peers") {
        CmdListMainPeers();
    } else if (command == L"add-main-peer") {
        if (args.size() < 2) {
            LogError(L"Использование: add-main-peer <uri>");
            return 1;
        }
        CmdAddMainPeer(args[1]);
    } else if (command == L"remove-main-peer") {
        if (args.size() < 2) {
            LogError(L"Использование: remove-main-peer <номер>");
            return 1;
        }
        CmdRemoveMainPeer(args[1]);
    } else if (command == L"restart-yggdrasil") {
        RestartYggdrasilServiceNow();
    } else if (command == L"restart-yggdrasil-delayed") {
        int delay = 10;
        if (args.size() >= 2) {
            try { delay = std::stoi(args[1]); } catch (...) {}
        }
        RestartYggdrasilServiceDelayed(delay);
    } else if (command == L"status-json") {
        CmdStatusJson();
    } else if (command == L"diagnose") {
        std::vector<std::wstring> mp;
        auto cfgPath = FindConfigPath();
        if (cfgPath.has_value()) mp = ReadMainPeers(*cfgPath);
        Print(DiagnoseNowText(mp));
    } else if (command == L"stats-json") {
        CmdStatsJson();
    } else if (command == L"stats-csv") {
        CmdStatsCsv();
    } else if (command == L"countries-json") {
        CmdCountriesJson();
    } else if (command == L"verify-peer") {
        if (args.size() < 2) { LogError(L"Использование: verify-peer <uri>"); return 1; }
        CmdVerifyPeer(args[1]);
    } else if (command == L"catalog-json") {
        CmdCatalogJson(args.size() >= 2 ? args[1] : L"");
    } else if (command == L"cities-json") {
        CmdCitiesJson(args.size() >= 2 ? args[1] : L"");
    } else if (command == L"set-home-region") {
        CmdSetHomeRegion(args.size() >= 2 ? args[1] : L"", args.size() >= 3 ? args[2] : L"");
    } else if (command == L"list-devices") {
        CmdListDevices();
    } else if (command == L"add-device") {
        if (args.size() < 3) {
            PrintError(L"использование: add-device <имя> <адрес> [общий пир ...] (или --no-peers - убрать общие пиры)");
            return 1;
        }
        std::vector<std::wstring> peers;
        bool clear = false;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == L"--no-peers") clear = true;
            else for (const auto& p : SplitBy(args[i], L' ')) peers.push_back(p);   // из окна - одной строкой через пробел
        }
        CmdAddDevice(args[1], args[2], peers, clear || !peers.empty());
    } else if (command == L"diagnose-device") {
        CmdDiagnoseDevice(args.size() >= 2 ? args[1] : L"");
    } else if (command == L"remove-device") {
        if (args.size() < 2) {
            LogError(L"Использование: remove-device <имя>");
            return 1;
        }
        CmdRemoveDevice(args[1]);
    } else if (command == L"approve-vpn-command") {
        if (args.size() < 2) {
            LogError(L"Использование: approve-vpn-command <путь к скрипту>");
            return 1;
        }
        ApproveVpnCommand(args[1]);
    } else if (command == L"clear-vpn-command") {
        ClearVpnApproval();
    } else if (command == L"check-update") {
        auto info = CheckForUpdate();
        if (g_updateCheckError.empty()) {   // проверка прошла - запомнить итог для окна
            WatchdogState st = LoadState();
            st.updateNotifiedTag = info.has_value() ? info->tag : L"";
            st.updateLastCheckedAt = (long long)time(nullptr);
            SaveState(st);
        }
        if (!info.has_value() && !g_updateCheckError.empty()) {
            PrintError(L"не удалось проверить: " + g_updateCheckError + L" (у тебя " + CURRENT_VERSION + L").");
            return 1;
        } else if (!info.has_value()) {
            Print(L"В репозитории та же версия или старше (у тебя " + CURRENT_VERSION + L"). "
                  L"Если только что закоммитил - GitHub отдаёт новый файл через несколько минут.");
        } else {
            Print(L"В репозитории новая версия: " + info->tag + L" - можно нажать «Установить».");
        }
    } else if (command == L"apply-update") {
        std::wstring err = ApplyUpdate();
        if (!err.empty()) { PrintError(err); return 1; }
        Print(L"Обновлено.");
    } else if (command == L"test-notify") {
        // Для кнопки «Проверить» в окне: notify.conf пишет само окно,
        // токен через командную строку не передаётся.
        NotifyConfig config = LoadNotifyConfig();
        if (!config.valid) {
            AppendEvent(L"warn", L"Telegram: не настроен - в notify.conf нет BOT_TOKEN или CHAT_ID");
            PrintError(L"Telegram не настроен: в notify.conf нет BOT_TOKEN или CHAT_ID.");
            return 1;
        }
        g_telegramForceEvent = true;
        bool sent = SendTelegramNotification(L"Проверка связи: уведомления ygg-watchdog работают.");
        if (sent) AppendEvent(L"ok", L"Telegram: тестовое сообщение ушло (" + g_telegramLastVia + L")");
        if (config.problem.empty()) {
            Print(L"Пути до Telegram по очереди:");
            for (const auto& t : g_routeTrace) Print(L"  " + t);
        }
        if (sent) {
            Print(L"Тестовое сообщение отправлено в Telegram (" + g_telegramLastVia + L").");
        } else {
            bool access = g_telegramLastErr.rfind(L"не достучался", 0) == 0;
            PrintError(L"не отправилось: " + g_telegramLastErr +
                       (access ? L". Нужен VPN: в TUN-режиме или с локальным HTTP-прокси (порт http или mixed; сторож найдёт его сам, "
                                 L"или впиши PROXY=127.0.0.1:порт в notify.conf)." : L""));
            return 1;
        }
    } else {
        LogError(L"Неизвестная команда: " + command);
        return 1;
    }

    return 0;
}

// ------------------------------------------------------------------
// Точка входа. Программа одна: без параметров открывается окно, с
// параметром выполняется команда (так её зовут задача Планировщика и само
// окно). Exe собран как оконный (-mwindows), поэтому проверки по таймеру
// не мелькают консолью. Если команду запустили из cmd - пишем в него.
// ------------------------------------------------------------------
namespace gui { int Run(HINSTANCE hInst, int nCmdShow); namespace tray { int Run(HINSTANCE hInst); } }

static void UseParentConsoleIfNoOutput() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return; // pipe или файл - уже есть
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;                                // запущено не из консоли
    HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (con == INVALID_HANDLE_VALUE) return;
    SetStdHandle(STD_OUTPUT_HANDLE, con);
    SetStdHandle(STD_ERROR_HANDLE, con);
    WriteOut(L"\r\n");
}

// Оставляем свой путь в реестре (HKLM\SOFTWARE\Yozmor\ygg-watchdog), чтобы
// панель ygg-control находила сторожа, где бы он ни лежал. Без прав
// администратора записать нельзя - тогда молча пропускаем.
static void RegisterSelfInRegistry() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Yozmor\\ygg-watchdog", 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    wchar_t cur[MAX_PATH * 2] = L"";
    DWORD size = sizeof(cur);
    if (RegQueryValueExW(k, L"Path", nullptr, nullptr, (BYTE*)cur, &size) != ERROR_SUCCESS || _wcsicmp(cur, exe) != 0)
        RegSetValueExW(k, L"Path", 0, REG_SZ, (const BYTE*)exe, (DWORD)((wcslen(exe) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"Version", 0, REG_SZ, (const BYTE*)CURRENT_VERSION.c_str(), (DWORD)((CURRENT_VERSION.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

#ifndef YGG_WATCHDOG_NO_MAIN   // тесты и страж подключают этот файл со своей точкой входа
#include <d2d1.h>      // только для selftest: проверить, что графика окна создаётся
#include <dwrite.h>
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    // selftest - для обновления: exe целиком загрузился (все DLL нашлись), графика
    // окна создаётся; печатает свою версию. Ничего не меняет, сразу выходит.
    if (argc >= 2 && std::wstring(argv[1]) == L"selftest") {
        LocalFree(argv);
        ID2D1Factory* d2 = nullptr;
        IDWriteFactory* dw = nullptr;
        bool ok = SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2)) &&
                  SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw));
        if (d2) d2->Release();
        if (dw) dw->Release();
        if (!ok) return 1;
        std::string v = util::ToUtf8(CURRENT_VERSION) + "\n";   // скрипт и ApplyUpdate сверяют версию
        DWORD n = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), v.data(), (DWORD)v.size(), &n, nullptr);
        return 0;
    }
    RegisterSelfInRegistry();
    bool hasCommand = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a != L"-v" && a != L"--verbose") hasCommand = true;
    }
    int rc;
    if (argc >= 2 && std::wstring(argv[1]) == L"tray") {
        // значок в трее: без прав администратора, без консоли
        rc = gui::tray::Run(hInst);
    } else if (!hasCommand) {
        rc = gui::Run(hInst, nCmdShow);
    } else {
        UseParentConsoleIfNoOutput();
        rc = WatchdogMain(argc, argv);
    }
    LocalFree(argv);
    return rc;
}
#endif
