// ygg_guard.cpp - страж ygg-watchdog (ygg_guard.exe).
//
// Задача Планировщика YggWatchdogTick запускает его вместо самого сторожа:
//   1. проверяет, что ygg_watchdog.exe цел: файл есть, заголовки MZ/PE на
//      месте, а если файл не пересобирали (тот же размер и время записи) -
//      SHA-256 тот же, что сторож записал после пересборки;
//   2. запускает проверку (ygg_watchdog.exe tick);
//   3. если exe повреждён или не запускается ДВА хода подряд (один сбой может
//      быть пересборкой в эту самую минуту) - откладывает битый файл рядом как
//      ygg_watchdog.exe.broken-дата и скачивает windows/ygg_watchdog.exe из
//      репозитория со сверкой отпечатка; не вышло - берёт
//      ygg_watchdog.exe.backup от прошлого обновления. Не чаще раза в час;
//   4. о восстановлении пишет в watchdog.log, events.log и в Telegram.
// Сторож, в свою очередь, на каждой проверке смотрит, цел ли страж.
//
// Код общий со сторожем (подключается ygg_watchdog.cpp без его точки входа),
// но это отдельный файл: повреждение одного не задевает другой.
#define YGG_WATCHDOG_NO_MAIN
#include "ygg_watchdog.cpp"

static const DWORD GUARD_TICK_TIMEOUT_MS = 9 * 60 * 1000;

// Запустить проверку. Пусто - отработала, иначе - почему не запустилась / упала.
std::wstring RunTickOnce(const std::wstring& exe) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\" tick";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        util::DirOf(exe).c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        return L"не запускается (ошибка Windows " + std::to_wstring(e) + L")";
    }
    std::wstring why;
    if (WaitForSingleObject(pi.hProcess, GUARD_TICK_TIMEOUT_MS) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        LogError(L"страж: проверка шла дольше 9 минут - остановлена");
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        if (code >= 0xC0000000) {               // NTSTATUS аварии: 0xC0000005 и т.п.
            wchar_t b[32];
            swprintf(b, 32, L"0x%08lX", (unsigned long)code);
            why = std::wstring(L"аварийно завершился (код ") + b + L")";
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return why;
}

long long RegNumber(const wchar_t* name) {
    try { return std::stoll(util::RegReadPath(REG_WD_KEY, name)); } catch (...) { return 0; }
}

bool RepairWatchdogExe(const std::wstring& exe, const std::wstring& why) {
    long long now = (long long)time(nullptr);
    if (now - RegNumber(L"GuardRepairAt") < 3600) {
        LogError(L"страж: ygg_watchdog.exe повреждён (" + why + L"), но восстановление уже пробовали меньше часа назад - жду");
        return false;
    }
    util::RegWriteString(REG_WD_KEY, L"GuardRepairAt", std::to_wstring(now));
    LogError(L"страж: ygg_watchdog.exe повреждён: " + why + L" - восстанавливаю");

    std::wstring tmp = exe + L".download", err, src;
    if (DownloadRepoFile(L"windows/ygg_watchdog.exe", tmp, err)) {
        src = L"скачан из репозитория";
    } else {
        LogError(L"страж: из репозитория не вышло: " + err);
        std::wstring bak = exe + L".backup";
        if (CheckExeIntegrity(bak, false).empty() && CopyFileW(bak.c_str(), tmp.c_str(), FALSE))
            src = L"взята прошлая версия ygg_watchdog.exe.backup";
    }
    if (src.empty()) {
        Notify(L"alert", L"ygg_watchdog.exe повреждён (" + why + L"), восстановить не удалось: " + err +
                         L", целой ygg_watchdog.exe.backup тоже нет. Собери заново (run_ygg_watchdog.bat) или скачай из репозитория.");
        return false;
    }
    if (util::FileExists(exe)) {
        std::wstring broken = exe + L".broken-" + util::TimeText(now, L"%Y%m%d-%H%M%S");
        if (!MoveFileExW(exe.c_str(), broken.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            Notify(L"alert", L"ygg_watchdog.exe повреждён (" + why + L"), но файл занят - заменить не удалось (ошибка " +
                             std::to_wstring(GetLastError()) + L").");
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Notify(L"alert", L"ygg_watchdog.exe повреждён (" + why + L"): целый файл скачан, но положить его на место не удалось.");
        return false;
    }
    util::RegWriteString(REG_WD_KEY, L"GuardFails", L"0");
    Notify(L"alert", L"ygg_watchdog.exe был повреждён (" + why + L") - восстановлен: " + src +
                     L". Битый лежит рядом (ygg_watchdog.exe.broken-дата). Если ты собирал свою версию - пересобери её.");
    return true;
}

int GuardMain() {
    std::wstring exe = WatchdogExePath();
    // exe только что записан (идёт сборка или обновление) - не мешаем
    long long age = FileAgeSeconds(exe);
    if (age >= 0 && age < 90) return 0;

    std::wstring why = CheckExeIntegrity(exe, true);
    if (why.empty()) why = RunTickOnce(exe);
    if (why.empty()) {
        if (RegNumber(L"GuardFails") != 0) util::RegWriteString(REG_WD_KEY, L"GuardFails", L"0");
        return 0;
    }
    long long fails = RegNumber(L"GuardFails") + 1;
    util::RegWriteString(REG_WD_KEY, L"GuardFails", std::to_wstring(fails));
    if (fails < 2) {
        LogError(L"страж: ygg_watchdog.exe: " + why + L" - если повторится на следующем ходе, восстановлю");
        return 1;
    }
    if (RepairWatchdogExe(exe, why)) RunTickOnce(exe);
    return 1;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return GuardMain();
}
