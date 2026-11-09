#!/usr/bin/env python3
"""Merge the custom wake-word image into the ESP-SR srmodels directory.

esp-sr ``model/movemodel.py`` already copied the models selected in sdkconfig
(WakeNet, VadNet, NSNet) into ``<build>/srmodels`` and packed ``srmodels.bin``.
``wakeword/srmodels.bin`` is a packed WakeNet only. Copying it over the ESP-SR
image drops VadNet and NSNet, so chat AFE cannot create VAD.

This script keeps the ESP-SR VadNet/NSNet directories, replaces WakeNet
directories with the models inside the custom image, and repacks with
esp-sr's ``model/pack_model.py``.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import struct
import sys


NAME_LEN = 32


def _cstring(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def unpack_srmodels(blob: bytes) -> dict[str, dict[str, bytes]]:
    """Inverse of esp-sr model/pack_model.py (model_num + model_info_t table)."""
    if len(blob) < 4:
        raise ValueError("srmodels image is too small")
    (model_num,) = struct.unpack_from("<I", blob, 0)
    offset = 4
    entries: list[tuple[str, list[tuple[str, int, int]]]] = []
    for _ in range(model_num):
        if offset + NAME_LEN + 4 > len(blob):
            raise ValueError("truncated model header")
        model_name = _cstring(blob[offset : offset + NAME_LEN])
        offset += NAME_LEN
        (file_num,) = struct.unpack_from("<I", blob, offset)
        offset += 4
        files: list[tuple[str, int, int]] = []
        for _ in range(file_num):
            if offset + NAME_LEN + 8 > len(blob):
                raise ValueError(f"truncated file header in {model_name}")
            file_name = _cstring(blob[offset : offset + NAME_LEN])
            offset += NAME_LEN
            start, length = struct.unpack_from("<II", blob, offset)
            offset += 8
            files.append((file_name, start, length))
        if not model_name:
            raise ValueError("empty model name in srmodels image")
        entries.append((model_name, files))

    models: dict[str, dict[str, bytes]] = {}
    for model_name, files in entries:
        if model_name in models:
            raise ValueError(f"duplicate model {model_name}")
        file_map: dict[str, bytes] = {}
        for file_name, start, length in files:
            end = start + length
            if start < 0 or length < 0 or end > len(blob):
                raise ValueError(f"{model_name}/{file_name} extent out of range")
            file_map[file_name] = blob[start:end]
        models[model_name] = file_map
    return models


def _load_pack_models(pack_script: str):
    spec = importlib.util.spec_from_file_location("esp_sr_pack_model", pack_script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load pack script {pack_script}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    pack_models = getattr(module, "pack_models", None)
    if pack_models is None:
        raise RuntimeError(f"{pack_script} has no pack_models()")
    return pack_models


def _parse_size(text: str) -> int:
    return int(text, 0)


def merge(srmodels_dir: str, wakeword_bin: str, pack_script: str, max_bytes: int) -> str:
    if not os.path.isdir(srmodels_dir):
        raise SystemExit(f"ESP-SR model dir not found: {srmodels_dir}")
    if not os.path.isfile(wakeword_bin):
        raise SystemExit(f"custom wake word image not found: {wakeword_bin}")
    if not os.path.isfile(pack_script):
        raise SystemExit(f"esp-sr pack_model.py not found: {pack_script}")

    with open(wakeword_bin, "rb") as handle:
        custom_models = unpack_srmodels(handle.read())
    if not custom_models:
        raise SystemExit(f"{wakeword_bin} contains no models")

    removed = []
    for name in list(os.listdir(srmodels_dir)):
        path = os.path.join(srmodels_dir, name)
        if os.path.isdir(path) and name.startswith("wn"):
            shutil.rmtree(path)
            removed.append(name)

    for model_name, files in custom_models.items():
        model_dir = os.path.join(srmodels_dir, model_name)
        if os.path.isdir(model_dir):
            shutil.rmtree(model_dir)
        os.makedirs(model_dir)
        for file_name, data in files.items():
            if os.path.basename(file_name) != file_name or not file_name:
                raise SystemExit(f"refusing unsafe model file name {file_name!r}")
            with open(os.path.join(model_dir, file_name), "wb") as handle:
                handle.write(data)

    packed_name = "srmodels.bin"
    packed_path = os.path.join(srmodels_dir, packed_name)
    if os.path.exists(packed_path):
        os.remove(packed_path)

    pack_models = _load_pack_models(pack_script)
    pack_models(srmodels_dir, packed_name)
    if not os.path.isfile(packed_path):
        raise SystemExit("pack_model.py did not write srmodels.bin")

    with open(packed_path, "rb") as handle:
        packed = handle.read()
    size = len(packed)
    names = sorted(unpack_srmodels(packed))
    print(
        "Merged srmodels.bin: %d bytes, models: %s (replaced wakenet: %s)"
        % (size, ", ".join(names), ", ".join(removed) or "none")
    )
    missing = [
        prefix
        for prefix in ("vadnet", "nsnet", "wn")
        if not any(name.startswith(prefix) for name in names)
    ]
    if missing:
        raise SystemExit(
            "srmodels.bin is missing required model prefix(es): " + ", ".join(missing)
        )
    if max_bytes and size > max_bytes:
        raise SystemExit(
            f"srmodels.bin is {size} bytes, model partition is {max_bytes} bytes"
        )
    return packed_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--srmodels-dir", required=True)
    parser.add_argument("--wakeword", required=True)
    parser.add_argument("--pack-script", required=True)
    parser.add_argument(
        "--max-bytes",
        default="0",
        help="Model partition size in bytes (decimal or 0x hex). 0 skips the check.",
    )
    args = parser.parse_args()
    merge(
        args.srmodels_dir,
        args.wakeword,
        args.pack_script,
        _parse_size(args.max_bytes),
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        sys.exit(0)
