"""校验并暂存 macOS CI 的已审核 FFmpeg；不编译或执行下载内容。"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

FILES = frozenset({"ffmpeg", "ffmpeg.README", "ffmpeg.LICENSE", "ffmpeg-8.1.2.tar.xz"})
MAX_BYTES = 384 * 1024 * 1024
SOURCE_SHA256 = "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c"
# 固定公开发行镜像只作为已审核运行资源的载体；不运行其中的应用。
ARM64_SOURCE_DMG_URL = "https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.199/HachimiDock_0.1.199_macOS-arm64.dmg"
ARM64_SOURCE_DMG_SHA256 = "9e56fb8b9875c55830d347f28a4e9dafa5652a7872060640af6b37b028a630ef"


def select_source(arch, url, checksum):
    if not url and not checksum and arch == "arm64":
        return ARM64_SOURCE_DMG_URL, ARM64_SOURCE_DMG_SHA256, "dmg"
    if not url or not re.fullmatch(r"[a-f0-9]{64}", checksum):
        raise ValueError(
            "Missing audited macOS runtime bundle. Set MACOS_RUNTIME_" + arch.upper()
            + "_URL and MACOS_RUNTIME_" + arch.upper()
            + "_SHA256 repository variables, or supply the workflow inputs. See pc/docs/macos-ci.md."
        )
    return validate_url(url), checksum, "tar.gz"


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def validate_url(value):
    parts = urlsplit(value)
    if parts.scheme != "https" or not parts.hostname or parts.username or parts.password:
        raise ValueError("Runtime bundle URL must use HTTPS without embedded credentials")
    return value


def extract_bundle(archive, destination):
    """只接受四个平铺普通文件，不恢复归档路径、链接或文件权限。"""
    if any(destination.iterdir()):
        raise ValueError("Runtime destination must be empty")
    seen = set()
    total = 0
    with tarfile.open(archive, "r:gz") as bundle:
        for member in bundle:
            name = member.name
            if name.startswith("./"):
                name = name[2:]
            if name not in FILES or not member.isfile() or name in seen:
                raise ValueError("Runtime bundle contains an unexpected path, duplicate, or non-regular file")
            total += member.size
            if member.size < 0 or total > MAX_BYTES:
                raise ValueError("Runtime bundle exceeds the size limit")
            seen.add(name)
            stream = bundle.extractfile(member)
            if stream is None:
                raise ValueError("Runtime archive member cannot be read")
            with stream, (destination / name).open("xb") as output:
                shutil.copyfileobj(stream, output)
    if seen != FILES:
        raise ValueError("Runtime bundle must contain ffmpeg, README, LGPL license, and matching source archive")


def copy_runtime_from_mount(mount, destination):
    apps = [path for path in mount.glob("*.app") if path.is_dir() and not path.is_symlink()]
    if len(apps) != 1:
        raise ValueError("Expected exactly one source application in runtime image")
    tools = apps[0] / "Contents/Resources/tools"
    total = 0
    for name in sorted(FILES):
        source = tools / ("source/" + name if name.endswith(".tar.xz") else name)
        if not source.resolve().is_relative_to(mount.resolve()) or not stat.S_ISREG(source.lstat().st_mode):
            raise ValueError("Runtime image contains an unsafe resource link or path")
        total += source.stat().st_size
        if total > MAX_BYTES:
            raise ValueError("Runtime image resources exceed the size limit")
        with source.open("rb") as stream, (destination / name).open("xb") as output:
            shutil.copyfileobj(stream, output)


def extract_dmg(archive, destination, mount):
    if sys.platform != "darwin":
        raise ValueError("Read-only runtime DMG extraction requires a macOS runner")
    mount.mkdir()
    try:
        subprocess.run(["/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen",
                        "-mountpoint", str(mount), str(archive)], check=True, timeout=120,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        copy_runtime_from_mount(mount, destination)
    finally:
        # 无论复制是否成功都尝试卸载；不使用强制卸载，不操作其他卷。
        subprocess.run(["/usr/bin/hdiutil", "detach", str(mount)], timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def audited_binary_hash(repository, arch):
    # 直接读取现有打包门禁的权威值，不引入第二套可配置白名单。
    source = (repository / "pc/scripts/prepare-desktop-resources.mjs").read_text(encoding="utf-8")
    match = re.search(r'"macos-' + re.escape(arch) + r'":\s*"([a-f0-9]{64})"', source)
    if not match:
        raise ValueError("Missing audited FFmpeg hash for macOS " + arch)
    return match.group(1)


def audited_binary_hashes(repository, arch):
    source = (repository / "pc/scripts/prepare-desktop-resources.mjs").read_text(encoding="utf-8")
    values = re.findall(r'"macos-' + re.escape(arch) + r'":\s*"([a-f0-9]{64})"', source)
    if not values:
        raise ValueError("Missing audited FFmpeg hashes for macOS " + arch)
    return values


def validate_files(destination, expected_binary_hash):
    allowed = [expected_binary_hash] if isinstance(expected_binary_hash, str) else expected_binary_hash
    if digest(destination / "ffmpeg") not in allowed:
        actual = digest(destination / "ffmpeg")
        raise ValueError("FFmpeg does not match the existing audited binary hash; no replacement is permitted"
                         + "; expected=" + ",".join(allowed) + "; actual=" + actual)
    if digest(destination / "ffmpeg-8.1.2.tar.xz") != SOURCE_SHA256:
        raise ValueError("FFmpeg source archive SHA-256 mismatch")
    readme = (destination / "ffmpeg.README").read_text(encoding="utf-8")
    license_text = (destination / "ffmpeg.LICENSE").read_text(encoding="utf-8")
    if not all(flag in readme for flag in ["--disable-gpl", "--disable-nonfree", "h264_videotoolbox"]):
        raise ValueError("Missing audited build flags in FFmpeg README")
    if any(flag in readme for flag in ["--enable-gpl", "--enable-nonfree", "--enable-libx264", "--enable-libopenh264"]):
        raise ValueError("FFmpeg README declares a prohibited build option")
    if "GNU LESSER GENERAL PUBLIC LICENSE" not in license_text:
        raise ValueError("FFmpeg LGPL notice is missing")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=["arm64", "x64"], required=True)
    args = parser.parse_args()
    url = os.environ.get("PET_MANAGER_CI_RUNTIME_URL", "").strip()
    expected = os.environ.get("PET_MANAGER_CI_RUNTIME_SHA256", "").strip().lower()
    url, expected, source_format = select_source(args.arch, url, expected)
    validate_url(url)
    repository = Path(__file__).resolve().parents[2]
    root = Path(tempfile.mkdtemp(prefix="easycoding-macos-runtime-", dir=os.environ.get("RUNNER_TEMP")))
    downloads = root / "download"
    inputs = root / "input"
    downloads.mkdir()
    inputs.mkdir()
    archive = downloads / ("runtime.dmg" if source_format == "dmg" else "runtime.tar.gz")
    with urlopen(Request(url, headers={"User-Agent": "EasyCoding-Deck-CI"}), timeout=60) as response:
        validate_url(response.geturl())
        total = 0
        with archive.open("xb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_BYTES:
                    raise ValueError("Runtime download exceeds the size limit")
                output.write(chunk)
    if digest(archive) != expected:
        raise ValueError("Runtime bundle SHA-256 mismatch")
    if source_format == "dmg":
        extract_dmg(archive, inputs, root / "mount")
    else:
        extract_bundle(archive, inputs)
    if source_format == "dmg" and digest(inputs / "ffmpeg") not in audited_binary_hashes(repository, args.arch):
        # 固定来源不命中现有哈希时只输出静态审计信息，仍然失败，不执行候选二进制。
        print("Pinned source DMG verified: " + ARM64_SOURCE_DMG_SHA256, flush=True)
        print("Extracted FFmpeg source archive SHA-256: " + digest(inputs / "ffmpeg-8.1.2.tar.xz"), flush=True)
        for command in [["/usr/bin/file", str(inputs / "ffmpeg")],
                        ["/usr/bin/otool", "-L", str(inputs / "ffmpeg")],
                        ["/usr/bin/codesign", "--display", "--verbose=4", str(inputs / "ffmpeg")]]:
            subprocess.run(command, check=False, timeout=30)
        print("FFmpeg build declaration:", flush=True)
        print((inputs / "ffmpeg.README").read_text(encoding="utf-8"), flush=True)
    validate_files(inputs, audited_binary_hashes(repository, args.arch))
    (inputs / "ffmpeg").chmod(0o755)
    environment = {
        "PET_MANAGER_FFMPEG_BIN": inputs / "ffmpeg",
        "PET_MANAGER_FFMPEG_README": inputs / "ffmpeg.README",
        "PET_MANAGER_FFMPEG_LICENSE": inputs / "ffmpeg.LICENSE",
        "PET_MANAGER_FFMPEG_SOURCE_ARCHIVE": inputs / "ffmpeg-8.1.2.tar.xz",
    }
    with Path(os.environ["GITHUB_ENV"]).open("a", encoding="utf-8") as output:
        for name, value in environment.items():
            output.write(name + "=" + str(value) + "\n")
    print("Audited FFmpeg bundle verified for macOS " + args.arch)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # 不输出下载 URL、重定向 URL 或潜在的查询参数。
        message = str(error) if isinstance(error, ValueError) else type(error).__name__
        print("::error::" + message)
        raise SystemExit(1)
