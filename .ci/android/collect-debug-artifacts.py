#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Keep symbols, Java mappings, and build IDs for the exact POCO release APK."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def build_id(readelf, path):
    notes = subprocess.check_output([str(readelf), "--notes", str(path)], text=True)
    match = re.search(r"Build ID: ([0-9a-f]+)", notes)
    if not match:
        raise RuntimeError(f"No native build ID in {path.name}")
    return match.group(1)


def main():
    root = Path(__file__).resolve().parents[2]
    outputs = root / "src/android/app/build/outputs"
    symbols = outputs / "native-debug-symbols/pocoX7ProRelease/native-debug-symbols.zip"
    mapping = outputs / "mapping/pocoX7ProRelease/mapping.txt"
    apk = root / "artifacts/app-pocoX7Pro-release.apk"
    for path in (symbols, mapping, apk):
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"Missing required debug artifact: {path}")

    sdk = Path(os.environ.get("ANDROID_HOME") or os.environ["ANDROID_SDK_ROOT"])
    ndk = sdk / "ndk/29.0.14206865"
    readelf = ndk / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    with tempfile.TemporaryDirectory() as temporary:
        temporary = Path(temporary)
        with zipfile.ZipFile(apk) as package, zipfile.ZipFile(symbols) as metadata:
            abis = {name.split("/")[1] for name in package.namelist()
                    if name.startswith("lib/") and name.endswith(".so")}
            if abis != {"arm64-v8a"}:
                raise RuntimeError(f"Expected an arm64-only APK, found {abis}")
            library_name = "lib/arm64-v8a/libyuzu-android.so"
            candidates = [name for name in metadata.namelist()
                          if name.endswith("/libyuzu-android.so.sym") and "arm64-v8a/" in name]
            if len(candidates) != 1:
                raise RuntimeError(f"Expected one arm64 emulator symbol file, found {candidates}")
            library = temporary / "libyuzu-android.so"
            symbol_file = temporary / "libyuzu-android.so.sym"
            library.write_bytes(package.read(library_name))
            symbol_file.write_bytes(metadata.read(candidates[0]))
        native_id = build_id(readelf, library)
        if native_id != build_id(readelf, symbol_file):
            raise RuntimeError("Native debug symbols do not match the APK build ID")

    destination = root / "artifacts/debug"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(symbols, destination / symbols.name)
    shutil.copy2(mapping, destination / mapping.name)
    manifest = {
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "variant": "pocoX7ProRelease",
        "native_build_id": native_id,
        "apk_sha256": sha256(apk),
        "native_symbols_sha256": sha256(symbols),
        "mapping_sha256": sha256(mapping),
        "symbol_level": "SYMBOL_TABLE",
    }
    (destination / "build-info.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
