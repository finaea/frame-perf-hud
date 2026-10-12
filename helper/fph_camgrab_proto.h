/*
 * The wire protocol between the headset agent and fph-camgrab (the camera helper).
 *
 * Transport: an AF_UNIX SOCK_SEQPACKET socketpair. The agent keeps one end and hands the other to the
 * helper as fd 3. One message per packet, fixed layouts, host byte order (both ends run on the same Frame).
 *
 * Agent -> helper: only struct fph_cmd, exactly sizeof(struct fph_cmd) bytes, no ancillary data. The
 * helper answers anything else with FPH_MSG_ERROR and ignores it. There is no way to name a path, a pid
 * or an fd: the targets are fixed inside the helper.
 *
 * Helper -> agent: a struct fph_msg_hdr first, then the body for its type. FPH_MSG_CAM and FPH_MSG_EYE
 * carry dmabuf fds as SCM_RIGHTS, in the order of their bufs[] / arenas[] entries.
 *
 * Plain C so the helper (C) and the agent (C++) share it. Bump FPH_PROTO_VERSION on any layout change:
 * the helper needs sudo to update, so the agent must cope with an older installed helper.
 */
#ifndef FPH_CAMGRAB_PROTO_H
#define FPH_CAMGRAB_PROTO_H

#include <stdint.h>

#define FPH_PROTO_VERSION 1

/* Bump on ANY change to fph-camgrab.c (each one needs sudo again). install.sh compares this marker string
   between the staged and the installed binary, so a rebuild of the same version doesn't ask for sudo. */
#define FPH_HELPER_VERSION 1
#define FPH_HELPER_VERSION_MARKER "fph-camgrab-version: 1;"

#define FPH_CMD_MAGIC 0x43485046u /* "FPHC" */
#define FPH_MSG_MAGIC 0x4D485046u /* "FPHM" */

/* Outward cams: the XRService V4L2 capture nodes, in this order. cam ids in the protocol index this. */
#define FPH_OUTWARD_CAMS 4
#define FPH_OUTWARD_NODES {0, 3, 9, 13} /* /dev/video<N> */

#define FPH_MAX_BUFS 64        /* V4L2 buffers per outward cam */
#define FPH_MAX_EYE_ARENAS 8   /* unique udmabufs shared with the eye tracker */
#define FPH_POLL_INTERVAL_MS 67 /* the helper's own poll clock: <= 15 Hz */

/* ---------------------------------------------------------------- agent -> helper */

enum fph_op {
    FPH_OP_OUTWARD_ON = 1,  /* discover the outward cams, send FPH_MSG_CAM per cam, start polling */
    FPH_OP_OUTWARD_OFF = 2, /* stop polling, drop the outward buffers */
    FPH_OP_EYE_ON = 3,      /* discover the eye arenas, send FPH_MSG_EYE */
    FPH_OP_EYE_OFF = 4,     /* drop the eye arenas */
    FPH_OP_RECHECK = 5      /* re-query (cam, index): is it still the frame `seq`, not requeued, same buffer? */
};

struct fph_cmd {
    uint32_t magic;    /* FPH_CMD_MAGIC */
    uint8_t op;        /* enum fph_op */
    uint8_t cam;       /* RECHECK: 0..FPH_OUTWARD_CAMS-1, else 0 */
    uint16_t reserved; /* 0 */
    uint32_t index;    /* RECHECK: V4L2 buffer index, else 0 */
    uint32_t seq;      /* RECHECK: the sequence the agent copied, else 0 */
};

/* ---------------------------------------------------------------- helper -> agent */

enum fph_msg_type {
    FPH_MSG_HELLO = 1,    /* first message after start */
    FPH_MSG_CAM = 2,      /* an outward cam's format + buffers (+ fds). A new gen replaces the old one */
    FPH_MSG_CAM_GONE = 3, /* an outward cam is unavailable; drop its buffers */
    FPH_MSG_EYE = 4,      /* the eye arenas (+ fds) */
    FPH_MSG_EYE_GONE = 5,
    FPH_MSG_FRAMES = 6,  /* per poll: finished (dequeued) buffers newer than the last report */
    FPH_MSG_RECHECK = 7, /* answer to FPH_OP_RECHECK */
    FPH_MSG_ERROR = 8,   /* a command was rejected */
    FPH_MSG_BYE = 9      /* the helper is about to exit */
};

struct fph_msg_hdr {
    uint32_t magic;   /* FPH_MSG_MAGIC */
    uint16_t type;    /* enum fph_msg_type */
    uint16_t version; /* FPH_PROTO_VERSION */
};

struct fph_msg_hello {
    struct fph_msg_hdr hdr;
    uint32_t helper_pid;
    uint32_t has_ptrace; /* 1 if the helper holds CAP_SYS_PTRACE in its effective set */
};

struct fph_buf_info {
    uint32_t index;  /* V4L2 buffer index */
    uint32_t length; /* plane 0 length, bytes (what to mmap) */
    uint64_t ino;    /* dmabuf inode: the buffer's identity */
};

struct fph_msg_cam {
    struct fph_msg_hdr hdr;
    uint8_t cam;   /* 0..FPH_OUTWARD_CAMS-1 */
    uint8_t node;  /* /dev/video<node> */
    uint8_t nbufs; /* fds attached, one per bufs[] entry */
    uint8_t reserved;
    uint32_t gen; /* bumps on every (re)discovery of this cam; FRAMES / RECHECK carry it */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;        /* derived from sizeimage, not bytesperline (qcom-camss reports 1056 for 1152) */
    uint32_t pixelformat;  /* V4L2 fourcc: NV12 or GREY */
    uint32_t bytesperline; /* raw, for diagnostics only */
    uint32_t sizeimage;    /* raw plane 0 sizeimage */
    struct fph_buf_info bufs[FPH_MAX_BUFS];
};

enum fph_gone_reason {
    FPH_GONE_OFF = 0,        /* turned off by the agent */
    FPH_GONE_NO_PROCESS = 1, /* XRService / eyetracking not running (as the agent's user) */
    FPH_GONE_NO_DEVICE = 2,  /* XRService holds no fd for this node */
    FPH_GONE_DENIED = 3,     /* pidfd_open / pidfd_getfd refused: helper not installed with the capability */
    FPH_GONE_FORMAT = 4,     /* G_FMT failed or a format / buffer we don't handle */
    FPH_GONE_FAILED = 5      /* any other error (errno in the message) */
};

struct fph_msg_gone {
    struct fph_msg_hdr hdr;
    uint8_t cam;    /* FPH_MSG_CAM_GONE: which cam; FPH_MSG_EYE_GONE: 0 */
    uint8_t reason; /* enum fph_gone_reason */
    uint16_t reserved;
    int32_t err; /* errno, 0 if none */
};

struct fph_eye_arena {
    uint64_t size;
    uint64_t ino;
};

struct fph_msg_eye {
    struct fph_msg_hdr hdr;
    uint8_t narenas; /* fds attached, one per arenas[] entry */
    uint8_t reserved[3];
    uint32_t gen;
    struct fph_eye_arena arenas[FPH_MAX_EYE_ARENAS];
};

#define FPH_BUF_FLAG_QUEUED 0x1 /* not used in FRAMES (only finished buffers are listed) */

struct fph_frame_entry {
    uint32_t index;
    uint32_t seq;
    int64_t ts_ns; /* V4L2 buffer timestamp (CLOCK_MONOTONIC), ns */
};

struct fph_msg_frames {
    struct fph_msg_hdr hdr;
    uint8_t cam;
    uint8_t n; /* entries used, sorted by seq, oldest first */
    uint16_t reserved;
    uint32_t gen;
    struct fph_frame_entry entries[FPH_MAX_BUFS];
};

enum fph_recheck_result {
    FPH_RECHECK_KEEP = 0,     /* same sequence, not requeued, same buffer: the copy is good */
    FPH_RECHECK_NEWER = 1,    /* the buffer now holds another frame: discard */
    FPH_RECHECK_REQUEUED = 2, /* queued again (the camera may be writing it): discard */
    FPH_RECHECK_STALE = 3,    /* the buffer or device changed: discard; a fresh FPH_MSG_CAM follows */
    FPH_RECHECK_INVALID = 4   /* no such cam / index / outward off: discard */
};

struct fph_msg_recheck {
    struct fph_msg_hdr hdr;
    uint8_t cam;
    uint8_t result; /* enum fph_recheck_result */
    uint16_t reserved;
    uint32_t gen;
    uint32_t index;
    uint32_t seq; /* the sequence asked about */
};

enum fph_error_code {
    FPH_ERR_BAD_LENGTH = 1, /* not exactly sizeof(struct fph_cmd), or fds attached */
    FPH_ERR_BAD_MAGIC = 2,
    FPH_ERR_BAD_OP = 3,
    FPH_ERR_BAD_ARGS = 4 /* non-zero fields an op doesn't take, or cam / index out of range */
};

struct fph_msg_error {
    struct fph_msg_hdr hdr;
    uint32_t code; /* enum fph_error_code */
};

enum fph_bye_reason {
    FPH_BYE_XRSERVICE_EXITED = 1,
    FPH_BYE_EYETRACKING_EXITED = 2,
    FPH_BYE_SOCKET_ERROR = 3
};

struct fph_msg_bye {
    struct fph_msg_hdr hdr;
    uint32_t reason; /* enum fph_bye_reason */
};

#endif /* FPH_CAMGRAB_PROTO_H */
