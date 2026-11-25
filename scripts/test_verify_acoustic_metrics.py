#!/usr/bin/env python3
"""
Automated Test Suite for verify_acoustic_metrics.py
Tests positive validation, negative injection scenarios, and error formatting
for EDT, T20, and T30 plausibility verification.
"""

import os
import sys
import json
import tempfile
import subprocess
import unittest
import math

class TestVerifyAcousticMetrics(unittest.TestCase):
    def setUp(self):
        self.script_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "verify_acoustic_metrics.py"
        )
        self.tmp_dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp_dir.cleanup()

    def _run_verifier(self, args):
        cmd = [sys.executable, self.script_path] + args
        res = subprocess.run(cmd, capture_output=True, text=True)
        return res.returncode, res.stdout, res.stderr

    def _write_json(self, data):
        p = os.path.join(self.tmp_dir.name, "test_metrics.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        return p

    def test_01_valid_metrics_pass(self):
        """Valid EDT, T20, T30 matching target T60 within tolerance must pass with exit 0."""
        data = {
            "mode": "impulse",
            "sample_rate": 48000.0,
            "duration_sec": 2.0,
            "t60_target_sec": 1.2,
            "fbase_hz": 120.0,
            "peak_l_dbfs": -34.5,
            "peak_r_dbfs": -29.8,
            "rms_l_dbfs": -53.8,
            "rms_r_dbfs": -46.5,
            "edt_l_sec": 1.201,
            "edt_r_sec": 1.199,
            "t20_l_sec": 1.200,
            "t20_r_sec": 1.200,
            "t30_l_sec": 1.200,
            "t30_r_sec": 1.200,
            "clipped": False
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(rc, 0, f"Expected 0 but got {rc}. Stderr: {err}")
        self.assertIn("AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG BESTANDEN", out)
        self.assertIn("Ziel-T60: 1.200 s", out)
        self.assertIn("EDT", out)
        self.assertIn("T20", out)
        self.assertIn("T30", out)

    def test_02_negative_uncalculated_edt(self):
        """Uncalculated EDT (-1.0 s) must fail with exit 1 and detailed diagnostic error output."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": -1.0,
            "edt_r_sec": 1.20,
            "t20_l_sec": 1.20,
            "t20_r_sec": 1.20,
            "t30_l_sec": 1.20,
            "t30_r_sec": 1.20
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("EDT (Links)", err)
        self.assertIn("konnte nicht bestimmt werden", err)
        self.assertIn("Metrik   Kanal Links", err)

    def test_03_negative_uncalculated_t30(self):
        """Uncalculated T30 (-1.0 s) must fail with exit 1 and specific reason."""
        data = {
            "t60_target_sec": 1.5,
            "edt_l_sec": 1.48,
            "edt_r_sec": 1.49,
            "t20_l_sec": 1.50,
            "t20_r_sec": 1.50,
            "t30_l_sec": 1.50,
            "t30_r_sec": -1.0
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("T30 (Rechts)", err)
        self.assertIn("konnte nicht bestimmt werden", err)

    def test_04_negative_nan_or_inf(self):
        """NaN or Inf metrics must fail with exit 1 and numerical instability notice."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": float("nan"),
            "edt_r_sec": 1.20,
            "t20_l_sec": 1.20,
            "t20_r_sec": 1.20,
            "t30_l_sec": 1.20,
            "t30_r_sec": 1.20
        }
        p = os.path.join(self.tmp_dir.name, "nan_metrics.json")
        with open(p, "w", encoding="utf-8") as f:
            f.write('{"t60_target_sec": 1.2, "edt_l_sec": NaN, "edt_r_sec": 1.2, "t20_l_sec": 1.2, "t20_r_sec": 1.2, "t30_l_sec": 1.2, "t30_r_sec": 1.2}')
        rc, out, err = self._run_verifier(["--metrics-json", p])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("nicht-finit", err)

    def test_05_negative_t60_out_of_tolerance(self):
        """Values deviating beyond tolerance (default 15%) must fail with exit 1."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": 1.20,
            "edt_r_sec": 1.20,
            "t20_l_sec": 1.20,
            "t20_r_sec": 1.20,
            "t30_l_sec": 1.20,
            "t30_r_sec": 0.85 # -29.2% error
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", "0.15"])
        self.assertEqual(rc, 1)
        self.assertIn("FEHLER: AKUSTISCHE METRIK-PLAUSIBILITÄTSPRÜFUNG FEHLGESCHLAGEN", err)
        self.assertIn("T30_R", err)
        self.assertIn("weicht um", err)
        self.assertIn("vom Zielwert T60", err)

    def test_06_negative_stereo_asymmetry(self):
        """Asymmetry between L and R exceeding stereo_tolerance must fail with exit 1."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": 1.10,
            "edt_r_sec": 1.30,
            "t20_l_sec": 1.05,
            "t20_r_sec": 1.32, # 20.5% diff
            "t30_l_sec": 1.20,
            "t30_r_sec": 1.20
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--stereo-tolerance", "0.10"])
        self.assertEqual(rc, 1)
        self.assertIn("Stereo-Asymmetrie bei T20", err)

    def test_07_negative_decay_profile_divergence(self):
        """Severe divergence between EDT and T20/T30 indicating abnormal decay curve must fail."""
        data = {
            "t60_target_sec": 1.2,
            "edt_l_sec": 1.35, # +28.6% vs T20
            "edt_r_sec": 1.05,
            "t20_l_sec": 1.05,
            "t20_r_sec": 1.05,
            "t30_l_sec": 1.05,
            "t30_r_sec": 1.05
        }
        json_path = self._write_json(data)
        rc, out, err = self._run_verifier(["--metrics-json", json_path, "--tolerance", "0.30", "--decay-tolerance", "0.20"])
        self.assertEqual(rc, 1)
        self.assertIn("Inkonsistente Abklingkurve", err)

    def test_08_missing_file_exit_2(self):
        """Missing input file must return exit code 2."""
        rc, out, err = self._run_verifier(["--metrics-json", "non_existent_file.json"])
        self.assertEqual(rc, 2)
        self.assertIn("[ERROR]", err)

    def test_09_corrupted_json_exit_2(self):
        """Corrupted JSON syntax must return exit code 2."""
        p = os.path.join(self.tmp_dir.name, "bad.json")
        with open(p, "w") as f:
            f.write("{invalid-json-content")
        rc, out, err = self._run_verifier(["--metrics-json", p])
        self.assertEqual(rc, 2)
        self.assertIn("[ERROR]", err)

    def test_10_missing_args_exit_2(self):
        """Running with no inputs must return exit code 2."""
        rc, out, err = self._run_verifier([])
        self.assertEqual(rc, 2)
        self.assertIn("[ERROR]", err)

if __name__ == "__main__":
    unittest.main()
