#!/usr/bin/env python3
"""Regressions for the cursor-warp package dependency, without installing packages."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = ROOT / "linux/packages"
# Revision and Aquamarine requirement come from the regeneration script, the single source of truth.
_REGEN = (PACKAGES / "regen-pkgbuilds.sh").read_text()
REL = re.search(r"^HYPRLAND_REL=(\d+)", _REGEN, re.M).group(1)
REQUIREMENT = re.search(r"--require '([^']+)'", _REGEN).group(1)


class HyprlandPkgbuildTests(unittest.TestCase):
    def generate(self, source, output, requirement=REQUIREMENT):
        return subprocess.run(
            [sys.executable, str(PACKAGES / "derive-pkgbuild.py"),
             str(source), str(output), "hyprland-source", "--rel", REL,
             "--require", requirement, "--only", "hyprland", "--",
             *map(str, sorted((PACKAGES / "hyprland").glob("0*.patch")))],
            capture_output=True, text=True, check=False,
        )

    def test_committed_pkgbuild_is_reproducible(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "PKGBUILD"
            result = self.generate(PACKAGES / "ref/hyprland.PKGBUILD", output)
            self.assertEqual(result.returncode, 0, result.stderr)
            text = output.read_text()
            self.assertEqual(text, (PACKAGES / "hyprland/PKGBUILD").read_text())
            self.assertIn(f"'{REQUIREMENT}' libaquamarine.so", text)
            self.assertRegex(text, rf"\npkgrel=\d+\.{REL}\n")
            self.assertEqual(subprocess.run(["bash", "-n", str(output)]).returncode, 0)

    def test_changed_dependency_layout_fails_closed(self):
        original = (PACKAGES / "ref/hyprland.PKGBUILD").read_text()
        for replacement in ("aquamarine-new libaquamarine.so",
                            "# aquamarine libaquamarine.so",
                            "aquamarine libaquamarine.so\n         aquamarine"):
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory() as tmp:
                source, output = Path(tmp) / "ref", Path(tmp) / "PKGBUILD"
                source.write_text(original.replace("aquamarine libaquamarine.so", replacement, 1))
                result = self.generate(source, output)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("expected one bare runtime dependency", result.stderr)
                self.assertFalse(output.exists())

    def test_invalid_requirement_is_rejected(self):
        for requirement in ("aquamarine", "aquamarine>=1'; touch /tmp/not-run", "aquamarine >=1"):
            with self.subTest(requirement=requirement), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "PKGBUILD"
                result = self.generate(PACKAGES / "ref/hyprland.PKGBUILD", output, requirement)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("invalid versioned dependency", result.stderr)
                self.assertFalse(output.exists())

    def test_packaged_protocol_header_matches_shared_specification(self):
        # Reconstruct just the header from the exported patches, without source clones or networking.
        with tempfile.TemporaryDirectory() as tmp:
            for patch in sorted((PACKAGES / "aquamarine").glob("0*.patch")):
                result = subprocess.run(
                    ["git", "apply", "--include=src/backend/wsl/wdp.h", str(patch)],
                    cwd=tmp, capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
            header = Path(tmp) / "src/backend/wsl/wdp.h"
            self.assertEqual(header.read_bytes(), (ROOT / "protocol/wdp.h").read_bytes())


if __name__ == "__main__":
    unittest.main()
