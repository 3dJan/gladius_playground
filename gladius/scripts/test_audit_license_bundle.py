import json
import tempfile
import unittest
from pathlib import Path

from audit_license_bundle import audit_profile, sha256_file


class LicenseBundleAuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.triplet_root = self.root / "installed" / "wasm32-emscripten"
        self.profile = {
            "dependencies": ["fmt"],
            "excluded": [{"package": "gtest", "reason": "test-only"}],
        }
        self.inventory = {
            "packages": {"fmt": {"share": "fmt", "notice": "fmt"}},
            "assets": [],
        }
        self._write(self.triplet_root / "share" / "fmt" / "copyright", "fmt notice")
        self._write(self.root / "components" / "licenses" / "fmt" / "LICENSE", "MIT")
        self._write(self.root / "components" / "licenses" / "fmt" / "url", "https://fmt.dev")

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    @staticmethod
    def _write(path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_auditProfile_WithRequiredNoticeAndExcludedTestPackage_Passes(self) -> None:
        self.assertEqual(
            audit_profile(self.root, self.triplet_root, self.profile, self.inventory), []
        )

    def test_auditProfile_WithMissingCuratedUrl_ReportsMissingFile(self) -> None:
        (self.root / "components" / "licenses" / "fmt" / "url").unlink()

        errors = audit_profile(self.root, self.triplet_root, self.profile, self.inventory)

        self.assertEqual(len(errors), 1)
        self.assertIn("curated notice file missing", errors[0])

    def test_auditProfile_WithMissingInstalledCopyright_ReportsPackageGap(self) -> None:
        (self.triplet_root / "share" / "fmt" / "copyright").unlink()

        errors = audit_profile(self.root, self.triplet_root, self.profile, self.inventory)

        self.assertEqual(len(errors), 1)
        self.assertIn("installed package notice missing", errors[0])

    def test_auditProfile_WithMissingSharedLicenseUrl_ReportsSharedNoticeGap(self) -> None:
        self.inventory["packages"]["fmt"]["shared_licenses"] = ["apache-2.0"]
        self.inventory["shared_licenses"] = {
            "apache-2.0": {"notice": "apache-2.0"},
        }
        self._write(
            self.root / "components" / "licenses" / "apache-2.0" / "LICENSE",
            "Apache license text",
        )

        errors = audit_profile(self.root, self.triplet_root, self.profile, self.inventory)

        self.assertEqual(len(errors), 1)
        self.assertIn("shared license file missing or empty", errors[0])

    def test_sha256File_ReturnsStableDigest(self) -> None:
        path = self.root / "font.ttf"
        path.write_bytes(b"test font")

        self.assertEqual(
            sha256_file(path), "c83f1088bdb1c0356983af7e1ec921fae23d583e1686181f1aa0d908d5155305"
        )


if __name__ == "__main__":
    unittest.main()
