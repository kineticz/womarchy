#!/usr/bin/env python3
"""Regression tests for linux/overlay/llvm-deunique (LLVM copy without GNU_UNIQUE symbols)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "linux/overlay/llvm-deunique"

# GCC marks a static local of an inline function as STB_GNU_UNIQUE, like LLVM's
# FoldingSet<T>::getFoldingSetInfo()::Info.
SOURCE = """
template <typename T> struct Box { static T &get() { static T value; return value; } };
extern "C" int touch() { return ++Box<int>::get() + ++Box<long>::get(); }
"""


def bindings(lib: Path) -> dict:
    out = subprocess.run(["readelf", "-W", "--dyn-syms", str(lib)], check=True,
                         capture_output=True, text=True).stdout
    result = {}
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[0].endswith(":") and "Box" in fields[7]:
            result[fields[7]] = fields[4]
    return result


@unittest.skipUnless(shutil.which("g++") and shutil.which("readelf") and shutil.which("perl"),
                     "needs g++, readelf and perl")
class LlvmDeuniqueTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.src, self.out = self.tmp / "lib", self.tmp / "out"
        self.src.mkdir()
        (self.tmp / "box.cpp").write_text(SOURCE)
        subprocess.run(["g++", "-shared", "-fPIC", "-O1", "-o", str(self.src / "libLLVM.so.23.1"),
                        str(self.tmp / "box.cpp")], check=True)
        os.symlink("libLLVM.so.23.1", self.src / "libLLVM.so")
        self.log = self.tmp / "ldconfig.log"

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_script(self):
        env = dict(os.environ, WOMARCHY_LLVM_SRC=str(self.src), WOMARCHY_LLVM_OUT=str(self.out),
                   WOMARCHY_LDCONFIG=f"echo ran >> {self.log}")
        return subprocess.run(["perl", str(SCRIPT)], env=env, input="", capture_output=True,
                              text=True, check=True)

    def ldconfig_runs(self):
        return self.log.read_text().count("ran") if self.log.exists() else 0

    def test_unique_symbols_become_global(self):
        before = bindings(self.src / "libLLVM.so.23.1")
        self.assertTrue(before and set(before.values()) == {"UNIQUE"}, before)
        self.run_script()
        copy = self.out / "libLLVM.so.23.1"
        self.assertEqual(set(bindings(copy).values()), {"GLOBAL"})
        self.assertEqual(bindings(copy).keys(), before.keys())
        self.assertFalse((self.out / "libLLVM.so").exists(), "symlinks are not copied")
        self.assertEqual(int(os.stat(copy).st_mtime), int(os.stat(self.src / "libLLVM.so.23.1").st_mtime))
        self.assertEqual(self.ldconfig_runs(), 1)
        # the copy still loads
        subprocess.run(["python3", "-c", f"import ctypes; ctypes.CDLL({str(copy)!r}).touch()"], check=True)

    def test_up_to_date_copy_is_left_alone(self):
        self.run_script()
        self.run_script()
        self.assertEqual(self.ldconfig_runs(), 1)

    def test_upgraded_library_is_rewritten_and_stale_copy_removed(self):
        self.run_script()
        os.rename(self.src / "libLLVM.so.23.1", self.src / "libLLVM.so.24.1")
        os.utime(self.src / "libLLVM.so.24.1", (1, 1))
        result = self.run_script()
        self.assertTrue((self.out / "libLLVM.so.24.1").exists())
        self.assertFalse((self.out / "libLLVM.so.23.1").exists())
        self.assertIn("removed stale", result.stdout)

    def test_missing_source_directory_is_not_an_error(self):
        shutil.rmtree(self.src)
        self.run_script()
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
