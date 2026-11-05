#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the Ben 10: Protector of Earth port for Nintendo Switch from the user's own disc image.

Nothing of the game ships with this program. It reads the user's ISO, decrypts the game's program on
this computer, translates it to C and compiles it with devkitPro. The result contains code from the
game, so it is for this user's console only.

The work is split into stages, and each one is skipped when its result already exists, so a run that
stops can simply be started again.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"
OUTPUT = ROOT / "output"
TITLE = json.loads((ROOT / "installer" / "title.json").read_text(encoding="utf-8"))
NAME = TITLE["game_name"]
SD_FOLDER = "switch/ben10-psp"
DEVKITPRO_INSTALLER_PAGE = "https://github.com/devkitPro/installer/releases/latest"
SECTOR = 2048
COMPILE_MEMORY_BYTES = 700 * 1024 * 1024

ARCH = ["-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE"]
COMPAT = ["-include", "src/rt/switch/nk_switch_compat.h"]
COMMON = ARCH + ["-ffunction-sections", "-D__SWITCH__", "-fno-strict-aliasing", "-w"] + COMPAT
DEFINES = ["-DSR_FLIGHT_RECORDER_LINKED", "-DPERF_AOT_INSTRUCTIONS=0", "-DSR_PUBLIC_SAFE", "-DSR_H264_FFMPEG",
           "-DSR_CORO_BACKEND_ASM"]
LINK_WRAPS = "-Wl,--wrap=open,--wrap=close,--wrap=dup,--wrap=fstat,--wrap=fsync,--wrap=closedir,--wrap=statvfs"

MESSAGES = {
    "en": {
        "banner": "Ben 10: Protector of Earth for Nintendo Switch: installer",
        "notice": "Nothing of the game comes with this program. It builds the port from your own disc image.\n"
                  "The result contains code from the game: it is for your own console, so please do not share it.",
        "pick": "Choose your disc image (.iso) in the window that opened...",
        "no_iso": "No disc image was chosen. Start again when you have it.",
        "check_iso": "Checking your disc image",
        "not_iso": "This file is not a PSP disc image: {detail}",
        "wrong_game": "This disc is {found}. This port needs {wanted}, the American edition of the game.",
        "wrong_version": "This copy's game program differs from the supported one, so the port cannot use it.\n"
                          "The supported copy is {wanted}, version 1.01. Your file may be a different version or damaged.",
        "need_devkitpro": "\nThe compiler is missing. devkitPro makes it, and this one step needs you:\n"
                          "  1. The devkitPro download page is opening. Download devkitProUpdater and run it.\n"
                          "  2. Accept the suggested folder, C:\\devkitPro, and tick Switch Development.\n"
                          "  3. Come back here when it finishes.",
        "press_enter": "Press Enter to continue...",
        "still_no_devkitpro": "devkitPro was not found in {path}. Install it and start this program again.",
        "packages": "Adding the Switch packages to devkitPro",
        "packages_failed": "devkitPro could not install its Switch packages. See the log, then try again.",
        "extract": "Decrypting the game's program from your disc",
        "translate": "Translating the game to C (a few minutes)",
        "compile": "Compiling for the Switch (5 to 30 minutes: your computer will be busy)",
        "progress": "  {done} of {total} files",
        "link": "Making the Switch program",
        "assemble": "Preparing the folder for your SD card",
        "done": "\nDone. Your folder is ready:\n  {path}\n\nCopy the \"switch\" folder inside it to the root of your SD card.",
        "drive": "\nIs your SD card connected? Type its drive letter to copy it now, or press Enter to skip: ",
        "copying": "Copying to {path} (the game image takes a few minutes)",
        "copied": "Copied. Your saves were not touched.",
        "bad_drive": "Could not copy to that drive: {detail}",
        "launch": "\nOn the Switch: hold R while starting any installed game, then open \"Ben 10: Protector of Earth\".\n"
                  "To quit the game, press + and - together.",
        "failed": "\nThis step failed: {stage}\nThe details are in {log}",
    },
    "pt": {
        "banner": "Ben 10: Protector of Earth para Nintendo Switch: instalador",
        "notice": "Nada do jogo vem com este programa. Ele monta o port a partir da sua própria imagem do disco.\n"
                  "O resultado contém código do jogo: é para o seu console, então não compartilhe.",
        "pick": "Escolha a imagem do seu disco (.iso) na janela que abriu...",
        "no_iso": "Nenhuma imagem foi escolhida. Comece de novo quando tiver o arquivo.",
        "check_iso": "Conferindo a imagem do disco",
        "not_iso": "Este arquivo não é uma imagem de disco de PSP: {detail}",
        "wrong_game": "Este disco é {found}. O port precisa de {wanted}, a edição americana do jogo.",
        "wrong_version": "O programa do jogo nesta cópia é diferente do suportado, então o port não pode usá-lo.\n"
                          "A cópia suportada é {wanted}, versão 1.01. O arquivo pode ser de outra versão ou estar danificado.",
        "need_devkitpro": "\nFalta o compilador. Ele vem do devkitPro, e esta é a única etapa que precisa de você:\n"
                          "  1. A página de download do devkitPro está abrindo. Baixe o devkitProUpdater e execute.\n"
                          "  2. Aceite a pasta sugerida, C:\\devkitPro, e marque Switch Development.\n"
                          "  3. Volte aqui quando terminar.",
        "press_enter": "Aperte Enter para continuar...",
        "still_no_devkitpro": "O devkitPro não foi encontrado em {path}. Instale e abra este programa de novo.",
        "packages": "Adicionando os pacotes do Switch ao devkitPro",
        "packages_failed": "O devkitPro não conseguiu instalar os pacotes do Switch. Veja o log e tente de novo.",
        "extract": "Decifrando o programa do jogo do seu disco",
        "translate": "Traduzindo o jogo para C (alguns minutos)",
        "compile": "Compilando para o Switch (5 a 30 minutos: o computador vai ficar ocupado)",
        "progress": "  {done} de {total} arquivos",
        "link": "Gerando o programa do Switch",
        "assemble": "Preparando a pasta para o cartão",
        "done": "\nPronto. Sua pasta está aqui:\n  {path}\n\nCopie a pasta \"switch\" que está dentro dela para a raiz do cartão.",
        "drive": "\nO cartão está conectado? Digite a letra da unidade para copiar agora, ou aperte Enter para pular: ",
        "copying": "Copiando para {path} (a imagem do jogo leva alguns minutos)",
        "copied": "Copiado. Seus saves não foram alterados.",
        "bad_drive": "Não foi possível copiar para essa unidade: {detail}",
        "launch": "\nNo Switch: segure R ao abrir qualquer jogo instalado e escolha \"Ben 10: Protector of Earth\".\n"
                  "Para sair do jogo, aperte + e - juntos.",
        "failed": "\nEsta etapa falhou: {stage}\nOs detalhes estão em {log}",
    },
}


def language():
    if os.name == "nt":
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF
        return "pt" if primary == 0x16 else "en"
    return "pt" if os.environ.get("LANG", "").startswith("pt") else "en"


TEXT = MESSAGES[language()]


class InstallError(Exception):
    """A problem the user can act on: the message is meant for them."""


def say(key, **values):
    print(TEXT[key].format(**values), flush=True)


def log_path():
    return WORK / "install.log"


def log(text):
    with open(log_path(), "a", encoding="utf-8", errors="replace") as handle:
        handle.write(text.rstrip("\n") + "\n")


def run(command, stage, env=None, cwd=ROOT):
    log("$ " + " ".join(str(part) for part in command))
    done = subprocess.run([str(part) for part in command], cwd=cwd, env=env, capture_output=True, text=True,
                          errors="replace")
    log(done.stdout + done.stderr)
    if done.returncode != 0:
        raise subprocess.CalledProcessError(done.returncode, command[0], done.stdout, done.stderr)
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
            raise ValueError("no ISO 9660 volume")
        self.root = self._record(descriptor[156:190])

    @staticmethod
    def _record(data):
        return struct.unpack_from("<I", data, 2)[0], struct.unpack_from("<I", data, 10)[0], bool(data[25] & 2)

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
            name = record[33:33 + record[32]].decode("ascii", "replace").split(";")[0].upper()
            yield name, self._record(record)
            offset += length

    def read(self, disc_path):
        extent, size, _ = self.root
        for part in disc_path.upper().split("/"):
            for name, record in self._entries(extent, size):
                if name == part:
                    extent, size, _ = record
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
        key = data[key_table + key_offset:].split(b"\0", 1)[0]
        if key == b"DISC_ID":
            return data[value_table + value_offset:].split(b"\0", 1)[0].decode("ascii", "replace")
    raise ValueError("no DISC_ID")


def check_iso(path):
    say("check_iso")
    try:
        iso = Iso(path)
        found = disc_id(iso)
    except (OSError, ValueError, FileNotFoundError, struct.error) as error:
        raise InstallError(TEXT["not_iso"].format(detail=error)) from error
    if found != TITLE["disc_id"]:
        raise InstallError(TEXT["wrong_game"].format(found=found, wanted=TITLE["disc_id"]))
    return iso


def decrypt_tool():
    return ROOT / "installer" / ("nk_decrypt.exe" if os.name == "nt" else "nk_decrypt")


def plain_module(iso, disc_path, destination, expected):
    """One program file from the disc, decrypted when it needs to be, checked against the known copy."""
    if destination.exists() and sha256(destination) == expected:
        return
    raw = WORK / "input" / (destination.name + ".raw")
    raw.write_bytes(iso.read(disc_path))
    if raw.read_bytes()[:4] == b"\x7fELF":
        shutil.copyfile(raw, destination)
    else:
        run([decrypt_tool(), "decrypt", "--in", raw, "--out", destination], "extract")
    raw.unlink()
    if sha256(destination) != expected:
        raise InstallError(TEXT["wrong_version"].format(wanted=TITLE["disc_id"]))


def stage_extract(iso):
    say("extract")
    folder = WORK / "input"
    folder.mkdir(parents=True, exist_ok=True)
    plain_module(iso, "PSP_GAME/SYSDIR/EBOOT.BIN", folder / "selected.elf", TITLE["executable_sha256"])
    for module in TITLE["modules"]:
        plain_module(iso, module["disc_path"], folder / module["name"], module["sha256"])


# ---- translation ----------------------------------------------------------------------------------


def stage_translate():
    say("translate")
    package = WORK / "package"
    package.mkdir(parents=True, exist_ok=True)
    image = package / f"{NAME}_image.bin"
    done = package / ".translated"
    if done.exists() and image.exists():
        return
    elf = WORK / "input" / "selected.elf"
    env = dict(os.environ, GAME_ELF=str(elf), PYTHONUTF8="1", PYTHONPATH=str(ROOT / "tools"))
    python = sys.executable
    run([python, "tools/prxload.py", "--env-elf", TITLE["base"], f"--out={image}"], "translate", env=env)
    if sha256(image) != TITLE["image_sha256"]:
        raise InstallError(TEXT["wrong_version"].format(wanted=TITLE["disc_id"]))
    extras = [f"--extra-elf={WORK / 'input' / module['name']}@runtime" for module in TITLE["codegen_modules"]]
    run([python, "tools/codegen.py", elf, package / f"{NAME}_recomp.c", f"--base={TITLE['base']}",
         "--funcs-per-chunk=2000", "--target-chunk-bytes=4000000", "--promote-registers", *extras], "translate", env=env)
    shutil.copyfile(ROOT / "installer" / "sr_title_config.h", package / "sr_title_config.h")
    done.write_text("ok\n", encoding="ascii")


# ---- the compiler ---------------------------------------------------------------------------------


def devkitpro_ready(path):
    gcc = path / "devkitA64" / "bin" / ("aarch64-none-elf-gcc" + (".exe" if os.name == "nt" else ""))
    return gcc.exists() and (path / "libnx" / "lib" / "libnx.a").exists() and (path / "libnx" / "lib" / "libdeko3d.a").exists()


def ensure_devkitpro(requested):
    path = Path(requested) if requested else Path(r"C:\devkitPro" if os.name == "nt" else "/opt/devkitpro")
    if devkitpro_ready(path):
        return path
    if not (path / "msys2").exists() and not (path / "devkitA64").exists():
        say("need_devkitpro")
        webbrowser.open(DEVKITPRO_INSTALLER_PAGE)
        input(TEXT["press_enter"])
    bash = path / "msys2" / "usr" / "bin" / "bash.exe"
    if not bash.exists() and not devkitpro_ready(path):
        raise InstallError(TEXT["still_no_devkitpro"].format(path=path))
    if not devkitpro_ready(path):
        say("packages")
        try:
            run([bash, "-lc", "pacman -S --needed --noconfirm switch-dev deko3d"], "packages")
        except subprocess.CalledProcessError as error:
            raise InstallError(TEXT["packages_failed"]) from error
    if not devkitpro_ready(path):
        raise InstallError(TEXT["packages_failed"])
    return path


def available_memory():
    if os.name == "nt":
        class Status(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                        ("available", ctypes.c_ulonglong), ("page_total", ctypes.c_ulonglong),
                        ("page_available", ctypes.c_ulonglong), ("virtual_total", ctypes.c_ulonglong),
                        ("virtual_available", ctypes.c_ulonglong), ("extended", ctypes.c_ulonglong)]
        status = Status()
        status.length = ctypes.sizeof(Status)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        return status.available
    return os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")


def compile_jobs(devkitpro):
    package = WORK / "package"
    dkp = devkitpro.as_posix()
    include = [f"-I{package.as_posix()}", f"-I{dkp}/libnx/include"]
    runtime_include = ["-Isrc/rt/switch/sdl_shim", "-Isrc/rt/switch", "-Isrc/rt", "-Isrc/core", "-Isrc/rt/atrac3p",
                       "-Isrc/rt/atrac3p/libavcodec", "-Isrc/rt/atrac3p/libavutil"] + include
    runtime = COMMON + ["-ffp-contract=off", "-O2"] + DEFINES + runtime_include + [
        f'-DNK_SWITCH_TITLE="{NAME}"', f'-DNK_SWITCH_ENTRY="{TITLE["entry"]}"']
    graphics = COMMON + ["-O3", "-fno-math-errno", "-Isrc/rt/switch/sdl_shim", "-Isrc/rt", f"-I{dkp}/libnx/include"]
    video = COMMON + ["-O2", "-std=gnu11", "-DSR_PUBLIC_SAFE", "-DSR_H264_FFMPEG", "-DHAVE_AV_CONFIG_H", "-D_ISOC99_SOURCE",
                      "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE", "-Isrc/rt/h264_ffmpeg_sub", "-Isrc/rt",
                      f"-I{dkp}/libnx/include", "-include", "src/rt/h264_ffmpeg_sub/nk_h264_symbols.h"]
    generated = ARCH + ["-ffunction-sections", "-D__SWITCH__", "-w", "-fno-strict-aliasing", "-fno-var-tracking",
                        "-ffp-contract=off", "-O2", "-DSR_FLIGHT_RECORDER_LINKED"] + COMPAT + include + [
        "-Isrc/rt/switch/sdl_shim", "-Isrc/rt"]

    def listed(name):
        return (ROOT / "src/rt/switch" / name).read_text(encoding="utf-8").split()

    jobs = []
    for source in listed("sources.txt") + listed("sources_atrac.txt"):
        extra = {"src/rt/mpeg.c": ["-DSR_SDL3VK"], "src/rt/pgf_public.c": ["-DSR_SDL3VK"],
                 "src/rt/driver.c": ["-Dmain=sr_driver_main"]}.get(source, [])
        jobs.append((source, runtime + extra))
    jobs.append(("src/rt/ge.c", graphics))
    jobs += [(source, video) for source in listed("sources_h264.txt")]
    jobs += [(path.relative_to(ROOT).as_posix(), generated) for path in sorted(package.glob("*.c"))]
    return jobs


def stage_compile(devkitpro):
    say("compile")
    objects = WORK / "obj"
    objects.mkdir(parents=True, exist_ok=True)
    gcc = devkitpro / "devkitA64" / "bin" / ("aarch64-none-elf-gcc" + (".exe" if os.name == "nt" else ""))
    jobs = compile_jobs(devkitpro)
    memory_jobs = max(1, available_memory() // COMPILE_MEMORY_BYTES)
    workers = max(1, min(os.cpu_count() or 1, memory_jobs))
    log(f"compiling {len(jobs)} files with {workers} workers")

    def object_for(source):
        return objects / (source.replace("/", "_") + ".o")

    def one(job):
        source, flags = job
        target = object_for(source)
        if target.exists():
            return target
        partial = target.with_suffix(".tmp")
        run([gcc, *flags, "-c", source, "-o", partial], "compile")
        partial.replace(target)
        return target

    finished = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(one, job) for job in jobs]
        for future in concurrent.futures.as_completed(futures):
            future.result()
            finished += 1
            if finished % 10 == 0 or finished == len(jobs):
                say("progress", done=finished, total=len(jobs))
    return [object_for(source) for source, _ in jobs]


def stage_link(devkitpro, objects):
    say("link")
    gcc = devkitpro / "devkitA64" / "bin" / ("aarch64-none-elf-gcc" + (".exe" if os.name == "nt" else ""))
    tools = devkitpro / "tools" / "bin"
    suffix = ".exe" if os.name == "nt" else ""
    env = dict(os.environ, DEVKITPRO=devkitpro.as_posix())
    shaders = sorted((ROOT / "installer" / "shaders").glob("*.o"))
    response = WORK / "objects.rsp"
    response.write_text("\n".join(f'"{path.as_posix()}"' for path in [*objects, *shaders]) + "\n", encoding="utf-8")
    elf = WORK / "ben10-psp.elf"
    run([gcc, *ARCH, f"-specs={devkitpro.as_posix()}/libnx/switch.specs", LINK_WRAPS, f"-L{devkitpro.as_posix()}/libnx/lib",
         f"@{response.as_posix()}", "-ldeko3d", "-lnx", "-lm", "-o", elf], "link", env=env)
    nacp = WORK / "ben10-psp.nacp"
    run([tools / ("nacptool" + suffix), "--create", TITLE["app_name"], TITLE["app_author"], TITLE["app_version"], nacp],
        "link", env=env)
    nro = WORK / "ben10-psp.nro"
    run([tools / ("elf2nro" + suffix), elf, nro, f"--nacp={nacp}", f"--icon={ROOT / 'src/rt/switch/icon/icon.jpg'}"],
        "link", env=env)
    return nro


# ---- the SD card ----------------------------------------------------------------------------------


def copy_if_changed(source, destination):
    if destination.exists() and destination.stat().st_size == source.stat().st_size and \
            destination.stat().st_mtime >= source.stat().st_mtime:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".partial")
    shutil.copyfile(source, partial)
    partial.replace(destination)


def fill_folder(folder, nro, iso_path):
    copy_if_changed(nro, folder / "ben10-psp.nro")
    copy_if_changed(WORK / "package" / f"{NAME}_image.bin", folder / f"{NAME}_image.bin")
    for name in ("assets", "font"):
        base = ROOT / "installer" / "payload" / name
        for path in base.rglob("*"):
            if path.is_file():
                copy_if_changed(path, folder / name / path.relative_to(base))
    copy_if_changed(iso_path, folder / "game.iso")


def stage_assemble(nro, iso_path):
    say("assemble")
    fill_folder(OUTPUT / SD_FOLDER, nro, iso_path)
    say("done", path=OUTPUT)


def offer_sd_copy(nro, iso_path):
    answer = input(TEXT["drive"]).strip().rstrip(":\\/").upper()
    if len(answer) != 1 or not answer.isalpha():
        return
    folder = Path(f"{answer}:/") / SD_FOLDER
    say("copying", path=folder)
    try:
        fill_folder(folder, nro, iso_path)
    except OSError as error:
        print(TEXT["bad_drive"].format(detail=error), flush=True)
        return
    say("copied")


def choose_iso():
    say("pick")
    script = ("Add-Type -AssemblyName System.Windows.Forms;"
              "$d=New-Object System.Windows.Forms.OpenFileDialog;"
              "$d.Filter='PSP disc image (*.iso)|*.iso';"
              "if($d.ShowDialog() -eq 'OK'){[Console]::OutputEncoding=[Text.Encoding]::UTF8;$d.FileName}")
    done = subprocess.run(["powershell", "-NoProfile", "-STA", "-Command", script], capture_output=True, text=True,
                          encoding="utf-8")
    return done.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", help="the disc image; a window asks when this is left out")
    parser.add_argument("--devkitpro", help="devkitPro's folder, when it is not the default one")
    parser.add_argument("--stop-after", choices=["translate"], help="for testing: stop after this stage")
    parser.add_argument("--no-sd", action="store_true", help="do not offer to copy to an SD card")
    args = parser.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    say("banner")
    say("notice")
    print(flush=True)
    iso_path = args.iso or choose_iso()
    if not iso_path:
        raise InstallError(TEXT["no_iso"])
    iso_path = Path(iso_path)
    iso = check_iso(iso_path)
    if not args.stop_after:
        devkitpro = ensure_devkitpro(args.devkitpro)
    stage_extract(iso)
    stage_translate()
    if args.stop_after == "translate":
        return
    objects = stage_compile(devkitpro)
    nro = stage_link(devkitpro, objects)
    stage_assemble(nro, iso_path)
    if not args.no_sd:
        offer_sd_copy(nro, iso_path)
    say("launch")


if __name__ == "__main__":
    try:
        main()
    except InstallError as error:
        print("\n" + str(error), flush=True)
        sys.exit(1)
    except subprocess.CalledProcessError as error:
        say("failed", stage=Path(str(error.cmd)).name, log=log_path())
        sys.exit(1)
