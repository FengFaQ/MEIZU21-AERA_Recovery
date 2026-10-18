#!/usr/bin/env python3
"""Generate the compact native AERA catalog."""

import argparse
import ast
import json
import pathlib
import re
import struct
import zlib


LOCALES = [
    "en", "ar_SA", "bg_BG", "bn_BD", "ca_ES", "cs_CZ", "de_DE",
    "el_GR", "es-ES", "fa_IR", "fr_FR", "he_IL", "hi_IN", "hu",
    "id_ID", "it_IT", "ja_JP", "ko_KR", "nl_NL", "no_NO", "pl_PL",
    "pt_BR", "pt_PT", "ro_RO", "ru", "sr_Cyrl", "sv_SE", "th_TH",
    "tr_TR", "uk_UA", "vi_VN", "zh_CN", "zh_TW",
]


def source_literals(aeraui):
    values = set()
    for path in aeraui.rglob("*"):
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        if "third_party" in path.parts or path.name == "catalog_generated.cpp":
            continue
        source = path.read_text(encoding="utf-8", errors="ignore")
        sequences = re.findall(r'(?:"(?:\\.|[^"\\])*"\s*)+', source)
        for sequence in sequences:
            try:
                value = "".join(
                    ast.literal_eval(token)
                    for token in re.findall(r'"(?:\\.|[^"\\])*"', sequence)
                ).strip()
            except (SyntaxError, ValueError):
                continue
            if value and any(character.isalpha() for character in value):
                values.add(value)
    return values


def pack_catalog(rows, keys, locales):
    pool = bytearray()
    offsets = {}

    def intern(value):
        if "\0" in value:
            raise ValueError("catalog strings must not contain embedded NUL bytes")
        if value not in offsets:
            offsets[value] = len(pool)
            pool.extend(value.encode("utf-8") + b"\0")
        return offsets[value]

    languages = [intern(locale) for locale in locales]
    tags = [intern(key) for key in keys]
    # Group the pool by language for compression; LVGL's table stays row-major.
    translations = {
        locale: [intern(key if locale == "en" else rows[key].get(locale, key) or key)
                 for key in keys]
        for locale in locales
    }
    table = languages + tags + [
        translations[locale][i] for i in range(len(keys)) for locale in locales
    ]
    header = b"AERACAT1" + struct.pack("<III", len(locales), len(keys), len(pool))
    raw = header + struct.pack(f"<{len(table)}I", *table) + pool
    return zlib.compress(raw, 9), len(raw)


def emit_catalog(rows, keys, locales):
    data, expanded_size = pack_catalog(rows, keys, locales)
    lines = ["constexpr unsigned char kData[] = {"]
    for i in range(0, len(data), 20):
        lines.append("    " + ",".join(f"0x{byte:02x}" for byte in data[i:i + 20]) + ",")
    lines.extend(["};", f"constexpr size_t kExpandedSize = {expanded_size};"])
    return lines, len(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recovery", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument(
        "--catalog",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("catalog.json"),
    )
    args = parser.parse_args()

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    if catalog.get("schema") != 1 or catalog.get("languages") != LOCALES:
        raise ValueError("catalog.json has an unsupported schema or language order")
    rows = catalog.get("strings", {})
    # catalog.json is already generated from the visible recovery surface plus
    # runtime-plugin messages that arrive over the host protocol. Those plugin
    # strings intentionally have no literal in this repository.
    keys = sorted(rows)

    multilingual, multilingual_size = emit_catalog(rows, keys, LOCALES)
    english, english_size = emit_catalog(rows, keys, ["en"])
    lines = [
        "/*",
        " * Generated from aeraui/localization/catalog/catalog.json.",
        " * Do not edit manually; run localization/catalog/generate_catalog.py.",
        " */",
        '#include "catalog.hpp"',
        "",
        "namespace aeraui::i18n {",
        "namespace {",
        "#ifdef AERA_EXTRA_LANGUAGES",
    ]
    lines.extend(multilingual)
    lines.append("#else")
    lines.extend(english)
    lines.extend([
        "#endif",
        "}  // namespace",
        "",
        "PackedCatalog GetPackedCatalog() {",
        "  return {kData, sizeof(kData), kExpandedSize};",
        "}",
        "",
        "}  // namespace aeraui::i18n",
        "",
    ])
    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(f"Generated {len(keys)} tags: {multilingual_size} packed bytes for "
          f"{len(LOCALES)} languages; {english_size} bytes for English-only builds.")


if __name__ == "__main__":
    main()
