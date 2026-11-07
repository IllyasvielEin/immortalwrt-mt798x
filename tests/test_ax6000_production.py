"""Regression checks for accidental diagnostic inclusion in production builds."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "production_check", ROOT / "scripts/check-ax6000-production.py"
)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


class ProductionCheckTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.patches = Path(self.temp.name) / "patches"
        self.patches.mkdir()
        (self.patches / "001-normal.patch").write_text("normal driver fix\n")

    def test_accepts_production_sources_and_normal_symbols(self):
        symbols = Path(self.temp.name) / "module.symbols"
        symbols.write_text("00000000 T WNM_ReadParametersFromFile\n")
        CHECK.check(self.patches, symbols)

    def test_rejects_renamed_diagnostic_code(self):
        for marker in ("ROAMDIAG", "roam_diag_mac", "mt_roam_diag_packet"):
            with self.subTest(marker=marker):
                (self.patches / "099-ordinary-name.patch").write_text(marker)
                with self.assertRaises(ValueError):
                    CHECK.check(self.patches)

    def test_rejects_diagnostic_filename(self):
        (self.patches / "043-roaming-path-diagnostics.patch").write_text("text\n")
        with self.assertRaises(ValueError):
            CHECK.check(self.patches)

    def test_rejects_compiled_parameter(self):
        symbols = Path(self.temp.name) / "module.symbols"
        symbols.write_text("00000000 r __param_roam_diag_mac\n")
        with self.assertRaises(ValueError):
            CHECK.check(self.patches, symbols)

    def test_missing_inputs_fail_closed(self):
        with self.assertRaises(ValueError):
            CHECK.check(self.patches / "missing")
        with self.assertRaises(OSError):
            CHECK.check(self.patches, Path(self.temp.name) / "missing.symbols")


if __name__ == "__main__":
    unittest.main()
