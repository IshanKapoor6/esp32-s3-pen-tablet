from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "wallpaper_still.jpg"
OUTPUT = ROOT / "wallpaper_image.h"


def main():
    jpeg = SOURCE.read_bytes()
    if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
        raise SystemExit(f"{SOURCE.name} is not a complete JPEG file")

    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "static const uint8_t wallpaperJpg[] PROGMEM = {",
    ]
    for start in range(0, len(jpeg), 16):
        chunk = jpeg[start : start + 16]
        lines.append("  " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")
    lines.extend(
        [
            "};",
            "static constexpr uint32_t wallpaperJpgLength = sizeof(wallpaperJpg);",
            "",
        ]
    )
    OUTPUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"Embedded {len(jpeg):,} JPEG bytes in {OUTPUT.name}")


if __name__ == "__main__":
    main()
