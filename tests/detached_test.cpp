// T05: Detached mode. The pose maths (grab, place in front, face, nudge, rotate) and the config's detached_pose key.
// Pure maths on OpenVR's matrix type; no SteamVR needed.
#include "config.h"
#include "detached.h"
#include "placement.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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

constexpr double kEps = 2e-4;  // matrices are floats

struct Vec3 {
    double x, y, z;
};

Vec3 col(const vr::HmdMatrix34_t& m, int c) {
    return {m.m[0][c], m.m[1][c], m.m[2][c]};
}
Vec3 col(const DetachedPose& p, int c) {
    return {p.m[0][c], p.m[1][c], p.m[2][c]};
}
double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double len(const Vec3& v) {
    return std::sqrt(dot(v, v));
}
bool near(double a, double b, double eps = kEps) {
    return std::fabs(a - b) < eps;
}
bool nearVec(const Vec3& a, const Vec3& b, double eps = kEps) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

bool nearMatrix(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b, double eps = kEps) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!near(a.m[r][c], b.m[r][c], eps)) return false;
        }
    }
    return true;
}
bool nearPose(const DetachedPose& a, const DetachedPose& b, double eps = kEps) {
    return nearMatrix(toHmdMatrix(a), toHmdMatrix(b), eps);
}

/** @return true if the rotation part is orthonormal and right-handed */
bool isRotation(const vr::HmdMatrix34_t& m, double eps = kEps) {
    const Vec3 x = col(m, 0), y = col(m, 1), z = col(m, 2);
    return near(len(x), 1, eps) && near(len(y), 1, eps) && near(len(z), 1, eps) && near(dot(x, y), 0, eps) &&
           near(dot(y, z), 0, eps) && near(dot(x, z), 0, eps) && nearVec(cross(x, y), z, eps);
}

/** Rotation about the y axis (a yaw of the whole body), then a position. */
vr::HmdMatrix34_t yawPose(double yawDeg, double x, double y, double z) {
    const double a = yawDeg * M_PI / 180.0;
    vr::HmdMatrix34_t m {};
    m.m[0][0] = static_cast<float>(std::cos(a));
    m.m[0][2] = static_cast<float>(std::sin(a));
    m.m[1][1] = 1.0f;
    m.m[2][0] = static_cast<float>(-std::sin(a));
    m.m[2][2] = static_cast<float>(std::cos(a));
    m.m[0][3] = static_cast<float>(x);
    m.m[1][3] = static_cast<float>(y);
    m.m[2][3] = static_cast<float>(z);
    return m;
}

/** An arbitrary rigid pose (rotation from panelRotation, so it's a true rotation). */
vr::HmdMatrix34_t anyPose(double yaw, double pitch, double roll, double x, double y, double z) {
    double r[3][3];
    panelRotation(yaw, pitch, roll, r);
    vr::HmdMatrix34_t m {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) m.m[i][j] = static_cast<float>(r[i][j]);
    }
    m.m[0][3] = static_cast<float>(x);
    m.m[1][3] = static_cast<float>(y);
    m.m[2][3] = static_cast<float>(z);
    return m;
}

vr::HmdMatrix34_t identityMatrix() {
    return yawPose(0, 0, 0, 0);
}

// ---- Pose maths ----

void testConvert() {
    const vr::HmdMatrix34_t m = anyPose(25, -10, 40, 0.3, 1.1, -0.7);
    const DetachedPose p = fromHmdMatrix(m);
    CHECK(p.valid);
    CHECK(nearMatrix(toHmdMatrix(p), m));
    for (int r = 0; r < 3; ++r) CHECK(near(p.m[r][3], m.m[r][3], 1e-6));  // position untouched
    // A slightly skewed rotation comes out orthonormal
    vr::HmdMatrix34_t skew = m;
    skew.m[0][0] *= 1.01f;
    skew.m[1][2] += 0.02f;
    CHECK(!isRotation(skew, 1e-3));
    CHECK(isRotation(toHmdMatrix(fromHmdMatrix(skew))));
}

void testInvertRigid() {
    const vr::HmdMatrix34_t poses[] = {identityMatrix(), yawPose(90, 1, 2, 3), anyPose(30, 20, -50, -0.4, 1.7, 2.2),
                                       anyPose(-170, -80, 10, 5, -3, 0.5)};
    for (const vr::HmdMatrix34_t& m : poses) {
        const vr::HmdMatrix34_t inv = invertRigid(m);
        CHECK(nearMatrix(composeTransform(m, inv), identityMatrix()));
        CHECK(nearMatrix(composeTransform(inv, m), identityMatrix()));
        CHECK(nearMatrix(invertRigid(inv), m));
    }
    // A pure translation inverts to the opposite one
    vr::HmdMatrix34_t t = identityMatrix();
    t.m[0][3] = 1.5f;
    t.m[2][3] = -2.0f;
    const vr::HmdMatrix34_t inv = invertRigid(t);
    CHECK(near(inv.m[0][3], -1.5) && near(inv.m[1][3], 0) && near(inv.m[2][3], 2.0));
}

void testOrthonormalize() {
    // Already a rotation: unchanged
    vr::HmdMatrix34_t m = anyPose(40, 15, -25, 1, 2, 3);
    vr::HmdMatrix34_t copy = m;
    orthonormalize(m);
    CHECK(nearMatrix(m, copy, 1e-5));

    // Scaled and sheared: orthonormal again, the z axis (the face) kept, translation untouched
    vr::HmdMatrix34_t bad = copy;
    for (int r = 0; r < 3; ++r) {
        bad.m[r][0] *= 1.2f;
        bad.m[r][1] = bad.m[r][1] * 0.9f + bad.m[r][0] * 0.1f;
    }
    const Vec3 z = col(bad, 2);
    orthonormalize(bad);
    CHECK(isRotation(bad));
    CHECK(nearVec(col(bad, 2), {z.x / len(z), z.y / len(z), z.z / len(z)}));
    for (int r = 0; r < 3; ++r) CHECK(near(bad.m[r][3], copy.m[r][3], 1e-6));

    // z column zero: identity rotation
    vr::HmdMatrix34_t zero {};
    zero.m[0][0] = 1.0f;
    zero.m[1][1] = 1.0f;
    zero.m[0][3] = 4.0f;
    orthonormalize(zero);
    CHECK(isRotation(zero));
    CHECK(nearMatrix(zero, [] {
        vr::HmdMatrix34_t id = identityMatrix();
        id.m[0][3] = 4.0f;
        return id;
    }()));

    // y parallel to z: still a valid rotation
    vr::HmdMatrix34_t par {};
    par.m[0][0] = 1.0f;
    par.m[1][1] = 1.0f;
    par.m[1][2] = 1.0f;  // z = (0,1,0), y = (0,1,0)
    orthonormalize(par);
    CHECK(isRotation(par));
    CHECK(nearVec(col(par, 2), {0, 1, 0}));

    // all zero
    vr::HmdMatrix34_t nothing {};
    orthonormalize(nothing);
    CHECK(isRotation(nothing));

    // Repeated compose + orthonormalize doesn't drift
    DetachedPose p = fromHmdMatrix(anyPose(10, 5, 3, 0, 1, -1));
    for (int i = 0; i < 2000; ++i) p = rotateDetached(p, i % 3, 0.7);
    CHECK(isRotation(toHmdMatrix(p)));
}

void testGrabKeepsTheHudRigid() {
    const vr::HmdMatrix34_t device = anyPose(20, -35, 10, 0.2, 1.0, -0.4);
    const DetachedPose panel = fromHmdMatrix(anyPose(-60, 5, 0, 0.4, 1.5, -1.2));
    DetachedGrab grab;
    CHECK(!grab.active());
    CHECK(grab.begin(7, device, panel));
    CHECK(grab.active() && grab.device() == 7);
    // begin + follow with the same device pose returns the original pose
    CHECK(nearPose(grab.follow(device), panel));
    CHECK(grab.follow(device).valid);

    // Move and turn the device: the HUD follows rigidly (its pose in the device's frame stays the same)
    const vr::HmdMatrix34_t offset = composeTransform(invertRigid(device), toHmdMatrix(panel));
    const vr::HmdMatrix34_t devices[] = {yawPose(0, 1, 0, 0), anyPose(100, 30, -20, -1, 0.3, 2), anyPose(-45, -60, 80, 0, 2, 0),
                                         yawPose(180, 0, 0, 0)};
    for (const vr::HmdMatrix34_t& d : devices) {
        const DetachedPose followed = grab.follow(d);
        CHECK(followed.valid);
        CHECK(isRotation(toHmdMatrix(followed)));
        CHECK(nearMatrix(toHmdMatrix(followed), composeTransform(d, offset), 5e-4));
        CHECK(nearMatrix(composeTransform(invertRigid(d), toHmdMatrix(followed)), offset, 5e-4));
    }
    // A pure translation of the device moves the HUD by the same amount, rotation unchanged
    vr::HmdMatrix34_t moved = device;
    moved.m[0][3] += 0.5f;
    moved.m[1][3] -= 0.25f;
    const DetachedPose shifted = grab.follow(moved);
    CHECK(near(shifted.m[0][3], panel.m[0][3] + 0.5) && near(shifted.m[1][3], panel.m[1][3] - 0.25) &&
          near(shifted.m[2][3], panel.m[2][3]));
    CHECK(nearVec(col(shifted, 0), col(panel, 0)) && nearVec(col(shifted, 2), col(panel, 2)));
    // A turn of the device about its own position swings the HUD around it, radius kept
    const vr::HmdMatrix34_t turned = composeTransform(yawPose(90, device.m[0][3], device.m[1][3], device.m[2][3]),
                                                      [&] {
                                                          vr::HmdMatrix34_t r = device;
                                                          r.m[0][3] = r.m[1][3] = r.m[2][3] = 0;
                                                          return r;
                                                      }());
    const DetachedPose swung = grab.follow(turned);
    const Vec3 before {panel.m[0][3] - device.m[0][3], panel.m[1][3] - device.m[1][3], panel.m[2][3] - device.m[2][3]};
    const Vec3 after {swung.m[0][3] - device.m[0][3], swung.m[1][3] - device.m[1][3], swung.m[2][3] - device.m[2][3]};
    CHECK(near(len(before), len(after), 1e-3));

    // end() releases
    grab.end();
    CHECK(!grab.active());

    // A second grab replaces the first one's offset
    CHECK(grab.begin(3, moved, panel));
    CHECK(grab.device() == 3 && nearPose(grab.follow(moved), panel));

    // An invalid panel pose can't be grabbed (and clears a previous grab)
    DetachedPose invalid;
    CHECK(grab.active());
    CHECK(!grab.begin(3, device, invalid));
    CHECK(!grab.active());
}

void testPlaceInFront() {
    // Standing, looking along -z (the default): the HUD is 1 m ahead at eye height, upright, facing back
    {
        const vr::HmdMatrix34_t hmd = identityMatrix();
        vr::HmdMatrix34_t eye = hmd;
        eye.m[0][3] = 0.3f;
        eye.m[1][3] = 1.6f;
        eye.m[2][3] = -0.2f;
        const DetachedPose p = placeInFront(eye);
        CHECK(p.valid);
        CHECK(isRotation(toHmdMatrix(p)));
        CHECK(near(p.m[0][3], 0.3) && near(p.m[1][3], 1.6) && near(p.m[2][3], -1.2));
        CHECK(nearVec(col(p, 1), {0, 1, 0}));
        CHECK(nearVec(col(p, 2), {0, 0, 1}));  // front (+z) toward the viewer
    }
    // Turned by 40°, with a pitch up and a roll of the head: gaze is flattened, distance and height as asked
    {
        const vr::HmdMatrix34_t hmd = anyPose(40, 25, 12, -1.0, 1.7, 0.5);
        const DetachedPose p = placeInFront(hmd);
        CHECK(isRotation(toHmdMatrix(p)));
        const Vec3 eye {-1.0, 1.7, 0.5};
        const Vec3 d {p.m[0][3] - eye.x, p.m[1][3] - eye.y, p.m[2][3] - eye.z};
        CHECK(near(d.y, 0));
        CHECK(near(std::hypot(d.x, d.z), kDetachedFrontDistanceM));
        // In the direction of the gaze's floor projection (the HMD's −z, flattened)
        const Vec3 gaze = col(hmd, 2);
        const double gx = -gaze.x, gz = -gaze.z, gl = std::hypot(gx, gz);
        CHECK(near(d.x, gx / gl * kDetachedFrontDistanceM, 1e-3) && near(d.z, gz / gl * kDetachedFrontDistanceM, 1e-3));
        // Upright and facing the HMD
        CHECK(nearVec(col(p, 1), {0, 1, 0}, 1e-3));
        CHECK(near(col(p, 0).y, 0, 1e-3));
        const Vec3 toEye {-d.x, 0, -d.z};
        CHECK(nearVec(col(p, 2), {toEye.x / len(toEye), 0, toEye.z / len(toEye)}, 1e-3));
    }
    // Looking straight down: the top of the head points the way the user faces
    {
        vr::HmdMatrix34_t down {};
        down.m[0][0] = 1.0f;   // x = (1, 0, 0)
        down.m[2][1] = -1.0f;  // y = (0, 0, -1)
        down.m[1][2] = 1.0f;   // z = (0, 1, 0): the gaze (−z) is straight down
        down.m[1][3] = 1.5f;
        CHECK(isRotation(down));
        const DetachedPose p = placeInFront(down);
        CHECK(p.valid && isRotation(toHmdMatrix(p)));
        CHECK(near(p.m[0][3], 0) && near(p.m[1][3], 1.5) && near(p.m[2][3], -1.0));
        CHECK(nearVec(col(p, 1), {0, 1, 0}) && nearVec(col(p, 2), {0, 0, 1}));
    }
    // Looking straight up (head tipped back: the top of the head points BEHIND the user). The HUD still goes
    // in front of them (along −z here).
    {
        vr::HmdMatrix34_t up {};
        up.m[0][0] = 1.0f;    // x = (1, 0, 0)
        up.m[2][1] = 1.0f;    // y = (0, 0, 1)
        up.m[1][2] = -1.0f;   // z = (0, -1, 0): the gaze (−z) is straight up
        up.m[1][3] = 1.5f;
        CHECK(isRotation(up));
        const DetachedPose p = placeInFront(up);
        CHECK(p.valid && isRotation(toHmdMatrix(p)));
        CHECK(near(p.m[1][3], 1.5));
        CHECK(near(std::hypot(p.m[0][3], p.m[2][3]), kDetachedFrontDistanceM));  // 1 m away, whichever side
        CHECK(near(p.m[0][3], 0) && near(p.m[2][3], -1.0));  // in front (-z), not behind
        CHECK(nearVec(col(p, 1), {0, 1, 0}) && nearVec(col(p, 2), {0, 0, 1}));
    }
}

void testFaceTowards() {
    DetachedPose start;
    start.valid = true;
    start.m[0][3] = 1;
    start.m[1][3] = 1.2;
    start.m[2][3] = -2;

    // Target to the side at the same height: z points at it, upright, x horizontal
    const DetachedPose a = faceTowards(start, 3, 1.2, 0);
    CHECK(a.valid && isRotation(toHmdMatrix(a)));
    CHECK(near(a.m[0][3], 1) && near(a.m[1][3], 1.2) && near(a.m[2][3], -2));  // position kept
    const double l = std::sqrt(8.0);
    CHECK(nearVec(col(a, 2), {2 / l, 0, 2 / l}));
    CHECK(nearVec(col(a, 1), {0, 1, 0}));
    CHECK(near(col(a, 0).y, 0));

    // Target above and in front: z tilts up, x stays horizontal, y stays on the upper side
    const DetachedPose b = faceTowards(start, 1, 2.2, -1);
    CHECK(isRotation(toHmdMatrix(b)));
    CHECK(near(col(b, 2).y, std::sqrt(0.5)) && near(col(b, 2).z, std::sqrt(0.5)));
    CHECK(near(col(b, 0).y, 0) && col(b, 1).y > 0);

    // Straight above or below: keeps its x axis, stays a rotation
    DetachedPose turned = start;
    turned = faceTowards(turned, 4, 1.2, -2);  // face +x first
    const DetachedPose above = faceTowards(turned, 1, 5, -2);
    CHECK(isRotation(toHmdMatrix(above)));
    CHECK(nearVec(col(above, 2), {0, 1, 0}));
    CHECK(near(len(col(above, 0)), 1));
    const DetachedPose below = faceTowards(turned, 1, -5, -2);
    CHECK(isRotation(toHmdMatrix(below)));
    CHECK(nearVec(col(below, 2), {0, -1, 0}));

    // A point on top of it: unchanged
    const DetachedPose same = faceTowards(turned, turned.m[0][3], turned.m[1][3], turned.m[2][3]);
    CHECK(same == turned);

    // Roundtrip with placeInFront: the HUD is already facing the HMD, so face-me changes nothing
    const vr::HmdMatrix34_t hmd = anyPose(-70, 0, 0, 0.5, 1.6, 0.5);
    const DetachedPose front = placeInFront(hmd);
    const DetachedPose again = faceTowards(front, 0.5, 1.6, 0.5);
    CHECK(nearPose(front, again, 1e-3));
}

void testNudge() {
    // Rotated about y by 90°: the HUD's right (x) is world −z, its front (z) is world +x
    DetachedPose p = fromHmdMatrix(yawPose(90, 1, 1.5, -1));
    CHECK(nearVec(col(p, 0), {0, 0, -1}) && nearVec(col(p, 2), {1, 0, 0}));
    const DetachedPose right = nudgeDetached(p, 0, 0.1);
    CHECK(near(right.m[0][3], 1) && near(right.m[1][3], 1.5) && near(right.m[2][3], -1.1));
    const DetachedPose up = nudgeDetached(p, 1, 0.05);
    CHECK(near(up.m[0][3], 1) && near(up.m[1][3], 1.55) && near(up.m[2][3], -1));
    const DetachedPose near_ = nudgeDetached(p, 2, kDetachedDepthStepM);  // towards the viewer = along +z of the HUD
    CHECK(near(near_.m[0][3], 1.05) && near(near_.m[1][3], 1.5) && near(near_.m[2][3], -1));
    const DetachedPose far_ = nudgeDetached(p, 2, -kDetachedDepthStepM);
    CHECK(near(far_.m[0][3], 0.95));
    // Rotation untouched, valid kept
    CHECK(right.valid && nearVec(col(right, 0), col(p, 0)) && nearVec(col(right, 2), col(p, 2)));
    // Nudge and back is the identity
    CHECK(nearPose(nudgeDetached(nudgeDetached(p, 0, kDetachedNudgeM), 0, -kDetachedNudgeM), p, 1e-9));
    // Tilted HUD: "up" follows the HUD, not the world
    const DetachedPose tilted = fromHmdMatrix(anyPose(0, 45, 0, 0, 1, 0));
    const DetachedPose tUp = nudgeDetached(tilted, 1, 1.0);
    CHECK(nearVec({tUp.m[0][3], tUp.m[1][3] - 1, tUp.m[2][3]}, col(tilted, 1)));
}

/** @return the rotation panelRotation builds, as a matrix */
vr::HmdMatrix34_t rotationOf(double yaw, double pitch, double roll) {
    return anyPose(yaw, pitch, roll, 0, 0, 0);
}

void testRotate() {
    const DetachedPose id = fromHmdMatrix(identityMatrix());
    // Same sign conventions as the head panel: yaw + / pitch + / roll + = panelRotation's
    for (const double a : {5.0, 30.0, -20.0, 90.0}) {
        vr::HmdMatrix34_t m = toHmdMatrix(rotateDetached(id, 1, a));
        CHECK(nearMatrix(m, rotationOf(a, 0, 0)));
        m = toHmdMatrix(rotateDetached(id, 0, a));
        CHECK(nearMatrix(m, rotationOf(0, a, 0)));
        m = toHmdMatrix(rotateDetached(id, 2, a));
        CHECK(nearMatrix(m, rotationOf(0, 0, a)));
    }
    // What the signs mean: yaw + turns the face towards +x, pitch + towards +y, roll + counter-clockwise from the front
    CHECK(col(rotateDetached(id, 1, 10), 2).x > 0);
    CHECK(col(rotateDetached(id, 0, 10), 2).y > 0);
    CHECK(col(rotateDetached(id, 2, 10), 0).y > 0);
    CHECK(col(rotateDetached(id, 1, -10), 2).x < 0);

    // In place: the position stays, valid stays
    DetachedPose p = fromHmdMatrix(anyPose(33, -12, 7, 0.4, 1.3, -0.9));
    for (int axis = 0; axis < 3; ++axis) {
        const DetachedPose r = rotateDetached(p, axis, 15);
        CHECK(r.valid && isRotation(toHmdMatrix(r)));
        CHECK(near(r.m[0][3], 0.4, 1e-6) && near(r.m[1][3], 1.3, 1e-6) && near(r.m[2][3], -0.9, 1e-6));
        // Turned about the HUD's own axis: that axis doesn't move
        const int fixedColumn = axis == 0 ? 0 : (axis == 1 ? 1 : 2);
        CHECK(nearVec(col(r, fixedColumn), col(p, fixedColumn), 1e-3));
        // ... by the right angle: the other two axes turned 15° about it
        const int other = axis == 0 ? 1 : (axis == 1 ? 2 : 0);
        CHECK(near(dot(col(r, other), col(p, other)), std::cos(15 * M_PI / 180), 1e-3));
        // Back again
        CHECK(nearPose(rotateDetached(r, axis, -15), p, 1e-3));
    }
    // The pose rotation is the composition of the old one and panelRotation's (HUD's own axes)
    {
        const DetachedPose r = rotateDetached(p, 1, 25);
        vr::HmdMatrix34_t local = rotationOf(25, 0, 0);
        CHECK(nearMatrix(toHmdMatrix(r), composeTransform(toHmdMatrix(p), local), 1e-3));
    }
}

// ---- Config ----

std::string tempDir;

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void writeFile(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

bool anyWarningMentions(const std::vector<std::string>& warnings, const std::string& what) {
    for (const std::string& w : warnings) {
        if (w.find(what) != std::string::npos) return true;
    }
    return false;
}

void testConfigRoundTrip() {
    const std::string path = tempDir + "/roundtrip.json";
    Config c;
    c.attachment = Attachment::Detached;
    c.detachedPose = fromHmdMatrix(anyPose(35, -14, 9, 0.123456789, 1.23456789, -2.3456789));
    std::string error;
    CHECK(saveConfig(path, c, error));
    const std::string text = readFile(path);
    CHECK(text.find("\"attachment\": \"detached\"") != std::string::npos);
    CHECK(text.find("\"detached_pose\": [") != std::string::npos);

    Config back;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, back, warnings, error));
    CHECK(back.attachment == Attachment::Detached);
    CHECK(back.detachedPose.valid);
    CHECK(!anyWarningMentions(warnings, "detached"));
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 4; ++k) {
            // 6 significant digits: relative error below 1e-5
            const double want = c.detachedPose.m[r][k];
            CHECK(std::fabs(back.detachedPose.m[r][k] - want) <= 1e-5 * std::max(1.0, std::fabs(want)));
        }
    }
    CHECK(isRotation(toHmdMatrix(back.detachedPose), 1e-4));

    // Saved again, a second load gives the same 6-digit values (no drift)
    const std::string path2 = tempDir + "/roundtrip2.json";
    CHECK(saveConfig(path2, back, error));
    Config again;
    warnings.clear();
    CHECK(loadConfig(path2, again, warnings, error));
    CHECK(again.detachedPose == back.detachedPose);

    // Exactly 6 significant digits on disk
    const std::string path3 = tempDir + "/digits.json";
    Config digits;
    digits.detachedPose.valid = true;
    digits.detachedPose.m[0][3] = 0.123456789;
    digits.detachedPose.m[1][3] = -1234.56789;
    digits.detachedPose.m[2][3] = -0.0;
    CHECK(saveConfig(path3, digits, error));
    const std::string t3 = readFile(path3);
    CHECK(t3.find("0.123457") != std::string::npos);
    CHECK(t3.find("-1234.57") != std::string::npos);
    CHECK(t3.find("-0,") == std::string::npos && t3.find("-0]") == std::string::npos);  // negative zero is written as 0

    // Every attachment name round-trips
    for (const Attachment a : {Attachment::Head, Attachment::LeftWrist, Attachment::RightWrist, Attachment::Detached}) {
        Config x;
        x.attachment = a;
        CHECK(saveConfig(path, x, error));
        Config y;
        warnings.clear();
        CHECK(loadConfig(path, y, warnings, error));
        CHECK(y.attachment == a);
        CHECK(std::string(attachmentName(a)).size() > 0);
    }
    CHECK(std::string(attachmentName(Attachment::Detached)) == "detached");
}

void testConfigNoPoseNoKey() {
    const std::string path = tempDir + "/nopose.json";
    Config c;
    c.attachment = Attachment::Detached;  // chosen but never placed
    CHECK(!c.detachedPose.valid);
    std::string error;
    CHECK(saveConfig(path, c, error));
    CHECK(readFile(path).find("detached_pose") == std::string::npos);
    Config back;
    std::vector<std::string> warnings;
    CHECK(loadConfig(path, back, warnings, error));
    CHECK(back.attachment == Attachment::Detached);
    CHECK(!back.detachedPose.valid);
    CHECK(!anyWarningMentions(warnings, "detached"));

    // A missing file: defaults, no pose
    Config none;
    CHECK(loadConfig(tempDir + "/does-not-exist.json", none, warnings, error));
    CHECK(!none.detachedPose.valid && none.attachment == Attachment::Head);
}

void testConfigMalformedPose() {
    const std::string cases[] = {
        "[1,0,0,0, 0,1,0,0, 0,0,1]",                  // 11 numbers
        "[1,0,0,0, 0,1,0,0, 0,0,1,0, 0]",             // 13 numbers
        "[]",                                         // empty
        "[1,0,0,0, 0,1,0,0, 0,0,\"x\",0]",            // a string
        "[1,0,0,0, 0,1,0,0, 0,0,null,0]",             // null
        "[1,0,0,0, 0,1,0,0, 0,0,true,0]",             // bool
        "[[1,0,0,0],[0,1,0,0],[0,0,1,0]]",            // nested 3x4
        "\"1,0,0,0\"",                                // a string, not an array
        "{\"m\": 1}",                                 // an object
        "12",                                         // a number
        "null",
    };
    int n = 0;
    for (const std::string& value : cases) {
        const std::string path = tempDir + "/bad" + std::to_string(n++) + ".json";
        writeFile(path, "{\n  \"attachment\": \"detached\",\n  \"detached_pose\": " + value + "\n}\n");
        Config c;
        std::vector<std::string> warnings;
        std::string error;
        CHECK(loadConfig(path, c, warnings, error));  // the file still loads
        CHECK(!c.detachedPose.valid);
        CHECK(anyWarningMentions(warnings, "detached_pose"));
        CHECK(c.attachment == Attachment::Detached);  // the rest of the file is honoured
        if (c.detachedPose.valid || !anyWarningMentions(warnings, "detached_pose")) {
            std::printf("  (case %d: %s)\n", n - 1, value.c_str());
        }
    }
    // Good one for contrast: the same shape with 12 numbers
    const std::string good = tempDir + "/good.json";
    writeFile(good, "{ \"detached_pose\": [1,0,0,0.5, 0,1,0,1.5, 0,0,1,-2] }");
    Config c;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(loadConfig(good, c, warnings, error));
    CHECK(c.detachedPose.valid && c.detachedPose.m[0][3] == 0.5 && c.detachedPose.m[1][3] == 1.5 && c.detachedPose.m[2][3] == -2);
    CHECK(!anyWarningMentions(warnings, "detached_pose"));

    // A bad attachment name warns and keeps the head
    const std::string badName = tempDir + "/badname.json";
    writeFile(badName, "{ \"attachment\": \"floating\" }");
    Config d;
    warnings.clear();
    CHECK(loadConfig(badName, d, warnings, error));
    CHECK(d.attachment == Attachment::Head);
    CHECK(anyWarningMentions(warnings, "attachment"));
}

}  // namespace

int main() {
    char pattern[] = "/tmp/fph-detached-test-XXXXXX";
    const char* dir = mkdtemp(pattern);
    if (dir == nullptr) {
        std::printf("mkdtemp failed\n");
        return 1;
    }
    tempDir = dir;

    testConvert();
    testInvertRigid();
    testOrthonormalize();
    testGrabKeepsTheHudRigid();
    testPlaceInFront();
    testFaceTowards();
    testNudge();
    testRotate();
    testConfigRoundTrip();
    testConfigNoPoseNoKey();
    testConfigMalformedPose();

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    if (failures == 0) std::printf("detached-test: all passed\n");
    return failures == 0 ? 0 : 1;
}
