#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the installer's release folder: its own Python, the translator, the runtime's source, the
decryption tool, the fonts and the compiled shaders. Nothing in it comes from a game.

usage: make_release.py <stage folder> --python-embed <python-*-embed-amd64.zip> --nk-decrypt <nk_decrypt.exe>
                       --fonts <folder> --vfpu <folder> --shaders <folder of *.dksh.o>
Run from the root of the full project, which holds tools/.
"""
import argparse
import modulefinder
import pathlib
import shutil
import subprocess
import sys
import zipfile

SKIP = ("__pycache__",)


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
    parser.add_argument("--fonts", type=pathlib.Path, required=True)
    parser.add_argument("--vfpu", type=pathlib.Path, required=True)
    parser.add_argument("--shaders", type=pathlib.Path, required=True)
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

    copy(root / "installer" / "Install Ben 10.bat", stage / "Install Ben 10.bat")
    for name in ("install.py", "title.json", "sr_title_config.h"):
        copy(root / "installer" / name, stage / "installer" / name)
    copy(args.nk_decrypt, stage / "installer" / "nk_decrypt.exe")
    for path in args.shaders.glob("*.o"):
        copy(path, stage / "installer" / "shaders" / path.name)
    shutil.copytree(args.fonts, stage / "installer" / "payload" / "font")
    shutil.copytree(args.vfpu, stage / "installer" / "payload" / "assets" / "vfpu")

    for path in tool_modules(root):
        copy(path, stage / path.relative_to(root))
    listed = subprocess.run(["git", "ls-files", "src/rt", "src/core", "third_party/portable-snippets", "assets/vfpu",
                             "THIRD_PARTY_LICENSES", "LICENSE", "NOTICE.md", "CREDITS.md"], cwd=root, capture_output=True,
                            text=True, check=True).stdout.split("\n")
    for name in listed:
        if name and not any(part in SKIP for part in pathlib.PurePosixPath(name).parts):
            copy(root / name, stage / name)
    copy(root / "README.md", stage / "README.md")
    print("staged", sum(1 for path in stage.rglob("*") if path.is_file()), "files")


if __name__ == "__main__":
    main()
