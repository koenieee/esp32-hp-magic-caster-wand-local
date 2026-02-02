#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <nvs.h>

// USB HID Settings structure stored in NVS
struct USBHIDSettings
{
    float mouse_sensitivity;    // Mouse sensitivity multiplier (default 1.0)
    uint8_t spell_keycodes[73]; // Maps spell 0-72 to keycodes (default all 0 = disabled)
};

// USB HID Manager for Magic Caster Wand
// Provides both mouse (gyro-based) and keyboard (spell-based) functionality

class USBHIDManager
{
public:
    USBHIDManager();
    ~USBHIDManager();

    // Initialize USB HID device (both mouse and keyboard)
    bool begin();

    // Mouse functions (gyro-based air mouse)
    void updateMouse(float gyro_x, float gyro_y, float gyro_z);
    void updateMouseFromGesture(float delta_x, float delta_y);
    void mouseClick(uint8_t button); // 1=left, 2=right, 4=middle
    void setMouseSensitivity(float sensitivity);

    // Keyboard functions (spell to key mapping)
    void sendKeyPress(uint8_t keycode, uint8_t modifiers = 0);
    void sendKeyRelease();
    void typeString(const char *text);
    void sendSpellKeyboard(const char *spell_name);
    void sendSpellKeyboardForSpell(const char *spell_name); // Send mapped key for detected spell

    // Configuration
    void setEnabled(bool mouse_enabled, bool keyboard_enabled);
    bool isMouseEnabled() const { return mouse_enabled; }
    bool isKeyboardEnabled() const { return keyboard_enabled; }
    void setInSpellMode(bool spelling) { in_spell_mode = spelling; }
    bool isInSpellMode() const { return in_spell_mode; }

    // Settings management
    bool loadSettings();
    bool saveSettings();
    bool resetSettings();
    void setMouseSensitivityValue(float sensitivity);
    void setSpellKeycode(const char *spell_name, uint8_t keycode);
    uint8_t getSpellKeycode(const char *spell_name) const;

    // Settings accessors for web interface
    float getMouseSensitivity() const { return settings.mouse_sensitivity; }
    float getMouseSensitivityValue() const { return mouse_sensitivity; }
    const USBHIDSettings &getSettings() const { return settings; }
    const uint8_t *getSpellKeycodes() const { return settings.spell_keycodes; }

private:
    bool initialized;
    bool mouse_enabled;
    bool keyboard_enabled;
    float mouse_sensitivity;
    bool in_spell_mode;      // True while spell is being tracked (all buttons held)
    USBHIDSettings settings; // Current NVS settings

    // Mouse state
    int8_t accumulated_x;
    int8_t accumulated_y;
    uint8_t button_state;

    // Helper functions
    void sendMouseReport(int8_t x, int8_t y, int8_t wheel, uint8_t buttons);
    void sendKeyboardReport(uint8_t modifiers, uint8_t keycode);
    uint8_t getKeycodeForSpell(const char *spell_name);
};
