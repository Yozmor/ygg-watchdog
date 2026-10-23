// main.cpp - ygg-watchdog для Linux: разбор команд и запуск окна.
// Сборка: build.sh (g++, OpenSSL). Подробно - README_LINUX.md.
#include "core.cpp"
#include "cmds.cpp"
#include "app.cpp"

// Команды, которым root не нужен (только читают)
static bool ReadOnlyCommand(const std::string& c) {
    return c == "help" || c == "--help" || c == "-h" || c == "version" || c == "--version" || c == "--selftest" || c == "selftest" ||
           c == "list-countries" || c == "list-cities" || c == "countries-json" || c == "cities-json";
}

// Перезапуск через sudo (как делала Python-версия): yggdrasilctl, конфиг и systemd требуют root
static void ElevateIfNeeded(int argc, char** argv) {
    if (util::IsRoot()) return;
    if (util::Which("sudo").empty()) { fprintf(stderr, "Нужны права root, а sudo не найден. Запусти от root.\n"); exit(1); }
    std::vector<char*> a;
    std::string sudo = "sudo", self = util::SelfExe();
    a.push_back(&sudo[0]);
    a.push_back(&self[0]);
    for (int i = 1; i < argc; ++i) a.push_back(argv[i]);
    a.push_back(nullptr);
    execvp("sudo", a.data());
    perror("sudo");
    exit(1);
}

static int InstallYgg(const std::string& url) {
    std::string name = util::BaseName(url);
    if (!util::EndsWith(name, ".deb")) { PrintError("ожидалась ссылка на .deb"); return 1; }
    Print("Скачиваю " + name + "...");
    net::HttpResult r = net::Get(url, net::RequestOpts());
    if (!r.transportOk || r.status != 200 || !r.complete || r.body.size() < 1024) { PrintError("не скачалось: " + (r.err.empty() ? "HTTP " + std::to_string(r.status) : r.err)); return 1; }
    std::string path = "/tmp/" + name;
    if (!util::WriteFile(path, r.body, 0644)) { PrintError("не удалось записать " + path); return 1; }
    Print("Ставлю через apt (конфиг и ключ не трогаются)...");
    util::ProcResult p = util::Run({ "apt-get", "install", "-y", "--allow-downgrades", path }, 600000);
    unlink(path.c_str());
    if (p.code != 0) { PrintError("apt не справился:\n" + util::Trim(p.out)); return 1; }
    LogInfo("Yggdrasil обновлён из " + name);
    AppendEvent("ok", "Yggdrasil обновлён: " + name);
    return 0;
}

static int SetNotify(const std::string& token, const std::string& chat, const std::string& proxy) {
    std::string out;
    bool hadClients = false;
    for (const auto& l : util::Lines(util::ReadFile(NotifyPath()))) {
        std::string t = util::Trim(l);
        if (util::StartsWith(t, "BOT_TOKEN=") || util::StartsWith(t, "CHAT_ID=") || util::StartsWith(t, "PROXY=")) continue;
        if (util::StartsWith(t, "PROXY_CLIENTS=")) hadClients = true;
        out += l + "\n";
    }
    (void)hadClients;
    out += "BOT_TOKEN=" + token + "\nCHAT_ID=" + chat + "\nPROXY=" + proxy + "\n";
    if (!util::WriteFileAtomic(NotifyPath(), out, 0600)) { PrintError("не удалось записать " + NotifyPath()); return 1; }
    chmod(NotifyPath().c_str(), 0600);
    LogInfo(token.empty() || chat.empty() ? "Telegram выключен (нет токена или chat_id)." : "Настройки Telegram сохранены.");
    return 0;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);   // обрыв соединения не должен ронять сторожа
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-v" || a == "--verbose") g_verbose = true;
        else args.push_back(a);
    }
    std::string cmd = args.empty() ? "" : args[0];
    auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };

    // самопроверка для обновления: загрузился целиком, TLS создаётся - печатает версию
    if (cmd == "--selftest" || cmd == "selftest") {
        if (!net::TlsCtx()) return 1;
        printf("%s\n", CURRENT_VERSION);
        return 0;
    }
    if (cmd == "version" || cmd == "--version") { printf("ygg-watchdog %s\n", CURRENT_VERSION); return 0; }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") { PrintHelp(); return 0; }

    if (!ReadOnlyCommand(cmd)) ElevateIfNeeded(argc, argv);
    ApplyNetSettings();

    if (cmd.empty() || cmd == "menu") { app::App a; return a.Main(); }
    if (cmd == "tick") return Tick() ? 0 : 0;
    if (cmd == "install") return CmdInstall() ? 0 : 1;
    if (cmd == "uninstall") return CmdUninstall() ? 0 : 1;
    if (cmd == "start-timer") return CmdTimer(true) ? 0 : 1;
    if (cmd == "stop-timer") return CmdTimer(false) ? 0 : 1;
    if (cmd == "status") { CmdStatus(); return 0; }
    if (cmd == "status-json") { printf("%s\n", StatusJson().c_str()); return 0; }
    if (cmd == "diagnose") { Print(DiagnoseNowText(ReadMainPeers())); return 0; }
    if (cmd == "stats-json") { printf("%s\n", StatsJson((int)util::ToLL(arg(1), 24), (int)util::ToLL(arg(2), 96)).c_str()); return 0; }
    if (cmd == "stats-csv") { CmdStatsCsv(); return 0; }
    if (cmd == "countries-json") { printf("%s\n", CountriesJson().c_str()); return 0; }
    if (cmd == "cities-json") { printf("%s\n", CitiesJson(arg(1)).c_str()); return 0; }
    if (cmd == "catalog-json") { printf("%s\n", CatalogJson(arg(1)).c_str()); return 0; }
    if (cmd == "list-countries") { CmdListCountries(); return 0; }
    if (cmd == "list-cities") { CmdListCities(arg(1)); return 0; }
    if (cmd == "set-region" || cmd == "add-region") return CmdSetRegion(arg(1), arg(2), false) ? 0 : 1;
    if (cmd == "remove-region") { CmdRemoveRegion(); return 0; }
    if (cmd == "set-home-region") return CmdSetRegion(arg(1), arg(2), true) ? 0 : 1;
    if (cmd == "list-main-peers") { CmdListMainPeers(); return 0; }
    if (cmd == "add-main-peer") return CmdAddMainPeer(arg(1)) ? 0 : 1;
    if (cmd == "remove-main-peer") return CmdRemoveMainPeer(arg(1)) ? 0 : 1;
    if (cmd == "disconnect-peer") {
        RemovePeer(arg(1));
        WatchdogState st = LoadState();
        st.backupPeers.erase(std::remove(st.backupPeers.begin(), st.backupPeers.end(), arg(1)), st.backupPeers.end());
        SaveState(st);
        LogInfo("Пир отключён: " + arg(1));
        return 0;
    }
    if (cmd == "verify-peer") {
        if (arg(1).empty()) { PrintError("использование: verify-peer <uri>"); return 1; }
        VerifyResult r = VerifyPeerNow(arg(1));
        printf("%s\n", VerifyJson(arg(1), r).c_str());
        return 0;
    }
    if (cmd == "restart-yggdrasil") { RestartYggdrasilNow(); return 0; }
    if (cmd == "restart-yggdrasil-delayed") { RestartYggdrasilDelayed(args.size() > 1 ? (int)util::ToLL(arg(1), 10) : 10); return 0; }
    if (cmd == "service") {
        std::string act = arg(1);
        if (act != "start" && act != "stop" && act != "enable" && act != "disable") { PrintError("использование: service start|stop|enable|disable"); return 1; }
        std::string e = ygg::ServiceCtl(act);
        if (e.empty()) LogInfo(std::string("Служба Yggdrasil: ") + (act == "start" ? "запущена" : act == "stop" ? "остановлена" : act == "enable" ? "автозапуск включён" : "автозапуск выключен"));
        else LogError("systemctl " + act + " yggdrasil: " + e);
        return e.empty() ? 0 : 1;
    }
    if (cmd == "list-devices") { CmdListDevices(); return 0; }
    if (cmd == "add-device") {
        if (args.size() < 3) { PrintError("использование: add-device <имя> <адрес> [общий пир ...] (--no-peers - убрать общие пиры)"); return 1; }
        std::vector<std::string> peers;
        bool clear = false;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--no-peers") clear = true;
            else for (const auto& p : util::Split(args[i], ' ')) peers.push_back(p);
        }
        return CmdAddDevice(args[1], args[2], peers, clear || !peers.empty()) ? 0 : 1;
    }
    if (cmd == "remove-device") return CmdRemoveDevice(arg(1)) ? 0 : 1;
    if (cmd == "diagnose-device") { CmdDiagnoseDevice(arg(1)); return 0; }
    if (cmd == "diagnose-device-json") { printf("%s\n", DeviceDiagJson(arg(1)).c_str()); return 0; }
    if (cmd == "approve-vpn-command") return ApproveVpnCommand(arg(1)) ? 0 : 1;
    if (cmd == "clear-vpn-command") { ClearVpnApproval(); return 0; }
    if (cmd == "check-update") {
        std::string tag = CheckForUpdate();
        if (tag.empty() && !g_updateCheckError.empty()) Print("Не удалось проверить: " + g_updateCheckError + " (у тебя " + CURRENT_VERSION + ").");
        else if (tag.empty()) Print(std::string("В репозитории та же версия или старше (у тебя ") + CURRENT_VERSION + ").");
        else {
            Print("В репозитории новая версия: " + tag + " " + REPO_PAGE);
            WatchdogState st = LoadState();
            st.updateNotifiedTag = tag;
            SaveState(st);
        }
        return 0;
    }
    if (cmd == "apply-update") {
        std::string tag, e = ApplyUpdate(&tag);
        if (!e.empty()) { LogError("Обновление: " + e); return 1; }
        Print("Обновлено до " + tag + ".");
        return 0;
    }
    if (cmd == "test-notify") {
        NotifyConfig c = LoadNotifyConfig();
        if (!c.valid) { PrintError("Telegram не настроен: в notify.conf нет BOT_TOKEN или CHAT_ID."); return 1; }
        if (SendTelegram("Тестовое сообщение от ygg-watchdog - уведомления работают.")) { Print("Отправлено (" + g_tgLastVia + ")."); return 0; }
        PrintError("не отправлено: " + g_tgLastErr);
        return 1;
    }
    if (cmd == "set-notify") return SetNotify(arg(1), arg(2), arg(3));
    if (cmd == "restore-config") { CmdRestoreConfig(); return 0; }
    if (cmd == "install-ygg") return InstallYgg(arg(1));
    if (cmd == "find-best-peers") {
        int count = 3, per = 1;
        bool add = false;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--count" && i + 1 < args.size()) count = (int)util::ToLL(args[++i], 3);
            else if (args[i] == "--per-country" && i + 1 < args.size()) per = (int)util::ToLL(args[++i], 1);
            else if (args[i] == "--add") add = true;
        }
        CmdFindBestPeers(count, per, add);
        return 0;
    }
    PrintError("неизвестная команда: " + cmd);
    PrintHelp();
    return 1;
}
