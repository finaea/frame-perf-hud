// GPU 使用率のテスト。作り物の DRM fdinfo とシンボリックリンクだけを使い、本物の /proc や GPU には触らない。
#include "gpu_usage.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

struct GpuUsageTestAccess {
    static void scan(GpuUsage& gpu, const std::string& root, double now) {
        gpu.rescan(root.c_str());
        gpu.lastScan_ = now;
    }
};

namespace {

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void near(double actual, double expected) {
    check(std::isfinite(actual) && std::fabs(actual - expected) < 1e-9, "unexpected GPU usage");
}

struct ProcTree {
    std::string root;
    ProcTree() {
        char path[] = "/tmp/frame-perf-gpu-XXXXXX";
        const char* created = ::mkdtemp(path);
        check(created != nullptr, "mkdtemp failed");
        root = created;
        std::filesystem::create_directories(root + "/123/fd");
        std::filesystem::create_directory(root + "/123/fdinfo");
    }
    ~ProcTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::string path(const char* fd) const { return root + "/123/fdinfo/" + fd; }
    void write(const char* fd, long client, uint64_t ns) const {
        const auto link = root + "/123/fd/" + fd;
        if (::access(link.c_str(), F_OK) != 0 && !std::filesystem::is_symlink(link)) {
            check(::symlink("/dev/dri/renderD128", link.c_str()) == 0, "symlink failed");
        }
        std::ofstream file(path(fd));
        file << "pos:\t0\nflags:\t02100002\ndrm-client-id:\t" << client << "\ndrm-engine-gpu:\t" << ns << " ns\n";
        check(static_cast<bool>(file), "fixture write failed");
    }
};

void scanReuseAndCounterReset() {
    ProcTree proc;
    GpuUsage gpu;
    proc.write("4", 7, 100000000);
    proc.write("5", 7, 100000000);  // dup された fd で同じクライアントを 2 回数えない
    GpuUsageTestAccess::scan(gpu, proc.root, 0);
    check(gpu.clientCount() == 1, "duplicate client counted twice");

    // 走査のときに fdinfo は読んである。消しても値が出れば、update が読み直していないと分かる
    std::filesystem::remove(proc.path("4"));
    std::filesystem::remove(proc.path("5"));
    check(std::isnan(gpu.update(0)), "first GPU sample needs elapsed time");
    check(gpu.clientCount() == 1, "scan sample was unnecessarily read again");
    proc.write("4", 7, 300000000);
    proc.write("5", 7, 300000000);
    near(gpu.update(2), 10);

    // 知っているクライアントを走査し直しても前回の値は引き継ぐ（働いていなければ 0）
    GpuUsageTestAccess::scan(gpu, proc.root, 4);
    near(gpu.update(4), 0);
    proc.write("4", 7, 100000000);
    proc.write("5", 7, 100000000);
    check(std::isnan(gpu.update(6)), "counter reset must not underflow");
    proc.write("4", 7, 500000000);
    proc.write("5", 7, 500000000);
    near(gpu.update(8), 20);
    proc.write("4", 8, 500000000);
    proc.write("5", 8, 500000000);
    check(std::isnan(gpu.update(10)) && gpu.clientCount() == 0, "reused fd kept the wrong client");
}

void dormantAndSaturation() {
    ProcTree proc;
    GpuUsage gpu;
    proc.write("4", 9, 0);
    GpuUsageTestAccess::scan(gpu, proc.root, 0);
    check(std::isnan(gpu.update(0)), "dormant client must be unavailable");
    proc.write("4", 9, 500000000);
    check(std::isnan(gpu.update(2)), "dormant client should wait for the next scan");
    GpuUsageTestAccess::scan(gpu, proc.root, 4);
    near(gpu.update(4), 0);  // 読んでいなかった間にたまった分を 1 回の間隔に押しつけない
    proc.write("4", 9, 900000000);
    near(gpu.update(6), 20);
    proc.write("4", 9, 9900000000);
    near(gpu.update(8), 100);
    std::filesystem::remove(proc.path("4"));
    check(std::isnan(gpu.update(10)) && gpu.clientCount() == 0, "closed fd was not removed");
}

}  // namespace

int main() {
    try {
        scanReuseAndCounterReset();
        dormantAndSaturation();
        std::puts("GPU usage tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
