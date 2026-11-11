#!/usr/bin/env python3
"""Regression tests for the Mesa rebuild, without installing packages."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ROOT / "linux/packages"


# The revision comes from the regeneration script, the single source of truth.
REL = re.search(r"^MESA_REL=(\d+)", (Path(__file__).resolve().parents[1] / "linux/packages/regen-pkgbuilds.sh").read_text(), re.M).group(1)

class MesaPkgbuildTests(unittest.TestCase):
    def generate(self, source, output):
        return subprocess.run(
            [sys.executable, str(PACKAGES / "mesa-womarchy/make-pkgbuild.py"),
             "--rel", REL, str(source), str(output),
             *map(str, sorted((PACKAGES / "mesa-womarchy").glob("0*.patch")))],
            capture_output=True, text=True, check=False,
        )

    def test_committed_pkgbuild_is_reproducible(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "PKGBUILD"
            result = self.generate(PACKAGES / "ref/mesa.PKGBUILD", output)
            self.assertEqual(result.returncode, 0, result.stderr)
            text = output.read_text()
            self.assertEqual(text, (PACKAGES / "mesa-womarchy/PKGBUILD").read_text())
            self.assertRegex(text, rf"\npkgrel=\d+\.{REL}\n")
            self.assertEqual(text.count("    libLLVM.so\n"), 2)
            # The build dependency remains a normal package dependency.
            self.assertIn("  llvm\n  llvm-libs\n", text)
            result = subprocess.run(["bash", "-n", str(output)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_changed_upstream_dependency_layout_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "reference"
            output = Path(tmp) / "PKGBUILD"
            text = (PACKAGES / "ref/mesa.PKGBUILD").read_text()
            source.write_text(text.replace("    llvm-libs\n", "    llvm-new-libs\n", 1))
            result = self.generate(source, output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("expected Mesa and lavapipe LLVM dependencies", result.stderr)
            self.assertFalse(output.exists())

    def test_fetch_reports_missing_cmp_before_network_or_crypto(self):
        with tempfile.TemporaryDirectory() as tmp:
            bindir = Path(tmp) / "bin"
            bindir.mkdir()
            # Only dirname is executed before the dependency check. The other
            # commands are placeholders: no networking or key import may occur.
            (bindir / "dirname").symlink_to(shutil.which("dirname"))
            for name in ("gpg", "curl", "bsdtar", "sha256sum"):
                stub = bindir / name
                stub.write_text("#!/bin/sh\necho 'unexpected tool execution' >&2\nexit 99\n")
                stub.chmod(0o755)
            result = subprocess.run(
                [shutil.which("bash"), str(PACKAGES / "fetch-signed-db.sh"), tmp],
                env={**os.environ, "PATH": str(bindir)}, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("missing cmp", result.stderr)
            self.assertNotIn("differ", result.stderr)
            self.assertNotIn("unexpected tool execution", result.stderr)

    def test_snapshot_report_distinguishes_health_from_tool_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            bindir = Path(tmp)
            (bindir / "pacman").write_text(
                '#!/bin/sh\necho "routine pacman output"\n'
                'if [ "$1" = -Syyuu ] && [ "$MODE" = sync-failure ]; then exit 1; fi\n'
                'if [ "$1" = -S ] && [ "$MODE" = install-failure ]; then exit 1; fi\n'
            )
            (bindir / "ldd").write_text(
                '#!/bin/sh\ncase "$MODE" in\n'
                'missing) echo "libLLVM.so.22.1 => not found" ;;\n'
                'ldd-failure) echo "ldd error" >&2; exit 1 ;;\n'
                '*) echo "libLLVM.so.23.1 => /usr/lib/libLLVM.so.23.1" ;;\nesac\n'
            )
            for path in bindir.iterdir():
                path.chmod(0o755)
            expected = {
                "healthy": "", "missing": "libLLVM.so.22.1 => not found",
                "ldd-failure": "ldd failed", "sync-failure": "moving the container",
                "install-failure": "packages no longer install",
            }
            for mode, marker in expected.items():
                with self.subTest(mode=mode):
                    result = subprocess.run(
                        ["bash", str(ROOT / "tools/snapshot-report.sh")],
                        env={**os.environ, "PATH": f"{bindir}:{os.environ['PATH']}", "MODE": mode},
                        capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if marker:
                        self.assertIn(marker, result.stdout)
                    else:
                        self.assertEqual(result.stdout, "")

    def test_new_scripts_parse(self):
        for name in ("publish-packages.sh", "check-mesa-llvm.sh", "snapshot-report.sh"):
            with self.subTest(script=name):
                result = subprocess.run(["bash", "-n", str(ROOT / "tools" / name)],
                                        capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
