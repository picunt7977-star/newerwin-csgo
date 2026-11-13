#!/usr/bin/env python3
# Достаёт SVG-иконки оружия Panorama из pak01 твоей CS:GO и растеризует их в PNG
# (build/weapon_icons/<имя>.png, имя = класс оружия без "weapon_": ak47, ssg08, knife_karambit...).
# В репозиторий картинки не попадают — это файлы Valve, генерируются локально из игры.
# Нужен rsvg-convert (пакет librsvg).
import os
import struct
import subprocess
import sys
import tempfile

GAME = os.path.expanduser("~/.local/share/Steam/steamapps/common/csgo legacy/csgo")
PREFIX = "materials/panorama/images/icons/equipment/"
HEIGHT = 48     # рисуется ~12–14 px, запас под чёткое сглаживание


def read_dir(path):
    with open(path, "rb") as f:
        _sig, version, tree_size = struct.unpack("<III", f.read(12))
        header = 12 if version == 1 else 28
        f.seek(header)
        data = f.read(tree_size)
    pos = 0

    def string():
        nonlocal pos
        end = data.index(b"\0", pos)
        value = data[pos:end].decode()
        pos = end + 1
        return value

    entries = {}
    while ext := string():
        while directory := string():
            while name := string():
                _crc, preload, archive, offset, length, _term = struct.unpack("<IHHIIH", data[pos:pos + 18])
                pos += 18
                entries[f"{directory}/{name}.{ext}"] = (archive, offset, length, data[pos:pos + preload])
                pos += preload
    return entries, header + tree_size


def read_file(vpk_dir, data_start, entry):
    archive, offset, length, preload = entry
    if length == 0:
        return preload
    if archive == 0x7FFF:
        path, offset = vpk_dir, data_start + offset
    else:
        path = vpk_dir.replace("_dir.vpk", f"_{archive:03d}.vpk")
    with open(path, "rb") as f:
        f.seek(offset)
        return preload + f.read(length)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "build", "weapon_icons")
    vpk = os.path.join(GAME, "pak01_dir.vpk")
    entries, data_start = read_dir(vpk)
    os.makedirs(out, exist_ok=True)
    count = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path, entry in entries.items():
            if not path.startswith(PREFIX) or not path.endswith(".svg"):
                continue
            name = path[len(PREFIX):-4]
            if "/" in name:
                continue
            svg = os.path.join(tmp, name + ".svg")
            with open(svg, "wb") as f:
                f.write(read_file(vpk, data_start, entry))
            subprocess.run(["rsvg-convert", "-h", str(HEIGHT), "-o", os.path.join(out, name + ".png"), svg], check=True)
            count += 1
    print(f"{count} иконок → {out}")


if __name__ == "__main__":
    main()
