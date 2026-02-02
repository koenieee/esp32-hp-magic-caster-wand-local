#include "spell_detector.h"
#include <cmath>
#include <algorithm>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "spell_detector";

// Helper macros to replace Arduino's min/max
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

// 73 Spell names from the Magic Caster Wand
const char *SPELL_NAMES[73] = {
    "The_Force_Spell",                   // 1
    "Colloportus",                       // 2
    "Colloshoo",                         // 3
    "The_Hour_Reversal_Reversal_Charm",  // 4
    "Evanesco",                          // 5
    "Herbivicus",                        // 6
    "Orchideous",                        // 7
    "Brachiabindo",                      // 8
    "Meteolojinx",                       // 9
    "Riddikulus",                        // 10
    "Silencio",                          // 11
    "Immobulus",                         // 12
    "Confringo",                         // 13
    "Petrificus_Totalus",                // 14
    "Flipendo",                          // 15
    "The_Cheering_Charm",                // 16
    "Salvio_Hexia",                      // 17
    "Pestis_Incendium",                  // 18
    "Alohomora",                         // 19
    "Protego",                           // 20
    "Langlock",                          // 21
    "Mucus_Ad_Nauseum",                  // 22
    "Flagrate",                          // 23
    "Glacius",                           // 24
    "Finite",                            // 25
    "Anteoculatia",                      // 26
    "Expelliarmus",                      // 27
    "Expecto_Patronum",                  // 28
    "Descendo",                          // 29
    "Depulso",                           // 30
    "Reducto",                           // 31
    "Colovaria",                         // 32
    "Aberto",                            // 33
    "Confundo",                          // 34
    "Densaugeo",                         // 35
    "The_Stretching_Jinx",               // 36
    "Entomorphis",                       // 37
    "The_Hair_Thickening_Growing_Charm", // 38
    "Bombarda",                          // 39
    "Finestra",                          // 40
    "The_Sleeping_Charm",                // 41
    "Rictusempra",                       // 42
    "Piertotum_Locomotor",               // 43
    "Expulso",                           // 44
    "Impedimenta",                       // 45
    "Ascendio",                          // 46
    "Incarcerous",                       // 47
    "Ventus",                            // 48
    "Revelio",                           // 49
    "Accio",                             // 50
    "Melefors",                          // 51
    "Scourgify",                         // 52
    "Wingardium_Leviosa",                // 53
    "Nox",                               // 54
    "Stupefy",                           // 55
    "Spongify",                          // 56
    "Lumos",                             // 57
    "Appare_Vestigium",                  // 58
    "Verdimillious",                     // 59
    "Fulgari",                           // 60
    "Reparo",                            // 61
    "Locomotor",                         // 62
    "Quietus",                           // 63
    "Everte_Statum",                     // 64
    "Incendio",                          // 65
    "Aguamenti",                         // 66
    "Sonorus",                           // 67
    "Cantis",                            // 68
    "Arania_Exumai",                     // 69
    "Calvorio",                          // 70
    "The_Hour_Reversal_Charm",           // 71
    "Vermillious",                       // 72
    "The_Pepper-Breath_Hex"              // 73
};

// ============================================================================
// IMU Parser Implementation
// ============================================================================

size_t IMUParser::parse(const uint8_t *data, size_t len, IMUSample *samples, size_t max_samples)
{
    if (!data || !samples || len < 4 || data[0] != 0x2C)
    {
        if (!data || !samples)
        {
            ESP_LOGW(TAG, "IMUParser::parse called with NULL pointer");
        }
        return 0; // Invalid packet
    }

    uint8_t sample_count = data[3];
    if (sample_count == 0 || len < 4 + sample_count * 12)
    {
        return 0; // Invalid length
    }

    size_t count = min((size_t)sample_count, max_samples);

    for (size_t i = 0; i < count; i++)
    {
        const uint8_t *ptr = data + 4 + i * 12;

        // Parse 6 signed shorts (little-endian)
        int16_t raw_gyro_x = (int16_t)(ptr[0] | (ptr[1] << 8));
        int16_t raw_gyro_y = (int16_t)(ptr[2] | (ptr[3] << 8));
        int16_t raw_gyro_z = (int16_t)(ptr[4] | (ptr[5] << 8));
        int16_t raw_accel_x = (int16_t)(ptr[6] | (ptr[7] << 8));
        int16_t raw_accel_y = (int16_t)(ptr[8] | (ptr[9] << 8));
        int16_t raw_accel_z = (int16_t)(ptr[10] | (ptr[11] << 8));

        // Apply scaling factors
        samples[i].gyro_x = raw_gyro_x * GYROSCOPE_SCALE;
        samples[i].gyro_y = raw_gyro_y * GYROSCOPE_SCALE;
        samples[i].gyro_z = raw_gyro_z * GYROSCOPE_SCALE;
        samples[i].accel_x = raw_accel_x * ACCELEROMETER_SCALE;
        samples[i].accel_y = raw_accel_y * ACCELEROMETER_SCALE;
        samples[i].accel_z = raw_accel_z * ACCELEROMETER_SCALE;

        // Apply coordinate transformation
        transformCoordinates(samples[i]);
    }

    return count;
}

void IMUParser::transformCoordinates(IMUSample &sample)
{
    // Android to standard frame transformation
    float temp_ax = sample.accel_x;
    float temp_ay = sample.accel_y;
    float temp_gx = sample.gyro_x;
    float temp_gy = sample.gyro_y;

    sample.accel_x = temp_ay;
    sample.accel_y = -temp_ax;
    // accel_z stays the same

    sample.gyro_x = temp_gy;
    sample.gyro_y = -temp_gx;
    // gyro_z stays the same
}

// ============================================================================
// AHRS Tracker Implementation
// ============================================================================

AHRSTracker::AHRSTracker() : position_count(0), tracking(false), beta(0.1f), initial_yaw(0.0f)
{
    positions = new (std::nothrow) Position2D[MAX_POSITIONS];
    if (!positions)
    {
        ESP_LOGE(TAG, "FATAL: Failed to allocate AHRS positions array");
        // Cannot recover from this in constructor, but log it
    }
    quat = Quaternion();           // Identity quaternion (1.0, 0.0, 0.0, 0.0) for AHRS
    start_quat = Quaternion(0.0f); // ZERO quaternion (0.0, 0.0, 0.0, 0.0) - matches Python
    inv_quat = Quaternion(0.0f);   // ZERO quaternion (0.0, 0.0, 0.0, 0.0) - matches Python
    ref_vec_x = ref_vec_y = ref_vec_z = 0.0f;
    start_pos_x = start_pos_y = 0.0f;
    start_pos_z = -294.0f; // Match Python's default start_pos_z = -294.0
}

AHRSTracker::~AHRSTracker()
{
    delete[] positions;
}

float AHRSTracker::invSqrt(float x)
{
    // Quake III fast inverse square root
    float halfx = 0.5f * x;
    float y = x;
    long i = *(long *)&y;
    i = 0x5f3759df - (i >> 1);
    y = *(float *)&i;
    y = y * (1.5f - (halfx * y * y));
    return y;
}

void AHRSTracker::update(const IMUSample &sample)
{
    // Python multiplies accel by gravity (9.81) to convert G to m/s²
    constexpr float GRAVITY = 9.8100004196167f;

    float gx = sample.gyro_x;
    float gy = sample.gyro_y;
    float gz = sample.gyro_z;
    float ax = sample.accel_x * GRAVITY;
    float ay = sample.accel_y * GRAVITY;
    float az = sample.accel_z * GRAVITY;

    // Python's exact AHRS implementation - matches _update_imu_only
    // Only apply accelerometer correction if accel is non-zero
    if (ax != 0.0f || ay != 0.0f || az != 0.0f)
    {
        // Python: fVar2 = norm², fVar1 = 1/sqrt(norm²)
        float norm_sq = az * az + ay * ay + ax * ax;
        float recip_norm = invSqrt(norm_sq);

        // Estimated direction of gravity from quaternion - Python's formulas
        float v2x = quat.q1 * quat.q3 - quat.q0 * quat.q2;        // fVar3
        float v2y = quat.q3 * quat.q2 + quat.q1 * quat.q0;        // fVar2 (reused)
        float v2z = quat.q3 * quat.q3 + quat.q0 * quat.q0 - 0.5f; // fVar4

        // Apply gyro correction - Python uses recip_norm in the cross product
        gx = gx + (ay * recip_norm * v2z - recip_norm * az * v2y);
        gy = gy + (recip_norm * az * v2x - v2z * ax * recip_norm);
        gz = gz + (v2y * ax * recip_norm - v2x * ay * recip_norm);
    }

    // Use FIXED dt matching Python (0.0042735s = 234 Hz)
    // The wand samples IMU at 234 Hz internally, and buffers samples into BLE packets.
    // We receive batches of samples, but each sample represents 1/234 second of real time.
    // Dynamic dt measurement doesn't work because we process batches too fast.
    float dt = IMU_SAMPLE_PERIOD; // 0.0042735f - matches Python exactly

    // Integrate quaternion rate (Python's exact integration)
    float half_dt = dt * 0.5f;
    float half_gx = gx * half_dt; // fVar6
    float half_gy = gy * half_dt; // fVar4
    float half_gz = gz * half_dt; // fVar1

    // Quaternion derivative - Python's exact formulas
    float qDot0 = ((-half_gx * quat.q1) - half_gy * quat.q2 - half_gz * quat.q3) + quat.q0; // fVar3
    float qDot1 = ((half_gz * quat.q2 + quat.q0 * half_gx) - half_gy * quat.q3) + quat.q1;  // fVar2
    float qDot2 = half_gx * quat.q3 + (half_gy * quat.q0 - half_gz * quat.q1) + quat.q2;    // fVar5
    float qDot3 = ((half_gy * quat.q1 + half_gz * quat.q0) - half_gx * quat.q2) + quat.q3;  // fVar4

    // Normalize - Python: fVar6 = norm², fVar1 = 1/sqrt(norm²)
    float norm = invSqrt(qDot3 * qDot3 + qDot2 * qDot2 + qDot1 * qDot1 + qDot0 * qDot0);
    quat.q0 = qDot0 * norm; // fVar3 * fVar1
    quat.q1 = qDot1 * norm; // fVar2 * fVar1
    quat.q2 = qDot2 * norm; // fVar5 * fVar1
    quat.q3 = qDot3 * norm; // fVar1 * fVar4 (Python writes this as "fVar1 * fVar4")

    // If tracking, compute and store position - EXACT Python translation (spell_tracker.py lines 172-237)
    if (tracking && positions && position_count < MAX_POSITIONS)
    {
        // Get Euler angles from current AHRS quaternion
        float roll, pitch, yaw;
        toEuler(quat, roll, pitch, yaw);

        // Python line 175: fVar1 = yaw - self._state.initial_yaw
        // Handle wrap-around: ensure yaw_delta is in range [-π, π]
        float fVar1 = yaw - initial_yaw;
        if (fVar1 > M_PI)
        {
            fVar1 -= 2.0f * M_PI;
        }
        else if (fVar1 < -M_PI)
        {
            fVar1 += 2.0f * M_PI;
        }

        // Python lines 177-179: half angles for roll
        float half_roll = roll * 0.5f;
        float dStack_24 = sinf(half_roll);
        float dStack_2c = cosf(half_roll);

        // Python lines 181-182: half angles for pitch
        float half_pitch = pitch * 0.5f;
        float dStack_14 = sinf(half_pitch);
        float dStack_1c = cosf(half_pitch);

        // Python lines 184-186: half angles for adjusted yaw
        float half_yaw = fVar1 * 0.5f;
        float dStack_34 = sinf(half_yaw);
        float dStack_3c = cosf(half_yaw);

        // Python lines 188-191: compute quaternion from Euler angles
        float fVar9 = dStack_34 * dStack_24 * dStack_14 + dStack_3c * dStack_2c * dStack_1c;
        float fVar5 = dStack_3c * dStack_24 * dStack_1c - dStack_34 * dStack_2c * dStack_14;
        float fVar11 = dStack_24 * dStack_1c * dStack_34 + dStack_2c * dStack_14 * dStack_3c;
        float fVar3 = dStack_2c * dStack_1c * dStack_34 - dStack_24 * dStack_14 * dStack_3c;

        // Python lines 193-197: normalize by -1.0 / norm²
        float fVar7 = -1.0f / (fVar3 * fVar3 + fVar11 * fVar11 + fVar5 * fVar5 + fVar9 * fVar9);
        float fVar2 = fVar7 * fVar9 * 0.0f;  // CONST_NEG_0_0 = -0.0 = 0.0
        float fVar10 = fVar7 * fVar5 * 0.0f; // CONST_0_0
        float fVar6 = fVar7 * fVar11 * 0.0f; // CONST_0_0
        float fVar8 = fVar7 * fVar3 * 0.0f;  // CONST_0_0

        // Python lines 199-202: quaternion transformation with start_pos_z
        float fVar4 = ((fVar10 - start_pos_z * fVar7 * fVar9) + fVar8) - fVar6;
        fVar1 = ((fVar2 - start_pos_z * fVar7 * fVar5) - fVar6) - fVar8;
        fVar6 = ((fVar6 + fVar2) - start_pos_z * fVar7 * fVar3) + fVar10;
        fVar10 = (fVar7 * fVar11 * start_pos_z + fVar8 + fVar2) - fVar10;
        fVar7 = (fVar10 * fVar11 + fVar1 * fVar5 + fVar4 * fVar9) - fVar6 * fVar3;
        fVar2 = fVar4 * fVar3 + ((fVar1 * fVar11 + fVar6 * fVar9) - fVar10 * fVar5);
        fVar4 = (fVar6 * fVar5 + fVar1 * fVar3 + fVar10 * fVar9) - fVar4 * fVar11;

        // Python lines 207-211: normalize inv_quat and multiply
        fVar6 = -1.0f / (inv_quat.q3 * inv_quat.q3 + inv_quat.q2 * inv_quat.q2 + inv_quat.q1 * inv_quat.q1 + inv_quat.q0 * inv_quat.q0);
        fVar8 = inv_quat.q0 * fVar6;
        fVar5 = inv_quat.q1 * fVar6;
        fVar3 = inv_quat.q2 * fVar6;
        fVar6 = fVar6 * inv_quat.q3;

        // Python lines 213-216: quaternion multiplication
        fVar11 = ((fVar8 * 0.0f - fVar5 * fVar7) - fVar3 * fVar2) - fVar6 * fVar4;
        fVar1 = (fVar6 * fVar2 + (fVar5 * 0.0f - fVar7 * fVar8)) - fVar3 * fVar4;
        float fVar12 = fVar5 * fVar4 + ((fVar3 * 0.0f - fVar2 * fVar8) - fVar6 * fVar7);
        fVar2 = (fVar3 * fVar7 + (fVar6 * 0.0f - fVar8 * fVar4)) - fVar5 * fVar2;

        // Python lines 218-221: normalize start_quat and compute offset from reference
        fVar9 = -1.0f / (start_quat.q3 * start_quat.q3 + start_quat.q2 * start_quat.q2 + start_quat.q1 * start_quat.q1 + start_quat.q0 * start_quat.q0);
        fVar3 = ((inv_quat.q2 * fVar2 + inv_quat.q1 * fVar11 + inv_quat.q0 * fVar1) - inv_quat.q3 * fVar12) - ref_vec_x;
        fVar7 = start_quat.q0 * fVar9;
        fVar10 = start_quat.q1 * fVar9;

        // Python lines 223-226
        fVar4 = (inv_quat.q3 * fVar1 + ((inv_quat.q2 * fVar11 + inv_quat.q0 * fVar12) - inv_quat.q1 * fVar2)) - ref_vec_y;
        fVar8 = start_quat.q2 * fVar9;
        fVar5 = ((fVar12 * inv_quat.q1 + fVar11 * inv_quat.q3 + fVar2 * inv_quat.q0) - fVar1 * inv_quat.q2) - ref_vec_z;
        fVar9 = fVar9 * start_quat.q3;

        // Python lines 228-231: quaternion multiplication
        fVar2 = ((fVar7 * 0.0f - fVar10 * fVar3) - fVar8 * fVar4) - fVar9 * fVar5;
        fVar1 = (fVar9 * fVar4 + (fVar10 * 0.0f - fVar3 * fVar7)) - fVar8 * fVar5;
        fVar6 = fVar10 * fVar5 + ((fVar8 * 0.0f - fVar4 * fVar7) - fVar9 * fVar3);
        fVar4 = (fVar8 * fVar3 + (fVar9 * 0.0f - fVar7 * fVar5)) - fVar10 * fVar4;

        // Python lines 233-234: final position calculation
        fVar3 = start_quat.q3 * fVar1 + ((start_quat.q2 * fVar2 + start_quat.q0 * fVar6) - start_quat.q1 * fVar4);
        fVar1 = (fVar6 * start_quat.q1 + fVar2 * start_quat.q3 + fVar4 * start_quat.q0) - fVar1 * start_quat.q2;

        // Python lines 236-237: store position if within buffer limit (0x2000 = 8192)
        positions[position_count].x = fVar3;
        positions[position_count].y = fVar1;
        position_count++;
    }
}

void AHRSTracker::startTracking()
{
    if (tracking)
    {
        ESP_LOGW(TAG, "Tracking already active!");
        return;
    }

    // EXACT Python translation from spell_tracker.py start() method (lines 70-129)

    // Python line 74: position_count = 0
    position_count = 0;

    ESP_LOGI(TAG, "=== TRACKING STARTED ===");
    ESP_LOGI(TAG, "Current AHRS quat: [%.4f, %.4f, %.4f, %.4f]", quat.q0, quat.q1, quat.q2, quat.q3);

    // Python lines 76-77: roll, pitch, yaw = self._calc_eulers_from_attitude()
    float roll, pitch, yaw;
    toEuler(quat, roll, pitch, yaw);

    // Python line 78: self._state.initial_yaw = yaw
    initial_yaw = yaw;
    ESP_LOGI(TAG, "Initial Euler: roll=%.2f, pitch=%.2f, yaw=%.2f", roll, pitch, yaw);

    // Python lines 80-82: half_roll and trig functions (use EXACT variable names!)
    float half_roll = roll * 0.5f;     // half_roll: np.float32
    float dStack_c = sinf(half_roll);  // dStack_c: np.float32 = np.sin(half_roll)
    float dStack_14 = cosf(half_roll); // dStack_14: np.float32 = np.cos(half_roll)

    // Python lines 84-86: half_pitch and trig functions
    float half_pitch = pitch * 0.5f;    // half_pitch: np.float32
    float dStack_1c = sinf(half_pitch); // dStack_1c: np.float32 = np.sin(half_pitch)
    float dStack_24 = cosf(half_pitch); // dStack_24: np.float32 = np.cos(half_pitch)

    // Python lines 88-91: start_quat computation (yaw terms multiplied by 0.0)
    start_quat.q0 = dStack_c * dStack_1c * 0.0f + dStack_14 * dStack_24;
    start_quat.q1 = dStack_c * dStack_24 - dStack_14 * dStack_1c * 0.0f;
    start_quat.q2 = dStack_c * dStack_24 * 0.0f + dStack_14 * dStack_1c;
    start_quat.q3 = dStack_14 * dStack_24 * 0.0f - dStack_c * dStack_1c;

    ESP_LOGI(TAG, "start_quat: [%.4f, %.4f, %.4f, %.4f]", start_quat.q0, start_quat.q1, start_quat.q2, start_quat.q3);

    // Python line 93: fVar4 = -1.0 / norm²
    float fVar4 = -1.0f / (start_quat.q3 * start_quat.q3 + start_quat.q2 * start_quat.q2 +
                           start_quat.q1 * start_quat.q1 + start_quat.q0 * start_quat.q0);

    // Python lines 94-100: inv_quat initialization
    float fVar1 = fVar4 * start_quat.q0; // fVar1: np.float32
    inv_quat.q1 = fVar4 * start_quat.q1; // self._state.inv_quat_q1
    float fVar2 = fVar1 * 0.0f;          // fVar1 * CONST_NEG_0_0 (which is -0.0 = 0.0)
    float fVar7 = inv_quat.q1 * 0.0f;    // inv_quat_q1 * CONST_0_0
    inv_quat.q2 = fVar4 * start_quat.q2; // self._state.inv_quat_q2
    inv_quat.q3 = fVar4 * start_quat.q3; // self._state.inv_quat_q3
    float fVar8 = inv_quat.q2 * 0.0f;    // inv_quat_q2 * CONST_0_0
    fVar4 = inv_quat.q3 * 0.0f;          // fVar4 REUSED! = inv_quat_q3 * CONST_0_0

    // Python lines 102-105: quaternion transformation of start_pos
    float fVar5 = ((fVar7 - start_pos_z * fVar1) - fVar8) - fVar4;
    float fVar3 = ((fVar2 - start_pos_z * inv_quat.q1) - fVar8) - fVar4;
    float fVar9 = ((fVar8 + fVar2) - start_pos_z * inv_quat.q3) + fVar7;
    fVar7 = (start_pos_z * inv_quat.q2 + fVar4 + fVar2) - fVar7; // fVar7 REUSED

    // Python lines 107-109: quaternion multiply by start_quat
    fVar8 = (fVar7 * start_quat.q2 + fVar3 * start_quat.q1 + fVar5 * start_quat.q0) - fVar9 * start_quat.q3;   // fVar8 REUSED
    fVar4 = fVar5 * start_quat.q3 + ((fVar3 * start_quat.q2 + fVar9 * start_quat.q0) - fVar7 * start_quat.q1); // fVar4 REUSED
    float fVar10 = (fVar9 * start_quat.q1 + fVar3 * start_quat.q3 + fVar7 * start_quat.q0) - fVar5 * start_quat.q2;

    // Python line 111: fVar6 = -1.0 / norm² (NOTE: uses fVar1 from line 94, NOT the reused value!)
    float fVar6 = -1.0f / (inv_quat.q3 * inv_quat.q3 + inv_quat.q2 * inv_quat.q2 +
                           inv_quat.q1 * inv_quat.q1 + fVar1 * fVar1);

    // Python lines 112-118: inv_quat finalization
    inv_quat.q0 = -fVar1;        // self._state.inv_quat_q0 = -fVar1
    fVar2 = -fVar1 * fVar6;      // fVar2 REUSED
    fVar5 = inv_quat.q1 * fVar6; // fVar5 REUSED
    float fVar11 = inv_quat.q2 * fVar6;
    fVar6 = inv_quat.q3 * fVar6;                                                // fVar6 REUSED
    fVar7 = ((fVar2 * 0.0f - fVar5 * fVar8) - fVar11 * fVar4) - fVar6 * fVar10; // fVar7 REUSED
    fVar9 = (fVar6 * fVar4 + (fVar5 * 0.0f - fVar8 * fVar2)) - fVar11 * fVar10; // fVar9 REUSED
    fVar3 = fVar5 * fVar10 + ((fVar11 * 0.0f - fVar4 * fVar2) - fVar6 * fVar8); // fVar3 REUSED
    fVar4 = (fVar11 * fVar8 + (fVar6 * 0.0f - fVar2 * fVar10)) - fVar5 * fVar4; // fVar4 REUSED

    // Python lines 124-126: final reference vector calculation
    // CRITICAL: Use inv_quat.q0, NOT fVar1 (which was reused and has stale value)
    ref_vec_x = (inv_quat.q2 * fVar4 + (inv_quat.q1 * fVar7 - fVar9 * inv_quat.q0)) - inv_quat.q3 * fVar3;
    ref_vec_y = (inv_quat.q3 * fVar9 + ((inv_quat.q2 * fVar7 - fVar3 * inv_quat.q0) - inv_quat.q1 * fVar4));
    ref_vec_z = ((fVar3 * inv_quat.q1 + (fVar7 * inv_quat.q3 - fVar4 * inv_quat.q0)) - fVar9 * inv_quat.q2);

    ESP_LOGI(TAG, "inv_quat: [%.4f, %.4f, %.4f, %.4f]", inv_quat.q0, inv_quat.q1, inv_quat.q2, inv_quat.q3);
    ESP_LOGI(TAG, "Ref vector: [%.4f, %.4f, %.4f]", ref_vec_x, ref_vec_y, ref_vec_z);

    // Python line 125: positions[0] = (0.0, 0.0)
    if (positions && position_count < MAX_POSITIONS)
    {
        positions[position_count].x = 0.0f;
        positions[position_count].y = 0.0f;
        position_count++; // Python line 126: position_count = 1
    }

    // Python line 127: tracking_active = 1 (LAST - prevents position calc during init)
    tracking = true;
}

bool AHRSTracker::stopTracking(Position2D **out_positions, size_t *out_count)
{
    ESP_LOGI(TAG, "=== TRACKING STOPPED ===");
    ESP_LOGI(TAG, "Captured %zu positions", position_count);

    tracking = false;

    if (!positions)
    {
        ESP_LOGE(TAG, "stopTracking: positions array is NULL");
        return false;
    }

    if (position_count < 10)
    {
        ESP_LOGW(TAG, "Too few positions captured: %zu (need >= 10)", position_count);
        return false; // Too few samples
    }

    *out_positions = positions;
    *out_count = position_count;
    return true;
}

void AHRSTracker::reset()
{
    quat = Quaternion();
    start_quat = Quaternion();
    position_count = 0;
    tracking = false;
}

float AHRSTracker::wrapTo2Pi(float angle)
{
    // Python: return angle if angle >= 0.0 else angle + 2.0 * pi
    return (angle >= 0.0f) ? angle : (angle + 2.0f * M_PI);
}

void AHRSTracker::toEuler(const Quaternion &q, float &roll, float &pitch, float &yaw)
{
    // EXACT Python translation from spell_tracker.py _calc_eulers_from_attitude() (lines 243-275)

    // Python lines 246-249: qw, qx, qy, qz = quaternion components
    float qw = q.q0; // Python: qw: np.float32 = self._state.ahrs_quat_q0
    float qx = q.q1; // Python: qx: np.float32 = self._state.ahrs_quat_q1
    float qy = q.q2; // Python: qy: np.float32 = self._state.ahrs_quat_q2
    float qz = q.q3; // Python: qz: np.float32 = self._state.ahrs_quat_q3

    // Python lines 251-253: Calculate roll
    float sinroll_cospitch = 2.0f * (qy * qz + qw * qx);        // Python: _CONST_2_0 * (qy*qz + qw*qx)
    float cosroll_cospitch = 1.0f - 2.0f * (qx * qx + qy * qy); // Python: _CONST_1_0 - _CONST_2_0 * (qx * qx + qy * qy)
    roll = atan2f(sinroll_cospitch, cosroll_cospitch);          // Python: np.arctan2(sinroll_cospitch, cosroll_cospitch)

    // Python lines 255-267: Calculate pitch with gimbal lock check
    float gimbal_test = qw * qz + qx * qy;
    if (gimbal_test != 0.5f || isnan(gimbal_test)) // Python: if gimbal_test != SpellTracker._CONST_0_5 or np.isnan(gimbal_test)
    {
        if (gimbal_test != -0.5f || isnan(gimbal_test)) // Python: if gimbal_test != SpellTracker._CONST_NEG_0_5 or np.isnan(gimbal_test)
        {
            // Standard calculation
            float sinpitch = 2.0f * (qw * qy - qz * qx);                  // Python: _CONST_2_0 * (qw * qy - qz * qx)
            float sinpitch_clamped = fminf(fmaxf(sinpitch, -1.0f), 1.0f); // Python: np.clip(sinpitch, _CONST_NEG_1_0, _CONST_1_0)
            pitch = asinf(sinpitch_clamped);                              // Python: np.arcsin(sinpitch_clamped)
        }
        else
        {
            // gimbal_test == -0.5
            pitch = -2.0f * atan2f(qx, qw); // Python: _CONST_NEG_2_0 * np.arctan2(qx, qw)
        }
    }
    else
    {
        // gimbal_test == 0.5
        pitch = 2.0f * atan2f(qx, qw); // Python: _CONST_2_0 * np.arctan2(qx, qw)
    }

    // Python lines 269-271: Calculate yaw
    float sinyaw_cospitch = 2.0f * (qw * qz + qx * qy);        // Python: _CONST_2_0 * (qw * qz + qx * qy)
    float cosyaw_cospitch = 1.0f - 2.0f * (qy * qy + qz * qz); // Python: _CONST_1_0 - _CONST_2_0 * (qy * qy + qz * qz)
    yaw = atan2f(sinyaw_cospitch, cosyaw_cospitch);            // Python: np.arctan2(sinyaw_cospitch, cosyaw_cospitch)

    // Python line 273: return self._wrap_to_2pi(roll), pitch, self._wrap_to_2pi(yaw)
    roll = wrapTo2Pi(roll);
    yaw = wrapTo2Pi(yaw);
    // Note: pitch is NOT wrapped (Python only wraps roll and yaw)
}

Quaternion AHRSTracker::conjugate(const Quaternion &q)
{
    Quaternion result;
    result.q0 = q.q0;
    result.q1 = -q.q1;
    result.q2 = -q.q2;
    result.q3 = -q.q3;
    return result;
}

Quaternion AHRSTracker::multiply(const Quaternion &a, const Quaternion &b)
{
    Quaternion result;
    result.q0 = a.q0 * b.q0 - a.q1 * b.q1 - a.q2 * b.q2 - a.q3 * b.q3;
    result.q1 = a.q0 * b.q1 + a.q1 * b.q0 + a.q2 * b.q3 - a.q3 * b.q2;
    result.q2 = a.q0 * b.q2 - a.q1 * b.q3 + a.q2 * b.q0 + a.q3 * b.q1;
    result.q3 = a.q0 * b.q3 + a.q1 * b.q2 - a.q2 * b.q1 + a.q3 * b.q0;
    return result;
}

// ============================================================================
// Gesture Preprocessor Implementation
// ============================================================================

bool GesturePreprocessor::preprocess(const Position2D *input, size_t input_count,
                                     float *output, size_t output_size)
{
    // EXACT Python translation from spell_tracker.py _recognize_spell() (lines 313-425)

    if (!input || !output || output_size != SPELL_INPUT_SIZE)
    {
        ESP_LOGW(TAG, "Invalid parameters for preprocess");
        return false;
    }

    // Python: positions: np.ndarray = self._state.positions
    // Python: position_count: int = self._state.position_count
    const Position2D *positions = input;
    size_t position_count = input_count;

    // Python Phase 1: Calculate bounding box (min/max X and Y) - lines 335-352
    float min_x = INFINITY;  // Python: np.float32(np.inf)
    float max_x = -INFINITY; // Python: np.float32(-np.inf)
    float min_y = INFINITY;  // Python: np.float32(np.inf)
    float max_y = -INFINITY; // Python: np.float32(-np.inf)

    for (size_t i = 0; i < position_count; i++)
    {
        float x = positions[i].x; // Python: x: np.float32 = positions[i, 0]
        float y = positions[i].y; // Python: y: np.float32 = positions[i, 1]

        if (x < min_x)
            min_x = x;
        if (x > max_x)
            max_x = x;
        if (y < min_y)
            min_y = y;
        if (y > max_y)
            max_y = y;
    }

    // Python lines 354-356: Compute bounding box size (larger of width or height)
    float width = max_x - min_x;
    float height = max_y - min_y;
    float bbox_size = fmaxf(width, height); // Python: np.maximum(width, height)

    // Python Phase 2: Early exit checks (lines 358-363)
    if (bbox_size <= 0.0f) // Python: if bbox_size <= SpellTracker._CONST_0_0
    {
        ESP_LOGW(TAG, "No movement detected");
        return false; // Python: return -1
    }

    if (position_count <= 99) // Python: if position_count <= 99
    {
        ESP_LOGW(TAG, "Not enough data points: %zu (need > 99)", position_count);
        return false; // Python: return -2
    }

    // Python Phase 3: Trim stationary tail (end of gesture) - lines 365-381
    float threshold_sq = 8.0f * 8.0f; // Python: _CONST_MILLIMETERMOVETHRESHOLD * _CONST_MILLIMETERMOVETHRESHOLD
    size_t end_index = position_count;

    if (threshold_sq > 0.0f) // Python: if threshold_sq > SpellTracker._CONST_0_0
    {
        while (end_index >= 121) // Python: while end_index >= 121: (0x79 = 121)
        {
            // Python: Compare points 40 apart from the end
            size_t curr_idx = end_index - 1;
            size_t prev_idx = curr_idx - 40;

            float dx = positions[curr_idx].x - positions[prev_idx].x;
            float dy = positions[curr_idx].y - positions[prev_idx].y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq >= threshold_sq)
                break;

            end_index -= 10;
        }
    }

    // Python Phase 4: Trim stationary head (start of gesture) - lines 383-400
    size_t start_index = 0;

    if (threshold_sq > 0.0f && end_index > 120) // Python: if threshold_sq > 0.0 and end_index > 120
    {
        while (start_index < end_index - 120) // Python: Keep at least 120 points
        {
            // Python: Compare points 10 apart from the start
            size_t curr_idx = start_index;
            size_t next_idx = curr_idx + 10;

            float dx = positions[next_idx].x - positions[curr_idx].x;
            float dy = positions[next_idx].y - positions[curr_idx].y;
            float dist_sq = dx * dx + dy * dy;

            if (dist_sq >= threshold_sq)
                break;

            start_index += 10;
        }
    }

    // Python lines 402-404: Adjust indices for resampling
    float start_float = (float)(start_index + 1); // Python: np.float32(start_index + 1)
    size_t trimmed_count = end_index - start_index;

    // Python Phase 5: Resample to 50 normalized points (100 floats) - lines 406-425
    float step = (float)trimmed_count / 50.0f; // Python: np.float32(trimmed_count) / np.float32(50.0)

    float sample_pos = start_float;
    for (size_t i = 0; i < 50; i++) // Python: for i in range(50)
    {
        size_t idx = (size_t)sample_pos; // Python: idx = int(sample_pos)

        // Python: Clamp index to valid range
        if (idx >= position_count)
            idx = position_count - 1;
        // Note: size_t is unsigned, no need to check < 0 (Python does: if idx < 0: idx = 0)

        // Python: Normalize to [0, 1] based on bounding box
        output[i * 2] = (positions[idx].x - min_x) / bbox_size;     // Python: pos_inputs[i, 0]
        output[i * 2 + 1] = (positions[idx].y - min_y) / bbox_size; // Python: pos_inputs[i, 1]

        sample_pos += step;
    }

    return true;
}

// ============================================================================
// SpellDetector Implementation
// ============================================================================

#ifdef USE_TENSORFLOW
// TensorFlow Lite enabled implementation
SpellDetector::SpellDetector()
    : model(nullptr), interpreter(nullptr),
      input_tensor(nullptr), output_tensor(nullptr),
      tensor_arena(nullptr), initialized(false), lastConfidence(0.0f),
      lastPredictedSpell(nullptr)
{
}

SpellDetector::~SpellDetector()
{
    if (interpreter)
    {
        delete interpreter;
    }
    if (tensor_arena)
    {
        delete[] tensor_arena;
    }
}

bool SpellDetector::begin(const unsigned char *model_data_ptr, size_t size)
{
    ESP_LOGI(TAG, "Initializing TensorFlow Lite spell detector...");

    // Check if model data is provided
    if (!model_data_ptr || size == 0)
    {
        ESP_LOGW(TAG, "No model data provided - spell detection disabled");
        ESP_LOGW(TAG, "Detector will run in pass-through mode (no spell detection)");
        model = nullptr;
        interpreter = nullptr;
        return true; // Return success to allow BLE client to work without model
    }

    // Load the model
    model = tflite::GetModel(model_data_ptr);
    if (model->version() != TFLITE_SCHEMA_VERSION)
    {
        ESP_LOGE(TAG, "Model schema version %d does not match supported version %d",
                 model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    // Allocate tensor arena
    tensor_arena = new (std::nothrow) uint8_t[TENSOR_ARENA_SIZE];
    if (!tensor_arena)
    {
        ESP_LOGE(TAG, "Failed to allocate tensor arena");
        return false;
    }

    // Create op resolver with all needed operations for the model
    static tflite::MicroMutableOpResolver<15> micro_op_resolver; // Increased from 10 to 15
    micro_op_resolver.AddFullyConnected();
    micro_op_resolver.AddSoftmax();
    micro_op_resolver.AddReshape();
    micro_op_resolver.AddQuantize();
    micro_op_resolver.AddDequantize();
    micro_op_resolver.AddLogistic(); // Sigmoid activation (LOGISTIC op)
    micro_op_resolver.AddRelu();     // Common activation
    micro_op_resolver.AddTanh();     // Common activation
    micro_op_resolver.AddMul();      // Multiplication
    micro_op_resolver.AddAdd();      // Addition

    // Build interpreter
    static tflite::MicroInterpreter static_interpreter(
        model, micro_op_resolver, tensor_arena, TENSOR_ARENA_SIZE);
    interpreter = &static_interpreter;

    // Allocate tensors
    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk)
    {
        ESP_LOGE(TAG, "AllocateTensors() failed");
        return false;
    }

    // Get input and output tensors
    input_tensor = interpreter->input(0);
    output_tensor = interpreter->output(0);

    // Verify input tensor shape
    ESP_LOGI(TAG, "Input tensor details:");
    ESP_LOGI(TAG, "  Dimensions: %d", input_tensor->dims->size);
    for (int i = 0; i < input_tensor->dims->size; i++)
    {
        ESP_LOGI(TAG, "  Dim[%d]: %d", i, input_tensor->dims->data[i]);
    }
    ESP_LOGI(TAG, "  Type: %d", input_tensor->type);

    // Model expects [1, 50, 2] - batch_size=1, positions=50, coords=2 (x,y)
    if (input_tensor->dims->size != 3 ||
        input_tensor->dims->data[0] != 1 ||
        input_tensor->dims->data[1] != SPELL_SAMPLE_COUNT ||
        input_tensor->dims->data[2] != 2)
    {
        ESP_LOGE(TAG, "Invalid input shape: expected [1, %d, 2], got [%d, %d, %d]",
                 SPELL_SAMPLE_COUNT,
                 input_tensor->dims->data[0],
                 input_tensor->dims->data[1],
                 input_tensor->dims->size >= 3 ? input_tensor->dims->data[2] : 0);
        return false;
    }

    // Verify output tensor shape
    if (output_tensor->dims->size != 2 ||
        output_tensor->dims->data[1] != SPELL_OUTPUT_SIZE)
    {
        ESP_LOGE(TAG, "Invalid output shape: expected [1, %d], got [%d, %d]",
                 SPELL_OUTPUT_SIZE,
                 output_tensor->dims->data[0],
                 output_tensor->dims->data[1]);
        return false;
    }

    ESP_LOGI(TAG, "TensorFlow Lite model loaded successfully");
    ESP_LOGI(TAG, "Input shape: [%d, %d]",
             input_tensor->dims->data[0],
             input_tensor->dims->data[1]);
    ESP_LOGI(TAG, "Output shape: [%d, %d]",
             output_tensor->dims->data[0],
             output_tensor->dims->data[1]);

    initialized = true;
    return true;
}

const char *SpellDetector::detect(float *positions, float confidence_threshold)
{
    // Check if initialized and model is loaded
    if (!initialized || !positions || !interpreter || !model)
    {
        return nullptr;
    }

    // // DEBUG: Log all 50 coordinate points for visualization
    // ESP_LOGI(TAG, "Gesture coordinates (all 50 points):");
    // for (int i = 0; i < SPELL_SAMPLE_COUNT; i++)
    // {
    //     float x = positions[i * 2];
    //     float y = positions[i * 2 + 1];
    //     // ESP_LOGI(TAG, "  Point %2d: (%.4f, %.4f)", i + 1, x, y);
    // }

    // Copy input data to tensor
    for (int i = 0; i < SPELL_INPUT_SIZE; i++)
    {
        input_tensor->data.f[i] = positions[i];
    }

    // Run inference
    TfLiteStatus invoke_status = interpreter->Invoke();
    if (invoke_status != kTfLiteOk)
    {
        ESP_LOGE(TAG, "Invoke() failed");
        return nullptr;
    }

    // Find highest probability spell
    int best_idx = 0;
    float best_prob = output_tensor->data.f[0];

    for (int i = 1; i < SPELL_OUTPUT_SIZE; i++)
    {
        if (output_tensor->data.f[i] > best_prob)
        {
            best_prob = output_tensor->data.f[i];
            best_idx = i;
        }
    }

    // DEBUG: Show top 5 predictions (simplified - just shows same spell 5 times for now)
    ESP_LOGI(TAG, "Top 5 predictions:");
    for (int attempt = 0; attempt < 5; attempt++)
    {
        int max_idx = 0;
        float max_prob = output_tensor->data.f[0];
        for (int i = 1; i < SPELL_OUTPUT_SIZE; i++)
        {
            if (output_tensor->data.f[i] > max_prob)
            {
                max_prob = output_tensor->data.f[i];
                max_idx = i;
            }
        }
        ESP_LOGI(TAG, "  %d. %s: %.4f%%", attempt + 1, SPELL_NAMES[max_idx], max_prob * 100.0f);
    }

    lastPredictedSpell = SPELL_NAMES[best_idx];
    lastConfidence = best_prob;

    // Check confidence threshold
    if (best_prob < confidence_threshold)
    {
        ESP_LOGW(TAG, "Low confidence: %.2f%% (threshold: %.2f%%)",
                 best_prob * 100.0f, confidence_threshold * 100.0f);
        return nullptr;
    }

    return SPELL_NAMES[best_idx];
}

#else
// Mock implementation when TensorFlow is disabled
SpellDetector::SpellDetector()
    : model_data(nullptr), model_size(0),
      initialized(false), lastConfidence(0.0f)
{
}

SpellDetector::~SpellDetector()
{
}

bool SpellDetector::begin(const unsigned char *model_data_ptr, size_t size)
{
    ESP_LOGI(TAG, "Initializing spell detector (MOCK MODE - TensorFlow disabled)...");
    ESP_LOGI(TAG, "To enable real inference:");
    ESP_LOGI(TAG, "  1. Uncomment -DUSE_TENSORFLOW in platformio.ini");
    ESP_LOGI(TAG, "  2. Uncomment tensorflow library in platformio.ini");
    ESP_LOGI(TAG, "  3. Build on Linux");

    model_data = (unsigned char *)model_data_ptr;
    model_size = size;

    ESP_LOGI(TAG, "Model loaded: %d bytes (not used in mock mode)", size);
    initialized = true;
    return true;
}

const char *SpellDetector::detect(float *positions, float confidence_threshold)
{
    if (!initialized || !positions)
    {
        return nullptr;
    }

    ESP_LOGI(TAG, "MOCK DETECTION: Returning test spell");
    ESP_LOGI(TAG, "Enable TensorFlow for real inference");

    // Mock: Return test spell
    lastConfidence = 0.95f;
    return SPELL_NAMES[0]; // "The_Force_Spell"
}
#endif