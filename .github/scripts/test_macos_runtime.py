"""macOS CI 资源导入的离线契约测试；不下载或执行任何运行资源。"""
import importlib.util
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("macos_runtime", Path(__file__).with_name("prepare-macos-runtime.py"))
RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNTIME)


class RuntimeBundleTests(unittest.TestCase):
    def make_bundle(self, path, members):
        with tarfile.open(path, "w:gz") as bundle:
            for name, data, kind in members:
                item = tarfile.TarInfo(name)
                item.type = kind
                if kind == tarfile.REGTYPE:
                    item.size = len(data)
                    bundle.addfile(item, io.BytesIO(data))
                else:
                    item.linkname = "ffmpeg"
                    bundle.addfile(item)

    def test_flat_regular_bundle_only(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            archive = root / "bundle.tar.gz"
            destination = root / "input"
            destination.mkdir()
            self.make_bundle(archive, [(name, b"fixture", tarfile.REGTYPE) for name in RUNTIME.FILES])
            RUNTIME.extract_bundle(archive, destination)
            self.assertEqual({entry.name for entry in destination.iterdir()}, RUNTIME.FILES)

    def test_traversal_links_and_duplicates_are_rejected(self):
        for name, kind in [("../ffmpeg", tarfile.REGTYPE), ("/ffmpeg", tarfile.REGTYPE),
                           ("ffmpeg", tarfile.SYMTYPE), ("ffmpeg", tarfile.LNKTYPE),
                           ("run.sh", tarfile.REGTYPE)]:
            with self.subTest(name=name, kind=kind), tempfile.TemporaryDirectory() as root:
                root = Path(root)
                archive = root / "bundle.tar.gz"
                destination = root / "input"
                destination.mkdir()
                self.make_bundle(archive, [(name, b"fixture", kind)])
                with self.assertRaises(ValueError):
                    RUNTIME.extract_bundle(archive, destination)
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            destination = root / "input"
            destination.mkdir()
            self.make_bundle(root / "bundle.tar.gz", [("ffmpeg", b"x", tarfile.REGTYPE)] * 2)
            with self.assertRaises(ValueError):
                RUNTIME.extract_bundle(root / "bundle.tar.gz", destination)

    def test_missing_members_are_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            destination = root / "input"
            destination.mkdir()
            self.make_bundle(root / "bundle.tar.gz", [("ffmpeg", b"x", tarfile.REGTYPE)])
            with self.assertRaises(ValueError):
                RUNTIME.extract_bundle(root / "bundle.tar.gz", destination)

    def test_https_url_without_credentials(self):
        self.assertEqual(RUNTIME.validate_url("https://example.com/runtime.tar.gz"), "https://example.com/runtime.tar.gz")
        for url in ["http://example.com/a", "file:///tmp/a", "https://name:token@example.com/a", "https:///a"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                RUNTIME.validate_url(url)

    def test_audited_hashes_are_read_from_existing_packaging_gate(self):
        repository = Path(__file__).resolve().parents[2]
        for arch in ["arm64", "x64"]:
            value = RUNTIME.audited_binary_hash(repository, arch)
            self.assertRegex(value, r"^[a-f0-9]{64}$")
        self.assertNotEqual(RUNTIME.audited_binary_hash(repository, "arm64"), RUNTIME.audited_binary_hash(repository, "x64"))
        self.assertEqual(RUNTIME.audited_binary_hashes(repository, "arm64"), [
            RUNTIME.audited_binary_hash(repository, "arm64"),
            "fbbccfb581309412c5f9353196e128741c3ad4fac229fd07061ddebb70107262",
        ])
        self.assertEqual(RUNTIME.audited_binary_hashes(repository, "x64"), [RUNTIME.audited_binary_hash(repository, "x64")])

    def test_default_arm64_source_is_pinned_and_never_masks_partial_configuration(self):
        url, checksum, kind = RUNTIME.select_source("arm64", "", "")
        self.assertEqual(url, RUNTIME.ARM64_SOURCE_DMG_URL)
        self.assertEqual(checksum, RUNTIME.ARM64_SOURCE_DMG_SHA256)
        self.assertEqual(kind, "dmg")
        for arch, url, checksum in [("x64", "", ""), ("arm64", "https://example.com/a", ""), ("arm64", "", "0" * 64)]:
            with self.subTest(arch=arch, url=url), self.assertRaises(ValueError):
                RUNTIME.select_source(arch, url, checksum)

    def test_mounted_image_copies_only_declared_resource_files(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            mount = root / "mount"
            tools = mount / "Source.app/Contents/Resources/tools"
            (tools / "source").mkdir(parents=True)
            destination = root / "input"
            destination.mkdir()
            for name in RUNTIME.FILES:
                source = tools / ("source/" + name if name.endswith(".tar.xz") else name)
                source.write_bytes(b"fixture")
            (tools / "do-not-run.sh").write_text("exit 99", encoding="utf-8")
            RUNTIME.copy_runtime_from_mount(mount, destination)
            self.assertEqual({entry.name for entry in destination.iterdir()}, RUNTIME.FILES)

    def test_dmg_extraction_is_macos_only(self):
        if sys.platform != "darwin":
            with self.assertRaisesRegex(ValueError, "macOS runner"):
                RUNTIME.extract_dmg(Path("not-mounted.dmg"), Path("unused"), Path("unused-mount"))

    def test_unapproved_binary_is_rejected_before_execution(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root)
            for name in RUNTIME.FILES:
                (destination / name).write_bytes(b"not-a-runtime")
            with self.assertRaisesRegex(ValueError, "audited binary hash"):
                RUNTIME.validate_files(destination, "0" * 64)


if __name__ == "__main__":
    unittest.main()
