#!/usr/bin/env python3
"""Regression tests for linux/overlay/ghostty-gl (Ghostty on llvmpipe under the d3d12 GPU path)."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "linux/overlay/ghostty-gl"
VARS = ("GALLIUM_DRIVER", "MESA_LOADER_DRIVER_OVERRIDE")


class GhosttyGlTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        # stands in for /usr/bin/ghostty: prints its arguments and the GPU variables
        self.fake = self.tmp / "ghostty"
        self.fake.write_text("#!/bin/sh\n"
                             'printf "arg=%s\\n" "$@"\n'
                             'echo "GALLIUM_DRIVER=${GALLIUM_DRIVER-<unset>}"\n'
                             'echo "MESA_LOADER_DRIVER_OVERRIDE=${MESA_LOADER_DRIVER_OVERRIDE-<unset>}"\n')
        self.fake.chmod(0o755)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def run_wrapper(self, *args, **env):
        base = {k: v for k, v in os.environ.items() if k not in VARS}
        base["WOMARCHY_GHOSTTY_BIN"] = str(self.fake)
        out = subprocess.run(["sh", str(SCRIPT), *args], env={**base, **env},
                             capture_output=True, text=True, check=True).stdout
        return out.splitlines()

    def test_d3d12_becomes_llvmpipe(self):
        out = self.run_wrapper(GALLIUM_DRIVER="d3d12")
        self.assertIn("GALLIUM_DRIVER=llvmpipe", out)

    def test_vgem_loader_override_dropped(self):
        out = self.run_wrapper(GALLIUM_DRIVER="d3d12", MESA_LOADER_DRIVER_OVERRIDE="kms_swrast")
        self.assertIn("MESA_LOADER_DRIVER_OVERRIDE=<unset>", out)

    def test_other_loader_override_kept(self):
        out = self.run_wrapper(GALLIUM_DRIVER="d3d12", MESA_LOADER_DRIVER_OVERRIDE="zink")
        self.assertIn("MESA_LOADER_DRIVER_OVERRIDE=zink", out)

    def test_without_d3d12_nothing_changes(self):
        out = self.run_wrapper(MESA_LOADER_DRIVER_OVERRIDE="kms_swrast")
        self.assertIn("GALLIUM_DRIVER=<unset>", out)
        self.assertIn("MESA_LOADER_DRIVER_OVERRIDE=kms_swrast", out)
        out = self.run_wrapper(GALLIUM_DRIVER="zink", MESA_LOADER_DRIVER_OVERRIDE="kms_swrast")
        self.assertIn("GALLIUM_DRIVER=zink", out)
        self.assertIn("MESA_LOADER_DRIVER_OVERRIDE=kms_swrast", out)

    def test_arguments_passed_unchanged(self):
        args = ["--gtk-single-instance=true", "-e", "sh", "-c", "echo 'a b'  \"$HOME\""]
        out = self.run_wrapper(*args, GALLIUM_DRIVER="d3d12")
        self.assertEqual([line[4:] for line in out if line.startswith("arg=")], args)


if __name__ == "__main__":
    unittest.main()
