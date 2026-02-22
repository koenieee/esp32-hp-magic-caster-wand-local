#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <nvs.h>

enum HIDMode : uint8_t
{
    HID_MODE_MOUSE = 0,
    HID_MODE_KEYBOARD = 1,
    HID_MODE_GAMEPAD_ONLY = 2,  // Wand controls joystick, spells send gamepad buttons
    HID_MODE_GAMEPAD_MIXED = 3, // Wand controls joystick, spells send keyboard keys
    HID_MODE_DISABLED = 4
};

// USB HID Settings structure stored in NVS
struct USBHIDSettings
{
    float mouse_sensitivity;           // Mouse sensitivity multiplier (default 1.0)
    uint8_t spell_keycodes[73];        // Maps spell 0-72 to keycodes (default all 0 = disabled)
    bool invert_mouse_y;               // Invert Y-axis (true = wand up -> cursor up, false = wand up -> cursor down)
    bool mouse_enabled;                // Enable/disable mouse input (default true)
    bool keyboard_enabled;             // Enable/disable keyboard input (default true)
    bool mouse_4button_click;          // Enable mouse click on 4-button press (mouse mode only, <400ms = click)
    uint8_t hid_mode;                  // Current HID mode (see HIDMode)
    float gamepad_sensitivity;         // Gamepad sensitivity multiplier (default 1.0)
    float gamepad_deadzone;            // Gamepad dead zone (0.0-0.5)
    bool gamepad_invert_y;             // Invert gamepad Y-axis
    uint8_t gamepad_stick_mode;        // Gamepad stick selection (0=left, 1=right)
    uint8_t spell_gamepad_buttons[73]; // Maps spell 0-72 to gamepad button (0=disabled, 1-20)
    bool auto_recenter_on_still;       // Auto-recalibrate when wand held still for 2+ seconds
    float stillness_threshold;         // Movement threshold for stillness detection (10-100, default 40)
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
    void updateMouseFromGesture(float delta_x, float delta_y);   // Legacy: delta-based movement
    void updateMouseFromPosition(float pos_x, float pos_y);      // New: velocity-based (position = velocity)
    void updateGamepadFromGesture(float delta_x, float delta_y); // Legacy: delta-based
    void updateGamepadFromPosition(float pos_x, float pos_y);    // New: absolute position for joystick
    void setGamepadButtons(uint16_t buttons);
    void mouseClick(uint8_t button); // 1=left, 2=right, 4=middle
    void setMouseSensitivity(float sensitivity);
    void setGamepadSensitivityValue(float sensitivity);
    void setGamepadDeadzoneValue(float deadzone);
    void setGamepadInvertY(bool invert);
    void resetGamepadSmoothing(); // Reset smoothing filter (call on recenter)
    void resetMouseSmoothing();   // Reset mouse smoothing filter

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
    void setMouseEnabled(bool enabled)
    {
        mouse_enabled = enabled;
        settings.mouse_enabled = enabled;
    }
    void setKeyboardEnabled(bool enabled)
    {
        keyboard_enabled = enabled;
        settings.keyboard_enabled = enabled;
    }
    void setInSpellMode(bool spelling) { in_spell_mode = spelling; }
    bool isInSpellMode() const { return in_spell_mode; }
    void setHidMode(HIDMode mode);
    HIDMode getHidMode() const { return static_cast<HIDMode>(settings.hid_mode); }

    // Settings management
    bool loadSettings();
    bool saveSettings();
    bool resetSettings();
    void setMouseSensitivityValue(float sensitivity);
    void setSpellKeycode(const char *spell_name, uint8_t keycode);
    uint8_t getSpellKeycode(const char *spell_name) const;
    void setSpellGamepadButton(const char *spell_name, uint8_t button);
    uint8_t getSpellGamepadButton(const char *spell_name) const;
    void sendSpellGamepadForSpell(const char *spell_name);

    // Settings accessors for web interface
    float getMouseSensitivity() const { return settings.mouse_sensitivity; }
    float getMouseSensitivityValue() const { return mouse_sensitivity; }
    float getGamepadSensitivity() const { return settings.gamepad_sensitivity; }
    float getGamepadDeadzone() const { return settings.gamepad_deadzone; }
    bool getGamepadInvertY() const { return settings.gamepad_invert_y; }
    uint8_t getGamepadStickMode() const { return settings.gamepad_stick_mode; }
    void setGamepadStickMode(uint8_t mode);
    const USBHIDSettings &getSettings() const { return settings; }
    const uint8_t *getSpellKeycodes() const { return settings.spell_keycodes; }
    const uint8_t *getSpellGamepadButtons() const { return settings.spell_gamepad_buttons; }
    bool getInvertMouseY() const { return settings.invert_mouse_y; }
    void setInvertMouseY(bool invert);
    bool getMouse4ButtonClick() const { return settings.mouse_4button_click; }
    void setMouse4ButtonClick(bool enabled);
    bool getAutoRecenterOnStill() const { return settings.auto_recenter_on_still; }
    void setAutoRecenterOnStill(bool enabled);
    float getStillnessThreshold() const { return settings.stillness_threshold; }
    void setStillnessThreshold(float threshold);

private:
    bool initialized;
    bool mouse_enabled;
    bool keyboard_enabled;
    float mouse_sensitivity;
    bool in_spell_mode;      // True while spell is being tracked (all buttons held)
    USBHIDSettings settings; // Current NVS settings

    // Mouse state
    uint8_t button_state;
    uint16_t gamepad_buttons;
    int8_t gamepad_lx;
    int8_t gamepad_ly;
    int8_t gamepad_rx;
    int8_t gamepad_ry;
    uint8_t gamepad_lt;  // Left trigger state (0-255)
    uint8_t gamepad_rt;  // Right trigger state (0-255)
    uint8_t gamepad_hat; // HAT switch state (0-7 directions, 8=center)

    // Smoothing for gamepad position
    float smoothed_lx;
    float smoothed_ly;
    bool smoothing_initialized;

    // Smoothing for mouse velocity
    float smoothed_mouse_x;
    float smoothed_mouse_y;
    bool mouse_smoothing_initialized;

    // Predictive motion tracking for gap-filling
    float prev_vel_x;
    float prev_vel_y;
    float predicted_x;
    float predicted_y;

    // Sub-pixel interpolation accumulator for ultra-smooth motion
    float accumulated_x;
    float accumulated_y;

    // Helper functions
    void sendMouseReport(int8_t x, int8_t y, int8_t wheel, uint8_t buttons);
    void sendKeyboardReport(uint8_t modifiers, uint8_t keycode);
    void sendGamepadReport(int8_t lx, int8_t ly, int8_t rx, int8_t ry, uint8_t lt, uint8_t rt, uint16_t buttons, uint8_t hat);
    uint8_t getKeycodeForSpell(const char *spell_name);
    void cleanupOldNvsEntries(); // Remove orphaned individual spell NVS entries
};
