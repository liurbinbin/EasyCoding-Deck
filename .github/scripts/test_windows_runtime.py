"""Windows CI 资源离线检查；不下载、编译或运行来源安装器。"""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("windows_runtime", Path(__file__).with_name("prepare-windows-runtime.py"))
RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNTIME)


def listing(names=None):
    return "\n\n".join("Path = tools/" + name + "\nSize = 16\nFolder = -\nAttributes = A" for name in (names or sorted(RUNTIME.FILES)))


class WindowsRuntimeTests(unittest.TestCase):
    def test_source_and_binary_are_pinned(self):
        self.assertRegex(RUNTIME.INSTALLER_SHA256, r"^[a-f0-9]{64}$")
        self.assertIn("/v0.1.199/", RUNTIME.SOURCE_URL)
        self.assertEqual(RUNTIME.audited_binary_hash(Path(__file__).resolve().parents[2]),
                         "1cefca14ea713d5d7e7a1b948299142831185487f5efb14914d049ba9090d95f")

    def test_only_five_exact_regular_members_are_selected(self):
        entries = listing() + "\n\nPath = $PLUGINSDIR/run.exe\nSize = 32\nFolder = -"
        members = RUNTIME.select_members(entries)
        self.assertEqual(set(members), {"tools/" + name for name in RUNTIME.FILES})

    def test_windows_separators_are_supported(self):
        self.assertEqual(len(RUNTIME.select_members(listing().replace("tools/", "tools\\"))), 5)

    def test_missing_duplicate_traversal_and_links_fail_closed(self):
        for entries in [
            listing(["ffmpeg.exe"]),
            listing() + "\n\nPath = other/ffmpeg.exe\nSize = 1",
            listing().replace("tools/ffmpeg.exe", "../ffmpeg.exe"),
            listing().replace("tools/ffmpeg.exe", "C:\\tools\\ffmpeg.exe"),
            listing().replace("tools/ffmpeg.exe", "/tools/ffmpeg.exe"),
            listing().replace("Path = tools/ffmpeg.exe", "Symbolic Link = other\nPath = tools/ffmpeg.exe"),
            listing().replace("Size = 16", "Size = -1"),
            listing().replace("Folder = -", "Folder = +"),
            listing().replace("Size = 16", "Size = " + str(RUNTIME.MAX_BYTES)),
        ]:
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                RUNTIME.select_members(entries)

    def test_https_without_credentials(self):
        self.assertEqual(RUNTIME.validate_url(RUNTIME.SOURCE_URL), RUNTIME.SOURCE_URL)
        for url in ["http://example.com/a", "file:///tmp/a", "https://user:token@example.com/a", "https:///a"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                RUNTIME.validate_url(url)

    def test_unapproved_binary_is_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root)
            for name in RUNTIME.FILES:
                (destination / name).write_bytes(b"fixture")
            with self.assertRaisesRegex(ValueError, "audited Windows binary hash"):
                RUNTIME.validate_files(destination, "0" * 64)

    def test_metadata_license_and_source_are_required(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root)
            for name in RUNTIME.FILES:
                (destination / name).write_text("fixture", encoding="utf-8")
            (destination / "ffmpeg.README").write_text("--disable-gpl --disable-nonfree h264_mf", encoding="utf-8")
            (destination / "ffmpeg.LICENSE").write_text("GNU LESSER GENERAL PUBLIC LICENSE", encoding="utf-8")
            (destination / "zlib.LICENSE").write_text("Jean-loup Gailly Mark Adler This notice may not be removed", encoding="utf-8")
            with patch.object(RUNTIME, "digest", side_effect=lambda p: RUNTIME.SOURCE_SHA256 if p.name.endswith(".tar.xz") else "0" * 64):
                RUNTIME.validate_files(destination, "0" * 64)
                (destination / "ffmpeg.README").write_text("--disable-gpl --disable-nonfree h264_mf --enable-gpl", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "prohibited"):
                    RUNTIME.validate_files(destination, "0" * 64)


if __name__ == "__main__":
    unittest.main()
