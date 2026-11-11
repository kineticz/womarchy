#!/usr/bin/env python3
"""Regression tests for linux/overlay/local-repo-sync (local [womarchy] fallback follows the hosted repo)."""
from pathlib import Path
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
import io

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "linux/overlay/local-repo-sync"


def make_db(path: Path, files: list[str]) -> None:
    """A pacman sync database (gzip tar) whose entries name the given package files."""
    with tarfile.open(path, "w:gz") as t:
        for f in files:
            name = f.split(".pkg.tar")[0]
            desc = f"%FILENAME%\n{f}\n\n%NAME%\n{name.rsplit('-', 3)[0]}\n\n".encode()
            info = tarfile.TarInfo(f"{name}/desc")
            info.size = len(desc)
            t.addfile(info, io.BytesIO(desc))


class LocalRepoSync(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.repo, self.sync, self.cache = (self.tmp / d for d in ("repo", "sync", "cache"))
        for d in (self.repo, self.sync, self.cache):
            d.mkdir()
        # what the image shipped
        self.old = ["mesa-1:26.2.3-2.1-x86_64.pkg.tar.zst", "hyprland-0.56.2-4.6-x86_64.pkg.tar.zst"]
        make_db(self.repo / "womarchy.db", self.old)
        (self.repo / "womarchy.db.sig").write_bytes(b"old-sig")
        for f in self.old:
            (self.repo / f).write_bytes(b"old")
            (self.repo / (f + ".sig")).write_bytes(b"old")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_sync(self):
        env = {**os.environ, "WOMARCHY_LOCAL_REPO": str(self.repo), "WOMARCHY_SYNC_DIR": str(self.sync),
               "WOMARCHY_CACHE_DIRS": str(self.cache)}
        return subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True, text=True)

    def test_follows_hosted_db(self):
        new = ["mesa-1:26.2.5-1.9-x86_64.pkg.tar.zst", "hyprland-0.56.2-4.8-x86_64.pkg.tar.zst",
               "vulkan-swrast-1:26.2.5-1.9-x86_64.pkg.tar.zst"]
        make_db(self.sync / "womarchy.db", new)
        (self.sync / "womarchy.db.sig").write_bytes(b"new-sig")
        for f in new[:2]:  # installed ones are cached; vulkan-swrast is not
            (self.cache / f).write_bytes(b"new")
            (self.cache / (f + ".sig")).write_bytes(b"new")
        r = self.run_sync()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.repo / "womarchy.db").read_bytes(), (self.sync / "womarchy.db").read_bytes())
        self.assertEqual((self.repo / "womarchy.db.sig").read_bytes(), b"new-sig")
        self.assertEqual((self.repo / "womarchy.db.tar.gz").read_bytes(), (self.sync / "womarchy.db").read_bytes())
        for f in new[:2]:
            self.assertEqual((self.repo / f).read_bytes(), b"new")
            self.assertTrue((self.repo / (f + ".sig")).exists())
        self.assertFalse((self.repo / new[2]).exists())
        for f in self.old:
            self.assertFalse((self.repo / f).exists(), f)
            self.assertFalse((self.repo / (f + ".sig")).exists(), f)

    def test_unsigned_or_missing_db_leaves_repo(self):
        before = sorted(p.name for p in self.repo.iterdir())
        self.assertEqual(self.run_sync().returncode, 0)          # no synced db
        make_db(self.sync / "womarchy.db", ["mesa-1:9-1-x86_64.pkg.tar.zst"])
        self.assertEqual(self.run_sync().returncode, 0)          # synced db without signature
        self.assertEqual(sorted(p.name for p in self.repo.iterdir()), before)

    def test_unreadable_db_leaves_repo(self):
        (self.sync / "womarchy.db").write_bytes(b"not a database")
        (self.sync / "womarchy.db.sig").write_bytes(b"sig")
        before = sorted(p.name for p in self.repo.iterdir())
        r = self.run_sync()
        self.assertEqual(r.returncode, 0)
        self.assertIn("could not read", r.stderr)
        self.assertEqual(sorted(p.name for p in self.repo.iterdir()), before)

    def test_unchanged_db_is_a_no_op(self):
        shutil.copy(self.repo / "womarchy.db", self.sync / "womarchy.db")
        shutil.copy(self.repo / "womarchy.db.sig", self.sync / "womarchy.db.sig")
        before = {p.name: p.stat().st_mtime_ns for p in self.repo.iterdir()}
        self.assertEqual(self.run_sync().returncode, 0)
        self.assertEqual({p.name: p.stat().st_mtime_ns for p in self.repo.iterdir()}, before)

    def test_odd_file_names_are_not_copied(self):
        make_db(self.sync / "womarchy.db", ["../../etc/passwd.pkg.tar.zst"])
        (self.sync / "womarchy.db.sig").write_bytes(b"sig")
        r = self.run_sync()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((self.tmp / "etc").exists())


if __name__ == "__main__":
    unittest.main()
