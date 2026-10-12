/*
 * fph-camgrab - frame-perf-hud's camera helper.
 *
 * Hands the headset agent the dmabufs behind the Steam Frame's camera frames, plus "newest frame" reports.
 * The frames sit in buffers held by XRService (outward tracking cams) and shared with the eye tracker (eye
 * cams). Borrowing them needs pidfd_getfd(), which kernel.yama.ptrace_scope=1 blocks for a normal user, so
 * this file is installed root-owned with cap_sys_ptrace=ep (install-camera-helper.sh) and kept tiny: every
 * change to it needs sudo again. All other logic (copying, conversion, dark frames, eye rings) is the agent's.
 *
 * Rules it keeps, because tracking must never be disturbed:
 *   - Read-only V4L2: VIDIOC_G_FMT at discovery and VIDIOC_QUERYBUF, nothing else. Never QBUF / DQBUF /
 *     STREAMON / STREAMOFF / S_FMT / REQBUFS. XRService's V4L2 fd is borrowed per use and closed right away
 *     (camviewer.py:9-10) and never handed to the agent: only dmabuf fds are.
 *   - Never signals, ptrace-attaches or stops XRService or the eye tracker.
 *   - Fixed targets by /proc/<pid>/exe, owned by the user who started the helper. The agent can only send
 *     the fixed commands in fph_camgrab_proto.h: no paths, pids or fds.
 *
 * Lifetime: started by the agent with a SOCK_SEQPACKET socketpair end as fd 3. Exits on socket EOF, on the
 * parent's death (PR_SET_PDEATHSIG, set here because exec of a capability binary clears it), and when a
 * target it watches exits (its pidfd turns readable), and when the agent stops reading for 1 s. The agent
 * respawns it, at most once per 5 s. A target or camera that isn't there yet is reported as GONE and the
 * helper stays up: the agent re-sends the ON command until it is (the retry policy lives in the agent).
 *
 * Build: gcc -O2 -Wall -Wextra -o fph-camgrab fph-camgrab.c   (Linux only; no libraries)
 * MIT licensed, part of frame-perf-hud.
 */
#define _GNU_SOURCE

#include "fph_camgrab_proto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/capability.h>
#include <linux/kcmp.h>
#include <linux/videodev2.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

#define XR_EXE "/opt/steamvr/drivers/cv/bin/linuxarm64/XRService"
#define ET_EXE "/opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking"

#define SOCK_FD 3

/* found by install.sh with grep -a (kept in the binary by `used`) */
__attribute__((used)) static const char version_marker[] = FPH_HELPER_VERSION_MARKER;

/* ======================================================================= pure parsers (unit-tested) */

/* What /proc/<pid>/fdinfo/<fd> says about a dmabuf. */
struct fph_fdinfo {
    unsigned long long size;
    unsigned long long ino;
    char exp_name[32];
    int have_size;
    int have_ino;
};

/* Parses fdinfo text ("size:\t123\nino:\t4\nexp_name:\tudmabuf\n..."). @return 1 if size and ino were found */
int fph_parse_fdinfo(const char *text, struct fph_fdinfo *out)
{
    memset(out, 0, sizeof(*out));
    const char *line = text;
    while (line && *line) {
        unsigned long long v;
        char name[32];
        if (sscanf(line, "size: %llu", &v) == 1) {
            out->size = v;
            out->have_size = 1;
        } else if (sscanf(line, "ino: %llu", &v) == 1) {
            out->ino = v;
            out->have_ino = 1;
        } else if (sscanf(line, "exp_name: %31s", name) == 1) {
            memcpy(out->exp_name, name, sizeof(name));
        }
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return out->have_size && out->have_ino;
}

/*
 * Does a readlink of /proc/<pid>/exe name exactly this binary? n is readlink's return value (no NUL).
 * A replaced binary reads "<path> (deleted)" and does not match, nor does a longer or shorter path.
 */
int fph_exe_matches(const char *link, long n, const char *want)
{
    size_t len = strlen(want);
    return n >= 0 && (size_t)n == len && memcmp(link, want, len) == 0;
}

/* Parses the real uid from /proc/<pid>/status text ("Uid:\t1000\t1000\t1000\t1000"). @return 1 if found */
int fph_parse_status_uid(const char *text, unsigned long *uid)
{
    const char *line = text;
    while (line && *line) {
        if (sscanf(line, "Uid: %lu", uid) == 1)
            return 1;
        line = strchr(line, '\n');
        if (line)
            line++;
    }
    return 0;
}

/* @return the cam id (0..FPH_OUTWARD_CAMS-1) for /dev/video<node>, or -1 */
int fph_node_to_cam(int node)
{
    static const int nodes[FPH_OUTWARD_CAMS] = FPH_OUTWARD_NODES;
    for (int i = 0; i < FPH_OUTWARD_CAMS; i++)
        if (nodes[i] == node)
            return i;
    return -1;
}

/* Parses an fd's link target "/dev/video<N>" (exactly). @return N, or -1 for anything else */
int fph_parse_video_node(const char *target, long n)
{
    static const char prefix[] = "/dev/video";
    const long plen = (long)sizeof(prefix) - 1;
    if (n <= plen || n > plen + 3 || memcmp(target, prefix, (size_t)plen) != 0)
        return -1;
    int node = 0;
    for (long i = plen; i < n; i++) {
        if (target[i] < '0' || target[i] > '9')
            return -1;
        node = node * 10 + (target[i] - '0');
    }
    if (n > plen + 1 && target[plen] == '0')
        return -1; /* no leading zeros: "/dev/video03" is not video3 */
    return node;
}

/*
 * The row pitch of plane 0, from G_FMT's sizeimage rather than bytesperline: qcom-camss reports 1056 for an
 * NV12 frame that is really 1152 bytes per row (camviewer.py:182-187).
 * @return the pitch, or 0 if the format isn't NV12 / GREY or the numbers don't add up
 */
uint32_t fph_pitch_from_fmt(uint32_t pixelformat, uint32_t width, uint32_t height, uint32_t sizeimage)
{
    if (width == 0 || height == 0)
        return 0;
    uint64_t rows;
    if (pixelformat == V4L2_PIX_FMT_NV12)
        rows = (uint64_t)height * 3 / 2;
    else if (pixelformat == V4L2_PIX_FMT_GREY)
        rows = height;
    else
        return 0;
    uint64_t pitch = sizeimage / rows;
    if (pitch < width || pitch > 65536)
        return 0;
    return (uint32_t)pitch;
}

/*
 * Checks one packet from the agent.
 * @param len what recvmsg returned; @param msg_flags its msg_flags; @param nfds fds that came with it
 * @return 0 and *out filled if it's a valid command, else an fph_error_code
 */
int fph_check_cmd(const void *buf, long len, int msg_flags, int nfds, struct fph_cmd *out)
{
    if (len != (long)sizeof(struct fph_cmd) || (msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || nfds != 0)
        return FPH_ERR_BAD_LENGTH;
    memcpy(out, buf, sizeof(*out));
    if (out->magic != FPH_CMD_MAGIC)
        return FPH_ERR_BAD_MAGIC;
    if (out->reserved != 0)
        return FPH_ERR_BAD_ARGS;
    switch (out->op) {
    case FPH_OP_OUTWARD_ON:
    case FPH_OP_OUTWARD_OFF:
    case FPH_OP_EYE_ON:
    case FPH_OP_EYE_OFF:
        return (out->cam || out->index || out->seq) ? FPH_ERR_BAD_ARGS : 0;
    case FPH_OP_RECHECK:
        return (out->cam >= FPH_OUTWARD_CAMS || out->index >= FPH_MAX_BUFS) ? FPH_ERR_BAD_ARGS : 0;
    default:
        return FPH_ERR_BAD_OP;
    }
}

#ifndef FPH_CAMGRAB_NO_MAIN

/* ======================================================================= runtime */

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("fph-camgrab: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int pidfd_open_(pid_t pid) { return (int)syscall(SYS_pidfd_open, pid, 0); }
static int pidfd_getfd_(int pidfd, int fd) { return (int)syscall(SYS_pidfd_getfd, pidfd, fd, 0); }

struct target {
    const char *exe;
    pid_t pid;
    int pidfd;
};

static struct target xr = {XR_EXE, 0, -1};
static struct target et = {ET_EXE, 0, -1};

struct buf {
    int xr_fd;   /* XRService's fd number for this dmabuf (QUERYBUF m.fd) */
    int held_fd; /* our dup of it, for the kcmp identity check */
    uint32_t length;
    uint64_t ino;
    uint32_t last_seq; /* last sequence reported in FRAMES */
    int reported;
};

struct cam {
    int present;
    int node;
    int xr_vfd; /* XRService's fd number for /dev/video<node> */
    dev_t rdev;
    uint32_t gen;
    uint32_t width, height, pitch, pixelformat, bytesperline, sizeimage;
    int nbufs;
    struct buf bufs[FPH_MAX_BUFS];
};

static struct cam cams[FPH_OUTWARD_CAMS];
static int outward_on;
static int eye_on;
static uint32_t eye_gen;
static int eye_fds[FPH_MAX_EYE_ARENAS];
static int eye_n;

/* ---------------------------------------------------------------- sending */

static void hdr_init(struct fph_msg_hdr *h, uint16_t type)
{
    h->magic = FPH_MSG_MAGIC;
    h->type = type;
    h->version = FPH_PROTO_VERSION;
}

static void bye_and_exit(uint32_t reason, int code);

static void send_msg(const void *msg, size_t len, const int *fds, int nfds)
{
    struct iovec iov = {(void *)msg, len};
    union {
        char buf[CMSG_SPACE(sizeof(int) * FPH_MAX_BUFS)];
        struct cmsghdr align;
    } ctrl;
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (nfds > 0) {
        memset(&ctrl, 0, sizeof(ctrl));
        mh.msg_control = ctrl.buf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    for (;;) {
        if (sendmsg(SOCK_FD, &mh, MSG_NOSIGNAL) >= 0)
            return;
        if (errno == EINTR)
            continue;
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
            exit(0); /* the agent is gone */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            logf_("the agent stopped reading for 1 s, exiting");
            exit(0);
        }
        logf_("send failed: %s", strerror(errno));
        bye_and_exit(FPH_BYE_SOCKET_ERROR, 1);
    }
}

static void bye_and_exit(uint32_t reason, int code)
{
    struct fph_msg_bye m;
    memset(&m, 0, sizeof(m));
    hdr_init(&m.hdr, FPH_MSG_BYE);
    m.reason = reason;
    send(SOCK_FD, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
    exit(code);
}

static void send_gone(uint16_t type, int cam, int reason, int err)
{
    struct fph_msg_gone m;
    memset(&m, 0, sizeof(m));
    hdr_init(&m.hdr, type);
    m.cam = (uint8_t)cam;
    m.reason = (uint8_t)reason;
    m.err = err;
    send_msg(&m, sizeof(m), NULL, 0);
}

static void send_error(uint32_t code)
{
    struct fph_msg_error m;
    memset(&m, 0, sizeof(m));
    hdr_init(&m.hdr, FPH_MSG_ERROR);
    m.code = code;
    send_msg(&m, sizeof(m), NULL, 0);
}

/* ---------------------------------------------------------------- targets */

static int gone_reason_for(int err) { return (err == EPERM || err == EACCES) ? FPH_GONE_DENIED : FPH_GONE_FAILED; }

/*
 * Finds the target by its exe path, owned by our real uid (the user who started us), and opens a pidfd.
 * The exe and owner are checked again after pidfd_open, which pins the pid against reuse.
 * @return 0, or an errno (ESRCH if not running)
 */
static int target_open(struct target *t)
{
    if (t->pidfd >= 0)
        return 0;
    DIR *d = opendir("/proc");
    if (!d)
        return errno;
    const uid_t me = getuid();
    struct dirent *e;
    int result = ESRCH;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        const pid_t pid = (pid_t)atoi(e->d_name);
        char path[64], link[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%d/exe", (int)pid);
        ssize_t n = readlink(path, link, sizeof(link));
        if (!fph_exe_matches(link, (long)n, t->exe))
            continue;
        int pidfd = pidfd_open_(pid);
        if (pidfd < 0) {
            result = errno;
            continue;
        }
        n = readlink(path, link, sizeof(link)); /* path is still /proc/<pid>/exe */
        unsigned long uid = (unsigned long)-1;
        snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
        int f = open(path, O_RDONLY | O_CLOEXEC);
        if (f >= 0) {
            char status[4096];
            ssize_t got = read(f, status, sizeof(status) - 1);
            close(f);
            if (got > 0) {
                status[got] = 0;
                fph_parse_status_uid(status, &uid);
            }
        }
        if (!fph_exe_matches(link, (long)n, t->exe) || uid != (unsigned long)me) {
            close(pidfd);
            continue;
        }
        t->pid = pid;
        t->pidfd = pidfd;
        result = 0;
        break;
    }
    closedir(d);
    return result;
}

static void target_close(struct target *t)
{
    if (t->pidfd >= 0)
        close(t->pidfd);
    t->pidfd = -1;
    t->pid = 0;
}

/* @return 1 if the fd we hold is a dmabuf (its link reads "/dmabuf:...") */
static int is_dmabuf(int fd)
{
    char path[64], link[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    return n >= 8 && memcmp(link, "/dmabuf:", 8) == 0;
}

/* @return 0 if our fd and the target's fd number are the same open file */
static int same_file(const struct target *t, int ours, int theirs)
{
    return (int)syscall(SYS_kcmp, getpid(), t->pid, KCMP_FILE, ours, theirs);
}

/* ---------------------------------------------------------------- outward cams */

static void cam_drop(struct cam *c)
{
    for (int i = 0; i < c->nbufs; i++)
        if (c->bufs[i].held_fd >= 0)
            close(c->bufs[i].held_fd);
    const uint32_t gen = c->gen;
    memset(c, 0, sizeof(*c));
    c->gen = gen;
}

/* Borrows XRService's V4L2 fd for this cam. @return our fd (close it right away after use), or -1 / errno */
static int cam_borrow(const struct cam *c)
{
    int fd = pidfd_getfd_(xr.pidfd, c->xr_vfd);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) || (c->rdev && st.st_rdev != c->rdev)) {
        close(fd);
        errno = ESTALE;
        return -1;
    }
    return fd;
}

static int querybuf(int vfd, uint32_t index, struct v4l2_buffer *b, struct v4l2_plane *planes)
{
    memset(b, 0, sizeof(*b));
    memset(planes, 0, sizeof(*planes) * VIDEO_MAX_PLANES);
    b->index = index;
    b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b->m.planes = planes;
    b->length = VIDEO_MAX_PLANES;
    return ioctl(vfd, VIDIOC_QUERYBUF, b);
}

/*
 * Reads the cam's format and grabs its buffers (G_FMT + QUERYBUF only), then sends FPH_MSG_CAM.
 * c->node and c->xr_vfd must be set. On failure sends FPH_MSG_CAM_GONE and leaves the cam absent.
 */
static void cam_discover(int id)
{
    struct cam *c = &cams[id];
    int node = c->node, xr_vfd = c->xr_vfd;
    cam_drop(c);
    c->node = node;
    c->xr_vfd = xr_vfd;

    int vfd = cam_borrow(c);
    if (vfd < 0) {
        int err = errno;
        logf_("video%d: borrow fd %d: %s", node, xr_vfd, strerror(err));
        send_gone(FPH_MSG_CAM_GONE, id, gone_reason_for(err), err);
        return;
    }
    int reason = FPH_GONE_FORMAT, err = 0;
    /* the fd number came from a directory scan: make sure it still is /dev/video<node> */
    char path[64], link[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", vfd);
    ssize_t n = readlink(path, link, sizeof(link));
    struct stat st;
    if (fph_parse_video_node(link, (long)n) != node || fstat(vfd, &st) != 0) {
        logf_("video%d: XRService fd %d now points elsewhere", node, xr_vfd);
        reason = FPH_GONE_NO_DEVICE;
        goto fail;
    }
    c->rdev = st.st_rdev;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(vfd, VIDIOC_G_FMT, &fmt) != 0) {
        err = errno;
        logf_("video%d: G_FMT: %s", node, strerror(err));
        goto fail;
    }
    c->width = fmt.fmt.pix_mp.width;
    c->height = fmt.fmt.pix_mp.height;
    c->pixelformat = fmt.fmt.pix_mp.pixelformat;
    c->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    c->sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    c->pitch = fph_pitch_from_fmt(c->pixelformat, c->width, c->height, c->sizeimage);
    if (!c->pitch) {
        logf_("video%d: unsupported format %.4s %ux%u sizeimage %u", node, (const char *)&c->pixelformat, c->width,
              c->height, c->sizeimage);
        goto fail;
    }
    const uint64_t need = (uint64_t)c->pitch * (c->height - 1) + c->width;

    for (uint32_t i = 0;; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        if (querybuf(vfd, i, &b, planes) != 0)
            break; /* EINVAL past the last buffer */
        if (i >= FPH_MAX_BUFS) {
            logf_("video%d: more than %d buffers", node, FPH_MAX_BUFS);
            goto fail;
        }
        if (b.memory != V4L2_MEMORY_DMABUF || b.length < 1 || planes[0].length < need) {
            logf_("video%d: buffer %u: memory %u, %u bytes (need %llu)", node, i, b.memory, planes[0].length,
                  (unsigned long long)need);
            goto fail;
        }
        struct buf *bf = &c->bufs[i];
        bf->xr_fd = planes[0].m.fd;
        bf->length = planes[0].length;
        bf->held_fd = pidfd_getfd_(xr.pidfd, bf->xr_fd);
        c->nbufs = (int)i + 1;
        if (bf->held_fd < 0) {
            err = errno;
            reason = gone_reason_for(err);
            logf_("video%d: buffer %u: grab fd %d: %s", node, i, bf->xr_fd, strerror(err));
            goto fail;
        }
        struct stat bst;
        if (!is_dmabuf(bf->held_fd) || fstat(bf->held_fd, &bst) != 0) {
            logf_("video%d: buffer %u: fd %d is not a dmabuf", node, i, bf->xr_fd);
            goto fail;
        }
        bf->ino = (uint64_t)bst.st_ino;
    }
    close(vfd);
    if (c->nbufs == 0) {
        logf_("video%d: no buffers", node);
        reason = FPH_GONE_FORMAT;
        vfd = -1;
        goto fail;
    }

    c->present = 1;
    c->gen++;
    struct fph_msg_cam m;
    memset(&m, 0, sizeof(m));
    hdr_init(&m.hdr, FPH_MSG_CAM);
    m.cam = (uint8_t)id;
    m.node = (uint8_t)node;
    m.nbufs = (uint8_t)c->nbufs;
    m.gen = c->gen;
    m.width = c->width;
    m.height = c->height;
    m.pitch = c->pitch;
    m.pixelformat = c->pixelformat;
    m.bytesperline = c->bytesperline;
    m.sizeimage = c->sizeimage;
    int fds[FPH_MAX_BUFS];
    for (int i = 0; i < c->nbufs; i++) {
        m.bufs[i].index = (uint32_t)i;
        m.bufs[i].length = c->bufs[i].length;
        m.bufs[i].ino = c->bufs[i].ino;
        fds[i] = c->bufs[i].held_fd;
    }
    send_msg(&m, sizeof(m), fds, c->nbufs);
    logf_("video%d: %ux%u %.4s pitch %u (bytesperline %u), %d buffers, gen %u", node, c->width, c->height,
          (const char *)&c->pixelformat, c->pitch, c->bytesperline, c->nbufs, c->gen);
    return;

fail:
    if (vfd >= 0)
        close(vfd);
    cam_drop(c);
    send_gone(FPH_MSG_CAM_GONE, id, reason, err);
}

static void outward_stop(int notify)
{
    for (int i = 0; i < FPH_OUTWARD_CAMS; i++) {
        cam_drop(&cams[i]);
        if (notify)
            send_gone(FPH_MSG_CAM_GONE, i, FPH_GONE_OFF, 0);
    }
    outward_on = 0;
    target_close(&xr);
}

/* Finds XRService's fds for the outward nodes (lowest fd number per node, like camviewer) and discovers each. */
static void outward_start(void)
{
    outward_on = 1;
    int err = target_open(&xr);
    if (err) {
        for (int i = 0; i < FPH_OUTWARD_CAMS; i++)
            send_gone(FPH_MSG_CAM_GONE, i, err == ESRCH ? FPH_GONE_NO_PROCESS : gone_reason_for(err), err);
        outward_on = 0;
        return;
    }
    int vfd[FPH_OUTWARD_CAMS];
    for (int i = 0; i < FPH_OUTWARD_CAMS; i++)
        vfd[i] = -1;
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", (int)xr.pid);
    DIR *d = opendir(dirpath);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        const int fd = atoi(e->d_name);
        char path[128], link[64];
        snprintf(path, sizeof(path), "%s/%d", dirpath, fd);
        ssize_t n = readlink(path, link, sizeof(link));
        int id = fph_node_to_cam(fph_parse_video_node(link, (long)n));
        if (id < 0)
            continue;
        if (vfd[id] < 0 || fd < vfd[id])
            vfd[id] = fd;
    }
    if (d)
        closedir(d);
    static const int nodes[FPH_OUTWARD_CAMS] = FPH_OUTWARD_NODES;
    for (int i = 0; i < FPH_OUTWARD_CAMS; i++) {
        if (vfd[i] < 0) {
            cam_drop(&cams[i]);
            send_gone(FPH_MSG_CAM_GONE, i, FPH_GONE_NO_DEVICE, 0);
            continue;
        }
        cams[i].node = nodes[i];
        cams[i].xr_vfd = vfd[i];
        cam_discover(i);
    }
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Something about the outward cams changed inside the same XRService: rescan its fds and send fresh CAMs.
 * Twice within a second means it isn't settling: turn outward off (CAM_GONE FAILED) and let the agent retry.
 */
static void outward_rediscover(void)
{
    static int64_t last;
    const int64_t now = now_ms();
    if (last && now - last < 1000) {
        logf_("outward cams keep changing, turning them off");
        outward_stop(0);
        for (int i = 0; i < FPH_OUTWARD_CAMS; i++)
            send_gone(FPH_MSG_CAM_GONE, i, FPH_GONE_FAILED, ESTALE);
        return;
    }
    last = now;
    outward_stop(0);
    outward_start();
}

/* @return 1 if buffer i still is the dmabuf we grabbed */
static int buf_same(const struct buf *bf, const struct v4l2_plane *plane0)
{
    return plane0->m.fd == bf->xr_fd && same_file(&xr, bf->held_fd, bf->xr_fd) == 0;
}

/* One poll of one cam: QUERYBUF every buffer, report finished ones not reported yet. */
static void cam_poll(int id)
{
    struct cam *c = &cams[id];
    if (!c->present)
        return;
    int vfd = cam_borrow(c);
    if (vfd < 0) {
        logf_("video%d: fd %d changed (%s), rediscovering", c->node, c->xr_vfd, strerror(errno));
        outward_rediscover();
        return;
    }
    struct fph_msg_frames m;
    memset(&m, 0, sizeof(m));
    hdr_init(&m.hdr, FPH_MSG_FRAMES);
    m.cam = (uint8_t)id;
    m.gen = c->gen;
    int stale = 0;
    for (int i = 0; i < c->nbufs; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        if (querybuf(vfd, (uint32_t)i, &b, planes) != 0 || !buf_same(&c->bufs[i], &planes[0])) {
            stale = 1;
            break;
        }
        struct buf *bf = &c->bufs[i];
        if (b.flags & V4L2_BUF_FLAG_QUEUED)
            continue;
        if (bf->reported && bf->last_seq == b.sequence)
            continue;
        bf->reported = 1;
        bf->last_seq = b.sequence;
        struct fph_frame_entry *fe = &m.entries[m.n++];
        fe->index = (uint32_t)i;
        fe->seq = b.sequence;
        fe->ts_ns = (int64_t)b.timestamp.tv_sec * 1000000000LL + (int64_t)b.timestamp.tv_usec * 1000LL;
    }
    close(vfd);
    if (stale) {
        logf_("video%d: a buffer changed, rediscovering", c->node);
        outward_rediscover();
        return;
    }
    if (m.n == 0)
        return;
    /* oldest first */
    for (int a = 1; a < m.n; a++)
        for (int b = a; b > 0 && m.entries[b - 1].seq > m.entries[b].seq; b--) {
            struct fph_frame_entry t = m.entries[b];
            m.entries[b] = m.entries[b - 1];
            m.entries[b - 1] = t;
        }
    send_msg(&m, sizeof(m), NULL, 0);
}

static void recheck(const struct fph_cmd *cmd)
{
    struct fph_msg_recheck m;
    memset(&m, 0, sizeof(m));
    hdr_init(&m.hdr, FPH_MSG_RECHECK);
    m.cam = cmd->cam;
    m.index = cmd->index;
    m.seq = cmd->seq;
    struct cam *c = &cams[cmd->cam];
    m.gen = c->gen;
    if (!outward_on || !c->present || cmd->index >= (uint32_t)c->nbufs) {
        m.result = FPH_RECHECK_INVALID;
        send_msg(&m, sizeof(m), NULL, 0);
        return;
    }
    int vfd = cam_borrow(c);
    struct v4l2_buffer b;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    if (vfd < 0 || querybuf(vfd, cmd->index, &b, planes) != 0 || !buf_same(&c->bufs[cmd->index], &planes[0])) {
        if (vfd >= 0)
            close(vfd);
        m.result = FPH_RECHECK_STALE;
        send_msg(&m, sizeof(m), NULL, 0);
        logf_("video%d: buffer %u changed, rediscovering", c->node, cmd->index);
        outward_rediscover();
        return;
    }
    close(vfd);
    if (b.flags & V4L2_BUF_FLAG_QUEUED)
        m.result = FPH_RECHECK_REQUEUED;
    else if (b.sequence != cmd->seq)
        m.result = FPH_RECHECK_NEWER;
    else
        m.result = FPH_RECHECK_KEEP;
    send_msg(&m, sizeof(m), NULL, 0);
}

/* ---------------------------------------------------------------- eye cams */

static void eye_stop(int notify)
{
    for (int i = 0; i < eye_n; i++)
        close(eye_fds[i]);
    eye_n = 0;
    eye_on = 0;
    target_close(&et);
    if (notify)
        send_gone(FPH_MSG_EYE_GONE, 0, FPH_GONE_OFF, 0);
}

/* The udmabufs the eye tracker holds (fdinfo exp_name), de-duplicated by inode, sent as FPH_MSG_EYE. */
static void eye_start(void)
{
    eye_stop(0);
    int err = target_open(&et);
    if (err) {
        send_gone(FPH_MSG_EYE_GONE, 0, err == ESRCH ? FPH_GONE_NO_PROCESS : gone_reason_for(err), err);
        return;
    }
    eye_on = 1;
    struct fph_msg_eye m;
    memset(&m, 0, sizeof(m));
    hdr_init(&m.hdr, FPH_MSG_EYE);
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", (int)et.pid);
    DIR *d = opendir(dirpath);
    struct dirent *e;
    int denied = 0;
    while (d && (e = readdir(d)) && eye_n < FPH_MAX_EYE_ARENAS) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        const int et_fd = atoi(e->d_name);
        char path[128], text[1024];
        snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", (int)et.pid, et_fd);
        int f = open(path, O_RDONLY | O_CLOEXEC);
        if (f < 0)
            continue;
        ssize_t n = read(f, text, sizeof(text) - 1);
        close(f);
        if (n <= 0)
            continue;
        text[n] = 0;
        struct fph_fdinfo info;
        if (!fph_parse_fdinfo(text, &info) || strcmp(info.exp_name, "udmabuf") != 0)
            continue;
        int dup = 0;
        for (int k = 0; k < eye_n; k++)
            dup |= m.arenas[k].ino == info.ino;
        if (dup)
            continue;
        int fd = pidfd_getfd_(et.pidfd, et_fd);
        if (fd < 0) {
            denied = errno;
            continue;
        }
        struct stat st;
        if (!is_dmabuf(fd) || fstat(fd, &st) != 0 || (uint64_t)st.st_ino != info.ino) {
            close(fd); /* closed and reopened between the fdinfo read and the grab: skip it */
            continue;
        }
        m.arenas[eye_n].size = info.size;
        m.arenas[eye_n].ino = info.ino;
        eye_fds[eye_n++] = fd;
    }
    if (d)
        closedir(d);
    if (eye_n == 0) {
        send_gone(FPH_MSG_EYE_GONE, 0, denied ? gone_reason_for(denied) : FPH_GONE_NO_DEVICE, denied);
        eye_stop(0);
        return;
    }
    m.narenas = (uint8_t)eye_n;
    m.gen = ++eye_gen;
    send_msg(&m, sizeof(m), eye_fds, eye_n);
    logf_("eyetracking pid %d: %d udmabuf arenas, gen %u", (int)et.pid, eye_n, eye_gen);
}

/* ---------------------------------------------------------------- main loop */

static int has_ptrace_cap(void)
{
    struct __user_cap_header_struct h = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof(data));
    if (syscall(SYS_capget, &h, data) != 0)
        return 0;
    return (data[CAP_SYS_PTRACE / 32].effective >> (CAP_SYS_PTRACE % 32)) & 1;
}

static void handle_cmd(void)
{
    union {
        char buf[sizeof(struct fph_cmd) + 16];
        struct fph_cmd cmd;
    } in;
    union {
        char buf[CMSG_SPACE(sizeof(int) * 8)];
        struct cmsghdr align;
    } ctrl;
    struct iovec iov = {in.buf, sizeof(in.buf)};
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = ctrl.buf;
    mh.msg_controllen = sizeof(ctrl.buf);
    ssize_t n = recvmsg(SOCK_FD, &mh, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return;
        exit(0);
    }
    if (n == 0)
        exit(0); /* EOF: the agent closed its end or died */
    /* we never accept fds from the agent: close any that came along */
    int nfds = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                close(fd);
            }
            nfds += k;
        } else {
            nfds++;
        }
    }
    struct fph_cmd cmd;
    int bad = fph_check_cmd(in.buf, (long)n, mh.msg_flags, nfds, &cmd);
    if (bad) {
        logf_("rejected a command (error %d, %zd bytes)", bad, n);
        send_error((uint32_t)bad);
        return;
    }
    switch (cmd.op) {
    case FPH_OP_OUTWARD_ON:
        outward_stop(0);
        outward_start();
        break;
    case FPH_OP_OUTWARD_OFF:
        outward_stop(1);
        break;
    case FPH_OP_EYE_ON:
        eye_start();
        break;
    case FPH_OP_EYE_OFF:
        eye_stop(1);
        break;
    case FPH_OP_RECHECK:
        recheck(&cmd);
        break;
    }
}

int main(void)
{
    /* exec of a capability binary clears the parent-death signal, so set it here; a parent that died
       before this line shows up as socket EOF below */
    /* the caller may have exec'd us with SIGTERM ignored or blocked, which would defeat the parent-death
       signal: reset it and unblock everything first */
    signal(SIGTERM, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    prctl(PR_SET_DUMPABLE, 0);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    signal(SIGPIPE, SIG_IGN);

    int type = 0, domain = 0;
    socklen_t len = sizeof(type);
    if (getsockopt(SOCK_FD, SOL_SOCKET, SO_TYPE, &type, &len) != 0 || type != SOCK_SEQPACKET ||
        (len = sizeof(domain), getsockopt(SOCK_FD, SOL_SOCKET, SO_DOMAIN, &domain, &len)) != 0 ||
        domain != AF_UNIX) {
        fprintf(stderr, "fph-camgrab: fd 3 must be a unix SOCK_SEQPACKET socket (started by frame-perf-hud)\n");
        return 2;
    }
    /* nothing else inherited stays open (fds 0-2 kept for logging) */
    if (syscall(SYS_close_range, 4, ~0U, 0) != 0)
        for (int fd = 4; fd < 1024; fd++)
            close(fd);
    fcntl(SOCK_FD, F_SETFD, FD_CLOEXEC);
    /* an agent that stops reading must not park us in sendmsg (we'd stop watching EOF and the targets):
       a send that can't finish within 1 s fails with EAGAIN and we exit */
    struct timeval send_timeout = {1, 0};
    setsockopt(SOCK_FD, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    struct fph_msg_hello hello;
    memset(&hello, 0, sizeof(hello));
    hdr_init(&hello.hdr, FPH_MSG_HELLO);
    hello.helper_pid = (uint32_t)getpid();
    hello.has_ptrace = (uint32_t)has_ptrace_cap();
    send_msg(&hello, sizeof(hello), NULL, 0);

    int64_t next_poll = 0;
    for (;;) {
        struct pollfd p[3];
        int np = 0;
        p[np++] = (struct pollfd){SOCK_FD, POLLIN, 0};
        const int xr_slot = (outward_on && xr.pidfd >= 0) ? np++ : -1;
        if (xr_slot >= 0)
            p[xr_slot] = (struct pollfd){xr.pidfd, POLLIN, 0};
        const int et_slot = (eye_on && et.pidfd >= 0) ? np++ : -1;
        if (et_slot >= 0)
            p[et_slot] = (struct pollfd){et.pidfd, POLLIN, 0};

        int timeout = -1;
        if (outward_on) {
            int64_t wait = next_poll - now_ms();
            timeout = wait < 0 ? 0 : (int)wait;
        }
        int r = poll(p, (nfds_t)np, timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            logf_("poll: %s", strerror(errno));
            return 1;
        }
        if (xr_slot >= 0 && p[xr_slot].revents) {
            logf_("XRService (pid %d) exited", (int)xr.pid);
            bye_and_exit(FPH_BYE_XRSERVICE_EXITED, 0);
        }
        if (et_slot >= 0 && p[et_slot].revents) {
            logf_("eyetracking (pid %d) exited", (int)et.pid);
            bye_and_exit(FPH_BYE_EYETRACKING_EXITED, 0);
        }
        if (p[0].revents & POLLIN)
            handle_cmd();
        else if (p[0].revents & (POLLHUP | POLLERR | POLLNVAL))
            return 0;

        if (outward_on && now_ms() >= next_poll) {
            next_poll = now_ms() + FPH_POLL_INTERVAL_MS;
            for (int i = 0; i < FPH_OUTWARD_CAMS && outward_on; i++)
                cam_poll(i);
        }
    }
}

#endif /* FPH_CAMGRAB_NO_MAIN */
