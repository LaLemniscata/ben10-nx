#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack the installer's release: its own Python, the translator, the decryption tool, the prebuilt
runtime and libraries, the fonts and the tables. Nothing in it comes from a game. Its source is this
repository at the release's tag.

usage: make_release.py <stage folder> --python-embed <python-*-embed-amd64.zip> --nk-decrypt <nk_decrypt.exe>
                       --launcher <launcher.exe> --bundle <folder from make_bundle.sh> --fonts <folder>
                       --zip <output zip>
Run from the root of the full project, which holds tools/.
"""
import argparse
import modulefinder
import pathlib
import shutil
import subprocess
import sys
import zipfile


def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def tool_modules(root):
    """The translator's own Python files: what codegen.py and prxload.py import from tools/."""
    tools = root / "tools"
    finder = modulefinder.ModuleFinder(path=[str(tools), *sys.path])
    for script in ("codegen.py", "prxload.py"):
        finder.run_script(str(tools / script))
    found = {pathlib.Path(module.__file__).resolve() for module in finder.modules.values() if module.__file__}
    return sorted(path for path in found if tools in path.parents)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("stage", type=pathlib.Path)
    parser.add_argument("--python-embed", type=pathlib.Path, required=True)
    parser.add_argument("--nk-decrypt", type=pathlib.Path, required=True)
    parser.add_argument("--launcher", type=pathlib.Path, required=True)
    parser.add_argument("--bundle", type=pathlib.Path, required=True)
    parser.add_argument("--fonts", type=pathlib.Path, required=True)
    parser.add_argument("--zip", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = pathlib.Path.cwd()
    stage = args.stage
    if stage.exists() and any(stage.iterdir()):
        sys.exit(f"{stage} is not empty")

    with zipfile.ZipFile(args.python_embed) as archive:
        archive.extractall(stage / "python")
    # The embeddable Python's ._pth file would keep a script's own folder off its import path.
    for path in (stage / "python").glob("*._pth"):
        path.unlink()

    copy(args.launcher, stage / "Install Ben 10.exe")
    for name in ("install.py", "ui.html", "title.json", "sr_title_config.h"):
        copy(root / "installer" / name, stage / "installer" / name)
    copy(args.nk_decrypt, stage / "installer" / "nk_decrypt.exe")
    shutil.copytree(args.bundle, stage / "installer" / "bundle")
    shutil.copytree(root / "installer" / "licenses", stage / "installer" / "licenses")
    copy(root / "src/rt/switch/icon/icon.jpg", stage / "installer" / "bundle" / "lib" / "icon.jpg")
    shutil.copytree(args.fonts, stage / "installer" / "payload" / "font")
    shutil.copytree(root / "assets" / "vfpu", stage / "installer" / "payload" / "assets" / "vfpu")

    for path in tool_modules(root):
        copy(path, stage / path.relative_to(root))
    for name in ("LICENSE", "NOTICE.md", "CREDITS.md", "README.md", "installer/THIRD_PARTY_BUILD.txt"):
        copy(root / name, stage / pathlib.Path(name).name)
    listed = subprocess.run(["git", "ls-files", "THIRD_PARTY_LICENSES"], cwd=root, capture_output=True, text=True,
                            check=True).stdout.split("\n")
    for name in listed:
        if name:
            copy(root / name, stage / name)

    files = sorted(path for path in stage.rglob("*") if path.is_file() and "__pycache__" not in path.parts)
    with zipfile.ZipFile(args.zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files:
            archive.write(path, path.relative_to(stage).as_posix())
    print("staged", len(files), "files;", args.zip.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
