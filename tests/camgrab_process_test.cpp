// T09: end-to-end tests of the camera helper process. Linux only.
// Usage: camgrab-process-test <path to the built fph-camgrab>
// The helper needn't be installed (no capability is fine). OUTWARD_ON / EYE_ON are never sent: they would make
// the helper look at XRService / the eye tracker.
#include "camera_helper.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, #expr);                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

using namespace camhelper;

const char* helperPath = nullptr;

double nowSec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

/** Waits up to timeoutMs for one message. @return the read result (WouldBlock = timed out) */
HelperProcess::ReadResult readMsg(HelperProcess& h, int timeoutMs, Message& m) {
    const double end = nowSec() + timeoutMs / 1000.0;
    for (;;) {
        const auto r = h.read(m);
        if (r != HelperProcess::ReadResult::WouldBlock) return r;
        const int left = int((end - nowSec()) * 1000.0);
        if (left <= 0) return r;
        pollfd p{h.fd(), POLLIN, 0};
        poll(&p, 1, left);
    }
}

/** @return true if the process no longer runs (gone, or a zombie waiting for its reaper) */
bool pidGone(pid_t pid) {
    if (kill(pid, 0) != 0 && errno == ESRCH) return true;
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return true;
    const size_t close = line.rfind(')');
    return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
}

bool waitGone(pid_t pid, int timeoutMs) {
    const double end = nowSec() + timeoutMs / 1000.0;
    while (nowSec() < end) {
        if (pidGone(pid)) return true;
        usleep(5 * 1000);
    }
    return pidGone(pid);
}

int highFd(int fd) {
    const int n = fcntl(fd, F_DUPFD, 10);
    close(fd);
    return n;
}

/** fork+exec the helper with `fd3` as its fd 3. @return the pid */
pid_t spawnRaw(int fd3, const char* closeInChild = nullptr) {
    (void)closeInChild;
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fd3, 3);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        execl(helperPath, "fph-camgrab", static_cast<char*>(nullptr));
        _exit(127);
    }
    return pid;
}

/** @return exit code, or 1000 + signal, or -1 if it didn't end within timeoutMs */
int waitExit(pid_t pid, int timeoutMs) {
    const double end = nowSec() + timeoutMs / 1000.0;
    for (;;) {
        int st = 0;
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
        if (nowSec() >= end) return -1;
        usleep(5 * 1000);
    }
}

bool expectHello(HelperProcess& h, uint32_t* ptrace = nullptr) {
    Message m;
    if (readMsg(h, 3000, m) != HelperProcess::ReadResult::Message) return false;
    if (m.type != FPH_MSG_HELLO) return false;
    if (ptrace) *ptrace = m.hello.has_ptrace;
    return m.hello.helper_pid == uint32_t(h.pid());
}

/** Sends raw bytes (optionally with fds) and expects exactly one FPH_MSG_ERROR with `code`. */
void expectErrorFor(HelperProcess& h, const void* data, size_t len, uint32_t code, const char* what,
                    const std::vector<int>& fds = {}) {
    iovec iov{const_cast<void*>(data), len};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    alignas(8) char ctrl[CMSG_SPACE(sizeof(int) * 4)];
    if (!fds.empty()) {
        std::memset(ctrl, 0, sizeof(ctrl));
        mh.msg_control = ctrl;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
    }
    if (sendmsg(h.fd(), &mh, MSG_NOSIGNAL) != ssize_t(len)) {
        std::printf("FAIL %s: sendmsg: %s\n", what, std::strerror(errno));
        ++failures;
        return;
    }
    Message m;
    const auto r = readMsg(h, 2000, m);
    if (r != HelperProcess::ReadResult::Message || m.type != FPH_MSG_ERROR || m.error.code != code) {
        std::printf("FAIL %s: expected ERROR %u, got read result %d type %d code %u\n", what, code, int(r),
                    int(m.type), m.error.code);
        ++failures;
    }
    CHECK(kill(h.pid(), 0) == 0);  // still alive
}

// ------------------------------------------------------------------------------------------------ tests

void testHelloAndRejections() {
    HelperProcess h;
    std::string err;
    if (!h.start(helperPath, &err)) {
        std::printf("FAIL start: %s\n", err.c_str());
        ++failures;
        return;
    }
    uint32_t ptrace = 99;
    CHECK(expectHello(h, &ptrace));
    CHECK(ptrace == 0);  // not installed with the capability, so none in the effective set
    const pid_t pid = h.pid();

    const fph_cmd good = makeCommand(FPH_OP_OUTWARD_OFF);
    const char two[2] = {0x46, 0x50};
    expectErrorFor(h, two, sizeof(two), FPH_ERR_BAD_LENGTH, "2 bytes");

    unsigned char seventeen[17];
    std::memset(seventeen, 0, sizeof(seventeen));
    std::memcpy(seventeen, &good, sizeof(good));
    expectErrorFor(h, seventeen, sizeof(seventeen), FPH_ERR_BAD_LENGTH, "17 bytes");

    unsigned char fifteen[15];
    std::memcpy(fifteen, &good, sizeof(fifteen));
    expectErrorFor(h, fifteen, sizeof(fifteen), FPH_ERR_BAD_LENGTH, "15 bytes");

    fph_cmd c = good;
    c.magic = 0x12345678;
    expectErrorFor(h, &c, sizeof(c), FPH_ERR_BAD_MAGIC, "bad magic");

    c = good;
    c.op = 9;
    expectErrorFor(h, &c, sizeof(c), FPH_ERR_BAD_OP, "bad op");
    c.op = 0;
    expectErrorFor(h, &c, sizeof(c), FPH_ERR_BAD_OP, "op 0");

    c = makeCommand(FPH_OP_RECHECK, 7, 0, 0);
    expectErrorFor(h, &c, sizeof(c), FPH_ERR_BAD_ARGS, "recheck cam 7");
    c = makeCommand(FPH_OP_RECHECK, 0, FPH_MAX_BUFS, 0);
    expectErrorFor(h, &c, sizeof(c), FPH_ERR_BAD_ARGS, "recheck index too big");
    c = makeCommand(FPH_OP_EYE_OFF, 1, 0, 0);
    expectErrorFor(h, &c, sizeof(c), FPH_ERR_BAD_ARGS, "EYE_OFF with a cam");
    c = good;
    c.reserved = 5;
    expectErrorFor(h, &c, sizeof(c), FPH_ERR_BAD_ARGS, "reserved != 0");

    // a valid command with an SCM_RIGHTS fd attached
    int p[2];
    CHECK(pipe(p) == 0);
    expectErrorFor(h, &good, sizeof(good), FPH_ERR_BAD_LENGTH, "valid cmd + fd", {p[0]});
    // two fds
    expectErrorFor(h, &good, sizeof(good), FPH_ERR_BAD_LENGTH, "valid cmd + 2 fds", {p[0], p[1]});
    close(p[0]);
    close(p[1]);

    // still serving after all that: RECHECK with outward off -> RECHECK INVALID
    CHECK(h.sendRecheck(1, 3, 42));
    Message m;
    CHECK(readMsg(h, 2000, m) == HelperProcess::ReadResult::Message);
    CHECK(m.type == FPH_MSG_RECHECK);
    CHECK(m.recheck.result == FPH_RECHECK_INVALID);
    CHECK(m.recheck.cam == 1 && m.recheck.index == 3 && m.recheck.seq == 42);

    // OUTWARD_OFF / EYE_OFF while off: CAM_GONE x4 (OFF) / EYE_GONE (OFF). Neither touches another process.
    CHECK(h.sendOutward(false));
    for (int i = 0; i < FPH_OUTWARD_CAMS; i++) {
        CHECK(readMsg(h, 2000, m) == HelperProcess::ReadResult::Message);
        CHECK(m.type == FPH_MSG_CAM_GONE && m.gone.cam == i && m.gone.reason == FPH_GONE_OFF);
    }
    CHECK(h.sendEye(false));
    CHECK(readMsg(h, 2000, m) == HelperProcess::ReadResult::Message);
    CHECK(m.type == FPH_MSG_EYE_GONE && m.gone.reason == FPH_GONE_OFF);

    CHECK(kill(pid, 0) == 0);
    // nothing else queued
    CHECK(readMsg(h, 150, m) == HelperProcess::ReadResult::WouldBlock);

    // stop() ends it well before its 1 s SIGKILL fallback: EOF makes the helper exit by itself
    const double t0 = nowSec();
    h.stop();
    CHECK(nowSec() - t0 < 0.5);
    CHECK(!h.running());
    CHECK(pidGone(pid));
}

void testEofExit() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    sv[0] = highFd(sv[0]);
    sv[1] = highFd(sv[1]);
    const pid_t pid = spawnRaw(sv[1]);
    close(sv[1]);
    CHECK(pid > 0);
    // wait for HELLO so the helper is past its setup
    pollfd p{sv[0], POLLIN, 0};
    CHECK(poll(&p, 1, 3000) == 1);
    Message m;
    fph_msg_hello hello{};
    CHECK(recv(sv[0], &hello, sizeof(hello), 0) == ssize_t(sizeof(hello)));
    CHECK(hello.hdr.type == FPH_MSG_HELLO);
    CHECK(hello.helper_pid == uint32_t(pid));
    close(sv[0]);  // EOF
    CHECK(waitExit(pid, 1000) == 0);
    (void)m;
}

void testEofWhileHelloUnread() {
    // closing our end before even reading HELLO still ends it
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    sv[0] = highFd(sv[0]);
    sv[1] = highFd(sv[1]);
    const pid_t pid = spawnRaw(sv[1]);
    close(sv[1]);
    close(sv[0]);
    CHECK(pid > 0);
    CHECK(waitExit(pid, 2000) == 0);
}

void testFd3NotSocket() {
    int p[2];
    CHECK(pipe(p) == 0);
    p[0] = highFd(p[0]);
    const pid_t pid = spawnRaw(p[0]);
    close(p[0]);
    close(p[1]);
    CHECK(pid > 0);
    CHECK(waitExit(pid, 2000) == 2);

    // a SOCK_STREAM unix socket isn't SEQPACKET either
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    sv[1] = highFd(sv[1]);
    const pid_t pid2 = spawnRaw(sv[1]);
    close(sv[1]);
    CHECK(pid2 > 0);
    CHECK(waitExit(pid2, 2000) == 2);
    close(sv[0]);
}

void testParentDeath() {
    int ctl[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) == 0);
    const pid_t mid = fork();
    if (mid == 0) {
        close(ctl[0]);
        HelperProcess h;
        std::string err;
        if (!h.start(helperPath, &err)) _exit(10);
        if (!expectHello(h)) _exit(11);  // HELLO is sent after the helper armed PDEATHSIG
        // hand the grandparent the helper pid and a copy of the agent-side socket
        int pid = int(h.pid());
        iovec iov{&pid, sizeof(pid)};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        alignas(8) char ctrl[CMSG_SPACE(sizeof(int))];
        std::memset(ctrl, 0, sizeof(ctrl));
        mh.msg_control = ctrl;
        mh.msg_controllen = sizeof(ctrl);
        cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        const int fd = h.fd();
        std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
        if (sendmsg(ctl[1], &mh, 0) != ssize_t(sizeof(pid))) _exit(12);
        _exit(0);  // no destructors: the socket isn't closed by stop(), only by the exit itself
    }
    close(ctl[1]);
    CHECK(mid > 0);

    int pid = 0;
    iovec iov{&pid, sizeof(pid)};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    alignas(8) char ctrl[CMSG_SPACE(sizeof(int))];
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof(ctrl);
    const ssize_t n = recvmsg(ctl[0], &mh, 0);
    CHECK(n == ssize_t(sizeof(pid)));
    int held = -1;
    if (cmsghdr* c = CMSG_FIRSTHDR(&mh)) std::memcpy(&held, CMSG_DATA(c), sizeof(int));
    CHECK(held >= 0);
    int st = 0;
    CHECK(waitpid(mid, &st, 0) == mid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    if (n != ssize_t(sizeof(pid)) || held < 0) return;

    // We hold the agent end of the socket, so only the parent's death can end the helper.
    CHECK(waitGone(pid, 1000));
    close(held);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <path to fph-camgrab>\n", argv[0]);
        return 2;
    }
    helperPath = argv[1];
    if (access(helperPath, X_OK) != 0) {
        std::printf("cannot execute %s: %s\n", helperPath, std::strerror(errno));
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    testHelloAndRejections();
    testEofExit();
    testEofWhileHelloUnread();
    testFd3NotSocket();
    testParentDeath();
    if (failures) {
        std::printf("camgrab_process_test: %d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("camgrab_process_test: all passed\n");
    return 0;
}
