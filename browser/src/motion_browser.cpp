// Browser MotionState: the desktop motion.cpp minus SDL sensor discovery.
//
// The browser SDL build has no sensor subsystem (SDL_OpenSensor and
// SDL_GetSensors are absent), so the device never reports built-in motion
// support, exactly as desktop Vita3K behaves on a host without an
// accelerometer and gyroscope: sampling starts and stops, and the readings
// are MotionInput's resting state (gravity down, no rotation).
#include <motion/functions.h>
#include <motion/state.h>

#include <numbers>

void MotionState::init() {
    reset_runtime();
    has_device_motion_support = false;
    device_accel_id = 0;
    device_gyro_id = 0;
}

void MotionState::clear_device_motion_support() {
    stop_sensor_sampling();
    has_device_motion_support = false;
    device_accel_id = 0;
    device_gyro_id = 0;
}

void MotionState::refresh_device_motion_support() {
    clear_device_motion_support();
}

void MotionState::stop_sensor_sampling() {
    is_sampling = false;
    device_accel.release();
    device_gyro.release();
}

void MotionState::start_sensor_sampling() {
    is_sampling = true;
}

void MotionState::reset_runtime() {
    stop_sensor_sampling();
    motion_data.ResetQuaternion();
    motion_data.ResetRotations();
    last_counter = 0;
    last_gyro_timestamp = 0;
    last_accel_timestamp = 0;
    last_updated_gyro_timestamp = 0;
    last_updated_accel_timestamp = 0;
}

SceFVector3 get_acceleration(const MotionState &state) {
    Util::Vec3f accelerometer = state.motion_data.GetAcceleration();
    return { accelerometer.x, accelerometer.y, accelerometer.z };
}

SceFVector3 get_gyroscope(const MotionState &state) {
    Util::Vec3f gyroscope = state.motion_data.GetGyroscope() * 2.f * std::numbers::pi_v<float>;
    return { gyroscope.x, gyroscope.y, gyroscope.z };
}

Util::Quaternion<SceFloat> get_orientation(const MotionState &state) {
    auto quat = state.motion_data.GetOrientation();
    return {
        { -quat.xyz[1], -quat.w, quat.xyz[0] },
        -quat.xyz[2],
    };
}

SceBool get_gyro_bias_correction(const MotionState &state) {
    return state.motion_data.IsGyroBiasEnabled();
}

void set_gyro_bias_correction(MotionState &state, SceBool setValue) {
    state.motion_data.EnableGyroBias(setValue);
}

SceBool get_tilt_correction(MotionState &state) {
    return state.motion_data.IsTiltCorrectionEnabled();
}

void set_tilt_correction(MotionState &state, SceBool setValue) {
    state.motion_data.EnableTiltCorrection(setValue);
}

SceBool get_deadband(MotionState &state) {
    return state.motion_data.IsDeadbandEnabled();
}

void set_deadband(MotionState &state, SceBool setValue) {
    state.motion_data.EnableDeadband(setValue);
}

SceFloat get_angle_threshold(const MotionState &state) {
    return state.motion_data.GetAngleThreshold();
}

void set_angle_threshold(MotionState &state, SceFloat setValue) {
    state.motion_data.SetAngleThreshold(setValue);
}

SceFVector3 get_basic_orientation(const MotionState &state) {
    return state.motion_data.GetBasicOrientation();
}
