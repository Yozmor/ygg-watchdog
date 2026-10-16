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
#include <bcrypt.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

// ------------------------------------------------------------------
// НАСТРОЙКИ
// ------------------------------------------------------------------
static const wchar_t* YGGCTL_PATH = L"C:\\Program Files\\Yggdrasil\\yggdrasilctl.exe";
static const wchar_t* YGGCTL_ENDPOINT = L"-endpoint=tcp://127.0.0.1:9002";

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
std::optional<std::wstring> FindConfigPath() {
    for (const auto& candidate : CONFIG_CANDIDATES) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
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
std::vector<std::wstring> ReadMainPeers(const std::wstring& configPath) {
    std::wstring text = ReadFileUtf8(configPath);
    std::vector<std::wstring> peers;

    // Ищем блок Peers: [ ... ] - не парсим весь HJSON-конфиг целиком,
    // только этот один блок
    std::wregex blockRe(L"Peers\\s*:\\s*\\[([\\s\\S]*?)\\]");
    std::wsmatch blockMatch;
    if (!std::regex_search(text, blockMatch, blockRe)) {
        return peers;
    }

    std::wstring block = blockMatch[1].str();
    std::wregex uriRe(L"\"([^\"]+)\"");
    auto begin = std::wsregex_iterator(block.begin(), block.end(), uriRe);
    auto end = std::wsregex_iterator();
    for (auto it = begin; it != end; ++it) {
        peers.push_back((*it)[1].str());
    }
    return peers;
}

bool ModifyMainPeers(const std::wstring& configPath, std::function<void(std::vector<std::wstring>&)> mutator) {
    std::wstring text = ReadFileUtf8(configPath);
    std::wregex blockRe(L"(Peers\\s*:\\s*\\[)([\\s\\S]*?)(\\])");
    std::wsmatch m;
    if (!std::regex_search(text, m, blockRe)) {
        LogError(L"Не найден блок Peers: [...] в конфиге");
        return false;
    }

    std::wstring before = m.prefix().str();
    std::wstring openBracket = m[1].str();
    std::wstring block = m[2].str();
    std::wstring closeBracket = m[3].str();
    std::wstring after = m.suffix().str();

    std::wregex uriRe(L"\"([^\"]+)\"");
    std::vector<std::wstring> peers;
    auto begin = std::wsregex_iterator(block.begin(), block.end(), uriRe);
    auto end = std::wsregex_iterator();
    for (auto it = begin; it != end; ++it) peers.push_back((*it)[1].str());

    mutator(peers);

    std::wstring newBlock = L"\n";
    for (const auto& p : peers) {
        newBlock += L"  \"" + p + L"\",\n";
    }

    std::wstring newText = before + openBracket + newBlock + closeBracket + after;

    int size = WideCharToMultiByte(CP_UTF8, 0, newText.c_str(), (int)newText.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, newText.c_str(), (int)newText.size(), &utf8[0], size, nullptr, nullptr);
    std::ofstream out(std::filesystem::path(configPath), std::ios::trunc | std::ios::binary);
    if (!out.is_open()) {
        LogError(L"Не удалось открыть конфиг для записи (нет прав?): " + configPath);
        return false;
    }
    out.write(utf8.c_str(), utf8.size());
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
std::wstring RunProcessCapture(const std::wstring& exe, const std::wstring& args) {
    SECURITY_ATTRIBUTES saAttr = {};
    saAttr.nLength = sizeof(saAttr);
    saAttr.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &saAttr, 0)) return L"";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);

    std::string raw;
    if (ok) {
        char chunk[4096];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead > 0) {
            raw.append(chunk, bytesRead);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);

    if (raw.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), nullptr, 0);
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), &wide[0], size);
    return wide;
}

std::wstring RunYggCtl(const std::wstring& args) {
    return RunProcessCapture(YGGCTL_PATH, std::wstring(YGGCTL_ENDPOINT) + L" " + args);
}

void AddPeer(const std::wstring& uri) {
    LogDebug(L"addPeer " + uri);
    RunYggCtl(L"addPeer uri=" + uri);
}

void RemovePeer(const std::wstring& uri) {
    LogDebug(L"removePeer " + uri);
    RunYggCtl(L"removePeer uri=" + uri);
}

// {uri: state} из живого getPeers
std::vector<std::pair<std::wstring, std::wstring>> GetLivePeers() {
    std::wstring output = RunYggCtl(L"getPeers");
    LogDebug(L"Сырой вывод getPeers:\r\n" + output);

    std::vector<std::pair<std::wstring, std::wstring>> result;
    std::wregex uriRe(L"(tcp|tls|quic|ws|wss)://[^\\s\x2502]+:\\d+");
    std::wregex wordRe(L"[^\\s\x2502]+");

    // Построчно - тот же принцип, что уже проверен в панели управления:
    // надёжнее, чем сканировать весь текст одним regex-проходом, потому
    // что не зависит от того, как именно построчно разбит вывод.
    std::wstringstream ss(output);
    std::wstring line;
    while (std::getline(ss, line)) {
        std::wsmatch uriMatch;
        if (!std::regex_search(line, uriMatch, uriRe)) continue;

        std::wstring uri = uriMatch.str();
        std::wstring remainder = line.substr(uriMatch.position() + uriMatch.length());

        std::wsmatch wordMatch;
        if (std::regex_search(remainder, wordMatch, wordRe)) {
            result.push_back({ uri, wordMatch.str() });
        }
    }
    return result;
}

std::optional<std::wstring> GetPeerState(const std::wstring& uri,
                                          const std::vector<std::pair<std::wstring, std::wstring>>& live) {
    for (const auto& [u, state] : live) {
        if (u == uri) return state;
    }
    return std::nullopt;
}

bool PeerHasLastError(const std::wstring& uri);   // определена ниже, рядом с GetPeerDetails

bool VerifyPeerHandshake(const std::wstring& uri) {
    AddPeer(uri);
    auto start = std::chrono::steady_clock::now();
    bool success = false;

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < HANDSHAKE_TIMEOUT_MS) {
        auto live = GetLivePeers();
        auto state = GetPeerState(uri, live);
        if (state.has_value() && _wcsicmp(state->c_str(), L"Up") == 0) {
            success = true;
            break;
        }
        // Yggdrasil уже написал ошибку подключения (обрыв, отказ, таймаут) - ждать
        // остаток времени нет смысла, пир не поднимется
        if (PeerHasLastError(uri)) break;
        Sleep(HANDSHAKE_POLL_INTERVAL_MS);
    }

    RemovePeer(uri);
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
    bool valid = false; // именно для Telegram - нужны оба BOT_TOKEN/CHAT_ID
};

NotifyConfig LoadNotifyConfig() {
    NotifyConfig config;
    std::wstring text = ReadFileUtf8(GetExeDir() + L"notify.conf");
    if (text.empty()) return config;

    for (auto line : SplitBy(text, L'\n')) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring value = line.substr(eq + 1);
        if (key == L"BOT_TOKEN") config.botToken = value;
        else if (key == L"CHAT_ID") config.chatId = value;
        else if (key == L"PROXY") config.proxyOverride = value;
    }
    config.valid = !config.botToken.empty() && !config.chatId.empty();
    return config;
}


HINTERNET OpenHttpSessionWithProxy() {
    // Сначала - явный PROXY из notify.conf, если прописан. Это нужно
    // для задачи планировщика (работает от имени SYSTEM и не видит
    // системную настройку прокси текущего пользователя из HKCU -
    // у SYSTEM она своя, отдельная и обычно пустая).
    NotifyConfig notifyConfig = LoadNotifyConfig();
    if (!notifyConfig.proxyOverride.empty()) {
        HINTERNET hSession = WinHttpOpen(L"ygg-watchdog/1.0",
            WINHTTP_ACCESS_TYPE_NAMED_PROXY, notifyConfig.proxyOverride.c_str(),
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession) return hSession;
        LogDebug(L"Явный PROXY из notify.conf не удалось использовать, "
                 L"пробую системную настройку текущего пользователя");
    }

    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ieProxyConfig = {};
    HINTERNET hSession = nullptr;

    if (WinHttpGetIEProxyConfigForCurrentUser(&ieProxyConfig) && ieProxyConfig.lpszProxy) {
        hSession = WinHttpOpen(L"ygg-watchdog/1.0",
            WINHTTP_ACCESS_TYPE_NAMED_PROXY, ieProxyConfig.lpszProxy,
            ieProxyConfig.lpszProxyBypass ? ieProxyConfig.lpszProxyBypass : WINHTTP_NO_PROXY_BYPASS, 0);
    } else {
        hSession = WinHttpOpen(L"ygg-watchdog/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }

    if (ieProxyConfig.lpszAutoConfigUrl) GlobalFree(ieProxyConfig.lpszAutoConfigUrl);
    if (ieProxyConfig.lpszProxy) GlobalFree(ieProxyConfig.lpszProxy);
    if (ieProxyConfig.lpszProxyBypass) GlobalFree(ieProxyConfig.lpszProxyBypass);

    return hSession;
}

std::string HttpGetUtf8(const std::wstring& host, const std::wstring& path) {
    std::string result;

    HINTERNET hSession = OpenHttpSessionWithProxy();
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    // GitHub требует User-Agent, иначе отдаёт 403
    std::wstring headers = L"User-Agent: ygg-watchdog\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    bool received = sent && WinHttpReceiveResponse(hRequest, nullptr);
    DWORD statusCode = 0;
    if (received) {
        DWORD sz = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
    }
    if (received && statusCode != 200) {
        // 403/429 у api.github.com - обычно лимит запросов без авторизации (60 в час)
        LogError(L"HTTP " + std::to_wstring(statusCode) + L" от " + host + path);
    } else if (received) {
        DWORD bytesAvailable = 0;
        do {
            bytesAvailable = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
            if (bytesAvailable == 0) break;

            std::vector<char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead)) {
                result.append(buf.data(), bytesRead);
            }
        } while (bytesAvailable > 0);
    } else {
        LogError(L"HTTP-запрос не удался (WinHTTP " + std::to_wstring(GetLastError()) + L"): " + host + path);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
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

std::wstring HostFromUri(const std::wstring& uri) {
    std::wregex hostRe(L"://\\[?([^:/\\]]+)\\]?:\\d+");
    std::wsmatch m;
    if (std::regex_search(uri, m, hostRe)) return m[1].str();
    return L"";
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
        std::string body = HttpGetUtf8(p.host, p.path);
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
// Простое состояние - свой минимальный текстовый формат вместо JSON
// (не тащим JSON-библиотеку ради трёх полей)
//   mode=normal|regional
//   backup_peers=uri1;uri2;...
//   backup_region=russia
// ------------------------------------------------------------------
// Устройство для мониторинга доступности (Level 2) - только ноутбуки,
// добавляются/именуются руками через меню (не телефон, не автоматически).
struct Device {
    std::wstring name;
    std::wstring host;
};

struct WatchdogState {
    std::wstring mode = L"normal";
    std::vector<std::wstring> backupPeers;
    std::wstring backupRegion;
    std::vector<std::wstring> regionalPeers;
    std::wstring regionalCountry; // slug, например "russia"
    std::wstring regionalCity;    // например "Vladivostok"
    long long internetDownSince = 0; // unix-время, 0 = интернет не пропадал

    // Домашний регион (ручной, постоянный fallback) и динамический
    // egress-регион (автоопределение через GeoIP, кэш на час) - заменяют
    // старую эвристику "регион основных пиров", которая была неверной.
    std::wstring homeRegion;
    std::wstring homeCity;     // ascii-имя города из cities.dat (пусто = любой город страны)
    std::wstring egressRegion;
    long long egressRegionCheckedAt = 0;

    // Крайний перебор (последняя надежда, когда обычный поиск не
    // помог) - возобновляемый курсор (индекс страны + индекс пира
    // внутри страны), снимок списка стран на момент начала прохода.
    std::vector<std::wstring> crisisCountrySlugs;
    size_t crisisCountryIndex = 0;
    size_t crisisPeerIndex = 0;
    long long crisisLastFullScanCompletedAt = 0;

    // Устройства для мониторинга доступности и их текущее состояние
    std::vector<Device> devices;
    std::map<std::wstring, long long> deviceDownSince;
    std::map<std::wstring, long long> deviceEscalatedUntil;

    // Аварийный режим по критическому заряду батареи
    bool batteryCriticalMode = false;
    int batteryCriticalEnteredPercent = -1;

    // Проверка обновлений с GitHub (только уведомление)
    long long updateLastCheckedAt = 0;
    std::wstring updateNotifiedTag;

    // Итог последней проверки - для окна сторожа (status-json)
    long long lastTickAt = 0;
    bool outageNotified = false; // уже сообщили "все пиры легли, замены нет" в этом простое
    std::wstring lastPhase; // ok / backup / down / crisis / no_internet / battery / regional / regional_search / error

    // Разбор "почему не работают основные пиры": считается в конце проверки (уже после
    // поиска замены) и кладётся сюда, а status-json только отдаёт готовое - окно
    // опрашивает его каждые несколько секунд, лишних сетевых проверок там быть не должно.
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

bool HttpPostUtf8(const std::wstring& host, const std::wstring& path, const std::string& body) {
    bool ok = false;

    HINTERNET hSession = OpenHttpSessionWithProxy();
    if (!hSession) {
        LogDebug(L"HttpPostUtf8: WinHttpOpen не удался, код ошибки " + std::to_wstring(GetLastError()));
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        LogDebug(L"HttpPostUtf8: WinHttpConnect не удался, код ошибки " + std::to_wstring(GetLastError()));
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        LogDebug(L"HttpPostUtf8: WinHttpOpenRequest не удался, код ошибки " + std::to_wstring(GetLastError()));
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
        (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);

    if (!sent) {
        LogDebug(L"HttpPostUtf8: WinHttpSendRequest не удался, код ошибки " + std::to_wstring(GetLastError()));
    } else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        LogDebug(L"HttpPostUtf8: WinHttpReceiveResponse не удался, код ошибки " + std::to_wstring(GetLastError()));
    } else {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        ok = (statusCode == 200);

        if (!ok) {
            // Читаем тело ответа - Telegram обычно присылает понятное
            // текстовое описание причины (например "Unauthorized",
            // "chat not found") прямо в JSON, это и есть самая полезная
            // диагностика при неверном токене/chat_id.
            std::string responseBody;
            DWORD bytesAvailable = 0;
            do {
                bytesAvailable = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
                if (bytesAvailable == 0) break;
                std::vector<char> buf(bytesAvailable);
                DWORD bytesRead = 0;
                if (WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead)) {
                    responseBody.append(buf.data(), bytesRead);
                }
            } while (bytesAvailable > 0);

            int wsize = MultiByteToWideChar(CP_UTF8, 0, responseBody.c_str(), -1, nullptr, 0);
            std::wstring wResponseBody(wsize > 0 ? wsize : 1, 0);
            if (wsize > 0) {
                MultiByteToWideChar(CP_UTF8, 0, responseBody.c_str(), -1, &wResponseBody[0], wsize);
            }

            LogDebug(L"HttpPostUtf8: код ответа " + std::to_wstring(statusCode) +
                     L", тело ответа: " + wResponseBody);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
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

bool SendTelegramNotification(const std::wstring& message) {
    NotifyConfig config = LoadNotifyConfig();
    if (!config.valid) return false;

    std::wstring fullMessage = L"[" + GetDeviceName() + L"] " + message;
    std::string body = "chat_id=" + UrlEncodeUtf8(config.chatId) +
                        "&text=" + UrlEncodeUtf8(fullMessage);
    std::wstring path = L"/bot" + config.botToken + L"/sendMessage";

    bool ok = HttpPostUtf8(L"api.telegram.org", path, body);
    if (!ok) {
        LogDebug(L"Не удалось отправить уведомление в Telegram");
    }
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

std::vector<City> LoadCityDatabase() {
    std::vector<City> cities;
    std::wstring path = GetExeDir() + L"cities.dat";
    std::wifstream file{std::filesystem::path(path)};
    if (!file.is_open()) {
        LogError(L"Не найден cities.dat рядом с exe - региональный режим не сможет искать по координатам");
        return cities;
    }
    file.imbue(std::locale(file.getloc(), new std::codecvt_utf8<wchar_t>));

    std::wstring line;
    while (std::getline(file, line)) {
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
        // SplitBy пропускает пустые токены, поэтому если у города нет
        // русского названия, 6-го элемента просто не будет вообще -
        // это нормально, оставляем nameRu пустым в этом случае.
        if (parts.size() >= 6) {
            c.nameRu = parts[5];
        }
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



WatchdogState LoadState() {
    WatchdogState state;
    std::wstring text = ReadFileUtf8(StatePath());
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring value = line.substr(eq + 1);
        if (key == L"mode") state.mode = value;
        else if (key == L"backup_peers") state.backupPeers = SplitBy(value, L';');
        else if (key == L"backup_region") state.backupRegion = value;
        else if (key == L"regional_peers") state.regionalPeers = SplitBy(value, L';');
        else if (key == L"regional_country") state.regionalCountry = value;
        else if (key == L"regional_city") state.regionalCity = value;
        else if (key == L"internet_down_since") {
            try { state.internetDownSince = std::stoll(value); } catch (...) { state.internetDownSince = 0; }
        }
        else if (key == L"home_region") state.homeRegion = value;
        else if (key == L"home_city") state.homeCity = value;
        else if (key == L"egress_region") state.egressRegion = value;
        else if (key == L"egress_region_checked_at") {
            try { state.egressRegionCheckedAt = std::stoll(value); } catch (...) {}
        }
        else if (key == L"crisis_country_slugs") state.crisisCountrySlugs = SplitBy(value, L';');
        else if (key == L"crisis_country_index") {
            try { state.crisisCountryIndex = (size_t)std::stoull(value); } catch (...) {}
        }
        else if (key == L"crisis_peer_index") {
            try { state.crisisPeerIndex = (size_t)std::stoull(value); } catch (...) {}
        }
        else if (key == L"crisis_last_full_scan_completed_at") {
            try { state.crisisLastFullScanCompletedAt = std::stoll(value); } catch (...) {}
        }
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
        else if (key == L"device_down_since") {
            for (const auto& entry : SplitBy(value, L';')) {
                size_t colon = entry.find_last_of(L':');
                if (colon == std::wstring::npos) continue;
                try { state.deviceDownSince[entry.substr(0, colon)] = std::stoll(entry.substr(colon + 1)); } catch (...) {}
            }
        }
        else if (key == L"device_escalated_until") {
            for (const auto& entry : SplitBy(value, L';')) {
                size_t colon = entry.find_last_of(L':');
                if (colon == std::wstring::npos) continue;
                try { state.deviceEscalatedUntil[entry.substr(0, colon)] = std::stoll(entry.substr(colon + 1)); } catch (...) {}
            }
        }
        else if (key == L"battery_critical_mode") state.batteryCriticalMode = (value == L"1");
        else if (key == L"battery_critical_entered_percent") {
            try { state.batteryCriticalEnteredPercent = std::stoi(value); } catch (...) {}
        }
        else if (key == L"update_last_checked_at") {
            try { state.updateLastCheckedAt = std::stoll(value); } catch (...) {}
        }
        else if (key == L"update_notified_tag") state.updateNotifiedTag = value;
        else if (key == L"last_tick_at") {
            try { state.lastTickAt = std::stoll(value); } catch (...) {}
        }
        else if (key == L"last_phase") state.lastPhase = value;
        else if (key == L"outage_notified") state.outageNotified = (value == L"1");
        else if (key == L"diag_verdict") state.diagVerdict = value;
        else if (key == L"diag_advice") state.diagAdvice = value;
        else if (key == L"diag_any_up") state.diagAnyUp = (value == L"1");
        else if (key == L"diag_peer") state.diagPeers.push_back(value);
    }
    return state;
}

void SaveState(const WatchdogState& state) {
    std::wstring text = L"mode=" + state.mode + L"\r\n";
    std::wstring joined;
    for (size_t i = 0; i < state.backupPeers.size(); ++i) {
        if (i > 0) joined += L";";
        joined += state.backupPeers[i];
    }
    text += L"backup_peers=" + joined + L"\r\n";
    text += L"backup_region=" + state.backupRegion + L"\r\n";

    std::wstring regionalJoined;
    for (size_t i = 0; i < state.regionalPeers.size(); ++i) {
        if (i > 0) regionalJoined += L";";
        regionalJoined += state.regionalPeers[i];
    }
    text += L"regional_peers=" + regionalJoined + L"\r\n";
    text += L"regional_country=" + state.regionalCountry + L"\r\n";
    text += L"regional_city=" + state.regionalCity + L"\r\n";
    text += L"internet_down_since=" + std::to_wstring(state.internetDownSince) + L"\r\n";

    text += L"home_region=" + state.homeRegion + L"\r\n";
    text += L"home_city=" + state.homeCity + L"\r\n";
    text += L"egress_region=" + state.egressRegion + L"\r\n";
    text += L"egress_region_checked_at=" + std::to_wstring(state.egressRegionCheckedAt) + L"\r\n";

    std::wstring crisisSlugsJoined;
    for (size_t i = 0; i < state.crisisCountrySlugs.size(); ++i) {
        if (i > 0) crisisSlugsJoined += L";";
        crisisSlugsJoined += state.crisisCountrySlugs[i];
    }
    text += L"crisis_country_slugs=" + crisisSlugsJoined + L"\r\n";
    text += L"crisis_country_index=" + std::to_wstring(state.crisisCountryIndex) + L"\r\n";
    text += L"crisis_peer_index=" + std::to_wstring(state.crisisPeerIndex) + L"\r\n";
    text += L"crisis_last_full_scan_completed_at=" + std::to_wstring(state.crisisLastFullScanCompletedAt) + L"\r\n";

    std::wstring devicesJoined;
    for (size_t i = 0; i < state.devices.size(); ++i) {
        if (i > 0) devicesJoined += L";";
        devicesJoined += state.devices[i].name + L"," + state.devices[i].host;
    }
    text += L"devices=" + devicesJoined + L"\r\n";

    std::wstring downSinceJoined;
    bool first = true;
    for (const auto& [name, t] : state.deviceDownSince) {
        if (!first) downSinceJoined += L";";
        first = false;
        downSinceJoined += name + L":" + std::to_wstring(t);
    }
    text += L"device_down_since=" + downSinceJoined + L"\r\n";

    std::wstring escalatedJoined;
    first = true;
    for (const auto& [name, t] : state.deviceEscalatedUntil) {
        if (!first) escalatedJoined += L";";
        first = false;
        escalatedJoined += name + L":" + std::to_wstring(t);
    }
    text += L"device_escalated_until=" + escalatedJoined + L"\r\n";

    text += L"battery_critical_mode=" + std::wstring(state.batteryCriticalMode ? L"1" : L"0") + L"\r\n";
    text += L"battery_critical_entered_percent=" + std::to_wstring(state.batteryCriticalEnteredPercent) + L"\r\n";
    text += L"update_last_checked_at=" + std::to_wstring(state.updateLastCheckedAt) + L"\r\n";
    text += L"update_notified_tag=" + state.updateNotifiedTag + L"\r\n";
    text += L"last_tick_at=" + std::to_wstring(state.lastTickAt) + L"\r\n";
    text += L"last_phase=" + state.lastPhase + L"\r\n";
    text += L"outage_notified=" + std::wstring(state.outageNotified ? L"1" : L"0") + L"\r\n";
    text += L"diag_verdict=" + state.diagVerdict + L"\r\n";
    text += L"diag_advice=" + state.diagAdvice + L"\r\n";
    text += L"diag_any_up=" + std::wstring(state.diagAnyUp ? L"1" : L"0") + L"\r\n";
    for (const auto& dp : state.diagPeers) text += L"diag_peer=" + dp + L"\r\n";

    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], size, nullptr, nullptr);

    std::ofstream f(std::filesystem::path(StatePath()), std::ios::binary | std::ios::trunc);
    f.write(utf8.c_str(), utf8.size());
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

std::wstring GetBackupSearchRegion(WatchdogState& state, const std::vector<std::wstring>& mainPeers) {
    RefreshEgressRegion(state);
    if (!state.egressRegion.empty()) return state.egressRegion;
    if (!state.homeRegion.empty()) return state.homeRegion;
    return DeriveBackupRegion(mainPeers);
}

// ------------------------------------------------------------------
// Фаервол - программе НЕ дан доступ менять правила, она их только
// ЧИТАЕТ через netsh (стандартная утилита Windows, ничего ставить не
// нужно). Вызывается ПЕРЕД крайним перебором - он самый долгий и
// дорогой способ, и если дело в заблокированных исходящих - есть
// смысл сказать об этом сразу, а не после часов безрезультатного
// перебора.
// ------------------------------------------------------------------
std::optional<std::wstring> CheckFirewallDiagnostics() {
    // Правила, которые упоминают саму программу и блокируют её -
    // тоже только чтение, ничего не меняем.
    std::wstring rules = RunProcessCapture(L"netsh.exe", L"advfirewall firewall show rule name=all verbose");
    if (rules.empty()) {
        LogDebug(L"Фаервол: не удалось прочитать правила через netsh");
        return std::nullopt;
    }

    bool blockingRuleFound = false;
    std::wstring foundRuleName;
    std::wstring currentRuleName;
    bool currentIsBlock = false;
    bool currentMentionsProgram = false;
    auto flushRule = [&]() {
        if (currentIsBlock && currentMentionsProgram) {
            blockingRuleFound = true;
            foundRuleName = currentRuleName;
        }
    };

    std::wstringstream ss(rules);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (line.find(L"Rule Name:") != std::wstring::npos) {
            flushRule();
            currentRuleName = line;
            currentIsBlock = false;
            currentMentionsProgram = false;
            continue;
        }
        std::wstring lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if (lower.find(L"ygg") != std::wstring::npos) currentMentionsProgram = true;
        if (line.find(L"Action:") != std::wstring::npos && lower.find(L"block") != std::wstring::npos) {
            currentIsBlock = true;
        }
    }
    flushRule();

    if (blockingRuleFound) {
        return L"в правилах фаервола Windows нашлось блокирующее правило, упоминающее Yggdrasil/watchdog (" +
               foundRuleName + L") - проверь вручную (netsh advfirewall firewall show rule name=all).";
    }
    LogDebug(L"Фаервол: явных блокирующих правил, упоминающих Yggdrasil, не нашлось");
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
bool CrisisScanStep(WatchdogState& state, const std::vector<std::wstring>& excludeHosts) {
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
                    std::chrono::steady_clock::now() - start).count() >= CRISIS_SCAN_TIME_BUDGET_SECONDS) {
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
bool PingOnce(const std::wstring& host, int timeoutMs) {
    std::wstring output = RunProcessCapture(L"ping.exe",
        L"-n 1 -w " + std::to_wstring(timeoutMs) + L" " + host);
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

std::vector<std::pair<std::wstring, std::wstring>> CheckDevices(WatchdogState& state) {
    std::vector<std::pair<std::wstring, std::wstring>> events; // {kind, message}
    if (state.devices.empty()) return events;

    long long now = (long long)time(nullptr);

    for (const auto& dev : state.devices) {
        bool reachable = PingDeviceDebounced(dev.host);
        auto downIt = state.deviceDownSince.find(dev.name);
        bool wasDown = (downIt != state.deviceDownSince.end());

        if (reachable) {
            if (wasDown) {
                long long downtimeSec = now - downIt->second;
                state.deviceDownSince.erase(dev.name);
                state.deviceEscalatedUntil.erase(dev.name);
                events.push_back({ L"device_recovered",
                    L"Устройство '" + dev.name + L"' (" + dev.host + L") снова на связи. "
                    L"Было недоступно " + std::to_wstring(downtimeSec / 60) + L" мин " +
                    std::to_wstring(downtimeSec % 60) + L" сек." });
            }
            continue;
        }

        if (!wasDown) {
            state.deviceDownSince[dev.name] = now;
            events.push_back({ L"device_unreachable",
                L"Устройство '" + dev.name + L"' (" + dev.host + L") не отвечает." });
            continue; // эскалация - только если недоступность подтвердится на следующей проверке
        }

        auto escIt = state.deviceEscalatedUntil.find(dev.name);
        bool onCooldown = (escIt != state.deviceEscalatedUntil.end() && now < escIt->second);
        if (onCooldown) continue;

        state.deviceEscalatedUntil[dev.name] = now + DEVICE_ESCALATION_COOLDOWN_SECONDS;

        if (!IsYggdrasilResponsive()) {
            LogInfo(L"Устройство '" + dev.name + L"' недоступно, и сам Yggdrasil не отзывается - "
                    L"перезапускаю службу");
            events.push_back({ L"device_unreachable",
                L"Устройство '" + dev.name + L"' недоступно, Yggdrasil на этой машине перегружен/не отзывается - "
                L"перезапускаю службу." });
            RestartYggdrasilServiceNow();
        } else {
            LogDebug(L"Устройство '" + dev.name + L"' недоступно, но Yggdrasil отзывается нормально - "
                     L"похоже, проблема не в локальной нагрузке");
        }
    }

    return events;
}

void CmdAddDevice(const std::wstring& name, const std::wstring& host) {
    if (name.empty() || host.empty()) { PrintError(L"имя и адрес устройства не могут быть пустыми - устройство не добавлено."); return; }
    WatchdogState state = LoadState();
    for (auto& d : state.devices) {
        if (_wcsicmp(d.name.c_str(), name.c_str()) == 0) {
            d.host = host;
            SaveState(state);
            LogInfo(L"Устройство обновлено: " + name + L" -> " + host);
            return;
        }
    }
    state.devices.push_back({ name, host });
    SaveState(state);
    LogInfo(L"Устройство добавлено: " + name + L" (" + host + L")");
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
        Print(L"  " + d.name + L" (" + d.host + L")" + (down ? L" - НЕДОСТУПНО" : L" - ок"));
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
const std::wstring CURRENT_VERSION = L"1.1.0"; // поднимать вручную при выпуске новой версии
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

struct UpdateInfo {
    std::wstring tag;
    std::wstring url;
    std::wstring assetUrl; // прямая ссылка на ygg_watchdog.exe из релиза, если он там есть
};

std::optional<UpdateInfo> CheckForUpdate() {
    std::string body = HttpGetUtf8(GITHUB_API_HOST, L"/repos/Yozmor/ygg-watchdog/releases/latest");
    if (body.empty()) return std::nullopt;

    auto tagOpt = ExtractJsonStringField(body, "tag_name");
    if (!tagOpt.has_value() || tagOpt->empty()) return std::nullopt;
    if (VersionLessOrEqual(*tagOpt, CURRENT_VERSION)) return std::nullopt;

    UpdateInfo info;
    info.tag = *tagOpt;
    auto urlOpt = ExtractJsonStringField(body, "html_url");
    info.url = urlOpt.value_or(L"");

    std::regex assetRe("\"browser_download_url\"\\s*:\\s*\"([^\"]*ygg_watchdog\\.exe)\"");
    std::smatch m;
    if (std::regex_search(body, m, assetRe)) {
        std::string u = m[1].str();
        info.assetUrl = std::wstring(u.begin(), u.end());
    }
    return info;
}

void CheckForUpdateIfDue(WatchdogState& state) {
    long long now = (long long)time(nullptr);
    if (state.updateLastCheckedAt != 0 && (now - state.updateLastCheckedAt) < UPDATE_CHECK_INTERVAL_SECONDS) {
        return;
    }
    state.updateLastCheckedAt = now; // отмечаем попытку независимо от результата
    auto info = CheckForUpdate();
    if (!info.has_value()) return;
    if (state.updateNotifiedTag == info->tag) return; // уже уведомляли про эту версию

    state.updateNotifiedTag = info->tag;
    std::wstring msg = L"Доступна новая версия ygg-watchdog: " + info->tag +
        L" (текущая: " + CURRENT_VERSION + L"). Подробности: " + info->url +
        L". Чтобы обновиться - команда apply-update (или пункт меню).";
    LogInfo(msg);
    Notify(L"info", msg);
}

bool DownloadFile(const std::wstring& url, const std::wstring& destPath) {
    std::wregex urlRe(L"^https://([^/]+)(/.*)$");
    std::wsmatch m;
    if (!std::regex_match(url, m, urlRe)) return false;
    std::wstring host = m[1].str(), path = m[2].str();

    HINTERNET hSession = OpenHttpSessionWithProxy();
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    bool ok = false;
    if (hRequest) {
        std::wstring headers = L"User-Agent: ygg-watchdog\r\n";
        if (WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.size(),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr)) {
            std::ofstream out(std::filesystem::path(destPath), std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                DWORD avail = 0;
                do {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                    std::vector<char> buf(avail);
                    DWORD readBytes = 0;
                    if (WinHttpReadData(hRequest, buf.data(), avail, &readBytes)) {
                        out.write(buf.data(), readBytes);
                    }
                } while (avail > 0);
                ok = true;
            }
        }
        WinHttpCloseHandle(hRequest);
    }
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// Применяет обновление - скачивает новый .exe из релиза GitHub и
// подменяет СЕБЯ ЖЕ. Напрямую заменить работающий .exe на Windows
// нельзя (файл заблокирован, пока процесс запущен), поэтому: скачиваем
// во временный файл, самопроверка (что это непустой .exe с
// MZ-заголовком - аналог компиляционной самопроверки с Linux, так как
// здесь меняется готовый бинарник, а не исходники), затем короткоживущий
// .cmd-скрипт подождёт выхода этого процесса и переименует файлы (со
// старой версией, сохранённой как .backup - откат вручную, если новая
// версия вообще не запустится). Запускать ТОЛЬКО по явной команде.
void ApplyUpdate() {
    auto info = CheckForUpdate();
    if (!info.has_value()) {
        LogInfo(L"Новых версий не найдено - обновляться не с чего.");
        return;
    }
    if (info->assetUrl.empty()) {
        LogError(L"В релизе " + info->tag + L" не найден готовый ygg_watchdog.exe. "
                 L"Собери вручную из исходников релиза: " + info->url);
        return;
    }

    wchar_t exePathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    std::wstring exePath = exePathBuf;
    std::wstring newPath = exePath + L".new";
    std::wstring backupPath = exePath + L".backup";

    LogInfo(L"Скачиваю новую версию " + info->tag + L"...");
    if (!DownloadFile(info->assetUrl, newPath)) {
        LogError(L"Не удалось скачать новую версию");
        return;
    }

    {
        std::ifstream check(std::filesystem::path(newPath), std::ios::binary);
        char header[2] = {};
        check.read(header, 2);
        if (!check || header[0] != 'M' || header[1] != 'Z') {
            LogError(L"Скачанный файл не похож на рабочий .exe (нет MZ-заголовка) - отменяю обновление");
            DeleteFileW(newPath.c_str());
            return;
        }
    }

    std::wstring exeFileName = std::filesystem::path(exePath).filename().wstring();
    std::wstring batPath = GetExeDir() + L"apply_update.cmd";
    std::wstring batContent =
        L"@echo off\r\n"
        L":wait\r\n"
        L"tasklist /FI \"IMAGENAME eq " + exeFileName + L"\" 2>NUL | find /I \"" + exeFileName + L"\" >NUL\r\n"
        L"if not errorlevel 1 (timeout /T 1 >NUL & goto wait)\r\n"
        L"copy /Y \"" + exePath + L"\" \"" + backupPath + L"\" >NUL\r\n"
        L"move /Y \"" + newPath + L"\" \"" + exePath + L"\" >NUL\r\n"
        L"del \"%~f0\"\r\n";

    int size = WideCharToMultiByte(CP_UTF8, 0, batContent.c_str(), (int)batContent.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, batContent.c_str(), (int)batContent.size(), &utf8[0], size, nullptr, nullptr);
    std::ofstream bat(std::filesystem::path(batPath), std::ios::binary | std::ios::trunc);
    bat.write(utf8.c_str(), utf8.size());
    bat.close();

    LogInfo(L"Обновление скачано и проверено. Файл будет заменён в течение нескольких секунд после "
            L"завершения этого запуска (резервная копия текущей версии: " + backupPath + L") - "
            L"если новая версия вообще не запустится, переименуй " + backupPath + L" обратно в " +
            exeFileName + L" вручную.");

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring cmdLine = L"cmd.exe /c \"" + batPath + L"\"";
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        LogError(L"Не удалось запустить скрипт замены файла");
    }
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


void AddRegionWorker(const std::wstring& countryInput, const std::wstring& cityName) {
    LogInfo(L"=== Фоновый поиск региона: " + countryInput + L"/" + cityName + L" ===");

    // countryInput может быть номером из списка list-countries (обычный
    // путь через интерактивное меню) или сразу slug'ом (прямой вызов
    // из командной строки в обход меню) - пробуем оба варианта.
    std::wstring countrySlug = countryInput;
    try {
        size_t countryIdx = std::stoul(countryInput);
        auto countryList = GetSortedCountryList();
        if (countryIdx >= 1 && countryIdx <= countryList.size()) {
            countrySlug = countryList[countryIdx - 1].slug;
            LogDebug(L"Номер " + countryInput + L" разрешён в страну: " + countrySlug);
        }
    } catch (...) {
        // не число - используем countryInput как есть (уже slug)
    }

    auto isoOpt = SlugToIso(countrySlug);
    if (!isoOpt.has_value()) {
        LogError(L"Неизвестная страна (нет в таблице соответствия slug->ISO): " + countrySlug);
        return;
    }
    std::wstring iso = *isoOpt;

    auto cityDb = LoadCityDatabase();
    auto candidateCities = GetCitiesForCountry(cityDb, iso);

    std::optional<City> destCity;
    try {
        size_t idx = std::stoul(cityName);
        if (idx >= 1 && idx <= candidateCities.size()) {
            destCity = candidateCities[idx - 1];
        }
    } catch (...) {
        // Не число - пробуем как точное имя (для прямого вызова из
        // командной строки в обход интерактивного меню)
        destCity = FindCity(cityDb, iso, cityName);
    }

    if (!destCity.has_value()) {
        LogError(L"Не удалось определить город по вводу '" + cityName + L"'. "
                 L"Используй номер из списка (команда list-cities " + countrySlug + L").");
        return;
    }
    LogInfo(L"Целевой город: " + destCity->displayName() + L" (" + std::to_wstring(destCity->lat) +
            L", " + std::to_wstring(destCity->lon) + L")");

    auto peers = FetchCountryPeers(countrySlug);
    LogDebug(L"Кандидатов в стране '" + countrySlug + L"': " + std::to_wstring(peers.size()));

    std::vector<ScoredPeer> scored;
    for (const auto& p : peers) {
        auto peerCity = FindCity(cityDb, iso, p.city);
        double dist = peerCity.has_value()
            ? HaversineDistanceKm(destCity->lat, destCity->lon, peerCity->lat, peerCity->lon)
            : 1e9; // город пира не нашёлся в базе - не выбрасываем, просто в конец очереди
        scored.push_back({ p, dist });
    }
    std::sort(scored.begin(), scored.end(),
              [](const ScoredPeer& a, const ScoredPeer& b) { return a.distanceKm < b.distanceKm; });

    WatchdogState state = LoadState();

    for (const auto& sp : scored) {
        std::wstring distText = (sp.distanceKm >= 1e8) ? L"город не найден в базе"
                                                        : (std::to_wstring((long)sp.distanceKm) + L" км");
        LogInfo(L"Проверяю рукопожатие: " + sp.peer.uri + L" (" + sp.peer.city + L", " + distText + L")");

        if (VerifyPeerHandshake(sp.peer.uri)) {
            LogInfo(L"  Up - добавляю как региональный пир");
            AddPeer(sp.peer.uri);
            state.mode = L"regional";
            state.regionalPeers.push_back(sp.peer.uri);
            state.regionalCountry = countrySlug;
            state.regionalCity = cityName;
            SaveState(state);
            LogInfo(L"=== Региональный пир найден и добавлен ===");
            AppendEvent(L"info", L"Региональный пир (" + countrySlug + L"/" + destCity->name + L"): " +
                        sp.peer.uri + L" (" + sp.peer.city + L")");
            return;
        } else {
            LogInfo(L"  не поднялся за отведённое время, пробую следующего");
        }
    }

    LogError(L"Не удалось найти рабочего регионального пира для " + countrySlug + L"/" + cityName);
    AppendEvent(L"alert", L"Не удалось найти рабочего регионального пира для " + countrySlug + L"/" + destCity->name);
}

// Спавнит фоновый процесс (сам себя же с командой add-region-worker) и
// сразу возвращает управление - вызывающий (интерактивное меню) не ждёт.
void AddRegionForeground(const std::wstring& countrySlug, const std::wstring& cityName) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring cmdLine = L"\"" + std::wstring(exePath) + L"\" add-region-worker \"" +
                            countrySlug + L"\" \"" + cityName + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        LogInfo(L"Запущен фоновый поиск пиров для региона '" + countrySlug + L"/" + cityName + L"'");
        Print(L"Прогресс смотри в watchdog.log, обычно занимает от нескольких секунд "
              L"до пары минут в зависимости от числа кандидатов.");
    } else {
        LogError(L"Не удалось запустить фоновый процесс поиска региона");
    }
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

    auto db = LoadCityDatabase();
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
        auto db = LoadCityDatabase();
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

// Если резервные ищутся в стране домашнего региона и в нём задан город -
// сортируем кандидатов по расстоянию до этого города (ближайшие первыми),
// так же, как в региональном режиме. Пиры, чей город не найден в базе,
// уходят в конец очереди, но не выбрасываются.
void SortCandidatesByHomeCity(std::vector<PeerCandidate>& candidates, const WatchdogState& state,
                              const std::wstring& region) {
    if (state.homeCity.empty() || _wcsicmp(region.c_str(), state.homeRegion.c_str()) != 0) return;
    auto isoOpt = SlugToIso(region);
    if (!isoOpt.has_value()) return;
    auto db = LoadCityDatabase();
    auto home = FindCity(db, *isoOpt, state.homeCity);
    if (!home.has_value()) {
        LogDebug(L"Город домашнего региона '" + state.homeCity + L"' не найден в cities.dat - без сортировки");
        return;
    }
    std::vector<ScoredPeer> scored;
    for (const auto& p : candidates) {
        auto pc = FindCity(db, *isoOpt, p.city);
        double dist = pc.has_value() ? HaversineDistanceKm(home->lat, home->lon, pc->lat, pc->lon) : 1e9;
        scored.push_back({ p, dist });
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const ScoredPeer& a, const ScoredPeer& b) { return a.distanceKm < b.distanceKm; });
    candidates.clear();
    for (const auto& sp : scored) candidates.push_back(sp.peer);
    LogDebug(L"Кандидаты отсортированы по расстоянию до " + home->displayName());
}

void RemoveRegion() {
    WatchdogState state = LoadState();
    for (const auto& uri : state.regionalPeers) {
        RemovePeer(uri);
    }
    state.mode = L"normal";
    state.regionalPeers.clear();
    state.regionalCountry.clear();
    state.regionalCity.clear();
    SaveState(state);
    LogInfo(L"Региональный режим выключен, возврат к обычному слежению за основными пирами");
    AppendEvent(L"info", L"Региональный режим выключен вручную");
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
    std::wregex re(L"://(?:\\[[^\\]]+\\]|[^:/]+):(\\d+)");
    std::wsmatch m;
    if (std::regex_search(uri, m, re)) { try { return std::stoi(m[1].str()); } catch (...) {} }
    return 0;
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

void TickInner() {
    g_tickPhase = L"error";
    CleanOldLogEntries();
    LogInfo(L"=== Начало проверки ===");

    WatchdogState state = LoadState();

    // Аварийный режим по батарее отключает ВСЕ остальные проверки до
    // восстановления заряда - проверяется первым делом, раньше вообще
    // всего остального.
    if (!CheckBattery(state)) {
        SaveState(state);
        LogInfo(L"Аварийный режим по батарее активен - остальные проверки этого тика пропущены.");
        g_tickPhase = L"battery";
        return;
    }

    // Проверка обновлений - не чаще раза в сутки, только уведомление,
    // ничего не скачивает и не ломает текущую проверку при недоступности.
    CheckForUpdateIfDue(state);
    SaveState(state);

    // Если ранее уже зафиксировали пропажу интернета - проверяем в
    // первую очередь, независимо от режима/состояния пиров. Иначе
    // резервные могут сами восстановиться раньше, чем мы заметим
    // возврат интернета, и уведомление о восстановлении не придёт.
    if (state.internetDownSince != 0) {
        bool internetOk = CheckInternet(state);
        SaveState(state);
        if (!internetOk) {
            g_tickPhase = L"no_internet";
            return;
        }
    }

    auto live = GetLivePeers();
    for (const auto& [uri, s] : live) {
        LogDebug(L"Живой пир: " + uri + L" -> " + s);
    }

    auto isUp = [&](const std::wstring& uri) {
        auto s = GetPeerState(uri, live);
        return s.has_value() && _wcsicmp(s->c_str(), L"Up") == 0;
    };

    // Устройства (Level 2) проверяются ТОЛЬКО если основная связность
    // (Level 1) уже подтверждена живой - независимо от того, в какую
    // ветку (regional/normal) мы ниже свернём, поэтому считаем это
    // здесь же, один раз, до любого ветвления.
    if (!state.devices.empty()) {
        bool connectivityHealthy = false;
        if (state.mode == L"regional") {
            for (const auto& p : state.regionalPeers) {
                if (isUp(p)) { connectivityHealthy = true; break; }
            }
        } else {
            auto cfgPathForDevices = FindConfigPath();
            if (cfgPathForDevices.has_value()) {
                auto mp = ReadMainPeers(*cfgPathForDevices);
                for (const auto& p : mp) if (isUp(p)) { connectivityHealthy = true; break; }
                if (!connectivityHealthy) {
                    for (const auto& p : state.backupPeers) if (isUp(p)) { connectivityHealthy = true; break; }
                }
            }
        }
        if (connectivityHealthy) {
            auto deviceEvents = CheckDevices(state);
            for (const auto& [kind, msg] : deviceEvents) {
                LogInfo(msg);
                Notify(kind == L"device_recovered" ? L"ok" : L"alert", msg);
            }
            SaveState(state);
        }
    }

    // Правило 4: пока региональный режим активен - основные пиры вообще
    // не проверяются, следим только за региональными.
    if (state.mode == L"regional") {
        if (state.regionalPeers.empty()) {
            LogError(L"Режим 'regional' активен, но список региональных пиров пуст");
            return;
        }
        bool anyRegionalUp = false;
        for (const auto& p : state.regionalPeers) {
            if (isUp(p)) { anyRegionalUp = true; break; }
        }
        if (anyRegionalUp) {
            LogInfo(L"Проверка выполнена: региональный пир жив, без изменений");
            g_tickPhase = L"regional";
            return;
        }

        if (!CheckInternet(state)) {
            SaveState(state);
            g_tickPhase = L"no_internet";
            return;
        }
        g_tickPhase = L"regional_search";

        LogInfo(L"Все региональные пиры легли - запускаю фоновый поиск замены для " +
                state.regionalCountry + L"/" + state.regionalCity);
        AddRegionForeground(state.regionalCountry, state.regionalCity);
        return;
    }

    auto configPath = FindConfigPath();
    if (!configPath.has_value()) {
        LogError(L"Не найден yggdrasil.conf ни по одному известному пути");
        return;
    }

    auto mainPeers = ReadMainPeers(*configPath);
    LogDebug(L"Основных пиров в конфиге: " + std::to_wstring(mainPeers.size()));

    bool anyMainUp = false;
    for (const auto& p : mainPeers) {
        if (isUp(p)) { anyMainUp = true; break; }
    }

    if (anyMainUp) {
        if (!state.backupPeers.empty()) {
            LogInfo(L"Основной пир снова в строю - удаляю резервные (" +
                    std::to_wstring(state.backupPeers.size()) + L" шт.)");
            std::wstring removedList;
            for (size_t i = 0; i < state.backupPeers.size(); ++i) {
                if (i > 0) removedList += L", ";
                removedList += state.backupPeers[i];
            }
            Notify(L"ok", L"Основной пир снова в строю. Удалены резервные: " + removedList);
            for (const auto& uri : state.backupPeers) {
                RemovePeer(uri);
            }
            state.backupPeers.clear();
            state.backupRegion.clear();
            SaveState(state);
        } else {
            LogInfo(L"Проверка выполнена: основные пиры живы, без изменений");
        }
        if (state.outageNotified) { state.outageNotified = false; SaveState(state); }
        g_tickPhase = L"ok";
        return;
    }

    bool anyBackupUp = false;
    for (const auto& p : state.backupPeers) {
        if (isUp(p)) { anyBackupUp = true; break; }
    }
    // Мёртвые резервные не копим: убираем из Yggdrasil и из списка - иначе каждая
    // проверка добавляла бы нового, а старые так и висели бы (было "резервных: 11").
    {
        std::vector<std::wstring> keep;
        for (const auto& p : state.backupPeers) {
            if (isUp(p)) keep.push_back(p);
            else { LogInfo(L"Резервный не работает - убираю: " + p); RemovePeer(p); }
        }
        if (keep.size() != state.backupPeers.size()) { state.backupPeers = keep; SaveState(state); }
    }
    if (anyBackupUp) {
        LogInfo(L"Основные легли, но резервный держит связь - без изменений");
        if (state.outageNotified) { state.outageNotified = false; SaveState(state); }
        g_tickPhase = L"backup";
        return;
    }

    LogInfo(L"Все основные и резервные пиры легли - ищу замену");
    g_tickPhase = L"down";

    if (!CheckInternet(state)) {
        SaveState(state);
        g_tickPhase = L"no_internet";
        return;
    }

    if (state.backupRegion.empty()) {
        LogInfo(L"Регион для резервных пиров ещё не определён - определяю через egress-регион "
                L"(или домашний, если GeoIP недоступен)");
        state.backupRegion = GetBackupSearchRegion(state, mainPeers);
        if (state.backupRegion.empty()) {
            LogError(L"Не удалось определить регион ни через GeoIP, ни через домашний регион, "
                     L"ни по стране основных пиров - поиск резервного невозможен");
            if (!state.outageNotified) {
                state.outageNotified = true;
                SaveState(state);
                Notify(L"alert", L"Все пиры легли, а регион для поиска замены не определён "
                                 L"(GeoIP не ответил, домашний регион не задан). Задай домашний регион.\n\n"
                                 L"Разбор: " + DiagnoseNowText(mainPeers));
            }
            return;
        }
        LogInfo(L"Регион определён и сохранён: " + state.backupRegion);
        SaveState(state);
    }

    auto candidates = FetchCountryPeers(state.backupRegion);
    SortCandidatesByHomeCity(candidates, state, state.backupRegion);
    LogDebug(L"Кандидатов из репозитория для региона '" + state.backupRegion + L"': " +
             std::to_wstring(candidates.size()));
    size_t candidatesTotal = candidates.size();

    // Сначала быстро (параллельно, пара секунд) смотрим, у кого вообще открыт порт: у
    // остальных рукопожатие заведомо не пройдёт, а ждать его - до 15 с на каждого.
    // Порядок по близости к городу сохраняется; quic:// по TCP не проверить - идёт после.
    if (!candidates.empty()) {
        auto jobs = ProbePeers(candidates);
        std::vector<PeerCandidate> open, other;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (jobs[i].ms >= 0) open.push_back(candidates[i]);
            else if (jobs[i].err == L"udp") other.push_back(candidates[i]);
        }
        LogInfo(L"Порт отвечает у " + std::to_wstring(open.size()) + L" из " + std::to_wstring(candidates.size()) + L" пиров региона");
        candidates = open;
        candidates.insert(candidates.end(), other.begin(), other.end());
    }
    int triedThisTick = 0;

    for (const auto& candidate : candidates) {
        bool alreadyTried = false;
        for (const auto& p : mainPeers) if (p == candidate.uri) alreadyTried = true;
        for (const auto& p : state.backupPeers) if (p == candidate.uri) alreadyTried = true;
        if (alreadyTried) continue;

        if (++triedThisTick > 8) {
            LogInfo(L"Проверено восемь пиров за эту проверку - продолжу в следующую, чтобы не держать проверку долго.");
            break;
        }
        LogInfo(L"Проверяю рукопожатие: " + candidate.uri + L" (" + candidate.city + L")");
        if (VerifyPeerHandshake(candidate.uri)) {
            LogInfo(L"  Up - добавляю как резервного");
            std::wstring mainPeersList;
            for (size_t i = 0; i < mainPeers.size(); ++i) {
                if (i > 0) mainPeersList += L", ";
                mainPeersList += mainPeers[i];
            }
            Notify(L"warn", L"Все основные пиры (" + mainPeersList + L") легли. "
                                      L"Добавлен резервный (" + state.backupRegion + L"): " +
                                      candidate.uri + L" (" + candidate.city + L")");
            AddPeer(candidate.uri);
            state.backupPeers.push_back(candidate.uri);
            SaveState(state);
            g_tickPhase = L"backup";
            return;
        } else {
            LogInfo(L"  не поднялся за отведённое время, пробую следующего");
        }
    }

    LogError(L"Не удалось найти рабочего резервного пира для региона '" + state.backupRegion + L"'.");
    if (!state.outageNotified) {
        state.outageNotified = true;
        SaveState(state);
        std::wstring why = candidatesTotal == 0
            ? L"база пиров недоступна (GitHub не отвечает, сохранённой базы нет)"
            : L"ни один из " + std::to_wstring(candidatesTotal) + L" пиров региона '" + state.backupRegion + L"' не поднялся";
        Notify(L"alert", L"Все пиры легли (основные и резервные), замену найти не удалось: " + why +
                         L". Дальше - крайний перебор.\n\nРазбор: " + DiagnoseNowText(mainPeers));
    }

    // VPN: продвинутый вариант (одобренный скрипт) - пробуем перезапустить
    // перед крайней мерой (крайним перебором); если скрипт не одобрен - только намёк в лог.
    if (!RunApprovedVpnCommand(L"restart")) {
        WarnVpnMayBeNeeded();
    }

    // Фаервол - только читаем правила (сама программа их не меняет),
    // перед самым долгим и дорогим способом - крайним перебором. Если
    // похоже на блокировку - сообщаем СРАЗУ, а не после часов
    // безрезультатного перебора.
    auto firewallNote = CheckFirewallDiagnostics();
    if (firewallNote.has_value()) {
        LogError(L"Фаервол: " + *firewallNote);
        Notify(L"alert", L"Возможная причина проблемы: " + *firewallNote);
    }

    // Крайняя мера, последняя надежда - перебор ВСЕХ пиров по всем
    // странам (ограничен по времени за тик, возобновляемый через курсор).
    std::vector<std::wstring> excludeHosts;
    for (const auto& p : mainPeers) excludeHosts.push_back(HostFromUri(p));
    for (const auto& p : state.backupPeers) excludeHosts.push_back(HostFromUri(p));
    bool found = CrisisScanStep(state, excludeHosts);
    SaveState(state);
    g_tickPhase = found ? L"backup" : L"crisis";
}

// Обёртка: сама проверка + запись итога (время, фаза) в state.txt и
// строки истории в history.log - для окна сторожа.
void RecordHistory(const std::wstring& phase);

void UpdateDiagnosisState(WatchdogState& st, const std::wstring& phase);   // ниже, рядом с Diagnose

void Tick() {
    // Одна проверка за раз: окно, Планировщик и консоль могут запустить её одновременно.
    // Вторая не должна лезть в те же пиры и state.txt - просто пропускает ход.
    HANDLE lockH = CreateMutexW(nullptr, TRUE, L"Global\\YggWatchdogTickLock");
    if (lockH && GetLastError() == ERROR_ALREADY_EXISTS) {
        LogInfo(L"Предыдущая проверка ещё идёт - эту пропускаю.");
        CloseHandle(lockH);
        return;
    }
    TickInner();
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
struct PeerDetail {
    std::wstring uri;
    bool up = false;
    double rttMs = -1;      // -1 = неизвестно
    std::wstring lastError; // "last_error" из getPeers - почему пир не поднимается
};

// getPeers в JSON (yggdrasilctl -json, Yggdrasil 0.5.x): массив "peers"
// из плоских объектов; "latency" - в НАНОСЕКУНДАХ, есть только у Up-пиров.
std::vector<PeerDetail> GetPeerDetails() {
    std::vector<PeerDetail> result;
    std::wstring out = RunYggCtl(L"-json getPeers");
    std::wregex objRe(L"\\{[^{}]*\\}");
    std::wregex remoteRe(L"\"remote\"\\s*:\\s*\"([^\"]*)\"");
    std::wregex upRe(L"\"up\"\\s*:\\s*(true|false)");
    std::wregex latRe(L"\"latency\"\\s*:\\s*([0-9.eE+]+)");
    std::wregex errRe(L"\"last_error\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
    for (auto it = std::wsregex_iterator(out.begin(), out.end(), objRe); it != std::wsregex_iterator(); ++it) {
        std::wstring obj = it->str();
        std::wsmatch m;
        if (!std::regex_search(obj, m, remoteRe)) continue;
        PeerDetail d;
        d.uri = m[1].str();
        if (std::regex_search(obj, m, upRe)) d.up = (m[1].str() == L"true");
        if (d.up && std::regex_search(obj, m, latRe)) {
            try { d.rttMs = std::stod(m[1].str()) / 1e6; } catch (...) {}
        }
        if (std::regex_search(obj, m, errRe)) d.lastError = m[1].str();
        result.push_back(d);
    }
    return result;
}


bool PeerHasLastError(const std::wstring& uri) {
    for (const auto& d : GetPeerDetails())
        if (_wcsicmp(d.uri.c_str(), uri.c_str()) == 0) return !d.up && !d.lastError.empty();
    return false;
}

// ------------------------------------------------------------------
// Разбор "почему нет связи": по last_error каждого основного пира
// (что сказал сам Yggdrasil) плюс пара быстрых проверок - есть ли
// интернет, работает ли DNS. Только чтение, ничего не меняет.
// ------------------------------------------------------------------
struct PeerDiag {
    std::wstring uri;
    std::wstring kind;   // up / dns / eof / timeout / refused / unreachable / tls / waiting / absent / other
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
    int nDns = 0, nEof = 0, nTimeout = 0, nRefused = 0, nUnreach = 0, nTls = 0, nOther = 0, nWait = 0, nAbsent = 0;
    for (const auto& uri : mainPeers) {
        PeerDiag pd;
        pd.uri = uri;
        const PeerDetail* found = nullptr;
        for (const auto& d : details) if (_wcsicmp(d.uri.c_str(), uri.c_str()) == 0) { found = &d; break; }
        if (!found) { pd.kind = L"absent"; pd.text = L"Yggdrasil не знает этот пир (служба не перечитала конфиг?)"; ++nAbsent; }
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
        int dead = nDns + nEof + nTimeout + nRefused + nUnreach + nTls + nOther + nAbsent;
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
    dg.verdict = L"Все основные пиры не работают, причины разные: " + parts + L".";
    dg.advice = L"Замени мёртвые пиры на живые из каталога.";
    return dg;
}

std::wstring DiagnosisShort(const Diagnosis& dg);

// Кладёт разбор в state (или очищает, если основные пиры в порядке или не проверяются)
void UpdateDiagnosisState(WatchdogState& st, const std::wstring& phase) {
    st.diagVerdict.clear(); st.diagAdvice.clear(); st.diagPeers.clear(); st.diagAnyUp = false;
    bool mainDown = phase == L"backup" || phase == L"down" || phase == L"crisis" || phase == L"no_internet";
    if (!mainDown) return;
    std::vector<std::wstring> mp;
    auto cfg = FindConfigPath();
    if (cfg.has_value()) mp = ReadMainPeers(*cfg);
    Diagnosis dg = Diagnose(mp, GetPeerDetails(), phase == L"no_internet" ? 0 : 1);
    auto clean = [](std::wstring t) { for (auto& c : t) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' '; return t; };
    st.diagVerdict = clean(dg.verdict);
    st.diagAdvice = clean(dg.advice);
    st.diagAnyUp = dg.anyUp;
    for (const auto& p : dg.peers)
        st.diagPeers.push_back(clean(p.uri) + L"\t" + clean(p.kind) + L"\t" + clean(p.text) + L"\t" + clean(p.error));
}

std::wstring DiagnoseNowText(const std::vector<std::wstring>& mainPeers) {
    return DiagnosisShort(Diagnose(mainPeers, GetPeerDetails()));
}

std::wstring DiagnosisShort(const Diagnosis& dg) {
    std::wstring s = dg.verdict;
    for (const auto& p : dg.peers) if (p.kind != L"up") s += L"\n- " + HostFromUri(p.uri) + L": " + p.text;
    if (!dg.advice.empty()) s += L"\n" + dg.advice;
    return s;
}

const long long HISTORY_RETENTION_SECONDS = 48 * 3600;

void PruneHistoryIfLarge() {
    std::error_code ec;
    auto size = std::filesystem::file_size(std::filesystem::path(HistoryPath()), ec);
    if (ec || size < 400 * 1024) return;
    std::wstring content = ReadFileUtf8(HistoryPath());
    long long cutoff = (long long)time(nullptr) - HISTORY_RETENTION_SECONDS;
    std::wstring kept;
    for (auto line : SplitBy(content, L'\n')) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        long long ts = 0;
        try { ts = std::stoll(line.substr(0, line.find(L'\t'))); } catch (...) { continue; }
        if (ts >= cutoff) kept += line + L"\r\n";
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, kept.c_str(), (int)kept.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, kept.c_str(), (int)kept.size(), &utf8[0], n, nullptr, nullptr);
    std::ofstream out(std::filesystem::path(HistoryPath()), std::ios::trunc | std::ios::binary);
    if (out.is_open()) out.write(utf8.c_str(), utf8.size());
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
    if (cfg.has_value()) mainPeers = ReadMainPeers(*cfg);
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
    j += L",\"mode\":" + JStr(st.mode);
    j += L",\"config_found\":" + std::wstring(cfg.has_value() ? L"true" : L"false");
    j += L",\"main_peers\":" + JArr(mainPeers);
    j += L",\"backup_peers\":" + JArr(st.backupPeers);
    j += L",\"backup_region\":" + JStr(st.backupRegion);
    j += L",\"regional_peers\":" + JArr(st.regionalPeers);
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
        j += L"{\"name\":" + JStr(d.name) + L",\"host\":" + JStr(d.host) +
             L",\"down_since\":" + std::to_wstring(it == st.deviceDownSince.end() ? 0 : it->second) + L"}";
    }
    j += L"]";
    j += L",\"battery\":";
    if (bat.has_value()) {
        j += L"{\"percent\":" + std::to_wstring(bat->percent) + L",\"charging\":" + (bat->charging ? L"true" : L"false") + L"}";
    } else {
        j += L"null";
    }
    j += L",\"battery_critical_mode\":" + std::wstring(st.batteryCriticalMode ? L"true" : L"false");
    j += L",\"update_available\":" + JStr(st.updateNotifiedTag);
    j += L",\"vpn_script_approved\":" + std::wstring(vpnApproved ? L"true" : L"false");
    j += L",\"telegram_configured\":" + std::wstring(notifyOn ? L"true" : L"false");
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
             L",\"err\":" + JStr(jobs[i].err) + L"}";
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
    if (!alreadyThere) RemovePeer(uri);
    std::wstringstream msS;
    msS << std::fixed << std::setprecision(1) << ms;
    PeerDiag pd;
    pd.error = err;
    if (!up) ClassifyPeerError(pd);
    WriteOut(L"{\"uri\":" + JStr(uri) + L",\"up\":" + (up ? L"true" : L"false") + L",\"ms\":" + (ms >= 0 ? msS.str() : std::wstring(L"-1")) +
             L",\"error\":" + JStr(err) + L",\"text\":" + JStr(up ? L"" : pd.text) +
             L",\"was_configured\":" + (alreadyThere ? L"true" : L"false") + L"}\n");
}

void CmdCitiesJson(const std::wstring& countrySlug) {
    std::wstring j = L"[";
    auto iso = SlugToIso(countrySlug);
    if (iso.has_value()) {
        auto db = LoadCityDatabase();
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
    RunSimpleCommand(L"net stop \"" + std::wstring(YGG_SERVICE_NAME) + L"\"");
    RunSimpleCommand(L"net start \"" + std::wstring(YGG_SERVICE_NAME) + L"\"");
    LogInfo(L"Служба Yggdrasil перезапущена вручную.");
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

void InstallScheduledTask() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring cmd = L"schtasks /Create /TN \"YggWatchdogTick\" /TR \"\\\"" +
        std::wstring(exePath) + L"\\\" tick\" /SC MINUTE /MO 3 /RL HIGHEST /RU SYSTEM /F";

    Print(L"Регистрирую задачу планировщика...");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (exitCode == 0) {
            LogInfo(L"Задача зарегистрирована успешно.");

            // schtasks не даёт настроить "выполнить как можно скорее после
            // пропущенного запуска" напрямую - добавляем через PowerShell.
            // Без этого после сна/выключения проверка произойдёт только
            // на ближайшем 3-минутном тике, а не сразу при пробуждении.
            //
            // Там же отключаем DisallowStartIfOnBatteries и
            // StopIfGoingOnBatteries - по умолчанию schtasks создаёт задачу
            // с ОБОИМИ этими флагами включёнными (= True), из-за чего на
            // ноутбуке, работающем от батареи, проверки либо не запускались
            // вовсе, либо прерывались посередине (подтверждено реальным
            // инцидентом: окно ~90 минут без единой проверки при работе от
            // аккумулятора, Get-ScheduledTask показал DisallowStartIfOnBatteries
            // = True, StopIfGoingOnBatteries = True). Watchdog должен
            // работать одинаково от сети и от батареи.
            std::wstring psCmd =
                L"powershell -NoProfile -Command "
                L"\"$t = Get-ScheduledTask -TaskName 'YggWatchdogTick'; "
                L"$t.Settings.StartWhenAvailable = $true; "
                L"$t.Settings.DisallowStartIfOnBatteries = $false; "
                L"$t.Settings.StopIfGoingOnBatteries = $false; "
                // Зависшая проверка не должна блокировать следующие: по
                // умолчанию Планировщик не запускает новый экземпляр, пока
                // жив старый, а лимит у schtasks - 72 часа. Одна проверка
                // (с крайним перебором, у него свой бюджет 90 с) укладывается
                // в минуты - 10 минут с запасом.
                L"$t.Settings.ExecutionTimeLimit = 'PT10M'; "
                L"Set-ScheduledTask -InputObject $t\"";

            STARTUPINFOW psSi = { sizeof(psSi) };
            PROCESS_INFORMATION psPi = {};
            std::vector<wchar_t> psBuf(psCmd.begin(), psCmd.end());
            psBuf.push_back(L'\0');
            if (CreateProcessW(nullptr, psBuf.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &psSi, &psPi)) {
                WaitForSingleObject(psPi.hProcess, INFINITE);
                CloseHandle(psPi.hProcess);
                CloseHandle(psPi.hThread);
                LogInfo(L"Настроено: проверка выполнится сразу при пробуждении "
                        L"(StartWhenAvailable) и НЕ будет пропускаться/прерываться "
                        L"из-за работы от батареи (DisallowStartIfOnBatteries и "
                        L"StopIfGoingOnBatteries отключены).");
            } else {
                LogError(L"Не удалось настроить StartWhenAvailable/battery-параметры "
                         L"через PowerShell (не критично - задача всё равно "
                         L"работает, просто без этих опций - проверь вручную "
                         L"через Get-ScheduledTask, если ноутбук работает от батареи).");
            }
        } else {
            LogError(L"schtasks вернул код ошибки: " + std::to_wstring(exitCode));
        }
    } else {
        LogError(L"Не удалось запустить schtasks.exe");
    }
}

// Полностью удаляет задачу планировщика "YggWatchdogTick" - после этого
// автоматические проверки (каждые 3 минуты) прекращаются до следующей
// установки через install-task / пункт меню. Используется, когда нужно
// временно или навсегда остановить watchdog (например, перед плановыми
// работами с Yggdrasil, или если решили больше не использовать прогу).
void UninstallScheduledTask() {
    std::wstring cmd = L"schtasks /Delete /TN \"YggWatchdogTick\" /F";

    Print(L"Удаляю задачу планировщика (останавливаю автоматические проверки)...");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (exitCode == 0) {
            LogInfo(L"Задача удалена. Автоматические проверки остановлены - "
                    L"чтобы включить снова, выбери установку задачи ещё раз.");
        } else {
            LogError(L"schtasks вернул код ошибки при удалении: " + std::to_wstring(exitCode) +
                      L" (возможно, задача уже не была установлена).");
        }
    } else {
        LogError(L"Не удалось запустить schtasks.exe");
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
         args[0] == L"diagnose" || args[0] == L"catalog-json");
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
    } else if (command == L"uninstall-task") {
        UninstallScheduledTask();
    } else if (command == L"add-region") {
        if (args.size() < 3) {
            LogError(L"Использование: add-region <страна> <город>");
            return 1;
        }
        AddRegionForeground(args[1], args[2]);
    } else if (command == L"add-region-worker") {
        if (args.size() < 3) {
            LogError(L"Использование (внутренняя команда): add-region-worker <страна> <город>");
            return 1;
        }
        AddRegionWorker(args[1], args[2]);
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
            LogError(L"Использование: add-device <имя> <адрес>");
            return 1;
        }
        CmdAddDevice(args[1], args[2]);
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
        if (!info.has_value()) {
            Print(L"Новых версий не найдено (текущая: " + CURRENT_VERSION + L").");
        } else {
            Print(L"Доступна новая версия: " + info->tag + L". " + info->url);
        }
    } else if (command == L"apply-update") {
        ApplyUpdate();
    } else if (command == L"test-notify") {
        // Для кнопки «Проверить» в окне: notify.conf пишет само окно,
        // токен через командную строку не передаётся.
        NotifyConfig config = LoadNotifyConfig();
        if (!config.valid) {
            PrintError(L"Telegram не настроен: в notify.conf нет BOT_TOKEN или CHAT_ID.");
            return 1;
        }
        if (SendTelegramNotification(L"Проверка связи: уведомления ygg-watchdog работают.")) {
            Print(L"Тестовое сообщение отправлено в Telegram.");
        } else {
            PrintError(L"Не удалось отправить в Telegram. Проверь токен и chat id; если Telegram "
                       L"не открывается без VPN - нужен VPN или строка PROXY в notify.conf.");
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    RegisterSelfInRegistry();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
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
