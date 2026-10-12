/* T09: the pure parsers / checks of the camera helper (helper/fph-camgrab.c), built without its main().
 * Linux only (the helper includes <linux/videodev2.h>). No arguments. */
#define FPH_CAMGRAB_NO_MAIN
#include "../helper/fph-camgrab.c"

static int failures;

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            printf("FAIL line %d: %s\n", __LINE__, #expr);           \
            ++failures;                                              \
        }                                                            \
    } while (0)

#define XR "/opt/steamvr/drivers/cv/bin/linuxarm64/XRService"

static void test_fdinfo(void)
{
    struct fph_fdinfo i;
    const char *real = "pos:\t0\nflags:\t02\nmnt_id:\t13\nino:\t1\nsize:\t16777216\ncount:\t23\nexp_name:\tudmabuf\n";
    CHECK(fph_parse_fdinfo(real, &i) == 1);
    CHECK(i.size == 16777216ull && i.ino == 1ull);
    CHECK(i.have_size && i.have_ino);
    CHECK(strcmp(i.exp_name, "udmabuf") == 0);

    /* big values */
    CHECK(fph_parse_fdinfo("size:\t4294967296\nino:\t18446744073709551615\nexp_name:\tudmabuf\n", &i) == 1);
    CHECK(i.size == 4294967296ull && i.ino == 18446744073709551615ull);

    /* missing size / ino */
    CHECK(fph_parse_fdinfo("ino:\t1\nexp_name:\tudmabuf\n", &i) == 0);
    CHECK(i.size == 0 && !i.have_size && i.have_ino);
    CHECK(fph_parse_fdinfo("size:\t99\nexp_name:\tudmabuf\n", &i) == 0);
    CHECK(i.ino == 0 && i.have_size && !i.have_ino);
    CHECK(fph_parse_fdinfo("", &i) == 0);
    CHECK(i.size == 0 && i.ino == 0 && i.exp_name[0] == 0);

    /* another exporter: parsed, but the caller compares exp_name */
    CHECK(fph_parse_fdinfo("ino:\t7\nsize:\t4096\nexp_name:\tqcom,system\n", &i) == 1);
    CHECK(strcmp(i.exp_name, "udmabuf") != 0);
    CHECK(strcmp(i.exp_name, "qcom,system") == 0);

    /* no exp_name at all */
    CHECK(fph_parse_fdinfo("ino:\t7\nsize:\t4096\n", &i) == 1);
    CHECK(i.exp_name[0] == 0);

    /* last line without a newline */
    CHECK(fph_parse_fdinfo("ino:\t7\nsize:\t4096", &i) == 1);
    CHECK(i.size == 4096);

    /* a long exp_name is truncated, NUL-terminated, and the lines after it still parse */
    char longtext[400];
    char name[101];
    memset(name, 'a', 100);
    name[100] = 0;
    snprintf(longtext, sizeof(longtext), "exp_name:\t%s\nino:\t5\nsize:\t8192\n", name);
    CHECK(fph_parse_fdinfo(longtext, &i) == 1);
    CHECK(strlen(i.exp_name) == 31);
    CHECK(i.exp_name[31] == 0);
    CHECK(i.size == 8192 && i.ino == 5);
}

static void test_status_uid(void)
{
    unsigned long uid = 99;
    CHECK(fph_parse_status_uid("Name:\tXRService\nUid:\t1000\t1000\t1000\t1000\n", &uid) == 1);
    CHECK(uid == 1000);
    CHECK(fph_parse_status_uid("Name:\tinit\nUmask:\t0022\nUid:\t0\t0\t0\t0\nGid:\t5\t5\t5\t5\n", &uid) == 1);
    CHECK(uid == 0);
    CHECK(fph_parse_status_uid("Uid:\t4294967294\t1\t1\t1", &uid) == 1); /* no trailing newline */
    CHECK(uid == 4294967294ul);
    uid = 77;
    CHECK(fph_parse_status_uid("Name:\tx\nGid:\t1000\t1000\t1000\t1000\n", &uid) == 0);
    CHECK(uid == 77);
    CHECK(fph_parse_status_uid("", &uid) == 0);
    CHECK(fph_parse_status_uid("Name:\tUid: 5\n", &uid) == 0); /* only a line that starts with Uid: */
    CHECK(fph_parse_status_uid("Uid:\tabc\n", &uid) == 0);
    CHECK(fph_parse_status_uid("Name:\tx\nUid:\t1000\n", &uid) == 1 && uid == 1000);
}

static void test_exe(void)
{
    const long n = (long)strlen(XR);
    CHECK(fph_exe_matches(XR, n, XR) == 1);
    CHECK(fph_exe_matches(XR " (deleted)", (long)strlen(XR " (deleted)"), XR) == 0);
    CHECK(fph_exe_matches(XR, n - 1, XR) == 0);          /* a prefix */
    CHECK(fph_exe_matches(XR "2", n + 1, XR) == 0);      /* a longer path */
    CHECK(fph_exe_matches(XR "/x", n + 2, XR) == 0);
    CHECK(fph_exe_matches(XR, -1, XR) == 0);             /* readlink failed */
    CHECK(fph_exe_matches(XR, 0, XR) == 0);
    CHECK(fph_exe_matches("/opt/steamvr/drivers/cv/bin/linuxarm64/XRServiX", n, XR) == 0); /* same length, different */
    CHECK(fph_exe_matches(ET_EXE, (long)strlen(ET_EXE), XR) == 0);
    CHECK(fph_exe_matches(XR, n, ET_EXE) == 0);
    CHECK(fph_exe_matches(ET_EXE, (long)strlen(ET_EXE), ET_EXE) == 1);
}

static int node_of(const char *s) { return fph_parse_video_node(s, (long)strlen(s)); }
static int cam_of(const char *s) { return fph_node_to_cam(node_of(s)); }

static void test_video_node(void)
{
    CHECK(cam_of("/dev/video0") == 0);
    CHECK(cam_of("/dev/video3") == 1);
    CHECK(cam_of("/dev/video9") == 2);
    CHECK(cam_of("/dev/video13") == 3);
    CHECK(node_of("/dev/video13") == 13);
    CHECK(node_of("/dev/video1") == 1);
    CHECK(cam_of("/dev/video1") == -1);   /* a node, but not an outward cam */
    CHECK(cam_of("/dev/video2") == -1);
    CHECK(node_of("/dev/video03") == -1); /* leading zero */
    CHECK(node_of("/dev/video013") == -1);
    CHECK(node_of("/dev/video") == -1);
    CHECK(node_of("/dev/video13x") == -1);
    CHECK(node_of("/dev/video-enc0") == -1);
    CHECK(node_of("/dev/video1234") == -1); /* too many digits */
    CHECK(node_of("/dmabuf:") == -1);
    CHECK(node_of("") == -1);
    CHECK(node_of("/dev/media0") == -1);
    CHECK(node_of("/dev/video0 (deleted)") == -1);
    CHECK(fph_parse_video_node("/dev/video0", -1) == -1);
    CHECK(fph_node_to_cam(-1) == -1);
    CHECK(fph_node_to_cam(100) == -1);
}

static void test_pitch(void)
{
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_NV12, 1056, 1024, 1152u * 1024u * 3u / 2u) == 1152); /* qcom-camss */
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_NV12, 640, 480, 640u * 480u * 3u / 2u) == 640);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_GREY, 640, 480, 640u * 480u) == 640);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_GREY, 640, 480, 704u * 480u) == 704);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_YUYV, 640, 480, 640u * 480u * 2u) == 0); /* unknown fourcc */
    CHECK(fph_pitch_from_fmt(0, 640, 480, 640u * 480u) == 0);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_NV12, 1056, 1024, 1000u * 1536u) == 0);  /* pitch < width */
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_GREY, 640, 480, 100) == 0);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_GREY, 640, 480, 0) == 0);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_NV12, 0, 1024, 1000000) == 0);           /* w or h 0 */
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_NV12, 1056, 0, 1000000) == 0);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_GREY, 0, 0, 0) == 0);
    CHECK(fph_pitch_from_fmt(V4L2_PIX_FMT_GREY, 640, 480, 70000u * 480u) == 0);    /* absurd pitch */
}

static struct fph_cmd mk(uint8_t op, uint8_t cam, uint32_t index, uint32_t seq)
{
    struct fph_cmd c;
    memset(&c, 0, sizeof(c));
    c.magic = FPH_CMD_MAGIC;
    c.op = op;
    c.cam = cam;
    c.index = index;
    c.seq = seq;
    return c;
}

static int check(const struct fph_cmd *c, long len, int flags, int nfds)
{
    union {
        char b[64];
        struct fph_cmd c;
    } buf;
    struct fph_cmd out;
    memset(&buf, 0, sizeof(buf));
    buf.c = *c;
    return fph_check_cmd(buf.b, len, flags, nfds, &out);
}

static int ok(struct fph_cmd c) { return check(&c, (long)sizeof(c), 0, 0); }

static void test_check_cmd(void)
{
    CHECK(sizeof(struct fph_cmd) == 16);

    /* every valid op */
    CHECK(ok(mk(FPH_OP_OUTWARD_ON, 0, 0, 0)) == 0);
    CHECK(ok(mk(FPH_OP_OUTWARD_OFF, 0, 0, 0)) == 0);
    CHECK(ok(mk(FPH_OP_EYE_ON, 0, 0, 0)) == 0);
    CHECK(ok(mk(FPH_OP_EYE_OFF, 0, 0, 0)) == 0);
    CHECK(ok(mk(FPH_OP_RECHECK, 0, 0, 0)) == 0);
    CHECK(ok(mk(FPH_OP_RECHECK, 3, FPH_MAX_BUFS - 1, 0xFFFFFFFFu)) == 0);

    /* the output is filled */
    {
        struct fph_cmd in = mk(FPH_OP_RECHECK, 2, 5, 77), out;
        memset(&out, 0xAA, sizeof(out));
        CHECK(fph_check_cmd(&in, sizeof(in), 0, 0, &out) == 0);
        CHECK(out.op == FPH_OP_RECHECK && out.cam == 2 && out.index == 5 && out.seq == 77);
    }

    /* length / flags / fds */
    struct fph_cmd good = mk(FPH_OP_OUTWARD_ON, 0, 0, 0);
    CHECK(check(&good, 15, 0, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 17, 0, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 0, 0, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, -1, 0, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 2, 0, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 16, MSG_TRUNC, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 16, MSG_CTRUNC, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 16, MSG_TRUNC | MSG_CTRUNC, 0) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 16, 0, 1) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 16, 0, 8) == FPH_ERR_BAD_LENGTH);
    CHECK(check(&good, 16, MSG_DONTWAIT, 0) == 0); /* unrelated flags are fine */

    /* magic */
    struct fph_cmd bad = good;
    bad.magic = FPH_MSG_MAGIC;
    CHECK(ok(bad) == FPH_ERR_BAD_MAGIC);
    bad.magic = 0;
    CHECK(ok(bad) == FPH_ERR_BAD_MAGIC);
    bad.magic = FPH_CMD_MAGIC ^ 1u;
    CHECK(ok(bad) == FPH_ERR_BAD_MAGIC);

    /* reserved */
    bad = good;
    bad.reserved = 1;
    CHECK(ok(bad) == FPH_ERR_BAD_ARGS);
    bad.reserved = 0x8000;
    CHECK(ok(bad) == FPH_ERR_BAD_ARGS);

    /* unknown ops */
    CHECK(ok(mk(0, 0, 0, 0)) == FPH_ERR_BAD_OP);
    CHECK(ok(mk(6, 0, 0, 0)) == FPH_ERR_BAD_OP);
    CHECK(ok(mk(255, 0, 0, 0)) == FPH_ERR_BAD_OP);

    /* ON / OFF take no arguments */
    for (uint8_t op = FPH_OP_OUTWARD_ON; op <= FPH_OP_EYE_OFF; op++) {
        CHECK(ok(mk(op, 1, 0, 0)) == FPH_ERR_BAD_ARGS);
        CHECK(ok(mk(op, 0, 1, 0)) == FPH_ERR_BAD_ARGS);
        CHECK(ok(mk(op, 0, 0, 1)) == FPH_ERR_BAD_ARGS);
    }

    /* RECHECK ranges */
    CHECK(ok(mk(FPH_OP_RECHECK, FPH_OUTWARD_CAMS, 0, 0)) == FPH_ERR_BAD_ARGS);
    CHECK(ok(mk(FPH_OP_RECHECK, 7, 0, 0)) == FPH_ERR_BAD_ARGS);
    CHECK(ok(mk(FPH_OP_RECHECK, 255, 0, 0)) == FPH_ERR_BAD_ARGS);
    CHECK(ok(mk(FPH_OP_RECHECK, 0, FPH_MAX_BUFS, 0)) == FPH_ERR_BAD_ARGS);
    CHECK(ok(mk(FPH_OP_RECHECK, 0, 0xFFFFFFFFu, 0)) == FPH_ERR_BAD_ARGS);
}

int main(void)
{
    test_fdinfo();
    test_status_uid();
    test_exe();
    test_video_node();
    test_pitch();
    test_check_cmd();
    if (failures) {
        printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("camgrab_helper_test: all passed\n");
    return 0;
}
