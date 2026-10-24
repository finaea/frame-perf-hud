// 自動起動の切り替えの実装。
#include "autostart.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/**
 * systemd のユーザー設定のフォルダ（$XDG_CONFIG_HOME/systemd/user、既定は ~/.config/systemd/user）。
 * @return パス
 */
std::string systemdUserDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/systemd/user";
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.config/systemd/user";
}

/**
 * systemctl の絶対パスを探す（fork した子で PATH を探さずに済むよう、親で先に決める）。
 * @return 見つかったパス。無ければ空
 */
std::string findSystemctl() {
    for (const char* path : {"/usr/bin/systemctl", "/bin/systemctl"}) {
        if (::access(path, X_OK) == 0) return path;
    }
    return "";
}

}  // namespace

Autostart::Autostart() {
    const std::string dir = systemdUserDir();
    unitPath_ = dir + "/" + kUnitName;
    linkPath_ = dir + "/steamvr.service.wants/" + kUnitName;
    systemctl_ = findSystemctl();
    refresh();
}

Autostart::~Autostart() {
    // 終了時に実行中なら、待たずに手放す（systemctl は短時間で終わる。回収できなかった分は init が引き取る）
    if (errorPipe_ >= 0) ::close(errorPipe_);
}

bool Autostart::refresh() {
    const AutostartStatus before = status_;
    struct stat st {};
    if (::stat(unitPath_.c_str(), &st) != 0) {
        status_.state = AutostartStatus::State::NotInstalled;
    } else {
        // enable で作られるのはシンボリックリンクなので lstat で有無を見る
        status_.state = ::lstat(linkPath_.c_str(), &st) == 0 ? AutostartStatus::State::Enabled
                                                              : AutostartStatus::State::Disabled;
    }
    return before.state != status_.state;
}

bool Autostart::request(bool enable) {
    if (child_ > 0) return false;
    refresh();
    if (status_.state == AutostartStatus::State::NotInstalled) return false;
    const bool already = enable == (status_.state == AutostartStatus::State::Enabled);
    if (already) return false;
    if (systemctl_.empty()) {
        std::fprintf(stderr, "[自動起動] systemctl が見つかりません\n");
        status_.failed = true;
        return false;
    }

    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        std::fprintf(stderr, "[自動起動] pipe に失敗: %s\n", std::strerror(errno));
        status_.failed = true;
        return false;
    }
    // 子に渡す引数は fork の前に作っておく（子では async-signal-safe な呼び出しだけにする）
    const char* verb = enable ? "enable" : "disable";
    char* const argv[] = {const_cast<char*>(systemctl_.c_str()), const_cast<char*>("--user"),
                          const_cast<char*>(verb), const_cast<char*>(kUnitName), nullptr};
    const pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "[自動起動] fork に失敗: %s\n", std::strerror(errno));
        ::close(fds[0]);
        ::close(fds[1]);
        status_.failed = true;
        return false;
    }
    if (pid == 0) {
        // 子: 標準出力と標準エラーをパイプへ、標準入力は /dev/null。ほかの fd は閉じてから systemctl に置き換わる
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        ::close_range(3, ~0U, 0);
        ::execv(argv[0], argv);
        ::_exit(127);
    }
    ::close(fds[1]);
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    errorPipe_ = fds[0];
    child_ = pid;
    childOutput_.clear();
    requestedEnable_ = enable;
    status_.busy = true;
    status_.failed = false;
    std::fprintf(stderr, "[自動起動] systemctl --user %s %s を実行します（PID %d）\n", verb, kUnitName,
                 static_cast<int>(pid));
    return true;
}

void Autostart::drainPipe() {
    if (errorPipe_ < 0) return;
    char buffer[512];
    while (true) {
        const ssize_t n = ::read(errorPipe_, buffer, sizeof(buffer));
        if (n <= 0) break;
        if (childOutput_.size() < 4096) childOutput_.append(buffer, static_cast<size_t>(n));
    }
}

bool Autostart::poll() {
    if (child_ <= 0) return false;
    drainPipe();
    int status = 0;
    const pid_t done = ::waitpid(child_, &status, WNOHANG);
    if (done == 0) return false;  // まだ実行中
    drainPipe();
    ::close(errorPipe_);
    errorPipe_ = -1;
    child_ = -1;
    status_.busy = false;

    const bool ok = done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    // systemctl の出力（Created symlink ... / Removed ...）は改行をつなげて 1 行でログに残す
    std::string output = childOutput_;
    for (char& c : output) {
        if (c == '\n') c = ' ';
    }
    const char* verb = requestedEnable_ ? "enable" : "disable";
    if (ok) {
        std::fprintf(stderr, "[自動起動] %s が終わりました: %s\n", verb, output.empty() ? "（出力なし）" : output.c_str());
    } else {
        const int code = done > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        std::fprintf(stderr, "[自動起動] %s に失敗しました（終了コード %d）: %s\n", verb, code,
                     output.empty() ? "（出力なし）" : output.c_str());
    }
    status_.failed = !ok;
    refresh();
    return true;
}
