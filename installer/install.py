#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the Ben 10: Protector of Earth port for Nintendo Switch from the user's own disc image.

Nothing of the game ships with this program. It reads the user's ISO, decrypts the game's program on
this computer, translates it to C and compiles that with a compiler it downloads once. The rest of
the port comes prebuilt from public source. The result contains code from the game, so it is for this
user's console only.

The window is a page served to the user's browser from this computer alone. Each stage is skipped
when its result already exists, so a run that stops can simply be started again.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import http.server
import json
import os
import secrets
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import webbrowser
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HERE = ROOT / "installer"
BUNDLE = HERE / "bundle"
WORK = ROOT / "work"
OUTPUT = ROOT / "output"
TITLE = json.loads((HERE / "title.json").read_text(encoding="utf-8"))
NAME = TITLE["game_name"]
SD_FOLDER = "switch/ben10-psp"
SECTOR = 2048
COMPILE_MEMORY_BYTES = 700 * 1024 * 1024
UI_PORT = 47123
IDLE_EXIT_SECONDS = 180  # a browser may wake a background tab only once a minute
COPY_BLOCK_BYTES = 4 * 1024 * 1024
MAX_ISO_BYTES = 2 * 1024 * 1024 * 1024  # a PSP disc holds 1.8 GB
FREE_BYTES_NEEDED = 3 * 1024 * 1024 * 1024

TOOLCHAIN_URL = ("https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/"
                 "arm-gnu-toolchain-15.2.rel1-mingw-w64-x86_64-aarch64-none-elf.zip")
TOOLCHAIN_SHA256 = "6f6cfd8732d7f5b5ef62ad0663bfe0ad5b89ee3a123ff3f6feda7d8018dd48c1"
# What building C needs from the archive. Its own C library stays out, so a header missing from the
# bundle stops the build instead of quietly coming from a different library.
TOOLCHAIN_SKIP = ("share/", "data/", "lib/python", "aarch64-none-elf/lib/", "aarch64-none-elf/include/")
TOOLCHAIN_SKIP_NAMES = ("gdb", "lto", "python", "gprof", "objdump", "readelf", "gcov", "f951", "cc1plus", "g++", "c++",
                        "gfortran")

ARCH = ["-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-fPIE", "-mno-outline-atomics"]
WRAPPED = ("open", "close", "dup", "fstat", "fsync", "closedir", "statvfs")

ERRORS = {
    "en": {
        "not_iso": "This file is not a PSP disc image.",
        "wrong_game": "This disc is {found}. The port needs {wanted}, the American edition of the game.",
        "wrong_version": "This copy's game program differs from the supported one. The supported copy is {wanted}, version 1.01. Your file may be a different version, or damaged.",
        "download": "The compiler could not be downloaded from Arm's website. Check your internet connection and try again.",
        "disk": "There is not enough free space on this drive. The installer needs about 4 GB.",
        "receive": "The file could not be read. Try choosing it again.",
        "step": "This step failed. The details are in the log.",
        "sd_space": "The card does not have enough free space: {needed} MB are needed.",
        "sd_write": "The files could not be copied to the card. Check that it is connected and not locked.",
    },
    "pt": {
        "not_iso": "Este arquivo não é uma imagem de disco de PSP.",
        "wrong_game": "Este disco é {found}. O port precisa de {wanted}, a edição americana do jogo.",
        "wrong_version": "O programa do jogo nesta cópia é diferente do suportado. A cópia suportada é {wanted}, versão 1.01. O arquivo pode ser de outra versão ou estar danificado.",
        "download": "Não foi possível baixar o compilador do site da Arm. Confira sua internet e tente de novo.",
        "disk": "Não há espaço livre suficiente nesta unidade. O instalador precisa de cerca de 4 GB.",
        "receive": "Não foi possível ler o arquivo. Tente escolher de novo.",
        "step": "Esta etapa falhou. Os detalhes estão no log.",
        "sd_space": "O cartão não tem espaço livre suficiente: são necessários {needed} MB.",
        "sd_write": "Não foi possível copiar os arquivos para o cartão. Confira se ele está conectado e destravado.",
    },
}


def language():
    if os.name == "nt":
        return "pt" if (ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF) == 0x16 else "en"
    return "pt" if os.environ.get("LANG", "").startswith("pt") else "en"


LANG = language()


class InstallError(Exception):
    """A problem the user can act on: the message is meant for them."""

    def __init__(self, key, **values):
        super().__init__(ERRORS[LANG][key].format(**values))
        self.key = key


class State:
    """What the page shows. Only the fields below change, under the lock."""

    def __init__(self):
        self.lock = threading.Lock()
        self.fields = {"screen": "welcome", "lang": LANG, "iso": "", "iso_name": "", "stage": "", "fraction": 0.0,
                       "detail": "", "error": "", "output": str(OUTPUT), "copy_state": "", "copy_fraction": 0.0,
                       "copy_error": "", "copied_to": ""}
        self.last_seen = time.monotonic()

    def set(self, **values):
        with self.lock:
            self.fields.update(values)

    def snapshot(self):
        with self.lock:
            self.last_seen = time.monotonic()
            return dict(self.fields)


STATE = State()
CLI_PRINT = False


def log(text):
    WORK.mkdir(parents=True, exist_ok=True)
    with open(WORK / "install.log", "a", encoding="utf-8", errors="replace") as handle:
        handle.write(text.rstrip("\n") + "\n")


def progress(stage, fraction=0.0, detail=""):
    STATE.set(stage=stage, fraction=fraction, detail=detail)
    if CLI_PRINT:
        print(f"[{stage}] {fraction * 100:5.1f}% {detail}", flush=True)


def run(command, env=None):
    log("$ " + " ".join(str(part) for part in command))
    # Below normal priority keeps the computer usable while it compiles.
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS if os.name == "nt" else 0
    done = subprocess.run([str(part) for part in command], cwd=ROOT, env=env, capture_output=True, text=True,
                          errors="replace", creationflags=flags, stdin=subprocess.DEVNULL)
    if done.stdout or done.stderr:
        log(done.stdout + done.stderr)
    if done.returncode != 0:
        log(f"exit code {done.returncode}")
        raise InstallError("step")
    return done.stdout


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


# ---- the disc -------------------------------------------------------------------------------------


class Iso:
    """Just enough ISO 9660 to read named files from a PSP disc."""

    def __init__(self, path):
        self.handle = open(path, "rb")
        self.handle.seek(16 * SECTOR)
        descriptor = self.handle.read(SECTOR)
        if descriptor[1:6] != b"CD001":
            self.handle.close()
            raise ValueError("no ISO 9660 volume")
        self.root = self._record(descriptor[156:190])

    @staticmethod
    def _record(data):
        return struct.unpack_from("<I", data, 2)[0], struct.unpack_from("<I", data, 10)[0]

    def _entries(self, extent, size):
        self.handle.seek(extent * SECTOR)
        data = self.handle.read(size)
        offset = 0
        while offset < len(data):
            length = data[offset]
            if length == 0:
                offset = (offset // SECTOR + 1) * SECTOR
                continue
            record = data[offset:offset + length]
            yield record[33:33 + record[32]].decode("ascii", "replace").split(";")[0].upper(), self._record(record)
            offset += length

    def read(self, disc_path):
        extent, size = self.root
        for part in disc_path.upper().split("/"):
            for name, record in self._entries(extent, size):
                if name == part:
                    extent, size = record
                    break
            else:
                raise FileNotFoundError(disc_path)
        self.handle.seek(extent * SECTOR)
        return self.handle.read(size)


def disc_id(iso):
    data = iso.read("PSP_GAME/PARAM.SFO")
    key_table, value_table, count = struct.unpack_from("<III", data, 8)
    for index in range(count):
        key_offset, _, _, _, value_offset = struct.unpack_from("<HHIII", data, 20 + index * 16)
        if data[key_table + key_offset:].split(b"\0", 1)[0] == b"DISC_ID":
            return data[value_table + value_offset:].split(b"\0", 1)[0].decode("ascii", "replace")
    raise ValueError("no DISC_ID")


def check_iso(path):
    iso = None
    try:
        iso = Iso(path)
        found = disc_id(iso)
    except (OSError, ValueError, struct.error) as error:
        if iso:
            iso.handle.close()
        log(f"not a disc image: {error}")
        raise InstallError("not_iso") from error
    if found != TITLE["disc_id"]:
        iso.handle.close()
        raise InstallError("wrong_game", found=found, wanted=TITLE["disc_id"])
    return iso


def plain_module(iso, disc_path, destination, expected):
    """One program file from the disc, decrypted when it needs to be, checked against the known copy."""
    if destination.exists() and sha256(destination) == expected:
        return
    raw = destination.with_name(destination.name + ".raw")
    raw.write_bytes(iso.read(disc_path))
    if raw.read_bytes()[:4] == b"\x7fELF":
        shutil.copyfile(raw, destination)
    else:
        run([HERE / "nk_decrypt.exe", "decrypt", "--in", raw, "--out", destination])
    raw.unlink()
    if sha256(destination) != expected:
        raise InstallError("wrong_version", wanted=TITLE["disc_id"])


def stage_extract(iso):
    folder = WORK / "input"
    folder.mkdir(parents=True, exist_ok=True)
    progress("decrypt", 0.0)
    plain_module(iso, "PSP_GAME/SYSDIR/EBOOT.BIN", folder / "selected.elf", TITLE["executable_sha256"])
    for index, module in enumerate(TITLE["modules"]):
        plain_module(iso, module["disc_path"], folder / module["name"], module["sha256"])
        progress("decrypt", (index + 1) / len(TITLE["modules"]))


# ---- the compiler ---------------------------------------------------------------------------------


def toolchain_path():
    return WORK / "tc"


def tool(name):
    return toolchain_path() / "bin" / f"aarch64-none-elf-{name}.exe"


def download_toolchain(partial):
    request = urllib.request.Request(TOOLCHAIN_URL, headers={"User-Agent": "Ben10-NX-Installer"})
    try:
        with urllib.request.urlopen(request, context=ssl.create_default_context(), timeout=60) as reply, \
                open(partial, "wb") as out:
            total = int(reply.headers.get("Content-Length") or 0)
            received = 0
            while block := reply.read(1 << 20):
                out.write(block)
                received += len(block)
                if total:
                    progress("compiler", 0.85 * received / total, f"{received >> 20} / {total >> 20} MB")
    except OSError as error:
        log(f"download failed: {error}")
        # A new PC may not hold the root certificate yet, which Windows' own curl fetches by itself.
        # The checksum decides whether the file is good, so this takes no trust away.
        try:
            run(["curl.exe", "-fsSL", "--retry", "3", "-o", partial, TOOLCHAIN_URL])
        except (InstallError, OSError) as second:
            raise InstallError("download") from second


def stage_toolchain():
    if tool("gcc").exists() and tool("ld").exists() and (toolchain_path() / ".complete").exists():
        return
    progress("compiler", 0.0)
    archive = WORK / "toolchain.zip"
    if not (archive.exists() and sha256(archive) == TOOLCHAIN_SHA256):
        partial = archive.with_suffix(".part")
        download_toolchain(partial)
        if sha256(partial) != TOOLCHAIN_SHA256:
            partial.unlink()
            raise InstallError("download")
        partial.replace(archive)
    target = toolchain_path()
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        for index, name in enumerate(names):
            leaf = name.rsplit("/", 1)[-1]
            if name.endswith("/") or name.startswith(TOOLCHAIN_SKIP) or \
                    any(word in leaf for word in TOOLCHAIN_SKIP_NAMES):
                continue
            destination = target / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(name) as source, open(destination, "wb") as out:
                shutil.copyfileobj(source, out)
            if index % 40 == 0:
                progress("compiler", 0.85 + 0.15 * index / len(names))
    (target / ".complete").write_text("ok\n", encoding="ascii")
    archive.unlink()


def stage_translate():
    progress("translate", 0.05)
    package = WORK / "package"
    package.mkdir(parents=True, exist_ok=True)
    image = package / f"{NAME}_image.bin"
    done = package / ".translated"
    if done.exists() and image.exists():
        return
    elf = WORK / "input" / "selected.elf"
    env = dict(os.environ, GAME_ELF=str(elf), PYTHONUTF8="1", PYTHONPATH=str(ROOT / "tools"))
    python = Path(sys.executable).with_name("python.exe") if os.name == "nt" else Path(sys.executable)
    run([python, "tools/prxload.py", "--env-elf", TITLE["base"], f"--out={image}"], env=env)
    if sha256(image) != TITLE["image_sha256"]:
        raise InstallError("wrong_version", wanted=TITLE["disc_id"])
    progress("translate", 0.2)
    extras = [f"--extra-elf={WORK / 'input' / module['name']}@runtime" for module in TITLE["codegen_modules"]]
    run([python, "tools/codegen.py", elf, package / f"{NAME}_recomp.c", f"--base={TITLE['base']}",
         "--funcs-per-chunk=2000", "--target-chunk-bytes=4000000", "--promote-registers", *extras], env=env)
    shutil.copyfile(HERE / "sr_title_config.h", package / "sr_title_config.h")
    done.write_text("ok\n", encoding="ascii")


def available_memory():
    class Status(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("available", ctypes.c_ulonglong), ("page_total", ctypes.c_ulonglong),
                    ("page_available", ctypes.c_ulonglong), ("virtual_total", ctypes.c_ulonglong),
                    ("virtual_available", ctypes.c_ulonglong), ("extended", ctypes.c_ulonglong)]
    status = Status()
    status.length = ctypes.sizeof(Status)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return status.available


def stage_compile():
    progress("compile", 0.0)
    package = WORK / "package"
    objects = WORK / "obj"
    objects.mkdir(parents=True, exist_ok=True)
    sources = sorted(package.glob("*.c"), key=lambda path: -path.stat().st_size)
    src = "installer/bundle/include/src"
    # The C library's headers change with these two, which devkitA64 defines by itself.
    flags = ARCH + ["-ffunction-sections", "-D__SWITCH__", "-D__DEVKITA64__", "-D__DEVKITPRO__", "-w",
                    "-fno-strict-aliasing", "-fno-var-tracking",
                    "-ffp-contract=off", "-O2", "-DSR_FLIGHT_RECORDER_LINKED", "-include",
                    f"{src}/src/rt/switch/nk_switch_compat.h", "-Iwork/package", f"-I{src}/src/rt/switch/sdl_shim",
                    f"-I{src}/src/rt", "-isystem", "installer/bundle/include/sys"]
    workers = max(1, min(os.cpu_count() or 1, available_memory() // COMPILE_MEMORY_BYTES))
    log(f"compiling {len(sources)} files with {workers} workers")

    def one(source):
        target = objects / (source.name + ".o")
        if target.exists():
            return target
        partial = target.with_suffix(".tmp")
        run([tool("gcc"), *flags, "-c", source.relative_to(ROOT).as_posix(), "-o", partial.relative_to(ROOT).as_posix()])
        partial.replace(target)
        return target

    # The work is the bytes of C, so the bar follows those and not the count of files.
    total = sum(path.stat().st_size for path in sources)
    finished = 0
    built = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(one, source): source for source in sources}
        try:
            for future in concurrent.futures.as_completed(futures):
                built.append(future.result())
                finished += futures[future].stat().st_size
                progress("compile", finished / total, f"{len(built)} / {len(sources)}")
        except BaseException:
            # One failure is enough: the files still waiting would only fail the same way.
            pool.shutdown(wait=False, cancel_futures=True)
            raise
    return sorted(built)


def stage_link(objects):
    progress("link", 0.1)
    lib = "installer/bundle/lib"
    response = WORK / "objects.rsp"
    response.write_text("\n".join(path.relative_to(ROOT).as_posix() for path in objects) + "\n", encoding="utf-8")
    elf = "work/ben10-psp.elf"
    wraps = [f"--wrap={name}" for name in WRAPPED]
    run([tool("ld"), "-EL", "-X", "-maarch64elf", "-T", f"{lib}/switch.ld", "-pie", "--no-dynamic-linker",
         "--spare-dynamic-tags=0", "--gc-sections", "-z", "text", "-z", "now", "-z", "pack-relative-relocs",
         "--build-id=sha1", *wraps, "-o", elf, f"-L{lib}", f"{lib}/crti.o",
         f"{lib}/crtbegin.o", "--require-defined=main", f"@{response.relative_to(ROOT).as_posix()}", "--whole-archive",
         f"{lib}/libruntime.a", "--no-whole-archive", "-ldeko3d", "-lnx", "-lm", "--start-group", "-lgcc", "-lc",
         "-lsysbase", "--end-group", f"{lib}/crtend.o", f"{lib}/crtn.o"])
    progress("link", 0.7)
    nro = WORK / "ben10-psp.nro"
    make_nro(ROOT / elf, nro, BUNDLE / "lib" / "icon.jpg", BUNDLE / "lib" / "ben10-psp.nacp")
    return nro


def make_nro(elf_path, nro_path, icon_path, nacp_path):
    """A port of elf2nro from switch-tools (Copyright 2017 plutoo, ISC licence: licenses/switch-tools.txt).

    Its output for the same ELF, icon and NACP is byte-identical to that tool's.
    """
    elf = elf_path.read_bytes()
    phoff, shoff = struct.unpack_from("<QQ", elf, 32)
    shentsize, shnum = struct.unpack_from("<HH", elf, 58)
    phnum = struct.unpack_from("<H", elf, 56)[0]
    PT_LOAD = 1
    loads = []
    for index in range(phnum):
        kind, _, offset, vaddr, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", elf, phoff + index * 56)
        if kind == PT_LOAD:
            loads.append((offset, vaddr, filesz, memsz))
    page = lambda size: (size + 0xFFF) & ~0xFFF
    segments = loads[:3]
    last = loads[2]
    # The .bss is whatever the data segment takes in memory past its bytes in the file.
    bss = page(last[3] - page(last[2])) if last[3] > page(last[2]) else 0
    build_id = b""
    for index in range(shnum):
        header = shoff + index * shentsize
        kind = struct.unpack_from("<I", elf, header + 4)[0]
        offset = struct.unpack_from("<Q", elf, header + 24)[0]
        if kind == 7:  # a note
            namesz, descsz, note = struct.unpack_from("<III", elf, offset)
            if note == 3 and namesz == 4 and elf[offset + 12:offset + 16] == b"GNU\0":
                build_id = elf[offset + 16:offset + 16 + min(descsz, 0x20)]
    file_size = 0
    image = bytearray()
    table = b""
    for offset, vaddr, filesz, _ in segments:
        size = page(filesz)
        if len(image) < vaddr + size:
            image.extend(bytes(vaddr + size - len(image)))
        image[vaddr:vaddr + filesz] = elf[offset:offset + filesz]
        table += struct.pack("<II", vaddr, size)
        file_size = page(file_size + size)
    header = b"NRO0" + struct.pack("<III", 0, file_size, 0) + table + struct.pack("<II", bss, 0) + \
        build_id.ljust(0x20, b"\0") + bytes(0x20)
    image[0x10:0x10 + len(header)] = header
    icon = icon_path.read_bytes()
    nacp = nacp_path.read_bytes()
    assets_at = file_size
    if len(image) < assets_at:
        image.extend(bytes(assets_at - len(image)))
    del image[assets_at:]
    icon_at = 0x38
    assets = b"ASET" + struct.pack("<I", 0) + struct.pack("<QQ", icon_at, len(icon)) + \
        struct.pack("<QQ", icon_at + len(icon), len(nacp)) + struct.pack("<QQ", 0, 0)
    nro_path.write_bytes(bytes(image) + assets + icon + nacp)


# ---- the SD card ----------------------------------------------------------------------------------


def linked_or_copied(source, destination):
    """The output folder's game image needs no second copy of the data when the drive allows a link."""
    if destination.exists():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def payload_files():
    base = HERE / "payload"
    return [(path, path.relative_to(base).as_posix()) for path in sorted(base.rglob("*")) if path.is_file()]


def stage_assemble(nro, iso_path):
    progress("ready", 0.3)
    folder = OUTPUT / SD_FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(nro, folder / "ben10-psp.nro")
    shutil.copyfile(WORK / "package" / f"{NAME}_image.bin", folder / f"{NAME}_image.bin")
    for path, relative in payload_files():
        target = folder / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
    linked_or_copied(iso_path, folder / "game.iso")


def clean_work():
    """The translated game and the compiler are no longer needed, and the first must not linger."""
    for name in ("input", "package", "obj", "tc"):
        shutil.rmtree(WORK / name, ignore_errors=True)
    # The output folder's game.iso is a link to or a copy of work/game.iso, so it stays.
    for name in ("ben10-psp.elf", "ben10-psp.nro", "objects.rsp", "game.iso", "game.iso.part"):
        (WORK / name).unlink(missing_ok=True)


def removable_drives():
    if os.name != "nt":
        return []
    drives = []
    kernel = ctypes.windll.kernel32
    mask = kernel.GetLogicalDrives()
    for index in range(26):
        if not mask & (1 << index):
            continue
        root = f"{chr(65 + index)}:\\"
        # Some card readers report a fixed drive, so a drive holding Atmosphère counts as well.
        kind = kernel.GetDriveTypeW(root)
        if kind != 2 and not (kind == 3 and Path(root, "atmosphere").exists()):
            continue
        try:
            free = shutil.disk_usage(root).free
        except OSError:
            continue
        label = ctypes.create_unicode_buffer(64)
        kernel.GetVolumeInformationW(root, label, 64, None, None, None, None, 0)
        drives.append({"letter": chr(65 + index), "label": label.value, "free_gb": round(free / (1 << 30), 1),
                       "switch": Path(root, "atmosphere").exists() or Path(root, "switch").exists()})
    return sorted(drives, key=lambda drive: not drive["switch"])


def copy_to_card(letter):
    STATE.set(copy_state="copying", copy_fraction=0.0, copy_error="", copied_to="")
    source = OUTPUT / SD_FOLDER
    target = Path(f"{letter}:/") / SD_FOLDER
    files = [path for path in sorted(source.rglob("*")) if path.is_file()]
    try:
        needed = sum(path.stat().st_size for path in files)
        present = sum((target / path.relative_to(source)).stat().st_size for path in files
                      if (target / path.relative_to(source)).exists())
        if shutil.disk_usage(f"{letter}:/").free + present < needed:
            raise InstallError("sd_space", needed=needed >> 20)
        done = 0
        for path in files:
            destination = target / path.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            size = path.stat().st_size
            if destination.exists() and destination.stat().st_size == size and path.name == "game.iso":
                done += size
                continue
            partial = destination.with_name(destination.name + ".partial")
            with open(path, "rb") as reader, open(partial, "wb") as writer:
                while block := reader.read(COPY_BLOCK_BYTES):
                    writer.write(block)
                    done += len(block)
                    STATE.set(copy_fraction=done / needed)
            partial.replace(destination)
            if destination.stat().st_size != size:
                raise InstallError("sd_write")
        STATE.set(copy_state="done", copy_fraction=1.0, copied_to=str(target))
    except InstallError as error:
        STATE.set(copy_state="failed", copy_error=str(error))
    except OSError as error:
        log(f"copy failed: {error}")
        STATE.set(copy_state="failed", copy_error=ERRORS[LANG]["sd_write"])


# ---- the run --------------------------------------------------------------------------------------


def pipeline(iso_path, keep_work=False):
    # The computer must not go to sleep in the middle of a long build; this ends with this thread.
    if os.name == "nt":
        ctypes.windll.kernel32.SetThreadExecutionState(0x80000001)  # ES_CONTINUOUS | ES_SYSTEM_REQUIRED
    try:
        if shutil.disk_usage(ROOT).free < 3 * (1 << 30) and not (WORK / "obj").exists():
            raise InstallError("disk")
        iso = check_iso(iso_path)
        stage_toolchain()
        stage_extract(iso)
        iso.handle.close()
        stage_translate()
        objects = stage_compile()
        nro = stage_link(objects)
        stage_assemble(nro, Path(iso_path))
        if not keep_work:
            clean_work()
        progress("ready", 1.0)
        STATE.set(screen="done")
    except InstallError as error:
        log(f"stopped: {error}")
        if error.key in ("not_iso", "wrong_game", "wrong_version"):
            # Trying this disc again cannot help, so the page goes back to choosing one.
            STATE.set(screen="welcome", iso="", iso_name="", error=str(error))
        else:
            STATE.set(screen="failed", error=str(error))
    except Exception as error:  # the page must always show that the run stopped
        log(f"unexpected: {error!r}")
        STATE.set(screen="failed", error=ERRORS[LANG]["step"])


def receive_iso(stream, length):
    """Take the disc image the page hands over, from the browser's own file dialog, into work/.

    A web page never learns a file's path, so the image comes through this computer's local connection.
    """
    if not 0 < length <= MAX_ISO_BYTES:
        raise InstallError("not_iso")
    if shutil.disk_usage(ROOT).free < length + FREE_BYTES_NEEDED:
        raise InstallError("disk")
    WORK.mkdir(parents=True, exist_ok=True)
    partial = WORK / "game.iso.part"
    left = length
    with open(partial, "wb") as out:
        while left:
            block = stream.read(min(left, COPY_BLOCK_BYTES))
            if not block:
                raise OSError("the page stopped sending")
            out.write(block)
            left -= len(block)
    try:
        check_iso(partial).handle.close()
    except InstallError:
        partial.unlink()
        raise
    received = WORK / "game.iso"
    partial.replace(received)
    return received


RECEIVING = threading.Lock()


class Page(http.server.BaseHTTPRequestHandler):
    token = ""
    keep_work = False

    def log_message(self, format, *args):
        return

    def _send(self, body, kind="application/json", status=200):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _allowed(self, query):
        # Only this page: another site open in the browser can reach the port but not the token.
        return self.headers.get("Host", "") == f"127.0.0.1:{UI_PORT}" and query.get("t", [""])[0] == self.token

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        if not self._allowed(query):
            self._send(b"{}", status=403)
        elif url.path == "/":
            self._send((HERE / "ui.html").read_bytes(), "text/html; charset=utf-8")
        elif url.path == "/cover.jpg":
            self._send((BUNDLE / "lib" / "icon.jpg").read_bytes(), "image/jpeg")
        elif url.path == "/state":
            snapshot = STATE.snapshot()
            if snapshot["screen"] == "done":
                snapshot["drives"] = removable_drives()
            self._send(json.dumps(snapshot).encode("utf-8"))
        else:
            self._send(b"{}", status=404)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        if not self._allowed(query):
            self._send(b"{}", status=403)
            return
        snapshot = STATE.snapshot()
        if url.path == "/upload" and snapshot["screen"] == "welcome":
            if RECEIVING.acquire(blocking=False):
                STATE.set(iso="", iso_name="", error="")
                try:
                    length = int(self.headers.get("Content-Length") or 0)
                    received = receive_iso(self.rfile, length)
                    name = Path(query.get("name", ["game.iso"])[0]).name[:200]
                    STATE.set(iso=str(received), iso_name=name)
                except InstallError as error:
                    STATE.set(error=str(error))
                except (OSError, ValueError) as error:
                    log(f"receiving the disc image failed: {error}")
                    STATE.set(error=ERRORS[LANG]["receive"])
                finally:
                    RECEIVING.release()
        elif url.path == "/start" and snapshot["iso"] and snapshot["screen"] in ("welcome", "failed"):
            STATE.set(screen="working", error="", stage="compiler", fraction=0.0, detail="")
            threading.Thread(target=pipeline, args=(snapshot["iso"], self.keep_work), daemon=True).start()
        elif url.path == "/copy" and snapshot["screen"] == "done" and snapshot["copy_state"] != "copying":
            letter = query.get("drive", [""])[0][:1].upper()
            if letter.isalpha():
                threading.Thread(target=copy_to_card, args=(letter,), daemon=True).start()
        elif url.path == "/folder" and snapshot["screen"] == "done":
            os.startfile(OUTPUT)
        elif url.path == "/log":
            if (WORK / "install.log").exists():
                os.startfile(WORK / "install.log")
        self._send(b"{}")


def run_window(keep_work):
    token_file = WORK / "window.token"
    WORK.mkdir(parents=True, exist_ok=True)
    # On Windows, address reuse would let a second copy bind the same port instead of finding the first.
    http.server.ThreadingHTTPServer.allow_reuse_address = False
    try:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", UI_PORT), Page)
    except OSError:
        # Already running: bring its page back.
        if token_file.exists():
            webbrowser.open(f"http://127.0.0.1:{UI_PORT}/?t={token_file.read_text(encoding='ascii').strip()}")
        return
    Page.token = secrets.token_urlsafe(24)
    Page.keep_work = keep_work
    token_file.write_text(Page.token, encoding="ascii")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    webbrowser.open(f"http://127.0.0.1:{UI_PORT}/?t={Page.token}")
    # The program ends once the page has been closed for a while and nothing is running.
    while True:
        time.sleep(5)
        with STATE.lock:
            idle = time.monotonic() - STATE.last_seen
            busy = STATE.fields["screen"] == "working" or STATE.fields["copy_state"] == "copying"
        if idle > IDLE_EXIT_SECONDS and not busy:
            break
    token_file.unlink(missing_ok=True)
    server.shutdown()


def main():
    global CLI_PRINT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", help="for testing: run without the window, on this disc image")
    parser.add_argument("--keep-work", action="store_true", help="for testing: keep the intermediate files")
    parser.add_argument("--stop-after", choices=["translate"], help="for testing: stop after this stage")
    args = parser.parse_args()
    if os.name == "nt":
        # Looking at an empty card reader must not bring up Windows' "insert a disk" message.
        ctypes.windll.kernel32.SetErrorMode(1)
    if not args.iso:
        run_window(args.keep_work)
        return
    CLI_PRINT = True
    if args.stop_after:
        iso = check_iso(args.iso)
        stage_extract(iso)
        stage_translate()
        return
    pipeline(args.iso, args.keep_work)
    final = STATE.snapshot()
    print(final["screen"], final["error"], flush=True)
    sys.exit(0 if final["screen"] == "done" else 1)


if __name__ == "__main__":
    main()
