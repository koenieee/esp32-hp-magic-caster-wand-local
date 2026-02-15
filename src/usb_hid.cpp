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

    // Triggers (LT, RT) - Z axis
    0x09, 0x32, //   Usage (Z) - Left Trigger
    0x09, 0x35, //   Usage (Rz) - Right Trigger
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0xFF, //   Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x02, //   Report Count (2)
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
      accumulated_x(0),
      accumulated_y(0),
      button_state(0),
      gamepad_buttons(0),
      gamepad_lx(0),
      gamepad_ly(0),
      gamepad_rx(0),
      gamepad_ry(0)
{
    // Initialize default settings
    settings.mouse_sensitivity = 1.0f;
    settings.invert_mouse_y = false;  // Default: natural (wand UP = cursor UP, since IMU gives negative for up)
    settings.mouse_enabled = true;    // Default: enabled
    settings.keyboard_enabled = true; // Default: enabled
    settings.hid_mode = HID_MODE_MOUSE;
    settings.gamepad_sensitivity = 1.0f;
    settings.gamepad_deadzone = 0.05f;
    settings.gamepad_invert_y = false; // Default: natural (wand UP = stick UP)
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

void USBHIDManager::updateGamepadFromGesture(float delta_x, float delta_y)
{
#if USE_USB_HID_DEVICE
    if (!initialized || in_spell_mode || getHidMode() != HID_MODE_GAMEPAD)
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

    // Use left stick only; right stick centered, triggers off, hat neutral.
    sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, 0, 0, gamepad_buttons, 0x0F);
#endif
}

void USBHIDManager::setGamepadButtons(uint16_t buttons)
{
    gamepad_buttons = buttons & 0x3FFF; // 14 buttons max

#if USE_USB_HID_DEVICE
    if (initialized && getHidMode() == HID_MODE_GAMEPAD)
    {
        sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, 0, 0, gamepad_buttons, 0x0F);
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
    case HID_MODE_GAMEPAD:
        mouse_enabled = false;
        keyboard_enabled = false;
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
    report[8] = (uint8_t)(hat & 0x0F);            // D-pad hat (4 bits) + 4 bits padding

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
    // Allow keyboard spell triggers in both Mouse and Keyboard modes
    if (!spell_name || (getHidMode() != HID_MODE_KEYBOARD && getHidMode() != HID_MODE_MOUSE))
        return;

    uint8_t keycode = getSpellKeycode(spell_name);
    if (keycode != 0)
    {
        ESP_LOGI(TAG, "Spell '%s': Sending key 0x%02X", spell_name, keycode);
        sendKeyPress(keycode, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        sendKeyRelease();
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

bool USBHIDManager::loadSettings()
{
    ESP_LOGI(TAG, "📂 Loading settings from NVS...");
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

    // Load gamepad sensitivity (stored as uint8_t: value * 10)
    uint8_t gpad_sens_10x = 10; // Default 1.0x
    err = nvs_get_u8(nvs_handle, "gamepad_sens_10x", &gpad_sens_10x);
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
    err = nvs_get_u8(nvs_handle, "gamepad_deadzone_100", &gpad_deadzone_100);
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
    err = nvs_get_u8(nvs_handle, "gamepad_invert_y", &gpad_invert_y);
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

    // Log current state for debugging
    ESP_LOGI(TAG, "🎯 Current axis inversion settings: mouse_y=%s, gamepad_y=%s",
             settings.invert_mouse_y ? "INVERTED" : "NORMAL",
             settings.gamepad_invert_y ? "INVERTED" : "NORMAL");

    // Load mouse_enabled
    uint8_t mouse_en = 1; // Default: enabled
    err = nvs_get_u8(nvs_handle, "mouse_enabled", &mouse_en);
    settings.mouse_enabled = (mouse_en != 0);

    // Load keyboard_enabled
    uint8_t keyboard_en = 1; // Default: enabled
    err = nvs_get_u8(nvs_handle, "keyboard_enabled", &keyboard_en);
    settings.keyboard_enabled = (keyboard_en != 0);

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

    // Load spell keycodes (73 spells)
    ESP_LOGI(TAG, "Loading spell keycodes from NVS...");
    int non_zero_count = 0;
    for (int i = 0; i < 73; i++)
    {
        char key[16];
        snprintf(key, sizeof(key), "spell%d", i);
        uint8_t old_value = settings.spell_keycodes[i];
        nvs_get_u8(nvs_handle, key, &settings.spell_keycodes[i]);
        if (settings.spell_keycodes[i] != 0)
        {
            non_zero_count++;
            if (settings.spell_keycodes[i] != old_value)
            {
                extern const char *SPELL_NAMES[73];
                ESP_LOGI(TAG, "  Spell[%d]='%s' loaded: 0x%02X (%d)",
                         i, SPELL_NAMES[i], settings.spell_keycodes[i], settings.spell_keycodes[i]);
            }
        }
    }
    ESP_LOGI(TAG, "Loaded %d non-zero spell mappings", non_zero_count);

    nvs_close(nvs_handle);

    // Open gamepad namespace for gamepad spell mappings
    err = nvs_open("gamepad", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        // Load spell gamepad button mappings (73 spells)
        for (int i = 0; i < 73; i++)
        {
            char key[20];
            snprintf(key, sizeof(key), "gpad_spell%d", i);
            nvs_get_u8(nvs_handle, key, &settings.spell_gamepad_buttons[i]);
            // If not found, spell_gamepad_buttons[i] remains 0 (disabled)
        }
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "USB HID settings loaded from NVS");
    return true;
}

bool USBHIDManager::saveSettings()
{
    ESP_LOGI(TAG, "🔍 saveSettings() called - checking current settings struct values:");
    ESP_LOGI(TAG, "   gamepad_sensitivity=%.2f, gamepad_deadzone=%.2f, gamepad_invert_y=%s",
             settings.gamepad_sensitivity, settings.gamepad_deadzone, settings.gamepad_invert_y ? "true" : "false");
    ESP_LOGI(TAG, "   mouse_sensitivity=%.2f, invert_mouse_y=%s",
             settings.mouse_sensitivity, settings.invert_mouse_y ? "true" : "false");

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("usb_hid", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS namespace 'usb_hid'");
        return false;
    }

    // Save mouse sensitivity (as 10x value to store as uint8)
    uint8_t sens_10x = (uint8_t)(settings.mouse_sensitivity * 10.0f);
    nvs_set_u8(nvs_handle, "mouse_sens_10x", sens_10x);

    // Save invert_mouse_y setting
    nvs_set_u8(nvs_handle, "invert_mouse_y", settings.invert_mouse_y ? 1 : 0);
    ESP_LOGI(TAG, "💾 Saved invert_mouse_y to NVS: %s", settings.invert_mouse_y ? "true" : "false");

    // Save mouse_enabled
    nvs_set_u8(nvs_handle, "mouse_enabled", settings.mouse_enabled ? 1 : 0);

    // Save keyboard_enabled
    nvs_set_u8(nvs_handle, "keyboard_enabled", settings.keyboard_enabled ? 1 : 0);

    // Save HID mode
    nvs_set_u8(nvs_handle, "hid_mode", settings.hid_mode);

    // Save spell keycodes (73 spells)
    ESP_LOGI(TAG, "Saving spell keycodes to NVS...");
    int saved_count = 0;
    for (int i = 0; i < 73; i++)
    {
        char key[16];
        if (settings.spell_keycodes[i] != 0)
        {
            extern const char *SPELL_NAMES[73];
            ESP_LOGI(TAG, "  Saving spell[%d]='%s' = 0x%02X (%d)",
                     i, SPELL_NAMES[i], settings.spell_keycodes[i], settings.spell_keycodes[i]);
            saved_count++;
        }
        snprintf(key, sizeof(key), "spell%d", i);
        nvs_set_u8(nvs_handle, key, settings.spell_keycodes[i]);
    }
    ESP_LOGI(TAG, "Saved %d non-zero spell mappings to NVS", saved_count);

    // Commit usb_hid namespace
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit usb_hid NVS settings");
        nvs_close(nvs_handle);
        return false;
    }
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "✅ Saved usb_hid settings to NVS");

    // Open gamepad namespace for gamepad-specific settings
    err = nvs_open("gamepad", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS namespace 'gamepad'");
        return false;
    }

    // Save gamepad sensitivity (as 10x value to store as uint8)
    uint8_t gpad_sens_10x = (uint8_t)(settings.gamepad_sensitivity * 10.0f);
    nvs_set_u8(nvs_handle, "gamepad_sens_10x", gpad_sens_10x);
    ESP_LOGI(TAG, "💾 Saved gamepad_sensitivity to NVS: %.2f (raw: %d)", settings.gamepad_sensitivity, gpad_sens_10x);

    // Save gamepad deadzone (as 100x value to store as uint8)
    uint8_t gpad_deadzone_100 = (uint8_t)(settings.gamepad_deadzone * 100.0f);
    nvs_set_u8(nvs_handle, "gamepad_deadzone_100", gpad_deadzone_100);
    ESP_LOGI(TAG, "💾 Saved gamepad_deadzone to NVS: %.2f (raw: %d)", settings.gamepad_deadzone, gpad_deadzone_100);

    // Save gamepad invert_y
    nvs_set_u8(nvs_handle, "gamepad_invert_y", settings.gamepad_invert_y ? 1 : 0);
    ESP_LOGI(TAG, "💾 Saved gamepad_invert_y to NVS: %s", settings.gamepad_invert_y ? "true" : "false");

    // Save spell gamepad button mappings (73 spells)
    ESP_LOGI(TAG, "Saving gamepad spell button mappings to NVS...");
    for (int i = 0; i < 73; i++)
    {
        char key[20];
        snprintf(key, sizeof(key), "gpad_spell%d", i);
        nvs_set_u8(nvs_handle, key, settings.spell_gamepad_buttons[i]);
    }

    // Commit gamepad namespace
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit gamepad NVS settings");
        nvs_close(nvs_handle);
        return false;
    }

    // Verify gamepad values were actually written
    uint8_t verify_gpad_sens = 0;
    uint8_t verify_gpad_deadzone = 0;
    uint8_t verify_gpad_invert = 0;
    nvs_get_u8(nvs_handle, "gamepad_sens_10x", &verify_gpad_sens);
    nvs_get_u8(nvs_handle, "gamepad_deadzone_100", &verify_gpad_deadzone);
    nvs_get_u8(nvs_handle, "gamepad_invert_y", &verify_gpad_invert);
    ESP_LOGI(TAG, "✅ Verified gamepad NVS write: sens=%d (%.1fx), deadzone=%d (%.2f), invert=%d",
             verify_gpad_sens, verify_gpad_sens / 10.0f, verify_gpad_deadzone, verify_gpad_deadzone / 100.0f, verify_gpad_invert);

    nvs_close(nvs_handle);

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

void USBHIDManager::setInvertMouseY(bool invert)
{
    settings.invert_mouse_y = invert;
    ESP_LOGI(TAG, "🔄 Mouse Y-axis invert set to: %s (wand UP -> cursor %s)",
             invert ? "true (INVERTED)" : "false (NORMAL)",
             invert ? "DOWN" : "UP");
}

void USBHIDManager::setGamepadInvertY(bool invert)
{
    settings.gamepad_invert_y = invert;
    ESP_LOGI(TAG, "🔄 Gamepad Y-axis invert set to: %s", invert ? "true (INVERTED)" : "false (NORMAL)");
}

void USBHIDManager::setSpellGamepadButton(const char *spell_name, uint8_t button)
{
    if (!spell_name)
        return;

    if (button > 10)
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
    if (!spell_name || getHidMode() != HID_MODE_GAMEPAD)
        return;

    uint8_t button = getSpellGamepadButton(spell_name);
    if (button == 0 || button > 14)
        return;

    uint16_t mask = (uint16_t)(1U << (button - 1));
    uint16_t previous = gamepad_buttons;
    gamepad_buttons = (previous | mask) & 0x3FFF;
    sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, 0, 0, gamepad_buttons, 0x0F);
    vTaskDelay(pdMS_TO_TICKS(50));
    gamepad_buttons = previous & 0x3FFF;
    sendGamepadReport(gamepad_lx, gamepad_ly, gamepad_rx, gamepad_ry, 0, 0, gamepad_buttons, 0x0F);
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
void USBHIDManager::setGamepadButtons(uint16_t buttons) {}
void USBHIDManager::setHidMode(HIDMode mode) {}
void USBHIDManager::setGamepadSensitivityValue(float sensitivity) {}
void USBHIDManager::setGamepadDeadzoneValue(float deadzone) {}
void USBHIDManager::setSpellGamepadButton(const char *spell_name, uint8_t button) {}
uint8_t USBHIDManager::getSpellGamepadButton(const char *spell_name) const { return 0; }
void USBHIDManager::sendSpellGamepadForSpell(const char *spell_name) {}
#endif // USE_USB_HID_DEVICE
