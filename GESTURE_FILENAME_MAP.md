# Gesture Filename Mapping

SPIFFS has a **32-character filename limit** (including `.png` extension). Some spell names are too long when converted directly. This document maps the spell names to their shortened filenames.

## Files That Need Renaming

The following files exceed the 32-character limit and must be renamed:

```bash
# In the /gestures directory, rename these files:
mv the_hair_thickening_growing_charm.png hair_grow_charm.png
```

## Full Spell Name to Filename Mapping

| Spell Name (from SPELL_NAMES) | Filename in SPIFFS | Status |
|-------------------------------|-------------------|---------|
| The_Force_Spell | the_force_spell.png | ✓ OK (19 chars) |
| Colloportus | colloportus.png | ✓ OK (15 chars) |
| Colloshoo | colloshoo.png | ✓ OK (13 chars) |
| The_Hour_Reversal_Reversal_Charm | hour_reversal_rev.png | ⚠️ NEEDS RENAME (21 chars) |
| Evanesco | evanesco.png | ✓ OK (12 chars) |
| Herbivicus | herbivicus.png | ✓ OK (14 chars) |
| Orchideous | orchideous.png | ✓ OK (14 chars) |
| Brachiabindo | brachiabindo.png | ✓ OK (16 chars) |
| Meteolojinx | meteolojinx.png | ✓ OK (15 chars) |
| Riddikulus | riddikulus.png | ✓ OK (14 chars) |
| Silencio | silencio.png | ✓ OK (12 chars) |
| Immobulus | immobulus.png | ✓ OK (13 chars) |
| Confringo | confringo.png | ✓ OK (13 chars) |
| Petrificus_Totalus | petrificus_totalus.png | ✓ OK (22 chars) |
| Flipendo | flipendo.png | ✓ OK (12 chars) |
| The_Cheering_Charm | the_cheering_charm.png | ✓ OK (22 chars) |
| Salvio_Hexia | salvio_hexia.png | ✓ OK (16 chars) |
| Pestis_Incendium | pestis_incendium.png | ⚠️ MISSING |
| Alohomora | alohomora.png | ✓ OK (13 chars) |
| Protego | protego.png | ✓ OK (11 chars) |
| Langlock | langlock.png | ⚠️ MISSING |
| Mucus_Ad_Nauseum | mucus_ad_nauseum.png | ✓ OK (20 chars) |
| Flagrate | flagrate.png | ✓ OK (12 chars) |
| Glacius | glacius.png | ✓ OK (11 chars) |
| Finite | finite.png | ✓ OK (10 chars) |
| Anteoculatia | anteoculatia.png | ✓ OK (16 chars) |
| Expelliarmus | expelliarmus.png | ✓ OK (16 chars) |
| Expecto_Patronum | expecto_patronum.png | ✓ OK (20 chars) |
| Descendo | descendo.png | ⚠️ MISSING |
| Depulso | depulso.png | ⚠️ MISSING |
| Reducto | reducto.png | ✓ OK (11 chars) |
| Colovaria | colovaria.png | ✓ OK (13 chars) |
| Aberto | aberto.png | ✓ OK (10 chars) |
| Confundo | confundo.png | ✓ OK (12 chars) |
| Densaugeo | densaugeo.png | ✓ OK (13 chars) |
| The_Stretching_Jinx | the_stretching_jinx.png | ✓ OK (23 chars) |
| Entomorphis | entomorphis.png | ✓ OK (15 chars) |
| The_Hair_Thickening_Growing_Charm | hair_grow_charm.png | ⚠️ NEEDS RENAME (19 chars) |
| Bombarda | bombarda.png | ✓ OK (12 chars) |
| Finestra | finestra.png | ✓ OK (12 chars) |
| The_Sleeping_Charm | the_sleeping_charm.png | ✓ OK (22 chars) |
| Rictusempra | rictusempra.png | ✓ OK (15 chars) |
| Piertotum_Locomotor | piertotum_locomotor.png | ✓ OK (23 chars) |
| Expulso | expulso.png | ✓ OK (11 chars) |
| Impedimenta | impedimenta.png | ✓ OK (15 chars) |
| Ascendio | ascendio.png | ✓ OK (12 chars) |
| Incarcerous | incarcerous.png | ✓ OK (15 chars) |
| Ventus | ventus.png | ✓ OK (10 chars) |
| Revelio | revelio.png | ✓ OK (11 chars) |
| Accio | accio.png | ✓ OK (9 chars) |
| Melefors | melefors.png | ✓ OK (12 chars) |
| Scourgify | scourgify.png | ✓ OK (13 chars) |
| Wingardium_Leviosa | wingardium_leviosa.png | ✓ OK (22 chars) |
| Nox | nox.png | ✓ OK (7 chars) |
| Stupefy | stupefy.png | ✓ OK (11 chars) |
| Spongify | spongify.png | ⚠️ MISSING |
| Lumos | lumos.png | ✓ OK (9 chars) |
| Appare_Vestigium | appare_vestigium.png | ✓ OK (20 chars) |
| Verdimillious | verdimillious.png | ✓ OK (17 chars) |
| Fulgari | fulgari.png | ✓ OK (11 chars) |
| Reparo | reparo.png | ✓ OK (10 chars) |
| Locomotor | locomotor.png | ⚠️ MISSING |
| Quietus | quietus.png | ✓ OK (11 chars) |
| Everte_Statum | everte_statum.png | ⚠️ MISSING |
| Incendio | incendio.png | ✓ OK (12 chars) |
| Aguamenti | aguamenti.png | ✓ OK (13 chars) |
| Sonorus | sonorus.png | ⚠️ MISSING |
| Cantis | cantis.png | ✓ OK (10 chars) |
| Arania_Exumai | arania_exumai.png | ✓ OK (17 chars) |
| Calvorio | calvorio.png | ✓ OK (12 chars) |
| The_Hour_Reversal_Charm | hour_reversal.png | ⚠️ NEEDS RENAME (17 chars) |
| Vermillious | vermillious.png | ✓ OK (15 chars) |
| The_Pepper-Breath_Hex | pepper_breath_hex.png | ⚠️ NEEDS RENAME (21 chars) |

## Rename Commands

Run these commands in the `/gestures` directory:

```bash
cd gestures

# Rename files that exceed 32 character limit
mv the_hair_thickening_growing_charm.png hair_grow_charm.png

# Create missing/renamed files if you have them with different names
# (You'll need to find or create these missing gesture images)
```

## Implementation

The web interface will use a JavaScript mapping to convert SPELL_NAMES to the actual filenames in SPIFFS.
