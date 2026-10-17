// T05: Detached mode maths. The HUD sits at a fixed standing-space pose (Config::detachedPose); the laser grabs it
// while the dashboard is open. Pure maths on OpenVR's matrix types, no OpenVR calls, so it's testable off the Frame.
#pragma once

#include "config.h"
#include "openvr.h"

#include <cstdint>

/** Distance "Bring it in front of me" puts the HUD at (m). */
constexpr double kDetachedFrontDistanceM = 1.0;
/** Step of the dashboard's move buttons in Detached mode (m), along the HUD's own axes. */
constexpr double kDetachedNudgeM = 0.02;
/** Step of the Near / Far buttons in Detached mode (m). */
constexpr double kDetachedDepthStepM = 0.05;

/** R2: Config::detachedCurve's steps (flat, slight, medium, strong). */
constexpr int kDetachedCurveSteps = 4;

/**
 * R2: the cylinder a Detached HUD is bent to, by its radius rather than its curvature, so a wider layout bends the
 * same per metre. Medium is kDetachedFrontDistanceM: a HUD brought in front of you curves around your head.
 * @param step Config::detachedCurve (clamped to 0..3)
 * @return the radius (m); 0 = flat
 */
inline double detachedCurveRadiusM(int step) {
    constexpr double kRadii[kDetachedCurveSteps] = {0.0, 2.0, kDetachedFrontDistanceM, 0.6};
    return kRadii[step < 0 ? 0 : (step >= kDetachedCurveSteps ? kDetachedCurveSteps - 1 : step)];
}

/** @return the pose as an OpenVR matrix */
vr::HmdMatrix34_t toHmdMatrix(const DetachedPose& pose);

/** @return the matrix as a valid pose (rotation re-orthonormalised, so rounding never builds up) */
DetachedPose fromHmdMatrix(const vr::HmdMatrix34_t& m);

/**
 * Inverse of a rigid transform (rotation + translation): [Rᵀ | −Rᵀt].
 * @param m the transform
 * @return its inverse
 */
vr::HmdMatrix34_t invertRigid(const vr::HmdMatrix34_t& m);

/**
 * Makes the rotation part orthonormal again (Gram-Schmidt on the z then y column, x = y × z). A degenerate
 * rotation becomes the identity.
 * @param m the transform (changed in place)
 */
void orthonormalize(vr::HmdMatrix34_t& m);

/**
 * "Bring it in front of me": kDetachedFrontDistanceM in front of the HMD along its gaze projected onto the floor
 * plane, at eye height, upright and facing the HMD. Looking straight up or down falls back to the HMD's own up axis.
 * @param hmd the HMD's standing-space pose
 * @return the pose
 */
DetachedPose placeInFront(const vr::HmdMatrix34_t& hmd);

/**
 * Turns the HUD in place so its front (+z) faces the given point, upright (its x axis stays horizontal).
 * @param pose the pose (position kept)
 * @param x the point (standing space)
 * @param y
 * @param z
 * @return the turned pose (unchanged if the point is on top of it)
 */
DetachedPose faceTowards(const DetachedPose& pose, double x, double y, double z);

/**
 * Moves the HUD along its own axis.
 * @param pose the pose
 * @param axis 0 = right, 1 = up, 2 = front (towards the viewer)
 * @param meters signed distance
 * @return the moved pose
 */
DetachedPose nudgeDetached(const DetachedPose& pose, int axis, double meters);

/**
 * Turns the HUD about its own axis through its centre, with the head panel's sign conventions (panelRotation):
 * yaw + = the face turns right, pitch + = the face turns up, roll + = counter-clockwise seen from the front.
 * @param pose the pose
 * @param axis 0 = pitch, 1 = yaw, 2 = roll
 * @param degrees signed angle
 * @return the turned pose
 */
DetachedPose rotateDetached(const DetachedPose& pose, int axis, double degrees);

/**
 * One laser grab of the Detached HUD: on press it remembers the HUD's pose relative to the pointing device, and while
 * held the HUD follows the device rigidly (device pose × grab offset).
 */
class DetachedGrab {
public:
    /**
     * Starts a grab.
     * @param device the pointing device (tracked device index)
     * @param devicePose its standing-space pose
     * @param panel the HUD's pose now
     * @return false (and no grab) if the panel pose isn't valid
     */
    bool begin(uint32_t device, const vr::HmdMatrix34_t& devicePose, const DetachedPose& panel);

    /**
     * @param devicePose the device's standing-space pose now
     * @return where the HUD goes: devicePose × offset
     */
    DetachedPose follow(const vr::HmdMatrix34_t& devicePose) const;

    /** Ends the grab (the caller saves the last pose it applied). */
    void end() { active_ = false; }

    /** @return true while held */
    bool active() const { return active_; }

    /** @return the device holding it (only meaningful while active) */
    uint32_t device() const { return device_; }

private:
    bool active_ = false;
    uint32_t device_ = vr::k_unTrackedDeviceIndexInvalid;
    vr::HmdMatrix34_t offset_ {};  ///< device → HUD
};
