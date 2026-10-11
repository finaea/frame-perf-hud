// GPU 使用率の目安の実装。
#include "gpu_usage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

bool GpuUsage::readFdinfo(const std::string& path, long& clientId, uint64_t& busyNs) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;  // プロセスが終わった・fd が閉じられた
    char buffer[2048];
    const ssize_t n = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (n <= 0) return false;
    buffer[n] = '\0';
    const char* id = std::strstr(buffer, "drm-client-id:");
    const char* busy = std::strstr(buffer, "drm-engine-gpu:");
    if (id == nullptr || busy == nullptr) return false;
    clientId = std::strtol(id + std::strlen("drm-client-id:"), nullptr, 10);
    busyNs = std::strtoull(busy + std::strlen("drm-engine-gpu:"), nullptr, 10);
    return true;
}

void GpuUsage::rescan() {
    // 読み慣れた使用中のクライアントは fdinfo を読み直さない（ゲームの fdinfo はドライバがメモリを数えるので
    // 1 回 1.6ms ほどかかる）。fd が別のものに使い回されていたら update() の読み直しで気づいて外れる
    std::map<std::string, long> knownActive;
    for (const auto& entry : clients_) {
        if (!entry.second.dormant) knownActive.emplace(entry.second.fdinfoPath, entry.first);
    }
    std::map<long, Client> found;
    const uid_t me = ::getuid();
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) return;
    while (const dirent* entry = ::readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        const std::string base = std::string("/proc/") + entry->d_name;
        // 他のユーザーの fdinfo は読めないので、自分のプロセスだけ見る
        struct stat st {};
        if (::stat(base.c_str(), &st) != 0 || st.st_uid != me) continue;
        DIR* fds = ::opendir((base + "/fd").c_str());
        if (fds == nullptr) continue;
        while (const dirent* f = ::readdir(fds)) {
            if (f->d_name[0] == '.') continue;
            char target[64];
            const ssize_t len = ::readlinkat(::dirfd(fds), f->d_name, target, sizeof(target) - 1);
            if (len <= 0) continue;
            target[len] = '\0';
            if (std::strncmp(target, "/dev/dri/", 9) != 0) continue;
            const std::string path = base + "/fdinfo/" + f->d_name;
            const auto known = knownActive.find(path);
            if (known != knownActive.end()) {
                found.emplace(known->second, clients_.at(known->second));
                continue;
            }
            long clientId = 0;
            uint64_t busyNs = 0;
            if (!readFdinfo(path, clientId, busyNs)) continue;
            if (found.count(clientId) != 0) continue;  // dup された fd は同じクライアント
            Client client;
            client.fdinfoPath = path;
            // 一度も GPU を使っていないクライアント（49 個中 39 個ほど）は、次の走査まで読み直さない
            client.dormant = busyNs == 0;
            const auto old = clients_.find(clientId);
            if (old != clients_.end() && !old->second.dormant) {
                // 前から読んでいたクライアントは前回の値を引き継ぐ（差分が途切れないように）
                client.lastBusyNs = old->second.lastBusyNs;
                client.haveLast = old->second.haveLast;
            } else {
                // 新しく見つけた・休眠から動き出したクライアントは今の値を起点にする
                // （休眠中にたまった分を 1 回にまとめて足すと跳ねるため）
                client.lastBusyNs = busyNs;
                client.haveLast = true;
            }
            found.emplace(clientId, client);
        }
        ::closedir(fds);
    }
    ::closedir(proc);
    clients_.swap(found);
}

double GpuUsage::update(double now) {
    if (lastScan_ < 0 || now - lastScan_ >= kRescanSec) {
        lastScan_ = now;
        rescan();
    }
    uint64_t deltaNs = 0;
    bool anyDelta = false;
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->second.dormant) {
            ++it;
            continue;
        }
        long clientId = 0;
        uint64_t busyNs = 0;
        if (!readFdinfo(it->second.fdinfoPath, clientId, busyNs) || clientId != it->first) {
            it = clients_.erase(it);  // 閉じられた（fd 番号が別のものに再利用された場合も含む）
            continue;
        }
        if (it->second.haveLast && busyNs >= it->second.lastBusyNs) {
            deltaNs += busyNs - it->second.lastBusyNs;
            anyDelta = true;
        }
        it->second.lastBusyNs = busyNs;
        it->second.haveLast = true;
        ++it;
    }
    const double elapsed = lastTime_ < 0 ? 0.0 : now - lastTime_;
    lastTime_ = now;
    if (!anyDelta || elapsed <= 0.0) return std::numeric_limits<double>::quiet_NaN();
    // 注: 走査のあいだに新しく GPU を使い始めたクライアントの分は、次の走査（最大 10 秒後）から数える
    // 目安の値: 複数のクライアントの仕事が GPU 上で重なると合計が経過時間を超えることがあるので 100% で止める
    return std::clamp(100.0 * static_cast<double>(deltaNs) / (elapsed * 1e9), 0.0, 100.0);
}
