#ifndef SPELL_EFFECTS_H
#define SPELL_EFFECTS_H

#include <stdint.h>
#include <stddef.h>

// Pre-built spell effect macros
class SpellEffects
{
public:
    // Build a spell effect macro for the given spell name
    // Returns the size of the macro data written to buffer
    // Returns 0 if spell is unknown or buffer is too small
    static size_t buildEffect(const char *spell_name, uint8_t *buffer, size_t buffer_size);

private:
    // Helper to add macro commands
    static size_t addBuzz(uint8_t *buffer, size_t offset, uint16_t duration_ms);
    static size_t addLEDTransition(uint8_t *buffer, size_t offset,
                                   uint8_t group, uint8_t r, uint8_t g, uint8_t b,
                                   uint16_t duration_ms);
    static size_t addDelay(uint8_t *buffer, size_t offset, uint16_t duration_ms);
    static size_t addClear(uint8_t *buffer, size_t offset);
};

#endif // SPELL_EFFECTS_H
