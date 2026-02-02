#ifndef SPELL_DETECTOR_H
#define SPELL_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#define USE_TENSORFLOW yes

#ifdef USE_TENSORFLOW
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"
#endif

// IMU Configuration
#define ACCELEROMETER_SCALE 0.00048828125f // Convert to G-forces
#define GYROSCOPE_SCALE 0.0010908308f      // Convert to rad/s
#define GRAVITY_CONSTANT 9.8100004196167f
#define IMU_SAMPLE_PERIOD 0.0042735f // ~234 Hz sampling rate

// Spell Detection Configuration
#define SPELL_SAMPLE_COUNT 50   // Resampled positions for model input
#define SPELL_INPUT_SIZE 100    // 50 positions * 2 coords (x,y) - model shape [1, 50, 2]
#define SPELL_OUTPUT_SIZE 73    // Number of spell classes
#define TENSOR_ARENA_SIZE 60000 // 60KB arena for TFLite (model is memory-mapped, plenty of RAM)

#ifndef MAX_POSITIONS
#define MAX_POSITIONS 8192 // Match Python's buffer size (~35 seconds at 234 Hz)
#endif

#define SPELL_CONFIDENCE_THRESHOLD 0.99f

// Spell names array (71 spells)
extern const char *SPELL_NAMES[SPELL_OUTPUT_SIZE];

// IMU sample structure
struct IMUSample
{
    float gyro_x, gyro_y, gyro_z;    // rad/s
    float accel_x, accel_y, accel_z; // G-forces
};

// Quaternion for AHRS
struct Quaternion
{
    float q0, q1, q2, q3;

    // Default constructor for AHRS quaternion (identity for orientation tracking)
    Quaternion() : q0(1.0f), q1(0.0f), q2(0.0f), q3(0.0f) {}

    // Zero constructor (matches Python's default for start_quat/inv_quat)
    Quaternion(float zero) : q0(zero), q1(zero), q2(zero), q3(zero) {}

    void normalize()
    {
        float norm = sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
        if (norm > 0.0f)
        {
            float inv_norm = 1.0f / norm;
            q0 *= inv_norm;
            q1 *= inv_norm;
            q2 *= inv_norm;
            q3 *= inv_norm;
        }
    }
};

// 2D Position
struct Position2D
{
    float x, y;
};

// AHRS Tracker - handles quaternion fusion and position tracking
class AHRSTracker
{
private:
    Quaternion quat;
    Quaternion start_quat;
    Quaternion inv_quat; // Inverse quaternion for Python-style projection

    Quaternion mouse_start_quat;
    Quaternion mouse_inv_quat;
    float mouse_ref_vec_x, mouse_ref_vec_y, mouse_ref_vec_z;
    float mouse_initial_yaw;
    bool mouse_ref_ready;

    Position2D *positions;
    size_t position_count;
    bool tracking;

    float beta; // AHRS feedback gain

    // Reference vectors for Python-style position calculation
    float ref_vec_x, ref_vec_y, ref_vec_z;
    float start_pos_x, start_pos_y, start_pos_z;
    float initial_yaw; // Save yaw at tracking start for relative calculations

    // Fast inverse square root (Quake III algorithm)
    float invSqrt(float x);

    // Wrap angle to [0, 2π]
    float wrapTo2Pi(float angle);

    // Convert quaternion to Euler angles
    void toEuler(const Quaternion &q, float &roll, float &pitch, float &yaw);

    // Quaternion conjugate
    Quaternion conjugate(const Quaternion &q);

    // Quaternion multiplication
    Quaternion multiply(const Quaternion &a, const Quaternion &b);

    // Initialize reference from current quaternion
    void initReferenceFromCurrentQuat(Quaternion &start_q, Quaternion &inv_q,
                                      float &ref_x, float &ref_y, float &ref_z,
                                      float &initial_yaw_out);

    // Compute position from current quaternion and reference
    bool computePositionFromReference(const Quaternion &start_q, const Quaternion &inv_q,
                                      float ref_x, float ref_y, float ref_z,
                                      float initial_yaw_in, Position2D &out_pos);

public:
    AHRSTracker();
    ~AHRSTracker();

    // Update AHRS with new IMU sample
    void update(const IMUSample &sample);

    // Start tracking positions (button pressed)
    void startTracking();

    // Stop tracking and return positions (button released)
    bool stopTracking(Position2D **out_positions, size_t *out_count);

    // Check if tracking is active
    bool isTracking() { return tracking; }

    // Get current position count (for web visualization)
    size_t getPositionCount() const { return position_count; }

    // Get positions array (for web visualization) - read-only access
    const Position2D *getPositions() const { return positions; }

    // Get current mouse position (AHRS fused path)
    bool getMousePosition(Position2D &out_pos);

    // Reset mouse reference (recenters on next update)
    void resetMouseReference();

    // Reset AHRS state
    void reset();
};

// Gesture Preprocessor - normalizes positions for model input
// Now matches Python implementation exactly:
// 1. Calculate bounding box from ALL data first
// 2. Trim stationary segments (head and tail)
// 3. Resample to 50 points WITH normalization (Python-style)
class GesturePreprocessor
{
public:
    // Preprocess positions: trim, resample, normalize to [0,1]
    // This now matches the Python spell_tracker.py implementation exactly
    static bool preprocess(const Position2D *input, size_t input_count,
                           float *output, size_t output_size);
};

// TensorFlow Lite Spell Detector
class SpellDetector
{
private:
#ifdef USE_TENSORFLOW
    const tflite::Model *model;
    tflite::MicroInterpreter *interpreter;
    TfLiteTensor *input_tensor;
    TfLiteTensor *output_tensor;
    uint8_t *tensor_arena;
#else
    unsigned char *model_data;
    size_t model_size;
#endif
    bool initialized;
    float lastConfidence;
    const char *lastPredictedSpell; // Last predicted spell (even if below threshold)

public:
    SpellDetector();
    ~SpellDetector();

    // Initialize TFLite model from flash/file
    bool begin(const unsigned char *model_data, size_t model_size);

    // Run inference on normalized positions (50x2 float array)
    const char *detect(float *positions, float confidence_threshold = SPELL_CONFIDENCE_THRESHOLD);

    // Get last inference confidence
    float getConfidence() { return lastConfidence; }

    // Get last predicted spell name (even if confidence was too low)
    const char *getLastPrediction() { return lastPredictedSpell; }

    // Check if model is loaded
    bool isReady() { return initialized; }
};

// IMU Parser - extracts samples from BLE packets
class IMUParser
{
public:
    // Parse IMU packet (0x2C) and extract samples
    static size_t parse(const uint8_t *data, size_t len, IMUSample *samples, size_t max_samples);

private:
    // Apply coordinate transformation (Android -> standard frame)
    static void transformCoordinates(IMUSample &sample);
};

#endif // SPELL_DETECTOR_H