# AERA font fallbacks

LVGL's built-in Montserrat fonts cover the compact ASCII UI. AERA chains
FreeType fallbacks from `aeraui/localization/fonts/assets` for translated text and filenames. Fallback
faces are created lazily for each UI size so the full multilingual set does
not inflate native code or eagerly consume recovery memory.

`aera_fallback.cpp` keeps the active locale's script early in the chain while
retaining coverage for every language shown by the language selector. Missing
font files degrade to the preceding font instead of preventing recovery from
starting.

`wqy-microhei.ttf` contains only WenQuanYi Micro Hei's proportional face. The
upstream collection also contains Micro Hei Mono, but LVGL always opens face
index 0 and never uses that second face. Extracting the first face retains its
complete Unicode map, glyph outlines, metrics, hinting and font names; this is
not a character subset, so Chinese/Korean text and arbitrary filenames retain
the same coverage.

To reproduce the extraction from the upstream collection, install fontTools
on the development machine and run:

```python
from fontTools.ttLib import TTFont
TTFont("upstream-wqy-microhei.ttc", fontNumber=0,
       recalcTimestamp=False, recalcBBoxes=False).save("wqy-microhei.ttf")
```

Font conversion is not part of the normal recovery build.
