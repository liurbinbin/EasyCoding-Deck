"""只解包固定 Windows 发行载体中的 FFmpeg 资源，不运行安装器或来源应用。"""
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

SOURCE_URL = "https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.199/HachimiDock_0.1.199_Windows-x64-setup.exe"
INSTALLER_SHA256 = "273a78c0aa6a2cfaa06060b30e9a187f3fd0ff6d2f5113ccfe47ed356fd53b0d"
SOURCE_SHA256 = "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
FILES = frozenset({"ffmpeg.exe", "ffmpeg.README", "ffmpeg.LICENSE", "ffmpeg-8.1.2.tar.xz", "zlib.LICENSE"})
MAX_BYTES = 384 * 1024 * 1024


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def validate_url(value):
    parts = urlsplit(value)
    if parts.scheme != "https" or not parts.hostname or parts.username or parts.password:
        raise ValueError("Runtime source must use HTTPS without embedded credentials")
    return value


def select_members(listing):
    """从 7-Zip 技术列表选择五个唯一普通文件，拒绝重复、链接、越界或超大条目。"""
    selected = {}
    total = 0
    for block in re.split(r"\r?\n\s*\r?\n", listing.strip()):
        fields = dict(line.split(" = ", 1) for line in block.splitlines() if " = " in line)
        raw = fields.get("Path", "")
        name = PurePosixPath(raw.replace("\\", "/")).name
        if name not in FILES:
            continue
        path = PurePosixPath(raw.replace("\\", "/"))
        if (path.is_absolute() or ".." in path.parts or ":" in raw or "\x00" in raw
                or name in selected or fields.get("Folder") == "+"
                or any("link" in key.lower() for key in fields)
                or "L" in fields.get("Attributes", "")):
            raise ValueError("Unsafe or duplicate runtime archive member")
        size = int(fields.get("Size", "-1"))
        if size < 0:
            raise ValueError("Missing runtime member size")
        total += size
        if total > MAX_BYTES:
            raise ValueError("Runtime archive resources exceed the size limit")
        selected[name] = raw
    if set(selected) != FILES:
        raise ValueError("Pinned Windows installer is missing required audited runtime resources")
    return [selected[name] for name in sorted(FILES)]


def audited_binary_hash(repository):
    source = (repository / "pc/scripts/prepare-desktop-resources.mjs").read_text(encoding="utf-8")
    match = re.search(r'"windows-x64":\s*"([a-f0-9]{64})"', source)
    if not match:
        raise ValueError("Missing audited Windows x64 FFmpeg hash")
    return match.group(1)


def validate_files(destination, expected):
    if {path.name for path in destination.iterdir()} != FILES:
        raise ValueError("Unexpected extracted runtime files")
    for name in FILES:
        path = destination / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Runtime input must be a regular file")
    actual = digest(destination / "ffmpeg.exe")
    if actual != expected:
        raise ValueError("FFmpeg does not match the audited Windows binary hash; expected=" + expected + "; actual=" + actual)
    if digest(destination / "ffmpeg-8.1.2.tar.xz") != SOURCE_SHA256:
        raise ValueError("FFmpeg source archive SHA-256 mismatch")
    readme = (destination / "ffmpeg.README").read_text(encoding="utf-8")
    for flag in ["--disable-gpl", "--disable-nonfree", "h264_mf"]:
        if flag not in readme:
            raise ValueError("FFmpeg build declaration is missing required flags")
    for flag in ["--enable-gpl", "--enable-nonfree", "--enable-libx264", "--enable-libopenh264"]:
        if flag in readme:
            raise ValueError("FFmpeg build declaration contains prohibited options")
    if "GNU LESSER GENERAL PUBLIC LICENSE" not in (destination / "ffmpeg.LICENSE").read_text(encoding="utf-8"):
        raise ValueError("Missing FFmpeg LGPL license")
    zlib = (destination / "zlib.LICENSE").read_text(encoding="utf-8")
    if "Jean-loup Gailly" not in zlib or "Mark Adler" not in zlib or "This notice may not be removed" not in zlib:
        raise ValueError("Missing zlib license attribution")


def main():
    seven_zip = shutil.which("7z")
    if not seven_zip:
        raise ValueError("Windows runtime import requires the runner's installed 7-Zip")
    repository = Path(__file__).resolve().parents[2]
    root = Path(tempfile.mkdtemp(prefix="easycoding-windows-runtime-", dir=os.environ.get("RUNNER_TEMP")))
    downloads = root / "download"
    inputs = root / "input"
    downloads.mkdir()
    inputs.mkdir()
    archive = downloads / "source-installer.exe"
    with urlopen(Request(validate_url(SOURCE_URL), headers={"User-Agent": "EasyCoding-Deck-CI"}), timeout=60) as response:
        validate_url(response.geturl())
        total = 0
        with archive.open("xb") as output:
            while chunk := response.read(1024 * 1024):
                total += len(chunk)
                if total > MAX_BYTES:
                    raise ValueError("Runtime source download exceeds the size limit")
                output.write(chunk)
    if digest(archive) != INSTALLER_SHA256:
        raise ValueError("Pinned Windows installer SHA-256 mismatch")
    listing = subprocess.run([seven_zip, "l", "-slt", "-ba", "-sccUTF-8", str(archive)],
                             check=True, capture_output=True, encoding="utf-8", timeout=60)
    members = select_members(listing.stdout)
    # e 只提取精确白名单并平铺；绝不启动下载的 .exe，也不提取安装脚本。
    subprocess.run([seven_zip, "e", "-y", "-spd", "-o" + str(inputs), "--", str(archive), *members],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    validate_files(inputs, audited_binary_hash(repository))
    environment = {
        "PET_MANAGER_FFMPEG_BIN": inputs / "ffmpeg.exe",
        "PET_MANAGER_FFMPEG_README": inputs / "ffmpeg.README",
        "PET_MANAGER_FFMPEG_LICENSE": inputs / "ffmpeg.LICENSE",
        "PET_MANAGER_FFMPEG_SOURCE_ARCHIVE": inputs / "ffmpeg-8.1.2.tar.xz",
        "PET_MANAGER_ZLIB_LICENSE": inputs / "zlib.LICENSE",
    }
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
        for name, value in environment.items():
            output.write(name + "=" + str(value) + "\n")
    print("Audited FFmpeg bundle verified for Windows x64; source installer was not executed")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        message = str(error) if isinstance(error, ValueError) else type(error).__name__
        print("::error::" + message)
        raise SystemExit(1)
