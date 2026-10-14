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

void Log(const std::wstring& level, const std::wstring& message) {
    // DEBUG полностью скрыт по умолчанию (ни в консоль, ни в файл) -
    // только сама суть событий (что произошло, какие решения приняты).
    // Включается флагом -v для отладки, когда реально нужно видеть
    // сырой вывод getPeers и состояние каждого пира по отдельности.
    if (level == L"DEBUG" && !g_verboseMode) {
        return;
    }

    std::wstring line = L"[" + NowTimestamp() + L"] " + level + L": " + message;

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    std::wstring withNewline = line + L"\r\n";
    DWORD written = 0;
    WriteConsoleW(hOut, withNewline.c_str(), (DWORD)withNewline.size(), &written, nullptr);

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
    LogInfo(L"Основные пиры (" + std::to_wstring(peers.size()) + L"):");
    for (size_t i = 0; i < peers.size(); ++i) {
        LogInfo(L"  " + std::to_wstring(i + 1) + L") " + peers[i]);
    }
}

void CmdAddMainPeer(const std::wstring& uri) {
    auto configPath = FindConfigPath();
    if (!configPath.has_value()) {
        LogError(L"Не найден yggdrasil.conf");
        return;
    }
    bool ok = ModifyMainPeers(*configPath, [&](std::vector<std::wstring>& peers) {
        peers.push_back(uri);
    });
    if (ok) {
        LogInfo(L"Пир добавлен в конфиг: " + uri);
        LogInfo(L"Изменения вступят в силу ТОЛЬКО после перезапуска службы Yggdrasil.");
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
        LogError(L"Нужен номер пира из списка (команда list-main-peers)");
        return;
    }
    if (idx < 1 || idx > currentPeers.size()) {
        LogError(L"Нет пира с номером " + indexStr);
        return;
    }
    std::wstring toRemove = currentPeers[idx - 1];

    bool ok = ModifyMainPeers(*configPath, [&](std::vector<std::wstring>& peers) {
        peers.erase(std::remove(peers.begin(), peers.end(), toRemove), peers.end());
    });
    if (ok) {
        LogInfo(L"Пир удалён из конфига: " + toRemove);
        LogInfo(L"Изменения вступят в силу ТОЛЬКО после перезапуска службы Yggdrasil.");
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

    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
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
        LogError(L"HTTP-запрос не удался: " + host + path);
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

std::vector<CountryFile> ListCountryFiles() {
    std::string response = HttpGetUtf8(GITHUB_API_HOST, GITHUB_TREE_PATH);
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
        LogError(L"Страна не найдена в репозитории: " + countrySlug);
        return result;
    }

    std::string raw = [&]() {
        std::string r;
        // raw.githubusercontent.com не требует авторизации/User-Agent так же
        // жёстко, но всё равно передаём на всякий случай
        HINTERNET hSession = OpenHttpSessionWithProxy();
        if (!hSession) return r;
        HINTERNET hConnect = WinHttpConnect(hSession, GITHUB_RAW_HOST.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return r; }
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", targetPath.c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hRequest && WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD avail = 0;
            do {
                avail = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                std::vector<char> buf(avail);
                DWORD readBytes = 0;
                if (WinHttpReadData(hRequest, buf.data(), avail, &readBytes)) r.append(buf.data(), readBytes);
            } while (avail > 0);
        }
        if (hRequest) WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return r;
    }();

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
    std::wstring egressRegion;
    long long egressRegionCheckedAt = 0;

    // Аварийный (SOS) перебор ВСЕХ пиров по всем странам - возобновляемый
    // курсор (индекс страны + индекс пира внутри страны), снимок списка
    // стран на момент начала текущего прохода.
    std::vector<std::wstring> sosCountrySlugs;
    size_t sosCountryIndex = 0;
    size_t sosPeerIndex = 0;
    long long sosLastFullScanCompletedAt = 0;

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
        else if (key == L"egress_region") state.egressRegion = value;
        else if (key == L"egress_region_checked_at") {
            try { state.egressRegionCheckedAt = std::stoll(value); } catch (...) {}
        }
        else if (key == L"sos_country_slugs") state.sosCountrySlugs = SplitBy(value, L';');
        else if (key == L"sos_country_index") {
            try { state.sosCountryIndex = (size_t)std::stoull(value); } catch (...) {}
        }
        else if (key == L"sos_peer_index") {
            try { state.sosPeerIndex = (size_t)std::stoull(value); } catch (...) {}
        }
        else if (key == L"sos_last_full_scan_completed_at") {
            try { state.sosLastFullScanCompletedAt = std::stoll(value); } catch (...) {}
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
    text += L"egress_region=" + state.egressRegion + L"\r\n";
    text += L"egress_region_checked_at=" + std::to_wstring(state.egressRegionCheckedAt) + L"\r\n";

    std::wstring sosSlugsJoined;
    for (size_t i = 0; i < state.sosCountrySlugs.size(); ++i) {
        if (i > 0) sosSlugsJoined += L";";
        sosSlugsJoined += state.sosCountrySlugs[i];
    }
    text += L"sos_country_slugs=" + sosSlugsJoined + L"\r\n";
    text += L"sos_country_index=" + std::to_wstring(state.sosCountryIndex) + L"\r\n";
    text += L"sos_peer_index=" + std::to_wstring(state.sosPeerIndex) + L"\r\n";
    text += L"sos_last_full_scan_completed_at=" + std::to_wstring(state.sosLastFullScanCompletedAt) + L"\r\n";

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
// Аварийный (SOS) перебор ВСЕХ пиров по всем странам - крайняя мера,
// когда обычный поиск резервного пира в своём регионе не дал
// результата. Ограничен ПО ВРЕМЕНИ за один тик (не по числу пиров) и
// возобновляемый через курсор (индекс страны + индекс пира внутри неё) -
// следующий тик продолжает ровно с того места, где этот остановился.
// После полного прохода без результата - 12-часовой кулдаун. Если
// список стран на GitHub обновился посреди прохода - проход СНАЧАЛА
// доканчивается по старому снимку, новый подхватывается только на
// следующем проходе.
// ------------------------------------------------------------------
const long long SOS_COOLDOWN_SECONDS = 12 * 3600;
const long long SOS_TICK_TIME_BUDGET_SECONDS = 90;

bool SosCooldownActive(const WatchdogState& state) {
    if (state.sosLastFullScanCompletedAt == 0) return false;
    long long elapsed = (long long)time(nullptr) - state.sosLastFullScanCompletedAt;
    return elapsed < SOS_COOLDOWN_SECONDS;
}

// true, если за этот шаг нашёлся и добавлен рабочий пир
bool SosScanStep(WatchdogState& state, const std::vector<std::wstring>& excludeHosts) {
    if (SosCooldownActive(state)) {
        LogDebug(L"SOS: кулдаун после последнего полного прохода ещё не закончился");
        return false;
    }

    if (state.sosCountrySlugs.empty()) {
        auto files = ListCountryFiles();
        if (files.empty()) {
            LogError(L"SOS: не удалось получить список стран - повторю в следующий тик");
            return false;
        }
        for (const auto& f : files) state.sosCountrySlugs.push_back(f.slug);
        state.sosCountryIndex = 0;
        state.sosPeerIndex = 0;
        LogInfo(L"=== SOS: начинаю полный перебор пиров по всем странам (" +
                std::to_wstring(state.sosCountrySlugs.size()) + L") ===");
        SendTelegramNotification(L"SOS: обычный поиск резервного пира не сработал. "
                                  L"Начинаю полный перебор пиров по всем странам в фоне.");
    }

    auto start = std::chrono::steady_clock::now();
    auto isExcluded = [&](const std::wstring& uri) {
        std::wstring host = HostFromUri(uri);
        for (const auto& h : excludeHosts) if (h == host) return true;
        return false;
    };

    while (state.sosCountryIndex < state.sosCountrySlugs.size()) {
        std::wstring slug = state.sosCountrySlugs[state.sosCountryIndex];
        auto peers = FetchCountryPeers(slug);

        while (state.sosPeerIndex < peers.size()) {
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count() >= SOS_TICK_TIME_BUDGET_SECONDS) {
                LogInfo(L"SOS: отведённое на этот тик время закончилось, продолжу со страны '" +
                        slug + L"', пир #" + std::to_wstring(state.sosPeerIndex + 1));
                return false;
            }

            const auto& candidate = peers[state.sosPeerIndex];
            if (!isExcluded(candidate.uri)) {
                LogInfo(L"SOS: проверяю " + candidate.uri + L" (" + slug + L"/" + candidate.city + L")");
                if (VerifyPeerHandshake(candidate.uri)) {
                    LogInfo(L"  Up - добавляю как резервного (SOS)");
                    SendTelegramNotification(L"SOS: найден рабочий пир - " + candidate.uri +
                                              L" (" + slug + L"/" + candidate.city + L")");
                    AddPeer(candidate.uri);
                    state.backupPeers.push_back(candidate.uri);
                    state.backupRegion = slug;
                    state.sosPeerIndex++;
                    return true;
                }
            }
            state.sosPeerIndex++;
        }

        state.sosCountryIndex++;
        state.sosPeerIndex = 0;
    }

    LogError(L"SOS: полный перебор всех стран завершён, рабочий пир не найден. "
             L"Если дело в антивирусе/фаерволе - проверь, не блокирует ли он ygg_watchdog.exe "
             L"(добавь исключение в Защитнике Windows для папки программы).");
    SendTelegramNotification(L"SOS: полный перебор всех пиров завершён без результата. "
                              L"Возможно, антивирус/фаервол блокирует соединения - см. лог на устройстве.");
    state.sosLastFullScanCompletedAt = (long long)time(nullptr);
    state.sosCountrySlugs.clear();
    state.sosCountryIndex = 0;
    state.sosPeerIndex = 0;
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
        LogError(L"Устройство не найдено: " + name);
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
        LogInfo(L"Устройств для мониторинга не добавлено.");
        return;
    }
    LogInfo(L"Устройства для мониторинга (" + std::to_wstring(state.devices.size()) + L"):");
    for (const auto& d : state.devices) {
        bool down = state.deviceDownSince.count(d.name) > 0;
        LogInfo(L"  " + d.name + L" (" + d.host + L")" + (down ? L" - НЕДОСТУПНО" : L" - ок"));
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
        SendTelegramNotification(msg);
        return true;
    }

    if (status->percent <= BATTERY_CRITICAL_PERCENT && !status->charging) {
        state.batteryCriticalMode = true;
        state.batteryCriticalEnteredPercent = status->percent;
        std::wstring msg = L"Заряд " + std::to_wstring(status->percent) + L"% (не на зарядке) - "
            L"вхожу в аварийный режим по батарее, все проверки выключены до восстановления заряда.";
        LogInfo(msg);
        SendTelegramNotification(msg);
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
        LogError(L"Файл скрипта не найден: " + scriptPath);
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
    SendTelegramNotification(msg);
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
            return;
        } else {
            LogInfo(L"  не поднялся за отведённое время, пробую следующего");
        }
    }

    LogError(L"Не удалось найти рабочего регионального пира для " + countrySlug + L"/" + cityName);
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
        LogInfo(L"Поиск пиров для региона '" + countrySlug + L"/" + cityName +
                L"' запущен в фоне. Прогресс смотри в watchdog.log, обычно занимает от "
                L"нескольких секунд до пары минут в зависимости от числа кандидатов.");
    } else {
        LogError(L"Не удалось запустить фоновый процесс поиска региона");
    }
}

// ------------------------------------------------------------------
// Показ реальных списков - страны и города конкретной страны, чтобы
// не гадать с транслитерацией/точным написанием при вводе региона.
// ------------------------------------------------------------------
void CmdListCountries() {
    LogInfo(L"Загружаю список стран из репозитория GitHub...");
    auto entries = GetSortedCountryList();
    if (entries.empty()) {
        LogError(L"Не удалось получить список стран - проверь интернет-соединение");
        return;
    }

    LogInfo(L"Доступные страны (" + std::to_wstring(entries.size()) + L"):");
    for (size_t i = 0; i < entries.size(); ++i) {
        LogInfo(L"  " + std::to_wstring(i + 1) + L") " + entries[i].ruName +
                (entries[i].supported ? L"" : L" (координаты пока не поддерживаются)"));
    }
    LogInfo(L"Вводи НОМЕР страны из списка выше (не название).");
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
        LogError(L"Неизвестная страна: '" + countrySlug + L"'. Список стран - команда list-countries.");
        return;
    }
    std::wstring iso = *isoOpt;

    auto db = LoadCityDatabase();
    auto cities = GetCitiesForCountry(db, iso);

    if (cities.empty()) {
        LogError(L"Городов для страны '" + countrySlug + L"' (" + iso + L") не найдено в cities.dat");
        return;
    }

    LogInfo(L"Города для '" + countrySlug + L"' (" + iso + L"), всего " +
            std::to_wstring(cities.size()) + L", по алфавиту:");
    for (size_t i = 0; i < cities.size(); ++i) {
        LogInfo(L"  " + std::to_wstring(i + 1) + L") " + cities[i].displayLabel() +
                L" (население: " + std::to_wstring(cities[i].population) + L")");
    }
    LogInfo(L"Вводи НОМЕР города из списка выше (не название) - например: 1");
}


// countryInput == "" или "clear" - снять домашний регион
void CmdSetHomeRegion(const std::wstring& countryInput) {
    std::wstring lower = countryInput;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (countryInput.empty() || lower == L"clear") {
        WatchdogState state = LoadState();
        state.homeRegion.clear();
        SaveState(state);
        LogInfo(L"Домашний регион снят.");
        return;
    }

    std::wstring countrySlug = countryInput;
    try {
        size_t idx = std::stoul(countryInput);
        auto countryList = GetSortedCountryList();
        if (idx >= 1 && idx <= countryList.size()) countrySlug = countryList[idx - 1].slug;
    } catch (...) {}

    if (!SlugToIso(countrySlug).has_value()) {
        LogError(L"Неизвестная страна: '" + countrySlug + L"'. Список стран - команда list-countries.");
        return;
    }
    WatchdogState state = LoadState();
    state.homeRegion = countrySlug;
    SaveState(state);
    LogInfo(L"Домашний регион установлен: " + countrySlug + L" (" + SlugToRussianName(countrySlug) + L")");
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
        SendTelegramNotification(message);
    }

    return true;
}

void Tick() {
    CleanOldLogEntries();
    LogInfo(L"=== Начало проверки ===");

    WatchdogState state = LoadState();

    // Аварийный режим по батарее отключает ВСЕ остальные проверки до
    // восстановления заряда - проверяется первым делом, раньше вообще
    // всего остального.
    if (!CheckBattery(state)) {
        SaveState(state);
        LogInfo(L"Аварийный режим по батарее активен - остальные проверки этого тика пропущены.");
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
                SendTelegramNotification(msg);
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
            return;
        }

        if (!CheckInternet(state)) {
            SaveState(state);
            return;
        }

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
            SendTelegramNotification(L"Основной пир снова в строю. Удалены резервные: " + removedList);
            for (const auto& uri : state.backupPeers) {
                RemovePeer(uri);
            }
            state.backupPeers.clear();
            state.backupRegion.clear();
            SaveState(state);
        } else {
            LogInfo(L"Проверка выполнена: основные пиры живы, без изменений");
        }
        return;
    }

    bool anyBackupUp = false;
    for (const auto& p : state.backupPeers) {
        if (isUp(p)) { anyBackupUp = true; break; }
    }
    if (anyBackupUp) {
        LogInfo(L"Основные легли, но резервный держит связь - без изменений");
        return;
    }

    LogInfo(L"Все основные и резервные пиры легли - ищу замену");

    if (!CheckInternet(state)) {
        SaveState(state);
        return;
    }

    if (state.backupRegion.empty()) {
        LogInfo(L"Регион для резервных пиров ещё не определён - определяю через egress-регион "
                L"(или домашний, если GeoIP недоступен)");
        state.backupRegion = GetBackupSearchRegion(state, mainPeers);
        if (state.backupRegion.empty()) {
            LogError(L"Не удалось определить регион ни через GeoIP, ни через домашний регион, "
                     L"ни по стране основных пиров - поиск резервного невозможен");
            return;
        }
        LogInfo(L"Регион определён и сохранён: " + state.backupRegion);
        SaveState(state);
    }

    auto candidates = FetchCountryPeers(state.backupRegion);
    LogDebug(L"Кандидатов из репозитория для региона '" + state.backupRegion + L"': " +
             std::to_wstring(candidates.size()));

    for (const auto& candidate : candidates) {
        bool alreadyTried = false;
        for (const auto& p : mainPeers) if (p == candidate.uri) alreadyTried = true;
        for (const auto& p : state.backupPeers) if (p == candidate.uri) alreadyTried = true;
        if (alreadyTried) continue;

        LogInfo(L"Проверяю рукопожатие: " + candidate.uri + L" (" + candidate.city + L")");
        if (VerifyPeerHandshake(candidate.uri)) {
            LogInfo(L"  Up - добавляю как резервного");
            std::wstring mainPeersList;
            for (size_t i = 0; i < mainPeers.size(); ++i) {
                if (i > 0) mainPeersList += L", ";
                mainPeersList += mainPeers[i];
            }
            SendTelegramNotification(L"Все основные пиры (" + mainPeersList + L") легли. "
                                      L"Добавлен резервный (" + state.backupRegion + L"): " +
                                      candidate.uri + L" (" + candidate.city + L")");
            AddPeer(candidate.uri);
            state.backupPeers.push_back(candidate.uri);
            SaveState(state);
            return;
        } else {
            LogInfo(L"  не поднялся за отведённое время, пробую следующего");
        }
    }

    LogError(L"Не удалось найти рабочего резервного пира для региона '" + state.backupRegion + L"'. "
             L"Если дело в антивирусе/фаерволе - проверь исключение для ygg_watchdog.exe в Защитнике Windows.");

    // VPN: продвинутый вариант (одобренный скрипт) - пробуем перезапустить
    // перед крайней мерой (SOS); если скрипт не одобрен - только намёк в лог.
    if (!RunApprovedVpnCommand(L"restart")) {
        WarnVpnMayBeNeeded();
    }

    // Крайняя мера - аварийный перебор ВСЕХ пиров по всем странам
    // (ограничен по времени за тик, возобновляемый через курсор).
    std::vector<std::wstring> excludeHosts;
    for (const auto& p : mainPeers) excludeHosts.push_back(HostFromUri(p));
    for (const auto& p : state.backupPeers) excludeHosts.push_back(HostFromUri(p));
    SosScanStep(state, excludeHosts);
    SaveState(state);
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
    LogInfo(L"Перезапускаю службу Yggdrasil немедленно...");
    LogInfo(L"Внимание: если подключён к этому компьютеру ЧЕРЕЗ Yggdrasil - "
            L"соединение сейчас оборвётся. Если это твой единственный канал - "
            L"используй отложенный перезапуск вместо этого.");
    RunSimpleCommand(L"net stop \"" + std::wstring(YGG_SERVICE_NAME) + L"\"");
    RunSimpleCommand(L"net start \"" + std::wstring(YGG_SERVICE_NAME) + L"\"");
    LogInfo(L"Перезапуск выполнен.");
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

    LogInfo(L"Регистрирую задачу планировщика...");
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

    LogInfo(L"Удаляю задачу планировщика (останавливаю автоматические проверки)...");
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
// Консольное меню - показывается, когда программу запускают без
// аргументов (двойной клик по exe). Раньше в этом случае exe просто
// печатал подсказку по использованию и сразу завершался - окно
// консоли закрывалось мгновенно, создавая впечатление, что программа
// "не открывается" без .bat-обёртки. Теперь вместо этого показываем
// обычное интерактивное меню и ждём Enter после каждого действия.
// ------------------------------------------------------------------
void PrintRaw(const std::wstring& s) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    std::wstring withNewline = s + L"\r\n";
    DWORD written = 0;
    WriteConsoleW(hOut, withNewline.c_str(), (DWORD)withNewline.size(), &written, nullptr);
}

void WaitForEnter() {
    PrintRaw(L"");
    PrintRaw(L"Нажми Enter, чтобы продолжить...");
    std::wstring dummy;
    std::getline(std::wcin, dummy);
}

int RunInteractiveMenu() {
    while (true) {
        PrintRaw(L"================================================");
        PrintRaw(L"         YGG WATCHDOG");
        PrintRaw(L"================================================");
        PrintRaw(L"");
        PrintRaw(L"  1 - Проверить сейчас (tick)");
        PrintRaw(L"  2 - Установить/переустановить задачу автозапуска");
        PrintRaw(L"  3 - Остановить задачу автозапуска (удалить из планировщика)");
        PrintRaw(L"  4 - Показать основные пиры");
        PrintRaw(L"  5 - Убрать региональный режим (вернуться на основные пиры)");
        PrintRaw(L"  6 - Перезапустить службу Yggdrasil сейчас (обрыв связи по Yggdrasil!)");
        PrintRaw(L"  7 - Задать домашний регион (постоянный fallback для резервных пиров)");
        PrintRaw(L"  8 - Список устройств для мониторинга");
        PrintRaw(L"  9 - Добавить устройство для мониторинга");
        PrintRaw(L"  10 - Удалить устройство из мониторинга");
        PrintRaw(L"  11 - Одобрить VPN-скрипт (продвинутый режим)");
        PrintRaw(L"  12 - Снять одобрение VPN-скрипта");
        PrintRaw(L"  13 - Проверить обновления на GitHub");
        PrintRaw(L"  14 - Скачать и применить обновление");
        PrintRaw(L"  0 - Выход");
        PrintRaw(L"");
        PrintRaw(L"Выбор: ");

        std::wstring choice;
        if (!std::getline(std::wcin, choice)) {
            break;
        }

        if (choice == L"1") {
            Tick();
        } else if (choice == L"2") {
            InstallScheduledTask();
        } else if (choice == L"3") {
            UninstallScheduledTask();
        } else if (choice == L"4") {
            CmdListMainPeers();
        } else if (choice == L"5") {
            RemoveRegion();
        } else if (choice == L"6") {
            RestartYggdrasilServiceNow();
        } else if (choice == L"7") {
            CmdListCountries();
            PrintRaw(L"");
            PrintRaw(L"Номер страны из списка выше: ");
            std::wstring country;
            std::getline(std::wcin, country);
            CmdSetHomeRegion(country);
        } else if (choice == L"8") {
            CmdListDevices();
        } else if (choice == L"9") {
            PrintRaw(L"Имя устройства (например, ThinkPad): ");
            std::wstring name;
            std::getline(std::wcin, name);
            PrintRaw(L"Адрес устройства (IP или Yggdrasil-адрес): ");
            std::wstring host;
            std::getline(std::wcin, host);
            CmdAddDevice(name, host);
        } else if (choice == L"10") {
            CmdListDevices();
            PrintRaw(L"");
            PrintRaw(L"Имя устройства для удаления: ");
            std::wstring name;
            std::getline(std::wcin, name);
            CmdRemoveDevice(name);
        } else if (choice == L"11") {
            PrintRaw(L"Полный путь к скрипту: ");
            std::wstring path;
            std::getline(std::wcin, path);
            ApproveVpnCommand(path);
        } else if (choice == L"12") {
            ClearVpnApproval();
        } else if (choice == L"13") {
            auto info = CheckForUpdate();
            if (!info.has_value()) {
                PrintRaw(L"Новых версий не найдено (текущая: " + CURRENT_VERSION + L").");
            } else {
                PrintRaw(L"Доступна новая версия: " + info->tag + L". " + info->url);
            }
        } else if (choice == L"14") {
            ApplyUpdate();
        } else if (choice == L"0") {
            break;
        } else {
            PrintRaw(L"Неизвестный пункт меню.");
        }

        WaitForEnter();
    }
    return 0;
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

// Перезапускает себя же с правами администратора через UAC, передавая
// дальше те же аргументы командной строки (argv[1], argv[2], ...).
void RelaunchElevated(int argc, wchar_t* argv[]) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring args;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) args += L" ";
        args += L"\"" + std::wstring(argv[i]) + L"\"";
    }

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        LogError(L"Не удалось получить права администратора (пользователь отменил UAC?)");
    }
}

int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-v" || a == L"--verbose") {
            g_verboseMode = true;
        } else {
            args.push_back(a);
        }
    }

    // Права администратора нужны и для самого планировщика, и для
    // интерактивного меню (там тоже есть install-task/uninstall-task) -
    // поэтому проверяем/поднимаем права ДО проверки на пустые аргументы.
    // Раньше это было наоборот: при запуске без аргументов (двойной клик
    // по exe без .bat-обёртки) программа просто печатала подсказку и
    // сразу завершалась, из-за чего казалось, что exe "не открывается".
    if (!IsElevated()) {
        LogInfo(L"Требуются права администратора, перезапуск с повышением прав...");
        RelaunchElevated(argc, argv); // передаём оригинальные argv, включая -v, если был
        return 0;
    }

    if (args.empty()) {
        // Запуск без аргументов (двойной клик) - показываем меню вместо
        // мгновенного закрытия окна.
        return RunInteractiveMenu();
    }

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
    } else if (command == L"set-home-region") {
        CmdSetHomeRegion(args.size() >= 2 ? args[1] : L"");
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
            LogInfo(L"Новых версий не найдено (текущая: " + CURRENT_VERSION + L").");
        } else {
            LogInfo(L"Доступна новая версия: " + info->tag + L". " + info->url);
        }
    } else if (command == L"apply-update") {
        ApplyUpdate();
    } else {
        LogError(L"Неизвестная команда: " + command);
        return 1;
    }

    return 0;
}
