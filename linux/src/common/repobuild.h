// repobuild.h - сборка программы из исходников репозитория (общее для
// ygg-control и ygg-watchdog). Нужна, когда в репозитории лежат только
// исходники (собранный файл не закоммичен или он старый): скачивается одна
// папка linux/ (sparse clone), запускается её build.sh.
#pragma once
#include <unistd.h>
#include <string>
#include <vector>
#include "util.h"

namespace repobuild {

inline bool HaveCommand(const std::string& c) {
    util::ProcResult r = util::Run({ "sh", "-c", "command -v " + c }, 5000);
    return r.code == 0 && !util::Trim(r.out).empty();
}

// Чего не хватает для сборки на этом компьютере (пусто - всё есть)
inline std::string MissingTools() {
    std::vector<std::string> miss;
    if (!HaveCommand("git")) miss.push_back("git");
    if (!HaveCommand("g++")) miss.push_back("g++");
    if (!util::FileExists("/usr/include/openssl/ssl.h")) miss.push_back("libssl-dev");
    if (miss.empty()) return "";
    std::string s;
    for (const auto& m : miss) s += (s.empty() ? "" : " ") + m;
    return s;
}

// Первая понятная строка с ошибкой из вывода компилятора / git
inline std::string ErrorLine(const std::string& out) {
    for (const auto& l : util::Lines(out)) {
        std::string t = util::Trim(l);
        if (util::IContains(t, "error") || util::IContains(t, "fatal") || util::StartsWith(t, "Нет ")) return util::U8Left(t, 240);
    }
    auto ls = util::Lines(util::Trim(out));
    return ls.empty() ? "" : util::U8Left(util::Trim(ls.back()), 240);
}

struct Result {
    std::string err;      // пусто - собралось
    std::string work;     // временная папка (удалить: Cleanup)
    std::string linuxDir; // work/linux - свежие исходники
    std::string built;    // путь к собранной программе
};

// repoUrl - https://github.com/Yozmor/<repo>.git, prog - имя программы
// (её кладёт build.sh рядом с собой), logPath - куда записать весь вывод.
inline Result Build(const std::string& repoUrl, const std::string& prog, const std::string& logPath) {
    Result res;
    std::string miss = MissingTools();
    if (!miss.empty()) { res.err = "для сборки из исходников не хватает: " + miss + " (поставь: sudo apt install g++ libssl-dev git)"; return res; }
    res.work = "/tmp/" + prog + "-build-" + std::to_string(getpid());
    res.linuxDir = res.work + "/linux";
    util::Run({ "rm", "-rf", res.work }, 30000);
    std::string log;
    // только папка linux/, без Windows-части и без истории
    util::ProcResult g = util::Run({ "env", "GIT_TERMINAL_PROMPT=0", "git", "clone", "--depth", "1", "--filter=blob:none", "--sparse", repoUrl, res.work }, 300000);
    log += "$ git clone " + repoUrl + "\n" + g.out + "\n";
    if (g.code == 0) {
        g = util::Run({ "git", "-C", res.work, "sparse-checkout", "set", "linux" }, 300000);
        log += "$ git sparse-checkout set linux\n" + g.out + "\n";
    }
    if (g.code != 0 || !util::FileExists(res.linuxDir + "/build.sh")) {
        util::WriteFile(logPath, log);
        res.err = "не удалось скачать исходники: " + (g.timedOut ? std::string("git не уложился в 5 минут") : ErrorLine(g.out)) + " (подробно: " + logPath + ")";
        return res;
    }
    util::ProcResult b = util::Run({ "bash", res.linuxDir + "/build.sh" }, 1200000);
    log += "$ ./build.sh\n" + b.out + "\n";
    util::WriteFile(logPath, log);
    res.built = res.linuxDir + "/" + prog;
    if (b.code != 0 || !util::FileExists(res.built)) {
        res.err = "сборка не удалась: " + (b.timedOut ? std::string("дольше 20 минут") : ErrorLine(b.out)) + " (весь вывод: " + logPath + ")";
        return res;
    }
    return res;
}

// Свежие исходники - в папку программы (если это папка с исходниками,
// т.е. там лежит build.sh): чтобы ./build.sh там собирал уже новую версию.
// Сама программа здесь не копируется - её ставит вызывающий переименованием.
// Владелец скопированного - тот же, что у папки (а не root).
inline void SyncSources(const Result& r, const std::string& progDir, const std::string& prog) {
    if (r.linuxDir.empty() || !util::FileExists(progDir + "build.sh")) return;
    unlink(r.built.c_str());
    util::ProcResult ls = util::Run({ "ls", "-A", r.linuxDir }, 10000);
    for (const auto& n0 : util::Lines(ls.out)) {
        std::string n = util::Trim(n0);
        if (n.empty() || n == prog) continue;
        util::Run({ "cp", "-r", r.linuxDir + "/" + n, progDir }, 60000);
        util::Run({ "chown", "-R", "--reference=" + progDir, progDir + n }, 30000);
    }
}

inline void Cleanup(const Result& r) {
    if (!r.work.empty() && util::StartsWith(r.work, "/tmp/")) util::Run({ "rm", "-rf", r.work }, 60000);
}

} // namespace repobuild
