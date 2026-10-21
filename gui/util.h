// util.h - всё, что не про рисование: запуск процессов, сеть (пинг,
// DNS, проверка порта), служба Windows, буфер обмена, файлы.
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <shellapi.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <algorithm>
#include "json_lite.h"

namespace util {

inline std::wstring FromUtf8(const std::string& s) { return json::Parser::FromUtf8(s); }
inline std::string ToUtf8(const std::wstring& w) { return json::ToUtf8(w); }

inline std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

inline std::wstring SelfExe() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

inline std::wstring DirOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p + 1);
}

inline bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::string ReadFileBytes(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool WriteFileBytes(const std::wstring& path, const std::string& data) {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return f.good();
}

// Запись "всё или ничего": сначала во временный файл рядом, сброс на диск,
// потом подмена одним MoveFileEx. Если питание пропадёт посреди записи,
// останется либо старый файл, либо новый - но не обрезанный.
inline bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size());
    if (ok) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// Хвост большого файла (лог Yggdrasil бывает в десятки мегабайт)
inline std::string ReadFileTail(const std::wstring& path, size_t maxBytes, unsigned long long* sizeOut = nullptr) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    if (sizeOut) *sizeOut = (unsigned long long)sz.QuadPart;
    long long start = sz.QuadPart > (long long)maxBytes ? sz.QuadPart - (long long)maxBytes : 0;
    LARGE_INTEGER pos; pos.QuadPart = start;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    std::string out((size_t)(sz.QuadPart - start), 0);
    DWORD got = 0;
    if (!out.empty()) ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
    out.resize(got);
    CloseHandle(h);
    if (start > 0) { size_t nl = out.find('\n'); if (nl != std::string::npos) out = out.substr(nl + 1); }
    return out;
}

inline std::wstring TimeText(long long ts, const wchar_t* fmt = L"%H:%M:%S") {
    if (ts <= 0) return L"--:--";
    time_t t = (time_t)ts;
    tm lt;
    localtime_s(&lt, &t);
    wchar_t b[64];
    wcsftime(b, 64, fmt, &lt);
    return b;
}

inline std::wstring Bytes(double b) {
    const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; ++i; }
    wchar_t s[32];
    swprintf(s, 32, i == 0 ? L"%.0f %ls" : (b < 10 ? L"%.1f %ls" : L"%.0f %ls"), b, u[i]);
    return s;
}

inline std::wstring Rate(double bps) { return Bytes(bps) + L"/s"; }

inline std::wstring Duration(double sec) {
    long long s = (long long)sec;
    wchar_t b[48];
    if (s < 60) swprintf(b, 48, L"%lld с", s);
    else if (s < 3600) swprintf(b, 48, L"%lld м", s / 60);
    else if (s < 86400) swprintf(b, 48, L"%lldч %02lldм", s / 3600, (s % 3600) / 60);
    else swprintf(b, 48, L"%lldд %02lldч", s / 86400, (s % 86400) / 3600);
    return b;
}

inline std::wstring ShortKey(const std::wstring& k) {
    return k.size() > 12 ? k.substr(0, 4) + L"…" + k.substr(k.size() - 4) : k;
}

// ------------------------------------------------------------------
// Запуск процесса без окна; весь вывод (stdout+stderr) -> строка
// ------------------------------------------------------------------
struct ProcResult { bool started = false; bool timedOut = false; DWORD code = (DWORD)-1; std::string out; };

// Таймаут настоящий: вывод читается только тогда, когда он есть (PeekNamedPipe),
// а между порциями проверяется, не вышло ли время. Раньше ReadFile ждал конца
// вывода без ограничения, и зависший yggdrasilctl вешал всю проверку навсегда -
// ожидание с таймаутом после него уже ничего не решало.
inline ProcResult Run(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs = 20000) {
    ProcResult r;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return r;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = hNul;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    r.started = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    CloseHandle(hWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (r.started) {
        ULONGLONG deadline = GetTickCount64() + timeoutMs;
        char chunk[4096];
        auto drain = [&]() {   // забрать всё, что уже лежит в трубе, не блокируясь
            DWORD avail = 0, n = 0;
            while (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                if (!ReadFile(hRead, chunk, (DWORD)(std::min)((DWORD)sizeof(chunk), avail), &n, nullptr) || n == 0) break;
                r.out.append(chunk, n);
            }
        };
        for (;;) {
            drain();
            DWORD w = WaitForSingleObject(pi.hProcess, 50);
            if (w == WAIT_OBJECT_0) { drain(); break; }
            if (GetTickCount64() >= deadline) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
                r.timedOut = true;
                drain();
                break;
            }
        }
        GetExitCodeProcess(pi.hProcess, &r.code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);
    return r;
}

// Запуск «и забыть» (например отложенный перезапуск службы)
inline bool RunDetached(const std::wstring& cmdLine) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                        nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

inline bool CopyToClipboard(HWND owner, const std::wstring& s) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
    return h != nullptr;
}

// ------------------------------------------------------------------
// Сеть
// ------------------------------------------------------------------
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } };

// Адрес в виде для SMB "201-831c-...-b93a.ipv6-literal.net" (через дефисы) -> обычный
// IPv6 "201:831c:...:b93a". Остальные адреса возвращаются как есть.
inline std::wstring NormalizeIpv6(std::wstring a) {
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

// Пинг IPv6 (адреса Yggdrasil). Возвращает мс или -1.
inline double Ping6(const std::wstring& addressIn, DWORD timeoutMs = 3000) {
    static WsaInit wsa;
    std::wstring address = NormalizeIpv6(addressIn);
    sockaddr_in6 dst = {}, src = {};
    dst.sin6_family = AF_INET6;
    src.sin6_family = AF_INET6;
    if (InetPtonW(AF_INET6, address.c_str(), &dst.sin6_addr) != 1) return -1;
    HANDLE h = Icmp6CreateFile();
    if (h == INVALID_HANDLE_VALUE) return -1;
    char data[32] = "ygg-control ping";
    std::vector<char> reply(sizeof(ICMPV6_ECHO_REPLY) + sizeof(data) + 64);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    DWORD n = Icmp6SendEcho2(h, nullptr, nullptr, nullptr, &src, &dst, data, (WORD)sizeof(data), nullptr,
                             reply.data(), (DWORD)reply.size(), timeoutMs);
    QueryPerformanceCounter(&t1);
    double ms = -1;
    if (n > 0) {
        auto* r = (ICMPV6_ECHO_REPLY*)reply.data();
        if (r->Status == IP_SUCCESS) {
            ms = r->RoundTripTime;
            if (ms <= 0) ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart; // точнее для < 1 мс
        }
    }
    IcmpCloseHandle(h);
    return ms;
}

// Разбор адреса пира: схема, хост, порт
struct PeerUri { std::wstring scheme, host, port, query; bool ok = false; };

inline PeerUri ParseUri(const std::wstring& uri) {
    PeerUri u;
    size_t s = uri.find(L"://");
    if (s == std::wstring::npos) return u;
    u.scheme = Lower(uri.substr(0, s));
    std::wstring rest = uri.substr(s + 3);
    size_t q = rest.find(L'?');
    if (q != std::wstring::npos) { u.query = rest.substr(q + 1); rest = rest.substr(0, q); }
    size_t slash = rest.find(L'/');
    if (slash != std::wstring::npos) rest = rest.substr(0, slash);
    if (!rest.empty() && rest[0] == L'[') {
        size_t e = rest.find(L']');
        if (e == std::wstring::npos) return u;
        u.host = rest.substr(1, e - 1);
        if (e + 1 < rest.size() && rest[e + 1] == L':') u.port = rest.substr(e + 2);
    } else {
        size_t c = rest.rfind(L':');
        if (c != std::wstring::npos) { u.host = rest.substr(0, c); u.port = rest.substr(c + 1); }
        else u.host = rest;
    }
    u.ok = !u.host.empty();
    return u;
}

// DNS: первый адрес хоста строкой (или пусто)
inline std::wstring Resolve(const std::wstring& host) {
    static WsaInit wsa;
    ADDRINFOW hints = {}, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &res) != 0 || !res) return L"";
    wchar_t buf[128] = L"";
    DWORD len = 128;
    WSAAddressToStringW(res->ai_addr, (DWORD)res->ai_addrlen, nullptr, buf, &len);
    FreeAddrInfoW(res);
    std::wstring s = buf;
    if (!s.empty() && s[0] == L'[') { size_t e = s.find(L']'); if (e != std::wstring::npos) s = s.substr(1, e - 1); }
    return s;
}

// TCP-подключение к порту: мс или -1
inline double TcpConnect(const std::wstring& host, const std::wstring& port, DWORD timeoutMs = 4000) {
    static WsaInit wsa;
    ADDRINFOW hints = {}, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (GetAddrInfoW(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return -1;
    double result = -1;
    for (ADDRINFOW* a = res; a && result < 0; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD t0 = GetTickCount();
        connect(s, a->ai_addr, (int)a->ai_addrlen);
        fd_set w, e;
        FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
        timeval tv = { (long)(timeoutMs / 1000), (long)((timeoutMs % 1000) * 1000) };
        if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) result = (double)(GetTickCount() - t0);
        closesocket(s);
    }
    FreeAddrInfoW(res);
    return result;
}

// ------------------------------------------------------------------
// Служба Windows
// ------------------------------------------------------------------
struct ServiceInfo {
    bool exists = false;
    DWORD state = 0;            // SERVICE_RUNNING и т.д.
    DWORD pid = 0;
    DWORD startType = 0;        // SERVICE_AUTO_START / DEMAND / DISABLED
    std::wstring binaryPath;    // командная строка службы целиком
    long long startedAt = 0;    // когда запущен процесс службы (unix)
};

inline ServiceInfo QueryService(const wchar_t* name) {
    ServiceInfo si;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return si;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (svc) {
        si.exists = true;
        SERVICE_STATUS_PROCESS sp = {};
        DWORD need = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE*)&sp, sizeof(sp), &need)) {
            si.state = sp.dwCurrentState;
            si.pid = sp.dwProcessId;
        }
        QueryServiceConfigW(svc, nullptr, 0, &need);
        std::vector<BYTE> buf(need + 16);
        auto* cfg = (QUERY_SERVICE_CONFIGW*)buf.data();
        if (need && QueryServiceConfigW(svc, cfg, (DWORD)buf.size(), &need)) {
            si.startType = cfg->dwStartType;
            if (cfg->lpBinaryPathName) si.binaryPath = cfg->lpBinaryPathName;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    if (si.pid) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, si.pid);
        if (p) {
            FILETIME c, e, k, u;
            if (GetProcessTimes(p, &c, &e, &k, &u)) {
                ULARGE_INTEGER t; t.LowPart = c.dwLowDateTime; t.HighPart = c.dwHighDateTime;
                si.startedAt = (long long)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
            }
            CloseHandle(p);
        }
    }
    return si;
}

inline std::wstring ServiceStateText(DWORD s) {
    switch (s) {
        case SERVICE_RUNNING: return L"работает";
        case SERVICE_STOPPED: return L"остановлена";
        case SERVICE_START_PENDING: return L"запускается";
        case SERVICE_STOP_PENDING: return L"останавливается";
        case SERVICE_PAUSED: return L"приостановлена";
        default: return L"меняет состояние";
    }
}

// Ждать нужного состояния службы, не дольше timeoutMs
inline bool WaitServiceState(SC_HANDLE svc, DWORD want, DWORD timeoutMs) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeoutMs) {
        SERVICE_STATUS st = {};
        if (!QueryServiceStatus(svc, &st)) return false;
        if (st.dwCurrentState == want) return true;
        Sleep(300);
    }
    return false;
}

// action: "start" / "stop" / "restart". Возвращает текст ошибки или пусто.
inline std::wstring ControlServiceNamed(const wchar_t* name, const std::wstring& action) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return L"нет доступа к диспетчеру служб";
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    std::wstring err;
    if (!svc) err = L"служба не найдена";
    else {
        if (action == L"stop" || action == L"restart") {
            SERVICE_STATUS st = {};
            QueryServiceStatus(svc, &st);
            if (st.dwCurrentState != SERVICE_STOPPED) {
                if (!ControlService(svc, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
                    err = L"не удалось остановить (код " + std::to_wstring(GetLastError()) + L")";
                else if (!WaitServiceState(svc, SERVICE_STOPPED, 20000)) err = L"служба не остановилась за 20 с";
            }
        }
        if (err.empty() && (action == L"start" || action == L"restart")) {
            if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
                err = L"не удалось запустить (код " + std::to_wstring(GetLastError()) + L")";
            else if (!WaitServiceState(svc, SERVICE_RUNNING, 20000)) err = L"служба не запустилась за 20 с";
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return err;
}

inline std::wstring SetServiceStartType(const wchar_t* name, DWORD type) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return L"нет доступа к диспетчеру служб";
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_CHANGE_CONFIG);
    std::wstring err;
    if (!svc) err = L"служба не найдена";
    else {
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr))
            err = L"код " + std::to_wstring(GetLastError());
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return err;
}

// Разбить командную строку на аргументы (как это делает Windows)
inline std::vector<std::wstring> SplitArgs(const std::wstring& cmd) {
    std::vector<std::wstring> v;
    int n = 0;
    wchar_t** a = CommandLineToArgvW(cmd.c_str(), &n);
    if (a) { for (int i = 0; i < n; ++i) v.push_back(a[i]); LocalFree(a); }
    return v;
}

// ------------------------------------------------------------------
// HTTPS (WinHTTP, системный прокси учитывается): GET -> тело или ""
// ------------------------------------------------------------------
inline std::string HttpGet(const std::wstring& url, DWORD* statusOut = nullptr, size_t maxBytes = 64 * 1024 * 1024,
                           const wchar_t* headers = WINHTTP_NO_ADDITIONAL_HEADERS) {
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = L"", path[2048] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return "";
    std::string body;
    HINTERNET s = WinHttpOpen(L"ygg-control", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"ygg-control", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return "";
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 60000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (r) {
        DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
        if (WinHttpSendRequest(r, headers, headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
            if (statusOut) *statusOut = status;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail > 0 && body.size() < maxBytes) {
                std::string chunk(avail, 0);
                DWORD got = 0;
                if (!WinHttpReadData(r, &chunk[0], avail, &got) || got == 0) break;
                body.append(chunk.data(), got);
            }
            if (status != 200) body.clear();
        }
        WinHttpCloseHandle(r);
    }
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return body;
}

// ------------------------------------------------------------------
// Реестр: программы оставляют там свой путь, чтобы находить друг друга
// (HKLM\SOFTWARE\Yozmor\<программа>, значения Path и Version)
// ------------------------------------------------------------------
inline std::wstring RegReadPath(const std::wstring& subkey, const wchar_t* value = L"Path") {
    wchar_t buf[MAX_PATH * 2] = L"";
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, subkey.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

inline bool RegWriteString(const std::wstring& subkey, const wchar_t* value, const std::wstring& data) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LONG rc = RegSetValueExW(k, value, 0, REG_SZ, (const BYTE*)data.c_str(), (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

// Сравнение версий "v0.5.13" / "0.5.14": true, если b новее a
inline bool VersionNewer(const std::wstring& a, const std::wstring& b) {
    auto parse = [](const std::wstring& v) {
        std::vector<int> n; int cur = -1;
        for (wchar_t c : v) {
            if (iswdigit(c)) cur = (cur < 0 ? 0 : cur * 10) + (c - L'0');
            else if (cur >= 0) { n.push_back(cur); cur = -1; if (c != L'.') break; }
        }
        if (cur >= 0) n.push_back(cur);
        while (n.size() < 3) n.push_back(0);
        return n;
    };
    auto x = parse(a), y = parse(b);
    return y > x;
}

} // namespace util
