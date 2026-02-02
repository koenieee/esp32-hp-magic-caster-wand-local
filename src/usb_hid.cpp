#include "usb_hid.h"
#include "config.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <cmath>

static const char *TAG = "usb_hid";

#if USE_USB_HID_DEVICE
#include "tinyusb.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "class/cdc/cdc_device.h"

// HID Report Descriptor for composite mouse + keyboard
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
    0xC0        // End Collection (Application)
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
#endif

USBHIDManager::USBHIDManager()
    : initialized(false),
      mouse_enabled(true),
      keyboard_enabled(true),
      mouse_sensitivity(1.0f),
      in_spell_mode(false),
      accumulated_x(0),
      accumulated_y(0),
      button_state(0)
{
    // Initialize default settings
    settings.mouse_sensitivity = 1.0f;
    // Initialize all spell keycodes to 0 (disabled)
    memset(settings.spell_keycodes, 0, sizeof(settings.spell_keycodes));
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
    s_prev_log_vprintf = esp_log_set_vprintf(usb_cdc_log_vprintf);
    ESP_LOGI(TAG, "USB CDC logging enabled");
#endif

    initialized = true;
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
    if (!initialized || !mouse_enabled || in_spell_mode)
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
    if (!initialized || !mouse_enabled || in_spell_mode)
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
    if (!initialized || !keyboard_enabled)
        return;

    sendKeyboardReport(modifiers, keycode);
#endif
}

void USBHIDManager::sendKeyRelease()
{
#if USE_USB_HID_DEVICE
    if (!initialized || !keyboard_enabled)
        return;

    sendKeyboardReport(0, 0);
#endif
}

void USBHIDManager::typeString(const char *text)
{
#if USE_USB_HID_DEVICE
    if (!initialized || !keyboard_enabled || !text)
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
    if (!initialized || !keyboard_enabled || !spell_name)
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
    ESP_LOGI(TAG, "USB HID enabled: mouse=%d, keyboard=%d", mouse_enabled, keyboard_enabled);
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
    if (!spell_name)
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
    }
    else
    {
        settings.mouse_sensitivity = 1.0f;
    }

    // Load spell keycodes (73 spells)
    for (int i = 0; i < 73; i++)
    {
        char key[16];
        snprintf(key, sizeof(key), "spell%d", i);
        nvs_get_u8(nvs_handle, key, &settings.spell_keycodes[i]);
        // If not found, spell_keycodes[i] remains 0 (disabled)
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "USB HID settings loaded from NVS");
    return true;
}

bool USBHIDManager::saveSettings()
{
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

    // Save spell keycodes (73 spells)
    for (int i = 0; i < 73; i++)
    {
        char key[16];
        snprintf(key, sizeof(key), "spell%d", i);
        nvs_set_u8(nvs_handle, key, settings.spell_keycodes[i]);
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to commit NVS settings");
        return false;
    }

    ESP_LOGI(TAG, "USB HID settings saved to NVS");
    return true;
}

bool USBHIDManager::resetSettings()
{
    // Reset to defaults
    settings.mouse_sensitivity = 1.0f;
    memset(settings.spell_keycodes, 0, sizeof(settings.spell_keycodes));

    mouse_sensitivity = 1.0f;

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

#endif // USE_USB_HID_DEVICE

#if !USE_USB_HID_DEVICE
// Stub implementations when USB HID is disabled
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
#endif // USE_USB_HID_DEVICE
