#include "usb_hid.h"
#include "config.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <cmath>
#include "soc/rtc_cntl_reg.h"
#include "rom/ets_sys.h"
#include "esp_private/system_internal.h"

static const char *TAG = "usb_hid";

#if USE_USB_HID_DEVICE
#include "tinyusb.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "class/cdc/cdc_device.h"

// HID Report Descriptor for composite mouse + keyboard + gamepad
static const uint8_t hid_report_descriptor[] = {
    // Mouse Report (Report ID 1)
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x02, // Usage (Mouse)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x09, 0x01, //   Usage (Pointer)
    0xA1, 0x00, //   Collection (Physical)
    0x05, 0x09, //     Usage Page (Buttons)
    0x19, 0x01, //     Usage Minimum (Button 1)
    0x29, 0x03, //     Usage Maximum (Button 3)
    0x15, 0x00, //     Logical Minimum (0)
    0x25, 0x01, //     Logical Maximum (1)
    0x95, 0x03, //     Report Count (3)
    0x75, 0x01, //     Report Size (1)
    0x81, 0x02, //     Input (Data, Variable, Absolute)
    0x95, 0x01, //     Report Count (1)
    0x75, 0x05, //     Report Size (5)
    0x81, 0x01, //     Input (Constant) - padding
    0x05, 0x01, //     Usage Page (Generic Desktop)
    0x09, 0x30, //     Usage (X)
    0x09, 0x31, //     Usage (Y)
    0x09, 0x38, //     Usage (Wheel)
    0x15, 0x81, //     Logical Minimum (-127)
    0x25, 0x7F, //     Logical Maximum (127)
    0x75, 0x08, //     Report Size (8)
    0x95, 0x03, //     Report Count (3)
    0x81, 0x06, //     Input (Data, Variable, Relative)
    0xC0,       //   End Collection (Physical)
    0xC0,       // End Collection (Application)

    // Keyboard Report (Report ID 2)
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x06, // Usage (Keyboard)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x02, //   Report ID (2)
    0x05, 0x07, //   Usage Page (Key Codes)
    0x19, 0xE0, //   Usage Minimum (Left Control)
    0x29, 0xE7, //   Usage Maximum (Right GUI)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x08, //   Report Count (8)
    0x81, 0x02, //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01, //   Report Count (1)
    0x75, 0x08, //   Report Size (8)
    0x81, 0x01, //   Input (Constant) - Reserved byte
    0x95, 0x06, //   Report Count (6)
    0x75, 0x08, //   Report Size (8)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x65, //   Logical Maximum (101)
    0x05, 0x07, //   Usage Page (Key Codes)
    0x19, 0x00, //   Usage Minimum (0)
    0x29, 0x65, //   Usage Maximum (101)
    0x81, 0x00, //   Input (Data, Array) - Key array
    0xC0,       // End Collection (Application)

    // Gamepad Report (Report ID 3) - Full Xbox-Compatible Controller
    // Compatible with games like Hogwarts Legacy that require complete gamepad support
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x05, // Usage (Gamepad)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x03, //   Report ID (3)

    // Left Stick (X, Y)
    0x05, 0x01, //   Usage Page (Generic Desktop)
    0x09, 0x30, //   Usage (X)
    0x09, 0x31, //   Usage (Y)
    0x15, 0x81, //   Logical Minimum (-127)
    0x25, 0x7F, //   Logical Maximum (127)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x02, //   Report Count (2)
    0x81, 0x02, //   Input (Data, Variable, Absolute)

    // Right Stick (Rx, Ry)
    0x09, 0x33, //   Usage (Rx)
    0x09, 0x34, //   Usage (Ry)
    0x15, 0x81, //   Logical Minimum (-127)
    0x25, 0x7F, //   Logical Maximum (127)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x02, //   Report Count (2)
    0x81, 0x02, //   Input (Data, Variable, Absolute)

    // Triggers (LT, RT) - Use Simulation Control page like Xbox controllers
    0x05, 0x02, //   Usage Page (Simulation Controls)

    // Left Trigger (Brake)
    0x09, 0xC5, //   Usage (Brake)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0xFF, //   Logical Maximum (255)
    0x35, 0x00, //   Physical Minimum (0)
    0x45, 0xFF, //   Physical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data, Variable, Absolute)

    // Right Trigger (Accelerator)
    0x09, 0xC4, //   Usage (Accelerator)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0xFF, //   Logical Maximum (255)
    0x35, 0x00, //   Physical Minimum (0)
    0x45, 0xFF, //   Physical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data, Variable, Absolute)

    // Buttons (14 buttons: A, B, X, Y, LB, RB, Back, Start, LS, RS, + 4 extra)
    0x05, 0x09, //   Usage Page (Buttons)
    0x19, 0x01, //   Usage Minimum (Button 1)
    0x29, 0x0E, //   Usage Maximum (Button 14)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x0E, //   Report Count (14)
    0x81, 0x02, //   Input (Data, Variable, Absolute)

    // Padding (2 bits to make 16 bits = 2 bytes)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x02, //   Report Count (2)
    0x81, 0x01, //   Input (Constant)

    // D-Pad (Hat Switch)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (Degrees)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data, Variable, Absolute, Null State)

    // Padding (4 bits to complete the byte)
    0x75, 0x04, //   Report Size (4)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x01, //   Input (Constant)

    0xC0 // End Collection (Application)
};

// USB Descriptors for Composite HID + CDC
enum
{
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_HID 0x83

#define _PID_MAP(itf, n) ((CFG_TUD_##itf) << (n))
#define USB_TUSB_PID (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | \
                      _PID_MAP(MIDI, 3) | _PID_MAP(AUDIO, 4) | _PID_MAP(VENDOR, 5))

#if CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID
#define USB_DEVICE_VID TINYUSB_ESPRESSIF_VID
#else
#define USB_DEVICE_VID CONFIG_TINYUSB_DESC_CUSTOM_VID
#endif

#if CONFIG_TINYUSB_DESC_USE_DEFAULT_PID
#define USB_DEVICE_PID USB_TUSB_PID
#else
#define USB_DEVICE_PID CONFIG_TINYUSB_DESC_CUSTOM_PID
#endif

#define USB_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_DEVICE_VID,
    .idProduct = USB_DEVICE_PID,
    .bcdDevice = CONFIG_TINYUSB_DESC_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, USB_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_descriptor), EPNUM_HID, 16, 10),
};

static const char *string_descriptor[] = {
    (const char[]){0x09, 0x04},
    CONFIG_TINYUSB_DESC_MANUFACTURER_STRING,
    CONFIG_TINYUSB_DESC_PRODUCT_STRING,
    CONFIG_TINYUSB_DESC_SERIAL_STRING,
    "CDC Log"};

#if CONFIG_TINYUSB_CDC_ENABLED
static vprintf_like_t s_prev_log_vprintf = nullptr;

static int usb_cdc_log_vprintf(const char *fmt, va_list args)
{
    if (!tud_mounted())
    {
        if (s_prev_log_vprintf)
        {
            return s_prev_log_vprintf(fmt, args);
        }
        return vprintf(fmt, args);
    }

    char buffer[256];
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (len <= 0)
    {
        return len;
    }

    if (len > (int)sizeof(buffer))
    {
        len = sizeof(buffer);
    }

    tud_cdc_n_write(0, buffer, (uint32_t)len);
    tud_cdc_n_write_flush(0);
    return len;
}
#endif

// USB HID Keycodes (subset for common keys)
#define HID_KEY_A 0x04
#define HID_KEY_B 0x05
#define HID_KEY_C 0x06
#define HID_KEY_D 0x07
#define HID_KEY_E 0x08
#define HID_KEY_F 0x09
#define HID_KEY_G 0x0A
#define HID_KEY_H 0x0B
#define HID_KEY_I 0x0C
#define HID_KEY_J 0x0D
#define HID_KEY_K 0x0E
#define HID_KEY_L 0x0F
#define HID_KEY_M 0x10
#define HID_KEY_N 0x11
#define HID_KEY_O 0x12
#define HID_KEY_P 0x13
#define HID_KEY_Q 0x14
#define HID_KEY_R 0x15
#define HID_KEY_S 0x16
#define HID_KEY_T 0x17
#define HID_KEY_U 0x18
#define HID_KEY_V 0x19
#define HID_KEY_W 0x1A
#define HID_KEY_X 0x1B
#define HID_KEY_Y 0x1C
#define HID_KEY_Z 0x1D
#define HID_KEY_1 0x1E
#define HID_KEY_2 0x1F
#define HID_KEY_3 0x20
#define HID_KEY_4 0x21
#define HID_KEY_5 0x22
#define HID_KEY_6 0x23
#define HID_KEY_7 0x24
#define HID_KEY_8 0x25
#define HID_KEY_9 0x26
#define HID_KEY_0 0x27
#define HID_KEY_ENTER 0x28
#define HID_KEY_ESC 0x29
#define HID_KEY_BACKSPACE 0x2A
#define HID_KEY_TAB 0x2B
#define HID_KEY_SPACE 0x2C
#define HID_KEY_F1 0x3A
#define HID_KEY_F2 0x3B
#define HID_KEY_F3 0x3C
#define HID_KEY_F4 0x3D
#define HID_KEY_F5 0x3E
#define HID_KEY_F6 0x3F
#define HID_KEY_F7 0x40
#define HID_KEY_F8 0x41
#define HID_KEY_F9 0x42
#define HID_KEY_F10 0x43
#define HID_KEY_F11 0x44
#define HID_KEY_F12 0x45

// Modifier keys
#define HID_MOD_LCTRL 0x01
#define HID_MOD_LSHIFT 0x02
#define HID_MOD_LALT 0x04
#define HID_MOD_LGUI 0x08
#define HID_MOD_RCTRL 0x10
#define HID_MOD_RSHIFT 0x20
#define HID_MOD_RALT 0x40
#define HID_MOD_RGUI 0x80

#if USE_USB_HID_DEVICE
// TinyUSB callbacks
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

// TinyUSB CDC line state callback - disabled for manual bootloader entry
// Use BOOT button to enter bootloader mode for flashing
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)dtr;
    (void)rts;
}
#endif

USBHIDManager::USBHIDManager()
    : initialized(false),
      mouse_enabled(true),
      keyboard_enabled(true),
      mouse_sensitivity(1.0f),
      in_spell_mode(false),
      button_state(0),
      gamepad_buttons(0),
      gamepad_lx(0),
      gamepad_ly(0),
      gamepad_rx(0),
      gamepad_ry(0),
      gamepad_lt(0),
      gamepad_rt(0),
      gamepad_hat(8),
      smoothed_lx(0.0f),
      smoothed_ly(0.0f),
      smoothing_initialized(false),
      smoothed_mouse_x(0.0f),
      smoothed_mouse_y(0.0f),
      mouse_smoothing_initialized(false),
      prev_vel_x(0.0f),
      prev_vel_y(0.0f),
      predicted_x(0.0f),
      predicted_y(0.0f),
      accumulated_x(0.0f),
      accumulated_y(0.0f)
{
    // Initialize default settings
    settings.mouse_sensitivity = 1.0f;
    settings.invert_mouse_y = false;      // Default: natural (wand UP = cursor UP, since IMU gives negative for up)
    settings.mouse_enabled = true;        // Default: enabled
    settings.keyboard_enabled = true;     // Default: enabled
    settings.mouse_4button_click = false; // Default: disabled (use 4-button for spell tracking)
    settings.hid_mode = HID_MODE_MOUSE;
    settings.gamepad_sensitivity = 1.0f;
    settings.gamepad_deadzone = 0.05f;
    settings.gamepad_invert_y = false;       // Default: natural (wand UP = stick UP)
    settings.gamepad_stick_mode = 0;         // Default: left stick
    settings.auto_recenter_on_still = false; // Default: disabled (manual recenter only)
    settings.stillness_threshold = 40.0f;    // Default: 40 (higher = easier to stay still)
    // Initialize all spell keycodes to 0 (disabled)
    memset(settings.spell_keycodes, 0, sizeof(settings.spell_keycodes));
    memset(settings.spell_gamepad_buttons, 0, sizeof(settings.spell_gamepad_buttons));
}

USBHIDManager::~USBHIDManager()
{
}

bool USBHIDManager::begin()
{
#if USE_USB_HID_DEVICE
    ESP_LOGI(TAG, "Initializing USB HID (Mouse + Keyboard)...");

    // Load settings from NVS
    if (!loadSettings())
    {
        ESP_LOGW(TAG, "Failed to load NVS settings, using defaults");
    }

    in_spell_mode = false;
    mouse_sensitivity = settings.mouse_sensitivity;

    // Configure and initialize TinyUSB using ESP-IDF wrapper
    tinyusb_config_t tusb_cfg = {};
    tusb_cfg.phy.skip_setup = false;
    tusb_cfg.phy.self_powered = false;
    tusb_cfg.phy.vbus_monitor_io = -1;
    tusb_cfg.task.size = 4096;
    tusb_cfg.task.priority = 5;
    tusb_cfg.task.xCoreID = 0;
    tusb_cfg.descriptor.device = &device_descriptor;
    tusb_cfg.descriptor.full_speed_config = configuration_descriptor;
    tusb_cfg.descriptor.string = string_descriptor;
    tusb_cfg.descriptor.string_count = 5;
    tusb_cfg.event_cb = NULL;
    tusb_cfg.event_arg = NULL;

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

#if CONFIG_TINYUSB_CDC_ENABLED
    // CDC line state callback (tud_cdc_line_state_cb) is automatically called by TinyUSB
    // Note: Auto-reset is currently disabled to prevent interference with monitoring

    s_prev_log_vprintf = esp_log_set_vprintf(usb_cdc_log_vprintf);
    ESP_LOGI(TAG, "USB CDC logging enabled");
#endif

    initialized = true;

    // Log final loaded settings for verification
    ESP_LOGI(TAG, "📋 Final settings after initialization:");
    ESP_LOGI(TAG, "   Mouse: sensitivity=%.2f, invert_y=%s",
             settings.mouse_sensitivity, settings.invert_mouse_y ? "true" : "false");
    ESP_LOGI(TAG, "   Gamepad: sensitivity=%.2f, deadzone=%.2f, invert_y=%s",
             settings.gamepad_sensitivity, settings.gamepad_deadzone, settings.gamepad_invert_y ? "true" : "false");
    ESP_LOGI(TAG, "   HID mode=%u, mouse_enabled=%d, keyboard_enabled=%d",
             settings.hid_mode, settings.mouse_enabled, settings.keyboard_enabled);

    ESP_LOGI(TAG, "USB HID initialized successfully");
    return true;
#else
    ESP_LOGW(TAG, "USB HID support not compiled in");
    return false;
#endif
}

void USBHIDManager::updateMouse(float gyro_x, float gyro_y, float gyro_z)
{
#if USE_USB_HID_DEVICE
    if (!initialized || !mouse_enabled || in_spell_mode || getHidMode() != HID_MODE_MOUSE)
        return;

    // Convert gyroscope data to mouse movement
    // gyro_x/y are in rad/s, scale to reasonable mouse speed
    // Typical gyro range: ±2000 dps = ±34.9 rad/s
    // Map to mouse delta: -127 to +127

    float scale = mouse_sensitivity * 2.0f;     // Adjust as needed
    int8_t delta_x = (int8_t)(gyro_y * scale);  // Pitch → X movement
    int8_t delta_y = (int8_t)(-gyro_x * scale); // Roll → Y movement (inverted)

    // Accumulate for smoother movement
    int16_t temp_x = accumulated_x + delta_x;
    int16_t temp_y = accumulated_y + delta_y;

    // Clamp to HID report range
    accumulated_x = (temp_x > 127) ? 127 : (temp_x < -127) ? -127
                                                           : (int8_t)temp_x;
    accumulated_y = (temp_y > 127) ? 127 : (temp_y < -127) ? -127
                                                           : (int8_t)temp_y;

    // Send mouse report if there's movement
    if (accumulated_x != 0 || accumulated_y != 0)
    {
        sendMouseReport(accumulated_x, accumulated_y, 0, button_state);
        accumulated_x = 0;
        accumulated_y = 0;
    }
#endif
}

void USBHIDManager::updateMouseFromGesture(float delta_x, float delta_y)
{
#if USE_USB_HID_DEVICE
    if (!initialized || !mouse_enabled || in_spell_mode || getHidMode() != HID_MODE_MOUSE)
        return;

    float scale = mouse_sensitivity;
    int16_t temp_x = (int16_t)(delta_x * scale);
    int16_t temp_y = (int16_t)(delta_y * scale);

    int8_t dx = (temp_x > 127) ? 127 : (temp_x < -127) ? -127
                                                       : (int8_t)temp_x;
    int8_t dy = (temp_y > 127) ? 127 : (temp_y < -127) ? -127
                                                       : (int8_t)temp_y;

    if (dx != 0 || dy != 0)
    {
        sendMouseReport(dx, dy, 0, button_state);
    }
#endif
}

void USBHIDManager::updateMouseFromPosition(float pos_x, float pos_y)
{
#if USE_USB_HID_DEVICE
    if (!initialized || !mouse_enabled || in_spell_mode || getHidMode() != HID_MODE_MOUSE)
        return;

    // Velocity-based mouse: position = cursor velocity
    // Position range: ~[-300, +300] from AHRS Euler angles
    // BASE_SCALE tuned for responsive, smooth motion at 240Hz
    constexpr float BASE_SCALE = 0.015f;
    float scale = BASE_SCALE * mouse_sensitivity;

    // Apply Y-axis inversion setting
    if (settings.invert_mouse_y)
    {
        pos_y = -pos_y;
    }

    // Scale position to velocity (pixels per frame)
    float vel_x = pos_x * scale;
    float vel_y = pos_y * scale;

    // Light exponential smoothing for responsive motion
    constexpr float SMOOTHING_ALPHA = 0.7f; // Very responsive

    if (!mouse_smoothing_initialized)
    {
        // Initialize filter
        smoothed_mouse_x = vel_x;
        smoothed_mouse_y = vel_y;
        accumulated_x = 0.0f;
        accumulated_y = 0.0f;
        mouse_smoothing_initialized = true;
    }
    else
    {
        // Simple exponential smoothing - responsive and clean
        smoothed_mouse_x = SMOOTHING_ALPHA * vel_x + (1.0f - SMOOTHING_ALPHA) * smoothed_mouse_x;
        smoothed_mouse_y = SMOOTHING_ALPHA * vel_y + (1.0f - SMOOTHING_ALPHA) * smoothed_mouse_y;
    }

    // Sub-pixel accumulation for smooth slow movements
    accumulated_x += smoothed_mouse_x;
    accumulated_y += smoothed_mouse_y;

    // Clamp accumulated values to valid HID range (±127)
    if (accumulated_x > 127.0f)
        accumulated_x = 127.0f;
    if (accumulated_x < -127.0f)
        accumulated_x = -127.0f;
    if (accumulated_y > 127.0f)
        accumulated_y = 127.0f;
    if (accumulated_y < -127.0f)
        accumulated_y = -127.0f;

    // Extract integer movement, keep fractional remainder
    int8_t dx = 0;
    int8_t dy = 0;

    if (accumulated_x >= 1.0f || accumulated_x <= -1.0f)
    {
        dx = (int8_t)accumulated_x;
        accumulated_x -= (float)dx;
    }

    if (accumulated_y >= 1.0f || accumulated_y <= -1.0f)
    {
        dy = (int8_t)accumulated_y;
        accumulated_y -= (float)dy;
    }

    // Send directly - fast and responsive
    sendMouseReport(dx, dy, 0, button_state);

    // Debug logging (throttled)
    static int pos_debug_counter = 0;
    if (++pos_debug_counter >= 100)
    {
        pos_debug_counter = 0;
        ESP_LOGI(TAG, "🖱️  Mouse: pos(%.1f, %.1f) → vel(%.2f, %.2f) → Δ(%d, %d)",
                 pos_x, pos_y, smoothed_mouse_x, smoothed_mouse_y, dx, dy);
    }
#endif
}

void USBHIDManager::updateGamepadFromGesture(float delta_x, float delta_y)
{
#if USE_USB_HID_DEVICE
    HIDMode mode = getHidMode();
    if (!initialized || in_spell_mode || (mode != HID_MODE_GAMEPAD_ONLY && mode != HID_MODE_GAMEPAD_MIXED))
        return;

    float scale = settings.gamepad_sensitivity;

    // Note: Inversion is already applied in ble_client.cpp before calling this function
    // Do NOT apply inversion here again to avoid double-inversion

    int16_t temp_x = (int16_t)(delta_x * scale);
    int16_t temp_y = (int16_t)(delta_y * scale);

    gamepad_lx = (temp_x > 127) ? 127 : (temp_x < -127) ? -127
                                                        : (int8_t)temp_x;
    gamepad_ly = (temp_y > 127) ? 127 : (temp_y < -127) ? -127
                                                        : (int8_t)temp_y;

    int8_t deadzone = (int8_t)(settings.gamepad_deadzone * 127.0f);
    if (deadzone < 0)
        deadzone = 0;
    if (deadzone > 63)
        deadzone = 63;
    if (gamepad_lx > -deadzone && gamepad_lx < deadzone)
        gamepad_lx = 0;
    if (gamepad_ly > -deadzone && gamepad_ly < deadzone)
        gamepad_ly = 0;

    // Use left stick only; preserve current triggers/hat state
    sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
#endif
}

void USBHIDManager::updateGamepadFromPosition(float pos_x, float pos_y)
{
#if USE_USB_HID_DEVICE
    HIDMode mode = getHidMode();
    if (!initialized || in_spell_mode || (mode != HID_MODE_GAMEPAD_ONLY && mode != HID_MODE_GAMEPAD_MIXED))
        return;

    // Position scaling: AHRS positions are typically in range ~[-300, +300]
    // Scale to gamepad range [-127, +127] with sensitivity multiplier
    // Base scaling factor: 127/300 ≈ 0.42, adjusted by sensitivity
    constexpr float BASE_SCALE = 0.42f;
    float scale = BASE_SCALE * settings.gamepad_sensitivity;

    // Apply Y-axis inversion (do NOT rely on ble_client.cpp for position mode)
    if (settings.gamepad_invert_y)
    {
        pos_y = -pos_y;
    }

    // Scale and clamp to stick range
    int16_t temp_x = (int16_t)(pos_x * scale);
    int16_t temp_y = (int16_t)(pos_y * scale);

    int8_t stick_x = (temp_x > 127) ? 127 : (temp_x < -127) ? -127
                                                            : (int8_t)temp_x;
    int8_t stick_y = (temp_y > 127) ? 127 : (temp_y < -127) ? -127
                                                            : (int8_t)temp_y;

    // Exponential smoothing to reduce hand tremor jitter
    // Alpha = 0.3: smooth but responsive (lower = smoother, higher = more responsive)
    constexpr float SMOOTHING_ALPHA = 0.3f;

    if (!smoothing_initialized)
    {
        // Initialize filter with first value
        smoothed_lx = (float)stick_x;
        smoothed_ly = (float)stick_y;
        smoothing_initialized = true;
    }
    else
    {
        // Apply exponential moving average: filtered = alpha * new + (1-alpha) * old
        smoothed_lx = SMOOTHING_ALPHA * (float)stick_x + (1.0f - SMOOTHING_ALPHA) * smoothed_lx;
        smoothed_ly = SMOOTHING_ALPHA * (float)stick_y + (1.0f - SMOOTHING_ALPHA) * smoothed_ly;
    }

    // Convert smoothed float back to int8 for HID report
    int8_t final_x = (int8_t)roundf(smoothed_lx);
    int8_t final_y = (int8_t)roundf(smoothed_ly);

    // Assign to selected stick (0=left, 1=right)
    if (settings.gamepad_stick_mode == 0)
    {
        // Left stick
        gamepad_lx = final_x;
        gamepad_ly = final_y;
        gamepad_rx = 0;
        gamepad_ry = 0;
    }
    else
    {
        // Right stick
        gamepad_lx = 0;
        gamepad_ly = 0;
        gamepad_rx = final_x;
        gamepad_ry = final_y;
    }

    // Calculate magnitude for logging and anomaly detection
    float magnitude = sqrtf((float)(stick_x * stick_x + stick_y * stick_y));

    // Debug logging (throttled)
    static int pos_debug_counter = 0;
    if (++pos_debug_counter >= 100)
    {
        pos_debug_counter = 0;
        ESP_LOGI(TAG, "🎮 Gamepad Position: pos(%.1f, %.1f) -> stick(%d, %d) | mag=%.1f",
                 pos_x, pos_y, stick_x, stick_y, magnitude);
    }

    // Safety: detect invalid position calculations (magnitude > 200 suggests calculation error)
    if (magnitude > 200.0f)
    {
        static int anomaly_counter = 0;
        if (++anomaly_counter >= 10)
        {
            anomaly_counter = 0;
            ESP_LOGW(TAG, "⚠️  Position anomaly: mag=%.1f, pos(%.1f, %.1f) - possible reference frame issue",
                     magnitude, pos_x, pos_y);
        }
    }

    // Use assigned stick based on gamepad_stick_mode; preserve current triggers/hat
    sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
#endif
}

void USBHIDManager::setGamepadButtons(uint16_t buttons)
{
    gamepad_buttons = buttons & 0x3FFF; // 14 buttons max

#if USE_USB_HID_DEVICE
    HIDMode mode = getHidMode();
    if (initialized && (mode == HID_MODE_GAMEPAD_ONLY || mode == HID_MODE_GAMEPAD_MIXED))
    {
        sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
    }
#endif
}

void USBHIDManager::mouseClick(uint8_t button)
{
#if USE_USB_HID_DEVICE
    if (!initialized || !mouse_enabled)
        return;

    // Press button
    button_state |= button;
    sendMouseReport(0, 0, 0, button_state);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Release button
    button_state &= ~button;
    sendMouseReport(0, 0, 0, button_state);
#endif
}

void USBHIDManager::setMouseSensitivity(float sensitivity)
{
    mouse_sensitivity = sensitivity;
}

void USBHIDManager::sendKeyPress(uint8_t keycode, uint8_t modifiers)
{
#if USE_USB_HID_DEVICE
    // Allow keyboard input in both MOUSE and KEYBOARD modes (for spell hotkeys + mouse movement)
    if (!initialized || !keyboard_enabled || (getHidMode() != HID_MODE_KEYBOARD && getHidMode() != HID_MODE_MOUSE))
    {
        ESP_LOGW(TAG, "⚠️ sendKeyPress blocked: init=%d, kbd_en=%d, mode=%d",
                 initialized, keyboard_enabled, getHidMode());
        return;
    }

    sendKeyboardReport(modifiers, keycode);
#endif
}

void USBHIDManager::sendKeyRelease()
{
#if USE_USB_HID_DEVICE
    // Allow keyboard input in both MOUSE and KEYBOARD modes (for spell hotkeys + mouse movement)
    if (!initialized || !keyboard_enabled || (getHidMode() != HID_MODE_KEYBOARD && getHidMode() != HID_MODE_MOUSE))
        return;

    sendKeyboardReport(0, 0);
#endif
}

void USBHIDManager::typeString(const char *text)
{
#if USE_USB_HID_DEVICE
    if (!initialized || !keyboard_enabled || getHidMode() != HID_MODE_KEYBOARD || !text)
        return;

    // Simple ASCII to HID keycode mapping
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        char c = text[i];
        uint8_t keycode = 0;
        uint8_t modifiers = 0;

        if (c >= 'a' && c <= 'z')
        {
            keycode = HID_KEY_A + (c - 'a');
        }
        else if (c >= 'A' && c <= 'Z')
        {
            keycode = HID_KEY_A + (c - 'A');
            modifiers = HID_MOD_LSHIFT;
        }
        else if (c >= '0' && c <= '9')
        {
            keycode = HID_KEY_0 + (c - '0');
        }
        else if (c == ' ')
        {
            keycode = HID_KEY_SPACE;
        }
        else if (c == '\n')
        {
            keycode = HID_KEY_ENTER;
        }

        if (keycode != 0)
        {
            sendKeyPress(keycode, modifiers);
            vTaskDelay(pdMS_TO_TICKS(20));
            sendKeyRelease();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
#endif
}

void USBHIDManager::sendSpellKeyboard(const char *spell_name)
{
#if USE_USB_HID_DEVICE
    if (!initialized || !keyboard_enabled || getHidMode() != HID_MODE_KEYBOARD || !spell_name)
        return;

    uint8_t keycode = getKeycodeForSpell(spell_name);

    if (keycode != 0)
    {
        ESP_LOGI(TAG, "Spell '%s' → Key 0x%02X", spell_name, keycode);
        sendKeyPress(keycode, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        sendKeyRelease();
    }
    else
    {
        ESP_LOGW(TAG, "No key mapping for spell: %s", spell_name);
    }
#endif
}

void USBHIDManager::setEnabled(bool mouse_en, bool keyboard_en)
{
    mouse_enabled = mouse_en;
    keyboard_enabled = keyboard_en;
    settings.mouse_enabled = mouse_en;
    settings.keyboard_enabled = keyboard_en;
    ESP_LOGI(TAG, "USB HID enabled: mouse=%d, keyboard=%d", mouse_enabled, keyboard_enabled);
}

void USBHIDManager::setHidMode(HIDMode mode)
{
    settings.hid_mode = static_cast<uint8_t>(mode);
    switch (mode)
    {
    case HID_MODE_MOUSE:
        mouse_enabled = true;
        keyboard_enabled = true; // Enable keyboard for spell hotkeys in mouse mode
        break;
    case HID_MODE_KEYBOARD:
        mouse_enabled = false;
        keyboard_enabled = true;
        break;
    case HID_MODE_GAMEPAD_ONLY:
        mouse_enabled = false;
        keyboard_enabled = false; // Gamepad buttons only
        break;
    case HID_MODE_GAMEPAD_MIXED:
        mouse_enabled = false;
        keyboard_enabled = true; // Enable keyboard for spell hotkeys
        break;
    case HID_MODE_DISABLED:
    default:
        mouse_enabled = false;
        keyboard_enabled = false;
        break;
    }
    settings.mouse_enabled = mouse_enabled;
    settings.keyboard_enabled = keyboard_enabled;
    ESP_LOGI(TAG, "HID mode set to %u (mouse=%d, keyboard=%d)", settings.hid_mode, mouse_enabled, keyboard_enabled);

#if USE_USB_HID_DEVICE
    // Send initial "all released" gamepad report when entering gamepad mode
    if (mode == HID_MODE_GAMEPAD_ONLY || mode == HID_MODE_GAMEPAD_MIXED)
    {
        gamepad_buttons = 0;
        gamepad_lx = 0;
        gamepad_ly = 0;
        gamepad_rx = 0;
        gamepad_ry = 0;
        gamepad_lt = 0;
        gamepad_rt = 0;
        gamepad_hat = 8;
        sendGamepadReport(0, 0, 0, 0, 0, 0, 0, 8); // All centered, HAT=8 (null state)
        ESP_LOGI(TAG, "Sent initial gamepad reset report");
    }
#endif
}

void USBHIDManager::sendMouseReport(int8_t x, int8_t y, int8_t wheel, uint8_t buttons)
{
#if USE_USB_HID_DEVICE
    if (!tud_hid_ready())
        return;

    uint8_t report[4];
    report[0] = buttons;
    report[1] = (uint8_t)x;
    report[2] = (uint8_t)y;
    report[3] = (uint8_t)wheel;

    tud_hid_report(1, report, sizeof(report)); // Report ID 1 = Mouse
#endif
}

void USBHIDManager::sendGamepadReport(int8_t lx, int8_t ly, int8_t rx, int8_t ry, uint8_t lt, uint8_t rt, uint16_t buttons, uint8_t hat)
{
#if USE_USB_HID_DEVICE
    if (!tud_hid_ready())
        return;

    // Full Xbox-compatible gamepad: 9 bytes total (per HID descriptor)
    // Left stick (2), right stick (2), triggers (2), buttons (2), hat (1)
    uint8_t report[9];
    report[0] = (uint8_t)lx;                      // Left stick X
    report[1] = (uint8_t)ly;                      // Left stick Y
    report[2] = (uint8_t)rx;                      // Right stick X
    report[3] = (uint8_t)ry;                      // Right stick Y
    report[4] = lt;                               // Left trigger (Z axis)
    report[5] = rt;                               // Right trigger (Rz axis)
    report[6] = (uint8_t)(buttons & 0xFF);        // Buttons 1-8
    report[7] = (uint8_t)((buttons >> 8) & 0x3F); // Buttons 9-14 (6 bits) + 2 bits padding
    report[8] = (uint8_t)(hat & 0x0F);            // D-pad hat (4 bits, 8=null/center) + 4 bits padding

    // Debug logging for trigger values (only log when triggers are non-zero)
    if (lt > 0 || rt > 0)
    {
        ESP_LOGI(TAG, "📊 USB Report: LX=%d LY=%d RX=%d RY=%d LT=%d RT=%d BTN=0x%04X HAT=%d",
                 lx, ly, rx, ry, lt, rt, buttons, hat);
    }

    tud_hid_report(3, report, sizeof(report)); // Report ID 3 = Gamepad
#endif
}

void USBHIDManager::sendKeyboardReport(uint8_t modifiers, uint8_t keycode)
{
#if USE_USB_HID_DEVICE
    if (!tud_hid_ready())
        return;

    uint8_t report[8] = {0};
    report[0] = modifiers;
    report[1] = 0; // Reserved
    report[2] = keycode;

    tud_hid_report(2, report, sizeof(report)); // Report ID 2 = Keyboard
#endif
}

uint8_t USBHIDManager::getKeycodeForSpell(const char *spell_name)
{
    // Map popular spells to function keys or shortcuts
    // Users can customize this later

    if (strcmp(spell_name, "Expelliarmus") == 0)
        return HID_KEY_F1;
    if (strcmp(spell_name, "Expecto_Patronum") == 0)
        return HID_KEY_F2;
    if (strcmp(spell_name, "Alohomora") == 0)
        return HID_KEY_F3;
    if (strcmp(spell_name, "Lumos") == 0)
        return HID_KEY_F4;
    if (strcmp(spell_name, "Protego") == 0)
        return HID_KEY_F5;
    if (strcmp(spell_name, "Stupefy") == 0)
        return HID_KEY_F6;
    if (strcmp(spell_name, "Wingardium_Leviosa") == 0)
        return HID_KEY_F7;
    if (strcmp(spell_name, "Accio") == 0)
        return HID_KEY_F8;
    if (strcmp(spell_name, "Riddikulus") == 0)
        return HID_KEY_F9;
    if (strcmp(spell_name, "Finite") == 0)
        return HID_KEY_F10;
    if (strcmp(spell_name, "Flipendo") == 0)
        return HID_KEY_F11;
    if (strcmp(spell_name, "Incendio") == 0)
        return HID_KEY_F12;

    // Default: map first letter
    if (spell_name[0] >= 'A' && spell_name[0] <= 'Z')
        return HID_KEY_A + (spell_name[0] - 'A');
    if (spell_name[0] >= 'a' && spell_name[0] <= 'z')
        return HID_KEY_A + (spell_name[0] - 'a');

    return 0;
}

void USBHIDManager::sendSpellKeyboardForSpell(const char *spell_name)
{
#if USE_USB_HID_DEVICE
    // Allow keyboard spell triggers in Mouse, Keyboard, and Gamepad modes (mixed mode)
    if (!spell_name)
        return;

    HIDMode mode = getHidMode();
    if (mode != HID_MODE_KEYBOARD && mode != HID_MODE_MOUSE && mode != HID_MODE_GAMEPAD_MIXED)
        return;

    uint8_t keycode = getSpellKeycode(spell_name);
    if (keycode != 0)
    {
        ESP_LOGI(TAG, "Spell '%s': Sending key 0x%02X (mode=%d)", spell_name, keycode, mode);

        if (mode == HID_MODE_GAMEPAD_MIXED)
        {
            // In gamepad mixed mode, bypass sendKeyPress gating and send keyboard report directly
            // This allows mixed gamepad joystick + keyboard spell keys
            if (initialized && tud_hid_ready())
            {
                sendKeyboardReport(0, keycode); // Press key
                vTaskDelay(pdMS_TO_TICKS(50));
                sendKeyboardReport(0, 0); // Release key
            }
        }
        else
        {
            // Mouse and Keyboard modes use normal keyboard path
            sendKeyPress(keycode, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            sendKeyRelease();
        }
    }
    else
    {
        ESP_LOGI(TAG, "Spell '%s' has no mapped key", spell_name);
    }
#endif
}

void USBHIDManager::setSpellKeycode(const char *spell_name, uint8_t keycode)
{
    if (!spell_name)
        return;

    // Find spell index by name
    extern const char *SPELL_NAMES[73];
    for (int i = 0; i < 73; i++)
    {
        if (strcmp(SPELL_NAMES[i], spell_name) == 0)
        {
            settings.spell_keycodes[i] = keycode;
            ESP_LOGI(TAG, "Spell '%s' (index %d) mapped to key 0x%02X", spell_name, i, keycode);
            return;
        }
    }
    ESP_LOGW(TAG, "Spell '%s' not found in spell list", spell_name);
}

uint8_t USBHIDManager::getSpellKeycode(const char *spell_name) const
{
    if (!spell_name)
        return 0;

    extern const char *SPELL_NAMES[73];
    for (int i = 0; i < 73; i++)
    {
        if (strcmp(SPELL_NAMES[i], spell_name) == 0)
        {
            return settings.spell_keycodes[i];
        }
    }
    return 0;
}

// Clean up old individual NVS entries from pre-blob storage format
// Before the blob refactor, each spell was stored as a separate key (spell_0, spell_1, etc.)
// These orphaned entries waste NVS space and can cause ESP_ERR_NVS_NOT_ENOUGH_SPACE
void USBHIDManager::cleanupOldNvsEntries()
{
    // Clean up old individual spell entries from "usb_hid" namespace
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("usb_hid", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        int cleaned = 0;
        char key[16];
        for (int i = 0; i < 73; i++)
        {
            snprintf(key, sizeof(key), "spell_%d", i);
            uint8_t dummy;
            if (nvs_get_u8(nvs_handle, key, &dummy) == ESP_OK)
            {
                nvs_erase_key(nvs_handle, key);
                cleaned++;
            }
        }
        if (cleaned > 0)
        {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "🧹 Cleaned up %d old individual spell entries from 'usb_hid' namespace", cleaned);
        }
        nvs_close(nvs_handle);
    }

    // Clean up old individual gamepad spell entries from "gamepad" namespace
    err = nvs_open("gamepad", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        int cleaned = 0;
        char key[16];
        for (int i = 0; i < 73; i++)
        {
            snprintf(key, sizeof(key), "gpad_%d", i);
            uint8_t dummy;
            if (nvs_get_u8(nvs_handle, key, &dummy) == ESP_OK)
            {
                nvs_erase_key(nvs_handle, key);
                cleaned++;
            }
        }
        if (cleaned > 0)
        {
            nvs_commit(nvs_handle);
            ESP_LOGI(TAG, "🧹 Cleaned up %d old individual gamepad spell entries from 'gamepad' namespace", cleaned);
        }
        nvs_close(nvs_handle);
    }
}

bool USBHIDManager::loadSettings()
{
    // ⚠️ CRITICAL: ESP-IDF NVS keys have a MAXIMUM length of 15 characters!
    // ⚠️ Key names here MUST match those in saveSettings() exactly.
    // ⚠️ Current keys: kbd_enabled(11), gpad_sens(9), gpad_dz(7), gpad_inv_y(11), gpad_stick(10)

    ESP_LOGI(TAG, "📂 Loading settings from NVS...");

    // Log NVS partition statistics on load
    nvs_stats_t nvs_stats;
    esp_err_t stats_err = nvs_get_stats("nvs", &nvs_stats);
    if (stats_err == ESP_OK)
    {
        ESP_LOGI(TAG, "📊 NVS stats at boot: used=%zu, free=%zu, total=%zu, ns_count=%zu",
                 nvs_stats.used_entries, nvs_stats.free_entries, nvs_stats.total_entries, nvs_stats.namespace_count);
    }

    // Clean up old individual spell entries (from pre-blob storage format)
    // These entries waste NVS space and can cause NVS_NOT_ENOUGH_SPACE errors
    cleanupOldNvsEntries();

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("usb_hid", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS namespace 'usb_hid' not found, using defaults");
        return false;
    }

    // Load mouse sensitivity (stored as uint8_t: value * 10)
    uint8_t sens_10x = 10; // Default 1.0x
    err = nvs_get_u8(nvs_handle, "mouse_sens_10x", &sens_10x);
    if (err == ESP_OK)
    {
        settings.mouse_sensitivity = (float)sens_10x / 10.0f;
        ESP_LOGI(TAG, "✓ Loaded mouse_sensitivity from NVS: %.2f (raw: %d)", settings.mouse_sensitivity, sens_10x);
    }
    else
    {
        settings.mouse_sensitivity = 1.0f;
        ESP_LOGI(TAG, "⚠ Mouse sensitivity not found in NVS, using default: 1.0");
    }

    // Load invert_mouse_y setting
    uint8_t invert_y = 0; // Default: natural (non-inverted)
    err = nvs_get_u8(nvs_handle, "invert_mouse_y", &invert_y);
    if (err == ESP_OK)
    {
        settings.invert_mouse_y = (invert_y != 0);
        ESP_LOGI(TAG, "✓ Loaded invert_mouse_y from NVS: %s", settings.invert_mouse_y ? "true" : "false");
    }
    else
    {
        settings.invert_mouse_y = false;
        ESP_LOGI(TAG, "⚠ Invert_mouse_y not found in NVS, using default: false (natural)");
    }

    // Load mouse_enabled
    uint8_t mouse_en = 1; // Default: enabled
    err = nvs_get_u8(nvs_handle, "mouse_enabled", &mouse_en);
    settings.mouse_enabled = (mouse_en != 0);

    // Load keyboard_enabled
    uint8_t keyboard_en = 1; // Default: enabled
    err = nvs_get_u8(nvs_handle, "kbd_enabled", &keyboard_en);
    settings.keyboard_enabled = (keyboard_en != 0);

    // Load mouse_4button_click (NVS key: 11 chars, under 15-char limit)
    uint8_t ms_4btn = 0; // Default: disabled
    err = nvs_get_u8(nvs_handle, "ms_4btn_clk", &ms_4btn);
    if (err == ESP_OK)
    {
        settings.mouse_4button_click = (ms_4btn != 0);
        ESP_LOGI(TAG, "✓ Loaded mouse_4button_click from NVS: %s", settings.mouse_4button_click ? "enabled" : "disabled");
    }
    else
    {
        settings.mouse_4button_click = false;
        ESP_LOGI(TAG, "⚠ mouse_4button_click not found in NVS, using default: disabled");
    }

    // Load auto_recenter_on_still (NVS key: 13 chars, under 15-char limit)
    uint8_t auto_rectr = 0; // Default: disabled
    err = nvs_get_u8(nvs_handle, "auto_rectr_st", &auto_rectr);
    if (err == ESP_OK)
    {
        settings.auto_recenter_on_still = (auto_rectr != 0);
        ESP_LOGI(TAG, "✓ Loaded auto_recenter_on_still from NVS: %s", settings.auto_recenter_on_still ? "enabled" : "disabled");
    }
    else
    {
        settings.auto_recenter_on_still = false;
        ESP_LOGI(TAG, "⚠ auto_recenter_on_still not found in NVS, using default: disabled");
    }

    // Load stillness_threshold (NVS key: stored as uint8_t, range 10-100)
    uint8_t still_thresh = 40; // Default: 40
    err = nvs_get_u8(nvs_handle, "still_thresh", &still_thresh);
    if (err == ESP_OK)
    {
        settings.stillness_threshold = (float)still_thresh;
        ESP_LOGI(TAG, "✓ Loaded stillness_threshold from NVS: %.0f", settings.stillness_threshold);
    }
    else
    {
        settings.stillness_threshold = 40.0f;
        ESP_LOGI(TAG, "⚠ stillness_threshold not found in NVS, using default: 40");
    }

    // Load HID mode
    uint8_t hid_mode = HID_MODE_MOUSE;
    err = nvs_get_u8(nvs_handle, "hid_mode", &hid_mode);
    if (err != ESP_OK)
    {
        if (settings.mouse_enabled && !settings.keyboard_enabled)
        {
            hid_mode = HID_MODE_MOUSE;
        }
        else if (!settings.mouse_enabled && settings.keyboard_enabled)
        {
            hid_mode = HID_MODE_KEYBOARD;
        }
        else if (!settings.mouse_enabled && !settings.keyboard_enabled)
        {
            hid_mode = HID_MODE_DISABLED;
        }
        else
        {
            hid_mode = HID_MODE_MOUSE;
        }
    }
    setHidMode(static_cast<HIDMode>(hid_mode));

    // Load spell keycodes (73 spells) - blob storage
    ESP_LOGI(TAG, "Loading spell keycodes from NVS...");
    size_t blob_size = 73;
    err = nvs_get_blob(nvs_handle, "spell_keycodes", settings.spell_keycodes, &blob_size);
    if (err == ESP_OK && blob_size == 73)
    {
        int non_zero_count = 0;
        for (int i = 0; i < 73; i++)
        {
            if (settings.spell_keycodes[i] != 0)
            {
                non_zero_count++;
            }
        }
        ESP_LOGI(TAG, "Loaded %d non-zero spell mappings", non_zero_count);
    }
    else
    {
        ESP_LOGW(TAG, "Spell keycodes blob not found in NVS");
    }

    nvs_close(nvs_handle);

    // Open gamepad namespace for gamepad-specific settings
    err = nvs_open("gamepad", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        // Load gamepad sensitivity (stored as uint8_t: value * 10)
        uint8_t gpad_sens_10x = 10; // Default 1.0x
        err = nvs_get_u8(nvs_handle, "gpad_sens", &gpad_sens_10x);
        if (err == ESP_OK)
        {
            settings.gamepad_sensitivity = (float)gpad_sens_10x / 10.0f;
            ESP_LOGI(TAG, "✓ Loaded gamepad_sensitivity from NVS: %.2f (raw: %d)", settings.gamepad_sensitivity, gpad_sens_10x);
        }
        else
        {
            settings.gamepad_sensitivity = 1.0f;
            ESP_LOGI(TAG, "⚠ Gamepad sensitivity not found in NVS, using default: 1.0");
        }

        // Load gamepad deadzone (stored as uint8_t: value * 100)
        uint8_t gpad_deadzone_100 = 5; // Default 0.05
        err = nvs_get_u8(nvs_handle, "gpad_dz", &gpad_deadzone_100);
        if (err == ESP_OK)
        {
            settings.gamepad_deadzone = (float)gpad_deadzone_100 / 100.0f;
            ESP_LOGI(TAG, "✓ Loaded gamepad_deadzone from NVS: %.2f (raw: %d)", settings.gamepad_deadzone, gpad_deadzone_100);
        }
        else
        {
            settings.gamepad_deadzone = 0.05f;
            ESP_LOGI(TAG, "⚠ Gamepad deadzone not found in NVS, using default: 0.05");
        }

        // Load gamepad invert_y
        uint8_t gpad_invert_y = 0; // Default: natural (non-inverted)
        err = nvs_get_u8(nvs_handle, "gpad_inv_y", &gpad_invert_y);
        if (err == ESP_OK)
        {
            settings.gamepad_invert_y = (gpad_invert_y != 0);
            ESP_LOGI(TAG, "✓ Loaded gamepad_invert_y from NVS: %s", settings.gamepad_invert_y ? "true" : "false");
        }
        else
        {
            settings.gamepad_invert_y = false;
            ESP_LOGI(TAG, "⚠ Gamepad invert_y not found in NVS, using default: false (natural)");
        }

        // Load gamepad stick mode
        uint8_t stick_mode = 0; // Default: left stick
        err = nvs_get_u8(nvs_handle, "gpad_stick", &stick_mode);
        if (err == ESP_OK)
        {
            settings.gamepad_stick_mode = stick_mode;
            ESP_LOGI(TAG, "✓ Loaded gamepad_stick_mode from NVS: %s", stick_mode == 0 ? "left" : "right");
        }
        else
        {
            settings.gamepad_stick_mode = 0;
            ESP_LOGI(TAG, "⚠ Gamepad stick_mode not found in NVS, using default: 0 (left)");
        }

        // Log current state for debugging
        ESP_LOGI(TAG, "🎯 Current axis inversion settings: mouse_y=%s, gamepad_y=%s",
                 settings.invert_mouse_y ? "INVERTED" : "NORMAL",
                 settings.gamepad_invert_y ? "INVERTED" : "NORMAL");

        // Load spell gamepad button mappings (73 spells) - blob storage
        size_t gpad_blob_size = 73;
        err = nvs_get_blob(nvs_handle, "gpad_spells", settings.spell_gamepad_buttons, &gpad_blob_size);
        if (err == ESP_OK && gpad_blob_size == 73)
        {
            ESP_LOGI(TAG, "✓ Loaded gamepad spell button mappings");
        }
        else
        {
            ESP_LOGW(TAG, "Gamepad spell mappings blob not found in NVS");
        }
        nvs_close(nvs_handle);
    }
    else
    {
        // If gamepad namespace doesn't exist, use defaults
        settings.gamepad_sensitivity = 1.0f;
        settings.gamepad_deadzone = 0.05f;
        settings.gamepad_invert_y = false;
        settings.gamepad_stick_mode = 0;
        ESP_LOGW(TAG, "NVS namespace 'gamepad' not found, using defaults");
    }

    ESP_LOGI(TAG, "✅ USB HID settings loaded from NVS");
    ESP_LOGI(TAG, "📊 Final loaded gamepad settings: sens=%.2f, deadzone=%.2f, invert_y=%s, stick_mode=%u (%s)",
             settings.gamepad_sensitivity, settings.gamepad_deadzone,
             settings.gamepad_invert_y ? "true" : "false",
             settings.gamepad_stick_mode, settings.gamepad_stick_mode == 0 ? "left" : "right");
    return true;
}

bool USBHIDManager::saveSettings()
{
    // ⚠️ CRITICAL: ESP-IDF NVS keys have a MAXIMUM length of 15 characters!
    // ⚠️ Exceeding this limit causes ESP_ERR_NVS_KEY_TOO_LONG errors.
    // ⚠️ Always use abbreviated key names (e.g., "gpad_sens" not "gamepad_sensitivity").
    // ⚠️ Current keys: kbd_enabled(11), gpad_sens(9), gpad_dz(7), gpad_inv_y(11), gpad_stick(10)

    ESP_LOGI(TAG, "🔍 saveSettings() called - checking current settings struct values:");
    ESP_LOGI(TAG, "   gamepad_sensitivity=%.2f, gamepad_deadzone=%.2f, gamepad_invert_y=%s, gamepad_stick_mode=%u",
             settings.gamepad_sensitivity, settings.gamepad_deadzone, settings.gamepad_invert_y ? "true" : "false", settings.gamepad_stick_mode);
    ESP_LOGI(TAG, "   mouse_sensitivity=%.2f, invert_mouse_y=%s",
             settings.mouse_sensitivity, settings.invert_mouse_y ? "true" : "false");

    // Track if ANY save operation fails
    bool any_errors = false;

    // Log NVS partition statistics
    nvs_stats_t nvs_stats;
    esp_err_t stats_err = nvs_get_stats("nvs", &nvs_stats);
    if (stats_err == ESP_OK)
    {
        ESP_LOGI(TAG, "📊 NVS stats: used=%zu, free=%zu, total=%zu, ns_count=%zu",
                 nvs_stats.used_entries, nvs_stats.free_entries, nvs_stats.total_entries, nvs_stats.namespace_count);
        if (nvs_stats.free_entries < 10)
        {
            ESP_LOGW(TAG, "⚠️ NVS partition is nearly full! Only %zu entries free", nvs_stats.free_entries);
        }
    }
    else
    {
        ESP_LOGW(TAG, "Could not get NVS stats: %s", esp_err_to_name(stats_err));
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("usb_hid", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Failed to open NVS namespace 'usb_hid': %s", esp_err_to_name(err));
        return false;
    }

    // Save mouse sensitivity (as 10x value to store as uint8)
    uint8_t sens_10x = (uint8_t)(settings.mouse_sensitivity * 10.0f);
    err = nvs_set_u8(nvs_handle, "mouse_sens_10x", sens_10x);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 mouse_sens_10x failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save invert_mouse_y setting
    err = nvs_set_u8(nvs_handle, "invert_mouse_y", settings.invert_mouse_y ? 1 : 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 invert_mouse_y failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save mouse_enabled
    err = nvs_set_u8(nvs_handle, "mouse_enabled", settings.mouse_enabled ? 1 : 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 mouse_enabled failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save keyboard_enabled (key shortened to fit 15-char NVS limit)
    err = nvs_set_u8(nvs_handle, "kbd_enabled", settings.keyboard_enabled ? 1 : 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 kbd_enabled failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save mouse_4button_click (key: 11 chars, under 15-char limit)
    err = nvs_set_u8(nvs_handle, "ms_4btn_clk", settings.mouse_4button_click ? 1 : 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 ms_4btn_clk failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save auto_recenter_on_still (key: 13 chars, under 15-char limit)
    err = nvs_set_u8(nvs_handle, "auto_rectr_st", settings.auto_recenter_on_still ? 1 : 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 auto_rectr_st failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save stillness_threshold (key: 12 chars, stored as uint8_t)
    uint8_t still_thresh = (uint8_t)settings.stillness_threshold;
    err = nvs_set_u8(nvs_handle, "still_thresh", still_thresh);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 still_thresh failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save HID mode
    err = nvs_set_u8(nvs_handle, "hid_mode", settings.hid_mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 hid_mode failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Save spell keycodes (73 spells) - blob storage
    err = nvs_set_blob(nvs_handle, "spell_keycodes", settings.spell_keycodes, 73);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_blob spell_keycodes failed: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Commit usb_hid namespace
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Failed to commit usb_hid NVS settings: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "✅ Saved usb_hid settings to NVS");

    // Open gamepad namespace for gamepad-specific settings
    err = nvs_open("gamepad", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Failed to open NVS namespace 'gamepad': %s", esp_err_to_name(err));
        return false;
    }

    // Save gamepad sensitivity (as 10x value to store as uint8, key shortened to fit 15-char NVS limit)
    uint8_t gpad_sens_10x = (uint8_t)(settings.gamepad_sensitivity * 10.0f);
    err = nvs_set_u8(nvs_handle, "gpad_sens", gpad_sens_10x);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 gpad_sens FAILED: %s (value=%d)", esp_err_to_name(err), gpad_sens_10x);
        any_errors = true;
    }
    else
    {
        ESP_LOGI(TAG, "💾 Saved gamepad_sensitivity to NVS: %.2f (raw: %d)", settings.gamepad_sensitivity, gpad_sens_10x);
    }

    // Save gamepad deadzone (as 100x value to store as uint8, key shortened to fit 15-char NVS limit)
    uint8_t gpad_deadzone_100 = (uint8_t)(settings.gamepad_deadzone * 100.0f);
    err = nvs_set_u8(nvs_handle, "gpad_dz", gpad_deadzone_100);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 gpad_dz FAILED: %s", esp_err_to_name(err));
        any_errors = true;
    }
    else
    {
        ESP_LOGI(TAG, "💾 Saved gamepad_deadzone to NVS: %.2f (raw: %d)", settings.gamepad_deadzone, gpad_deadzone_100);
    }

    // Save gamepad invert_y (key shortened to fit 15-char NVS limit)
    err = nvs_set_u8(nvs_handle, "gpad_inv_y", settings.gamepad_invert_y ? 1 : 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 gpad_inv_y FAILED: %s", esp_err_to_name(err));
        any_errors = true;
    }
    else
    {
        ESP_LOGI(TAG, "💾 Saved gamepad_invert_y to NVS: %s", settings.gamepad_invert_y ? "true" : "false");
    }

    // Save gamepad stick mode (key shortened to fit 15-char NVS limit)
    err = nvs_set_u8(nvs_handle, "gpad_stick", settings.gamepad_stick_mode);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_u8 gpad_stick FAILED: %s (value=%d)", esp_err_to_name(err), settings.gamepad_stick_mode);
        any_errors = true;
    }
    else
    {
        ESP_LOGI(TAG, "💾 Saved gamepad_stick_mode to NVS: %s", settings.gamepad_stick_mode == 0 ? "left" : "right");
    }

    // Save spell gamepad button mappings (73 spells) - blob storage
    err = nvs_set_blob(nvs_handle, "gpad_spells", settings.spell_gamepad_buttons, 73);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ nvs_set_blob gpad_spells FAILED: %s", esp_err_to_name(err));
        any_errors = true;
    }

    // Commit gamepad namespace
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ Failed to commit gamepad NVS settings: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    // Verify gamepad values were actually written
    uint8_t verify_gpad_sens = 0;
    uint8_t verify_gpad_deadzone = 0;
    uint8_t verify_gpad_invert = 0;
    uint8_t verify_gpad_stick = 0;
    nvs_get_u8(nvs_handle, "gpad_sens", &verify_gpad_sens);
    nvs_get_u8(nvs_handle, "gpad_dz", &verify_gpad_deadzone);
    nvs_get_u8(nvs_handle, "gpad_inv_y", &verify_gpad_invert);
    nvs_get_u8(nvs_handle, "gpad_stick", &verify_gpad_stick);
    ESP_LOGI(TAG, "✅ Verified gamepad NVS write: sens=%d (%.1fx), deadzone=%d (%.2f), invert=%d, stick_mode=%d (%s)",
             verify_gpad_sens, verify_gpad_sens / 10.0f, verify_gpad_deadzone, verify_gpad_deadzone / 100.0f,
             verify_gpad_invert, verify_gpad_stick, verify_gpad_stick == 0 ? "left" : "right");

    nvs_close(nvs_handle);

    if (any_errors)
    {
        ESP_LOGE(TAG, "❌ Some settings failed to save to NVS - check errors above");
        return false;
    }

    ESP_LOGI(TAG, "✅ All USB HID settings saved to NVS");
    return true;
}

bool USBHIDManager::resetSettings()
{
    // Reset to defaults
    settings.mouse_sensitivity = 1.0f;
    settings.invert_mouse_y = false;  // Default: natural (wand UP = cursor UP)
    settings.mouse_enabled = true;    // Default: enabled
    settings.keyboard_enabled = true; // Default: enabled
    settings.hid_mode = HID_MODE_MOUSE;
    settings.gamepad_sensitivity = 1.0f;
    settings.gamepad_deadzone = 0.05f;
    settings.gamepad_invert_y = false; // Default: natural (wand UP = stick UP)
    memset(settings.spell_keycodes, 0, sizeof(settings.spell_keycodes));
    memset(settings.spell_gamepad_buttons, 0, sizeof(settings.spell_gamepad_buttons));

    mouse_sensitivity = 1.0f;
    setHidMode(HID_MODE_MOUSE);
    gamepad_buttons = 0;
    gamepad_lx = 0;
    gamepad_ly = 0;
    gamepad_rx = 0;
    gamepad_ry = 0;

    // Clear NVS namespace
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("usb_hid", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "USB HID settings reset to defaults");
    return true;
}

void USBHIDManager::setMouseSensitivityValue(float sensitivity)
{
    if (sensitivity < 0.1f)
        sensitivity = 0.1f;
    if (sensitivity > 5.0f)
        sensitivity = 5.0f;

    mouse_sensitivity = sensitivity;
    settings.mouse_sensitivity = sensitivity;

    // Reset smoothing to prevent position jumps when changing sensitivity
    resetMouseSmoothing();

    ESP_LOGI(TAG, "Mouse sensitivity set to %.2f", sensitivity);
}

void USBHIDManager::setGamepadSensitivityValue(float sensitivity)
{
    if (sensitivity < 0.1f)
        sensitivity = 0.1f;
    if (sensitivity > 5.0f)
        sensitivity = 5.0f;

    settings.gamepad_sensitivity = sensitivity;
    ESP_LOGI(TAG, "🎮 Gamepad sensitivity set to %.2f (will be saved on settings save)", sensitivity);
}

void USBHIDManager::setGamepadDeadzoneValue(float deadzone)
{
    if (deadzone < 0.0f)
        deadzone = 0.0f;
    if (deadzone > 0.5f)
        deadzone = 0.5f;

    settings.gamepad_deadzone = deadzone;
    ESP_LOGI(TAG, "Gamepad dead zone set to %.2f", deadzone);
}

void USBHIDManager::setGamepadStickMode(uint8_t mode)
{
    if (mode > 1)
        mode = 0; // Default to left stick if invalid

    settings.gamepad_stick_mode = mode;
    ESP_LOGI(TAG, "🎮 Gamepad stick mode set to %s (will be saved on settings save)", mode == 0 ? "left" : "right");
}

void USBHIDManager::setInvertMouseY(bool invert)
{
    settings.invert_mouse_y = invert;
    ESP_LOGI(TAG, "🔄 Mouse Y-axis invert set to: %s (wand UP -> cursor %s)",
             invert ? "true (INVERTED)" : "false (NORMAL)",
             invert ? "DOWN" : "UP");
}

void USBHIDManager::setMouse4ButtonClick(bool enabled)
{
    settings.mouse_4button_click = enabled;
    ESP_LOGI(TAG, "🔄 Mouse 4-button click set to: %s (mouse mode only, <400ms = click)",
             enabled ? "enabled" : "disabled");
}

void USBHIDManager::setAutoRecenterOnStill(bool enabled)
{
    settings.auto_recenter_on_still = enabled;
    ESP_LOGI(TAG, "🔄 Auto-recenter on still set to: %s (2s hold = recalibrate)",
             enabled ? "enabled" : "disabled");
}

void USBHIDManager::setStillnessThreshold(float threshold)
{
    if (threshold < 10.0f)
        threshold = 10.0f;
    if (threshold > 100.0f)
        threshold = 100.0f;

    settings.stillness_threshold = threshold;
    ESP_LOGI(TAG, "🔄 Stillness threshold set to: %.0f (lower = more sensitive)", threshold);
}

void USBHIDManager::setGamepadInvertY(bool invert)
{
    settings.gamepad_invert_y = invert;
    ESP_LOGI(TAG, "🔄 Gamepad Y-axis invert set to: %s", invert ? "true (INVERTED)" : "false (NORMAL)");
}

void USBHIDManager::resetGamepadSmoothing()
{
    smoothing_initialized = false;
    smoothed_lx = 0.0f;
    smoothed_ly = 0.0f;
    ESP_LOGI(TAG, "🔄 Gamepad smoothing reset");
}

void USBHIDManager::resetMouseSmoothing()
{
    mouse_smoothing_initialized = false;
    smoothed_mouse_x = 0.0f;
    smoothed_mouse_y = 0.0f;
    prev_vel_x = 0.0f;
    prev_vel_y = 0.0f;
    predicted_x = 0.0f;
    predicted_y = 0.0f;
    accumulated_x = 0.0f;
    accumulated_y = 0.0f;
    ESP_LOGI(TAG, "🔄 Mouse smoothing reset");
}

void USBHIDManager::setSpellGamepadButton(const char *spell_name, uint8_t button)
{
    if (!spell_name)
        return;

    if (button > 20)
        button = 0;

    extern const char *SPELL_NAMES[73];
    for (int i = 0; i < 73; i++)
    {
        if (strcmp(SPELL_NAMES[i], spell_name) == 0)
        {
            settings.spell_gamepad_buttons[i] = button;
            ESP_LOGI(TAG, "Spell '%s' (index %d) mapped to gamepad button %u", spell_name, i, button);
            return;
        }
    }
    ESP_LOGW(TAG, "Spell '%s' not found in spell list", spell_name);
}

uint8_t USBHIDManager::getSpellGamepadButton(const char *spell_name) const
{
    if (!spell_name)
        return 0;

    extern const char *SPELL_NAMES[73];
    for (int i = 0; i < 73; i++)
    {
        if (strcmp(SPELL_NAMES[i], spell_name) == 0)
        {
            return settings.spell_gamepad_buttons[i];
        }
    }
    return 0;
}

void USBHIDManager::sendSpellGamepadForSpell(const char *spell_name)
{
#if USE_USB_HID_DEVICE
    if (!spell_name)
        return;

    HIDMode mode = getHidMode();
    if (mode != HID_MODE_GAMEPAD_ONLY)
    {
        ESP_LOGD(TAG, "Spell '%s': Skipping gamepad output (mode=%d, need GAMEPAD_ONLY=2)", spell_name, mode);
        return;
    }

    uint8_t button = getSpellGamepadButton(spell_name);
    if (button == 0)
    {
        ESP_LOGD(TAG, "Spell '%s': No gamepad button mapped", spell_name);
        return;
    }

    if (button > 20)
    {
        ESP_LOGW(TAG, "Spell '%s': Invalid gamepad button %d (max 20)", spell_name, button);
        return;
    }

    ESP_LOGI(TAG, "Spell '%s': Sending gamepad input %d", spell_name, button);

    // Block position updates during spell gamepad input
    in_spell_mode = true;

    if (button <= 14)
    {
        // Buttons 1-14: Standard face/shoulder/menu/stick buttons
        uint16_t mask = (uint16_t)(1U << (button - 1));
        uint16_t previous = gamepad_buttons;
        gamepad_buttons = (previous | mask) & 0x3FFF;
        sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
        vTaskDelay(pdMS_TO_TICKS(50));
        gamepad_buttons = previous & 0x3FFF;
        sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
    }
    else if (button >= 15 && button <= 18)
    {
        // D-Pad (15=Up, 16=Down, 17=Left, 18=Right)
        // HAT values: 0=Up, 1=UpRight, 2=Right, 3=DownRight, 4=Down, 5=DownLeft, 6=Left, 7=UpLeft, 8=Center(null)
        uint8_t hat_value = 8; // Default centered
        switch (button)
        {
        case 15:
            hat_value = 0; // Up
            break;
        case 16:
            hat_value = 4; // Down
            break;
        case 17:
            hat_value = 6; // Left
            break;
        case 18:
            hat_value = 2; // Right
            break;
        }
        gamepad_hat = hat_value;
        ESP_LOGI(TAG, "🎮 D-Pad: direction=%d, ready=%d", hat_value, tud_hid_ready());
        // Send multiple times over 200ms to ensure Windows polls detect it
        for (int i = 0; i < 4; i++)
        {
            sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        gamepad_hat = 8; // Release (center)
        sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
        ESP_LOGI(TAG, "🎮 D-Pad released");
    }
    else if (button == 19 || button == 20)
    {
        // Triggers (19=LT, 20=RT)
        gamepad_lt = (button == 19) ? 255 : 0;
        gamepad_rt = (button == 20) ? 255 : 0;
        ESP_LOGI(TAG, "🎮 Trigger: LT=%d, RT=%d, ready=%d", gamepad_lt, gamepad_rt, tud_hid_ready());
        // Send multiple times over 200ms to ensure Windows polls detect it
        for (int i = 0; i < 4; i++)
        {
            sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        gamepad_lt = 0;
        gamepad_rt = 0;
        sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, gamepad_lt, gamepad_rt, gamepad_buttons, gamepad_hat); // Release
        ESP_LOGI(TAG, "🎮 Trigger released");
    }

    // Re-enable position updates
    in_spell_mode = false;
#endif
}

#endif // USE_USB_HID_DEVICE

#if !USE_USB_HID_DEVICE
// Stub implementations when USB HID is disabled
// Note: setInvertMouseY() and getInvertMouseY() are inline in header - not redefined here
void USBHIDManager::sendMouseReport(int8_t x, int8_t y, int8_t wheel, uint8_t buttons) {}
void USBHIDManager::sendKeyboardReport(uint8_t modifiers, uint8_t keycode) {}
uint8_t USBHIDManager::getKeycodeForSpell(const char *spell_name) { return 0; }
void USBHIDManager::sendSpellKeyboardForSpell(const char *spell_name) {}
void USBHIDManager::setSpellKeycode(const char *spell_name, uint8_t keycode) {}
uint8_t USBHIDManager::getSpellKeycode(const char *spell_name) const { return 0; }
bool USBHIDManager::loadSettings() { return true; }
bool USBHIDManager::saveSettings() { return true; }
bool USBHIDManager::resetSettings() { return true; }
void USBHIDManager::setMouseSensitivityValue(float sensitivity) {}
void USBHIDManager::updateGamepadFromGesture(float delta_x, float delta_y) {}
void USBHIDManager::updateGamepadFromPosition(float pos_x, float pos_y) {}
void USBHIDManager::setGamepadButtons(uint16_t buttons) {}
void USBHIDManager::setHidMode(HIDMode mode) {}
void USBHIDManager::setGamepadSensitivityValue(float sensitivity) {}
void USBHIDManager::setGamepadDeadzoneValue(float deadzone) {}
void USBHIDManager::setGamepadStickMode(uint8_t mode) {}
void USBHIDManager::setSpellGamepadButton(const char *spell_name, uint8_t button) {}
uint8_t USBHIDManager::getSpellGamepadButton(const char *spell_name) const { return 0; }
void USBHIDManager::sendSpellGamepadForSpell(const char *spell_name) {}
void USBHIDManager::cleanupOldNvsEntries() {}
#endif // USE_USB_HID_DEVICE
