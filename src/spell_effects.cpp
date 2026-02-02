#include "spell_effects.h"
#include "wand_protocol.h"
#include <string.h>

size_t SpellEffects::addBuzz(uint8_t *buffer, size_t offset, uint16_t duration_ms)
{
    buffer[offset++] = MACRO_HAP_BUZZ;
    buffer[offset++] = (duration_ms >> 8) & 0xFF; // Big-endian
    buffer[offset++] = duration_ms & 0xFF;
    return 3;
}

size_t SpellEffects::addLEDTransition(uint8_t *buffer, size_t offset,
                                      uint8_t group, uint8_t r, uint8_t g, uint8_t b,
                                      uint16_t duration_ms)
{
    buffer[offset++] = MACRO_LIGHT_TRANSITION;
    buffer[offset++] = group;
    buffer[offset++] = r;
    buffer[offset++] = g;
    buffer[offset++] = b;
    buffer[offset++] = (duration_ms >> 8) & 0xFF; // Big-endian
    buffer[offset++] = duration_ms & 0xFF;
    return 7;
}

size_t SpellEffects::addDelay(uint8_t *buffer, size_t offset, uint16_t duration_ms)
{
    buffer[offset++] = MACRO_DELAY;
    buffer[offset++] = (duration_ms >> 8) & 0xFF; // Big-endian
    buffer[offset++] = duration_ms & 0xFF;
    return 3;
}

size_t SpellEffects::addClear(uint8_t *buffer, size_t offset)
{
    buffer[offset++] = MACRO_LIGHT_CLEAR;
    return 1;
}

size_t SpellEffects::buildEffect(const char *spell_name, uint8_t *buffer, size_t buffer_size)
{
    if (!spell_name || !buffer || buffer_size < 32)
    {
        return 0;
    }

    size_t len = 0;
    buffer[len++] = MACRO_CONTROL; // Macro control byte

    // Build effect based on spell name
    if (strcmp(spell_name, "Lumos") == 0)
    {
        // Buzz 150ms + White LED 2s
        len += addBuzz(buffer, len, 150);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                255, 255, 255, 2000);
    }
    else if (strcmp(spell_name, "Nox") == 0)
    {
        // Buzz 100ms + Purple flash + Clear
        len += addBuzz(buffer, len, 100);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                51, 0, 51, 200);
        len += addDelay(buffer, len, 100);
        len += addClear(buffer, len);
    }
    else if (strcmp(spell_name, "Verdimillious") == 0 || strcmp(spell_name, "Reducto") == 0)
    {
        // Green spell effect
        len += addBuzz(buffer, len, 200);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                0, 255, 0, 200);
    }
    else if (strcmp(spell_name, "Incendio") == 0 || strcmp(spell_name, "Flagrate") == 0)
    {
        // Fire spell effect (orange)
        len += addBuzz(buffer, len, 150);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                255, 102, 0, 400);
    }
    else if (strcmp(spell_name, "Expelliarmus") == 0)
    {
        // Red disarming spell
        len += addBuzz(buffer, len, 200);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                255, 0, 0, 300);
    }
    else if (strcmp(spell_name, "Stupefy") == 0)
    {
        // Red stunning spell with longer buzz
        len += addBuzz(buffer, len, 250);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                200, 0, 0, 400);
    }
    else if (strcmp(spell_name, "Protego") == 0)
    {
        // Blue shield spell
        len += addBuzz(buffer, len, 150);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                0, 100, 255, 500);
    }
    else if (strcmp(spell_name, "Wingardium Leviosa") == 0)
    {
        // Light blue levitation spell
        len += addBuzz(buffer, len, 100);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                100, 200, 255, 600);
    }
    else if (strcmp(spell_name, "Accio") == 0)
    {
        // Cyan summoning spell
        len += addBuzz(buffer, len, 120);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                0, 255, 255, 300);
    }
    else
    {
        // Default effect for unknown spells (blue flash)
        len += addBuzz(buffer, len, 100);
        len += addLEDTransition(buffer, len, (uint8_t)LedGroup::TIP,
                                0, 100, 255, 200);
    }

    return len;
}
