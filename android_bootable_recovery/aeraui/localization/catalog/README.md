# AERA native translations

`catalog.json` is the source of truth for native AERA UI translations. English
UI text is also the stable lookup tag, which guarantees a readable English
fallback when a locale or individual translation is missing. The catalog uses
a fixed language order for LVGL's translation rows.

The generated C++ file embeds a zlib-compressed UTF-8 string pool and 32-bit
offsets, rather than thousands of relocated string pointers. It is unpacked
once during UI initialization with the zlib already shipped in recovery. The
result and LVGL pointer arrays remain alive for the lifetime of the UI; changing
language or opening a plugin never decompresses the catalog again. Nothing is
loaded from storage or downloaded, so translations remain available before
decryption and without Wi-Fi.

Regenerate the compiled catalog after editing translations:

```sh
python3 aeraui/localization/catalog/generate_catalog.py \
  --recovery . \
  --output aeraui/localization/catalog/catalog_generated.cpp
```

The generator retains all catalog entries, including messages sent by runtime
plugins that have no literal in the recovery source. It also emits a separate
English-only payload for builds without `AERA_EXTRA_LANGUAGES`. The legacy XML
theme tree is no longer packaged; translations belong only in this catalog.

Host API 2 plugins receive the selected locale through `AERA_LOCALE`. Official
plugin metadata may also provide localized `name` and `description` fields in
its manifest. Both mechanisms retain English fallback behavior.
