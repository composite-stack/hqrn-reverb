#!/usr/bin/env python3
"""
Unit and regression test suite for scripts/verify_markdown_anchors.py

Covers:
1. Basic explicit HTML anchors and Markdown headings
2. Relative links (Markdown & Mermaid):
   - Intra-file relative links (./file.md#anchor and file.md#anchor)
   - Cross-file relative links to existing files with valid anchors
   - Rejection of missing relative files or missing anchors in relative files
3. URL-encoded anchors (Markdown & Mermaid):
   - %2D for dashes, UTF-8 percent-encoding for umlauts (%C3%BC for ü, etc.)
   - Proper resolution and rejection of invalid encoded targets
4. Duplicate IDs:
   - Detection of duplicate explicit HTML IDs
   - Default warning behavior vs. strict failure with --strict-duplicates
   - Disambiguation of duplicate Markdown headings (-1, -2) and targeted linking
5. Exit-Codes & Standardfehlerausgabe (stderr) - Kategorie 1: Ungültige Prozentkodierung
   - Exit-Code 1 bei RFC-3986-Syntaxfehlern (%ZZ, %2, trailing %)
   - Vollständige Formatierung auf stderr ([FAIL], Line <N>, Parser-Grund)
   - Keine Fehlermeldungen auf stdout
6. Exit-Codes & Standardfehlerausgabe (stderr) - Kategorie 2: Fehlende Zieldateien & Ordner
   - Exit-Code 1 bei fehlenden Dateien in Root & Unterordnern
   - Exit-Code 1 bei Verzeichnissen als Linkziel
   - Eindeutige Fehlermeldungen auf stderr
7. Exit-Codes & Standardfehlerausgabe (stderr) - Kategorie 3: Leere Anker & Ziel-URLs
   - Exit-Code 1 bei '#', 'file.md#' und leeren Ziel-URLs '()'
   - Eindeutige Fehlermeldung 'Empty anchor reference' auf stderr
8. Exit-Codes & Standardfehlerausgabe (stderr) - CLI- und Eingabefehler
   - Exit-Code 2 bei nicht existierender Eingabedatei mit '[ERROR] Target file not found' auf stderr
   - Exit-Code 2 bei unbekannten CLI-Optionen mit 'unrecognized arguments' auf stderr
9. Gültige Unicode-Anker & mehrbytekodierte Prozentsequenzen (2-Byte, 3-Byte, 4-Byte)
   - Literale Unicode-Anker (Griechisch λ/θ, Deutsch ä/ö/ü/ß, CJK 日本語, Währung €, Emoji 🚀)
   - Valide UTF-8 Prozentkodierungen (%CE%BB, %E6%97..., %F0%9F%9A%80) in Markdown & Mermaid
   - Exit-Code 0, leerer stderr, '[OK]' auf stdout
10. Grenzfälle bei mehrbytekodierten Prozentsequenzen & Ablehnungsdiagnose
   - Abgeschnittene Mehrbyte-Sequenzen (2-Byte, 3-Byte, 4-Byte)
   - Ungültige Continuation-Bytes (%C3%28, %E6%97%28)
   - Overlong-Encoding (%C0%AF, %C1%BF) und Out-of-Range (> U+10FFFF: %F5%80%80%80)
   - Exit-Code 1, genaue UTF-8 Fehlerdiagnose auf stderr
11. Produktionsdokument (DIAGNOSTIC_METADATA_TESTING.md)
   - Volle Prüfung mit --required und --strict-duplicates
"""

import unittest
import tempfile
import os
import subprocess
import sys
import shutil
import urllib.parse

SCRIPT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "verify_markdown_anchors.py")

class TestVerifyMarkdownAnchors(unittest.TestCase):

    def run_verifier(self, md_content: str, extra_args=None, custom_dir=None, filename="test.md"):
        target_dir = custom_dir or tempfile.gettempdir()
        temp_path = os.path.join(target_dir, filename)
        with open(temp_path, "w", encoding="utf-8") as f:
            f.write(md_content)
        try:
            cmd = [sys.executable, SCRIPT_PATH, temp_path]
            if extra_args:
                cmd.extend(extra_args)
            res = subprocess.run(cmd, capture_output=True, text=True)
            return res.returncode, res.stdout, res.stderr
        finally:
            if not custom_dir and os.path.exists(temp_path):
                os.remove(temp_path)

    # =========================================================================
    # 1. Basis-Funktionalität
    # =========================================================================
    def test_basic_explicit_anchors_and_mermaid(self):
        content = """# Header
<a id="beispielpfad-1"></a>
### Pfad 1
[Springe zu Pfad 1](#beispielpfad-1)
```mermaid
graph TD
A --> B
click A href "#beispielpfad-1" "Triage 1"
```
"""
        code, out, err = self.run_verifier(content, ["--required", "beispielpfad-1"])
        self.assertEqual(code, 0)
        self.assertEqual(err, "", "Stderr must be empty on success")
        self.assertIn("[OK] All 2 internal anchor references successfully resolved", out)

    def test_broken_markdown_and_mermaid_links(self):
        content = """# Header
<a id="valid-anchor"></a>
[Kaputter Link](#non-existent-anchor)
```mermaid
graph TD
A --> B
click A href "#broken-mermaid-target" "Kaputt"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 1)
        self.assertIn("Found 2 broken anchor references", err)
        self.assertIn("#non-existent-anchor", err)
        self.assertIn("#broken-mermaid-target", err)
        self.assertNotIn("[OK]", out)

    # =========================================================================
    # 2. Relative Links (Markdown & Mermaid)
    # =========================================================================
    def test_relative_links_same_file(self):
        content = """# Dokumentation
<a id="ziel-abschnitt"></a>
## Ziel

[Relativer Link mit Punkt](./doc_test.md#ziel-abschnitt)
[Relativer Link ohne Punkt](doc_test.md#ziel-abschnitt)

```mermaid
graph TD
M1[Knoten 1] --> M2[Knoten 2]
click M1 href "./doc_test.md#ziel-abschnitt" "Springe relativ"
click M2 href "doc_test.md#ziel-abschnitt" "Springe relativ ohne Punkt"
```
"""
        code, out, err = self.run_verifier(content, filename="doc_test.md")
        self.assertEqual(code, 0, f"Failed on intra-file relative links:\n{err}")
        self.assertEqual(err, "")
        self.assertIn("[OK] All 4 internal anchor references successfully resolved", out)

    def test_relative_links_cross_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            file_b = os.path.join(tmpdir, "guide.md")
            with open(file_b, "w", encoding="utf-8") as f:
                f.write("""# Externe Anleitung\n<a id="guide-step-2"></a>\n## Schritt 2\n""")

            content_a = """# Hauptdokument
[Verweis auf Guide](./guide.md#guide-step-2)

```mermaid
graph TD
Start --> Guide
click Guide href "./guide.md#guide-step-2" "Öffne Schritt 2 in Guide"
```
"""
            file_a = os.path.join(tmpdir, "main.md")
            with open(file_a, "w", encoding="utf-8") as f:
                f.write(content_a)

            cmd = [sys.executable, SCRIPT_PATH, file_a]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Cross-file validation failed:\n{res.stderr}")
            self.assertEqual(res.stderr, "")
            self.assertIn("[OK] All 2 internal anchor references successfully resolved", res.stdout)

    def test_relative_links_broken_target_file_or_anchor(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            file_b = os.path.join(tmpdir, "existing.md")
            with open(file_b, "w", encoding="utf-8") as f:
                f.write("""# Vorhanden\n<a id="real-anchor"></a>\n""")

            content_a = """# Hauptdokument
[Guter Link](./existing.md#real-anchor)
[Fehlende Datei](./missing_file.md#some-anchor)
[Fehlender Anker in existierender Datei](./existing.md#non-existent-anchor)

```mermaid
graph TD
A --> B
click A href "./missing_file.md#some-anchor" "Fehlende Datei"
click B href "./existing.md#non-existent-anchor" "Fehlender Anker"
```
"""
            file_a = os.path.join(tmpdir, "main.md")
            with open(file_a, "w", encoding="utf-8") as f:
                f.write(content_a)

            cmd = [sys.executable, SCRIPT_PATH, file_a]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 1)
            self.assertIn("Found 4 broken anchor references", res.stderr)
            self.assertIn("Target file does not exist: ./missing_file.md", res.stderr)
            self.assertIn("Anchor #non-existent-anchor not found in existing.md", res.stderr)
            self.assertNotIn("[OK]", res.stdout)

    # =========================================================================
    # 3. URL-kodierte Anker (Markdown & Mermaid)
    # =========================================================================
    def test_url_encoded_anchors_resolution(self):
        content = """# Dokumentation
<a id="beispielpfad-1"></a>
#### Stufe 1: Ausführung & Prüfung

[Link mit %2D](#beispielpfad%2D1)
[Link mit Umlauten](#stufe%2D1%2Dausf%C3%BChrung%2Dpr%C3%BCfung)

```mermaid
graph TD
N1 --> N2
click N1 href "#beispielpfad%2D1" "Encodierter Klick"
click N2 href "#stufe%2D1%2Dausf%C3%BChrung%2Dpr%C3%BCfung" "Encodierter Heading-Klick"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 0, f"URL-encoded anchors failed to resolve:\n{err}")
        self.assertEqual(err, "")
        self.assertIn("[OK] All 4 internal anchor references successfully resolved", out)

    def test_url_encoded_broken_anchor_rejection(self):
        content = """# Dokumentation
<a id="gueltig"></a>
[Kaputter URL-Link](#ung%C3%BCltiges%2Dziel)

```mermaid
graph TD
A --> B
click A href "#nicht%2Dvorhanden" "Kaputter Klick"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 1)
        self.assertIn("Found 2 broken anchor references", err)
        self.assertIn("#ung%C3%BCltiges%2Dziel", err)
        self.assertIn("#nicht%2Dvorhanden", err)
        self.assertNotIn("[OK]", out)

    # =========================================================================
    # 4. Mehrfach verwendete IDs (Duplikate)
    # =========================================================================
    def test_duplicate_explicit_id_warning(self):
        content = """# Dokumentation
<a id="dup-anchor"></a>
### Abschnitt 1
<a id="dup-anchor"></a>
### Abschnitt 2

[Link auf Duplikat](#dup-anchor)
```mermaid
graph TD
A --> B
click A href "#dup-anchor" "Mermaid Klick auf Duplikat"
```
"""
        # Standard: Warnung, Exit-Code 0
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 0)
        self.assertIn("[WARNING] Duplicate explicit anchor definition 'dup-anchor'", out)
        self.assertIn("[OK] All 2 internal anchor references successfully resolved", out)

        # Strict: Fehler, Exit-Code 1
        code_strict, out_strict, err_strict = self.run_verifier(content, ["--strict-duplicates"])
        self.assertEqual(code_strict, 1)
        self.assertIn("[ERROR - STRICT] [WARNING] Duplicate explicit anchor definition 'dup-anchor'", err_strict)

    def test_duplicate_markdown_headings_disambiguation(self):
        content = """# Dokumentation
## Konfiguration
Inhalt 1
## Konfiguration
Inhalt 2

[Erste Konfiguration](#konfiguration)
[Zweite Konfiguration](#konfiguration-1)

```mermaid
graph TD
C1 --> C2
click C1 href "#konfiguration" "Zur ersten Konfiguration"
click C2 href "#konfiguration-1" "Zur zweiten Konfiguration"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 0, f"Duplicate heading disambiguation failed:\n{err}")
        self.assertEqual(err, "")
        self.assertIn("[OK] All 4 internal anchor references successfully resolved", out)

    # =========================================================================
    # 5. Exit-Codes & stderr - Kategorie 1: Ungültige Prozentkodierungen
    # =========================================================================
    def test_exit_code_and_stderr_invalid_percent_encoding(self):
        content = """# Dokumentation
<a id="gueltiger-anker"></a>

[Link mit ungültigen Hex-Zeichen](#kaputt%ZZ)
[Link mit unvollständigem Prozent-Token](#teil%2)
[Link mit Prozent am Zeilenende](#ende%)

```mermaid
graph TD
A --> B
click A href "#mermaid%GG" "Ungültiges Hex in Mermaid"
```
"""
        code, out, err = self.run_verifier(content)
        # 1. Exit-Code muss exakt 1 sein
        self.assertEqual(code, 1, "Exit code must be 1 on invalid percent-encodings")

        # 2. Standardfehlerausgabe (stderr) formatieren und prüfen
        self.assertIn("[FAIL] Found 4 broken anchor references:", err)
        self.assertIn("Line 4 [Markdown link]: [Link mit ungültigen Hex-Zeichen] -> #kaputt%ZZ (Invalid percent-encoding in anchor #kaputt%ZZ: malformed percent-encoding syntax", err)
        self.assertIn("Line 5 [Markdown link]: [Link mit unvollständigem Prozent-Token] -> #teil%2 (Invalid percent-encoding in anchor #teil%2: malformed percent-encoding syntax", err)
        self.assertIn("Line 6 [Markdown link]: [Link mit Prozent am Zeilenende] -> #ende% (Invalid percent-encoding in anchor #ende%: malformed percent-encoding syntax", err)
        self.assertIn("Line 11 [Mermaid click]: node A -> #mermaid%GG (Invalid percent-encoding in anchor #mermaid%GG: malformed percent-encoding syntax", err)

        # 3. Standardausgabe (stdout) darf keine Fehler enthalten und kein [OK] melden
        self.assertIn("=== Verifying anchors in:", out)
        self.assertIn("Found 2 defined target anchors/slugs.", out)
        self.assertNotIn("[OK]", out)
        self.assertNotIn("[FAIL]", out)

    # =========================================================================
    # 6. Exit-Codes & stderr - Kategorie 2: Fehlende Zieldateien & Verzeichnisse
    # =========================================================================
    def test_exit_code_and_stderr_missing_target_files_and_directories(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            subdir = os.path.join(tmpdir, "existing_subfolder")
            os.makedirs(subdir, exist_ok=True)

            content = """# Dokumentation
[Link auf fehlende Datei im selben Ordner](./absent_file.md#anker)
[Link auf fehlende Datei in Unterordner](./existing_subfolder/missing_sub.md#anker)
[Link auf Verzeichnis statt Datei](./existing_subfolder#anker)

```mermaid
graph TD
N1 --> N2
click N1 href "./absent_diagram_target.md#diagram" "Fehlende Diagrammdatei"
```
"""
            main_file = os.path.join(tmpdir, "main.md")
            with open(main_file, "w", encoding="utf-8") as f:
                f.write(content)

            cmd = [sys.executable, SCRIPT_PATH, main_file]
            res = subprocess.run(cmd, capture_output=True, text=True)

            # 1. Exit-Code muss exakt 1 sein
            self.assertEqual(res.returncode, 1, "Exit code must be 1 on missing target files")

            # 2. Standardfehlerausgabe (stderr) prüfen
            self.assertIn("[FAIL] Found 4 broken anchor references:", res.stderr)
            self.assertIn("Target file does not exist: ./absent_file.md", res.stderr)
            self.assertIn("Target file does not exist: ./existing_subfolder/missing_sub.md", res.stderr)
            self.assertIn("Target path is a directory, not a file: ./existing_subfolder", res.stderr)
            self.assertIn("Target file does not exist: ./absent_diagram_target.md", res.stderr)

            # 3. Standardausgabe prüfen
            self.assertIn("=== Verifying anchors in:", res.stdout)
            self.assertNotIn("[OK]", res.stdout)
            self.assertNotIn("[FAIL]", res.stdout)

    # =========================================================================
    # 7. Exit-Codes & stderr - Kategorie 3: Leere Anker & Ziel-URLs
    # =========================================================================
    def test_exit_code_and_stderr_empty_anchors(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            file_b = os.path.join(tmpdir, "doc_target.md")
            with open(file_b, "w", encoding="utf-8") as f:
                f.write("# Ziel\n<a id='echter-anker'></a>\n")

            content = """# Dokumentation
[Link mit alleinigem Rautezeichen](#)
[Link auf Datei ohne Anker-ID](./doc_target.md#)
[Link mit komplett leerem URL-Ziel]()

```mermaid
graph TD
M1 --> M2
M3 --> M4
click M1 href "#" "Leerer Klick"
click M2 href "./doc_target.md#" "Klick auf Datei ohne Anker"
click M3 href "" "Komplett leerer Klick"
```
"""
            main_file = os.path.join(tmpdir, "main.md")
            with open(main_file, "w", encoding="utf-8") as f:
                f.write(content)

            cmd = [sys.executable, SCRIPT_PATH, main_file]
            res = subprocess.run(cmd, capture_output=True, text=True)

            # 1. Exit-Code muss exakt 1 sein
            self.assertEqual(res.returncode, 1, "Exit code must be 1 on empty anchors")

            # 2. Standardfehlerausgabe (stderr) prüfen
            self.assertIn("[FAIL] Found 6 broken anchor references:", res.stderr)
            self.assertIn("Line 2 [Markdown link]: [Link mit alleinigem Rautezeichen] -> # (Empty anchor reference: '#' (no anchor ID specified))", res.stderr)
            self.assertIn("Line 3 [Markdown link]: [Link auf Datei ohne Anker-ID] -> ./doc_target.md# (Empty anchor reference: './doc_target.md#' (no anchor ID specified))", res.stderr)
            self.assertIn("Line 10 [Mermaid click]: node M1 -> # (Empty anchor reference: '#' (no anchor ID specified))", res.stderr)
            self.assertIn("Line 11 [Mermaid click]: node M2 -> ./doc_target.md# (Empty anchor reference: './doc_target.md#' (no anchor ID specified))", res.stderr)

            # 3. Standardausgabe prüfen
            self.assertNotIn("[OK]", res.stdout)
            self.assertNotIn("[FAIL]", res.stdout)

    # =========================================================================
    # 8. Exit-Codes & stderr - CLI- und Argumentfehler (Exit-Code 2)
    # =========================================================================
    def test_exit_code_and_stderr_cli_and_missing_input_file(self):
        # Fall A: Eingabedatei existiert nicht
        cmd_missing = [sys.executable, SCRIPT_PATH, "/path/does/not/exist/missing_input.md"]
        res_missing = subprocess.run(cmd_missing, capture_output=True, text=True)
        self.assertEqual(res_missing.returncode, 2, "Exit code must be 2 when input file does not exist")
        self.assertIn("[ERROR] Target file not found: /path/does/not/exist/missing_input.md", res_missing.stderr)
        self.assertEqual(res_missing.stdout.strip(), "")

        # Fall B: Unbekannte CLI-Option
        cmd_bad_arg = [sys.executable, SCRIPT_PATH, "dummy.md", "--unbekanntes-flag"]
        res_bad_arg = subprocess.run(cmd_bad_arg, capture_output=True, text=True)
        self.assertEqual(res_bad_arg.returncode, 2, "Exit code must be 2 on unrecognized CLI arguments")
        self.assertIn("unrecognized arguments: --unbekanntes-flag", res_bad_arg.stderr)

    # =========================================================================
    # 9. Gültige Unicode-Anker & mehrbytekodierte Prozentsequenzen (2-, 3-, 4-Byte)
    # =========================================================================
    def test_unicode_anchors_and_multibyte_percent_encoding_success(self):
        content = """# Unicode und DSP Dokumentation

## Griechische Resonanz: λ-Moden & θ-Phasen
Inhalt zu Eigenwerten...

## CJK-Abschnitt: 日本語とDSPアルゴリズム
Inhalt zu FFT...

<a id="audio-dsp-🚀-fasttrack"></a>
### High-Performance Audio Pipeline
Inhalt zu Pipeline...

<a id="währung-€-und-akzente-élévation"></a>
### Währung & Akzente

<!-- 1. Literale Unicode-Links -->
[Link zu Griechisch](#griechische-resonanz-λ-moden-θ-phasen)
[Link zu CJK](#cjk-abschnitt-日本語とdspアルゴリズム)
[Link zu Emoji-Anchor](#audio-dsp-🚀-fasttrack)
[Link zu Währung und Akzenten](#währung-€-und-akzente-élévation)

<!-- 2. Prozentkodierte Mehrbyte-Links (2-Byte, 3-Byte, 4-Byte) -->
[2-Byte Griechisch (%CE%BB & %CE%B8)](#griechische-resonanz-%CE%BB-moden-%CE%B8-phasen)
[3-Byte CJK (%E6%97%A5...)](#cjk-abschnitt-%E6%97%A5%E6%9C%AC%E8%AA%9E%E3%81%A8dsp%E3%82%A2%E3%83%AB%E3%82%B4%E3%83%AA%E3%82%BA%E3%83%A0)
[4-Byte Emoji (%F0%9F%9A%80)](#audio-dsp-%F0%9F%9A%80-fasttrack)
[3-Byte Euro & 2-Byte Akzente](#w%C3%A4hrung-%E2%82%AC-und-akzente-%C3%A9l%C3%A9vation)

```mermaid
graph TD
G1 --> G2
J1 --> J2
E1 --> E2
click G1 href "#griechische-resonanz-λ-moden-θ-phasen" "Griechisch Literal"
click G2 href "#griechische-resonanz-%CE%BB-moden-%CE%B8-phasen" "Griechisch 2-Byte Encodiert"
click J1 href "#cjk-abschnitt-日本語とdspアルゴリズム" "CJK Literal"
click J2 href "#cjk-abschnitt-%E6%97%A5%E6%9C%AC%E8%AA%9E%E3%81%A8dsp%E3%82%A2%E3%83%AB%E3%82%B4%E3%83%AA%E3%82%BA%E3%83%A0" "CJK 3-Byte Encodiert"
click E1 href "#audio-dsp-🚀-fasttrack" "Emoji Literal"
click E2 href "#audio-dsp-%F0%9F%9A%80-fasttrack" "Emoji 4-Byte Encodiert"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 0, f"Unicode validation failed:\n{err}")
        self.assertEqual(err, "", "Standard error stream must be completely empty on success")
        self.assertIn("[OK] All 14 internal anchor references successfully resolved", out)

    # =========================================================================
    # 10. Grenzfälle bei mehrbytekodierten Prozentsequenzen
    # =========================================================================
    def test_multibyte_percent_encoding_edge_cases_and_rejections(self):
        content = """# Mehrbyte Grenzfälle
<a id="ziel"></a>

[Abgeschnittene 2-Byte Sequenz](#trunc2-%C3)
[Abgeschnittene 3-Byte Sequenz](#trunc3-%E6%97)
[Abgeschnittene 4-Byte Sequenz](#trunc4-%F0%9F%9A)
[Ungültiges Continuation-Byte in 2-Byte](#badcont2-%C3%28)
[Ungültiges Continuation-Byte in 3-Byte](#badcont3-%E6%97%28)
[Overlong UTF-8 Encoding](#overlong-%C0%AF)
[Out-of-range UTF-8 Codepoint](#outofrange-%F5%80%80%80)

```mermaid
graph TD
M1 --> M2
click M1 href "#mermaid-trunc-%F0%9F" "Mermaid Truncated 4-Byte"
click M2 href "#mermaid-overlong-%C1%BF" "Mermaid Overlong"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 1, "Must exit with code 1 on malformed multi-byte sequences")
        self.assertIn("Found 9 broken anchor references", err)
        self.assertIn("invalid UTF-8 byte sequence in percent-encoding", err)
        self.assertNotIn("[OK]", out)

    # =========================================================================
    # 11. Produktionsdokument (DIAGNOSTIC_METADATA_TESTING.md)
    # =========================================================================
    def test_production_diagnostic_metadata_doc(self):
        doc_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "DIAGNOSTIC_METADATA_TESTING.md")
        self.assertTrue(os.path.exists(doc_path), f"File {doc_path} must exist")
        cmd = [sys.executable, SCRIPT_PATH, doc_path, "--required", "beispielpfad-1", "beispielpfad-2", "beispielpfad-3", "beispielpfad-4", "beispielpfad-5", "--strict-duplicates"]
        res = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Verifier failed on production doc with --strict-duplicates:\n{res.stderr}")
        self.assertEqual(res.stderr, "")
        self.assertIn("[OK]", res.stdout)

    # =========================================================================
    # 12. Property-Based Tests (Invariants)
    # =========================================================================
    def test_property_rfc3986_unicode_roundtrip(self):
        """Property 1: Any valid Unicode string percent-encoded roundtrips accurately."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from fuzz_percent_encoding import run_property_roundtrip
        success = run_property_roundtrip(iterations=1000, seed=42)
        self.assertTrue(success, "Property roundtrip preservation violated")

    def test_property_hex_case_insensitivity(self):
        """Property 2: Case variation in %HH hex digits preserves decoded string identity."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from fuzz_percent_encoding import run_property_hex_case
        success = run_property_hex_case(iterations=1000, seed=42)
        self.assertTrue(success, "Hex case insensitivity invariant violated")

    def test_property_crash_freedom_total_function(self):
        """Property 3: check_percent_encoding never crashes on arbitrary input or surrogates."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from fuzz_percent_encoding import run_property_crash_freedom
        success = run_property_crash_freedom(iterations=2000, seed=42)
        self.assertTrue(success, "Crash freedom / total function invariant violated")

    def test_property_groundtruth_utf8_equivalence(self):
        """Property 4: Complete equivalence with Python UTF-8 codec on random byte sequences."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from fuzz_percent_encoding import run_fuzz_groundtruth
        success = run_fuzz_groundtruth(iterations=1500, seed=42)
        self.assertTrue(success, "Ground-truth UTF-8 equivalence violated")

    # =========================================================================
    # 13. Fuzzing Tests (Syntaktische und Semantische Mutationen)
    # =========================================================================
    def test_fuzz_syntax_corruptions_rejection(self):
        """Fuzz 1: Malformed percent tokens are cleanly rejected without crashes."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from fuzz_percent_encoding import run_fuzz_syntax
        success = run_fuzz_syntax(iterations=1000, seed=42)
        self.assertTrue(success, "Syntax fuzzing detected a defect or crash")

    def test_fuzz_utf8_boundary_mutations_rejection(self):
        """Fuzz 2: UTF-8 boundary mutations (overlong, truncated, surrogates) are rejected."""
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from fuzz_percent_encoding import run_fuzz_utf8_boundaries
        success = run_fuzz_utf8_boundaries(iterations=1000, seed=42)
        self.assertTrue(success, "UTF-8 boundary fuzzing detected a defect or crash")

    def test_fuzz_markdown_pipeline_end_to_end(self):
        """Fuzz 3: End-to-end markdown pipeline execution with 50 randomized documents."""
        import random
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from verify_markdown_anchors import verify_anchors

        rng = random.Random(2026)
        for i in range(50):
            lines = ['# Random Fuzzed Document']
            defined = []
            for h in range(rng.randint(2, 6)):
                words = ['section', 'abschnitt', 'moden', 'lambda', 'cjk', 'emoji', 'test']
                text = ' '.join(rng.choices(words, k=rng.randint(1, 3)))
                lines.append(f'## {text}')
                anchor_id = f'anchor-{rng.randint(1, 50)}'
                lines.append(f'<a id="{anchor_id}"></a>')
                defined.append(anchor_id)

            for l in range(rng.randint(3, 8)):
                target = rng.choice(defined + ['non-existent', '%ZZ', '%C3%BC', '%C0%AF', '', ' '])
                link_type = rng.choice(['markdown', 'mermaid'])
                if link_type == 'markdown':
                    lines.append(f'[Link {l}](#{target})')
                else:
                    lines.append('```mermaid')
                    lines.append('graph TD')
                    lines.append('A --> B')
                    lines.append(f'click A href "#{target}" "Mermaid click"')
                    lines.append('```')

            content = '\n'.join(lines)
            with tempfile.NamedTemporaryFile('w', encoding='utf-8', suffix='.md', delete=False) as f:
                f.write(content)
                f_path = f.name
            import io, contextlib
            try:
                buf_out = io.StringIO()
                buf_err = io.StringIO()
                with contextlib.redirect_stdout(buf_out), contextlib.redirect_stderr(buf_err):
                    res = verify_anchors(f_path, strict_duplicates=False)
                self.assertIsInstance(res, bool)
            finally:
                if os.path.exists(f_path):
                    os.remove(f_path)


    # =========================================================================
    # 7. Unicode-Parität: Markdown-Links vs. Mermaid-Klickziele
    # =========================================================================
    def test_unicode_parity_success(self):
        """Parität 1: Identische erfolgreiche Auflösung von Unicode-Ankern (Literal & Percent)."""
        content = """# Unicode Parity Test Document
<a id="λ-resonator-48k"></a>
<a id="übertragungsfunktion-äöüß"></a>
<a id="日本語-音響フィルタ"></a>
<a id="true-peak-🚀-limiter"></a>

## Resonanz θ-Winkel & Dämpfung
### CJK サブシステムとDSP処理

<!-- Markdown Links: Literal & Percent-encoded -->
[MD Greek Literal](#λ-resonator-48k)
[MD Greek Encoded](#%CE%BB-resonator-48k)
[MD German Literal](#übertragungsfunktion-äöüß)
[MD German Encoded](#%C3%BCbertragungsfunktion-%C3%A4%C3%B6%C3%BC%C3%9F)
[MD CJK Literal](#日本語-音響フィルタ)
[MD CJK Encoded](#%E6%97%A5%E6%9C%AC%E8%AA%9E-%E9%9F%B3%E9%9F%BF%E3%83%95%E3%82%A3%E3%83%AB%E3%82%BF)
[MD Emoji Literal](#true-peak-🚀-limiter)
[MD Emoji Encoded](#true-peak-%F0%9F%9A%80-limiter)
[MD Heading Greek](#resonanz-θ-winkel-dämpfung)
[MD Heading CJK](#cjk-サブシステムとdsp処理)

```mermaid
graph TD
A --> B
click A href "#λ-resonator-48k" "Greek Literal"
click B href "#%CE%BB-resonator-48k" "Greek Encoded"
click C href "#übertragungsfunktion-äöüß" "German Literal"
click D href "#%C3%BCbertragungsfunktion-%C3%A4%C3%B6%C3%BC%C3%9F" "German Encoded"
click E href "#日本語-音響フィルタ" "CJK Literal"
click F href "#%E6%97%A5%E6%9C%AC%E8%AA%9E-%E9%9F%B3%E9%9F%BF%E3%83%95%E3%82%A3%E3%83%AB%E3%82%BF" "CJK Encoded"
click G href "#true-peak-🚀-limiter" "Emoji Literal"
click H href "#true-peak-%F0%9F%9A%80-limiter" "Emoji Encoded"
click I href "#resonanz-θ-winkel-dämpfung" "Heading Greek"
click J href "#cjk-サブシステムとdsp処理" "Heading CJK"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 0, f"Unicode Parity success failed with code {code}:\n{err}")
        self.assertIn("Found 20 internal anchor references", out)
        self.assertEqual(err.strip(), "")

    def test_unicode_parity_broken_targets(self):
        """Parität 2: Identische Erkennung und Diagnose fehlender Unicode-Anker."""
        content = """# Broken Unicode Targets
<a id="vorhanden-λ"></a>

<!-- Markdown Broken Links -->
[MD Broken Greek](#fehlt-λ)
[MD Broken German](#fehlt-äöü)
[MD Broken CJK](#不在-フィルタ)

```mermaid
graph TD
A --> B
click A href "#fehlt-λ" "Broken Greek"
click B href "#fehlt-äöü" "Broken German"
click C href "#不在-フィルタ" "Broken CJK"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 1)
        self.assertIn("Found 6 broken anchor references", err)
        # Verify both Markdown link and Mermaid click appear for each target
        for target in ["#fehlt-λ", "#fehlt-äöü", "#不在-フィルタ"]:
            self.assertIn(f"[Markdown link]: [MD Broken", err)
            self.assertIn(f"-> {target} (Anchor {target} not found", err)
            self.assertIn(f"[Mermaid click]: node", err)

    def test_unicode_parity_malformed_encoding(self):
        """Parität 3: Identische Abweisung syntaktisch defekter Mehrbyte-Prozentfolgen."""
        content = """# Malformed Percent Sequences
<a id="dummy"></a>

<!-- Markdown Malformed Percent -->
[MD Bad 1](#%CE)
[MD Bad 2](#%C3%28)
[MD Bad 3](#%C0%AF)

```mermaid
graph TD
A --> B
click A href "#%CE"
click B href "#%C3%28"
click C href "#%C0%AF"
```
"""
        code, out, err = self.run_verifier(content)
        self.assertEqual(code, 1)
        self.assertIn("Found 6 broken anchor references", err)
        self.assertIn("Invalid percent-encoding in anchor #%CE", err)
        self.assertIn("Invalid percent-encoding in anchor #%C3%28", err)
        self.assertIn("Invalid percent-encoding in anchor #%C0%AF", err)

    def test_unicode_parity_cross_file(self):
        """Parität 4: Identische Auflösung relativer Unicode-Pfade und Anker über Dateigrenzen."""
        temp_dir = tempfile.mkdtemp()
        try:
            target_path = os.path.join(temp_dir, "ziel_dokument_λ.md")
            with open(target_path, "w", encoding="utf-8") as f:
                f.write("""# Ziel
<a id="moden-θ-48khz"></a>
## Externe Dämpfung & Sättigung
""")

            main_path = os.path.join(temp_dir, "main.md")
            content = """# Main
[MD Cross Valid](#dummy)
[MD Cross File Literal](./ziel_dokument_λ.md#moden-θ-48khz)
[MD Cross File Encoded](./ziel_dokument_λ.md#%E6%9C%AA%E5%AE%9A%E7%BE%A9)

```mermaid
graph TD
A --> B
click A href "./ziel_dokument_λ.md#moden-θ-48khz" "Mermaid Valid"
click B href "./ziel_dokument_λ.md#%E6%9C%AA%E5%AE%9A%E7%BE%A9" "Mermaid Missing"
```
<a id="dummy"></a>
"""
            with open(main_path, "w", encoding="utf-8") as f:
                f.write(content)

            code, out, err = self.run_verifier(content, custom_dir=temp_dir, filename="main.md")
            self.assertEqual(code, 1)
            # 2 valid refs (dummy, valid cross MD, valid cross Mermaid)
            # 2 broken refs: MD encoded non-existent and Mermaid encoded non-existent
            self.assertIn("Found 2 broken anchor references", err)
            self.assertIn("[Markdown link]: [MD Cross File Encoded] -> ./ziel_dokument_λ.md#%E6%9C%AA%E5%AE%9A%E7%BE%A9", err)
            self.assertIn("[Mermaid click]: node B -> ./ziel_dokument_λ.md#%E6%9C%AA%E5%AE%9A%E7%BE%A9", err)
        finally:
            shutil.rmtree(temp_dir)

    def test_unicode_parity_differential_fuzz(self):
        """Parität 5: Differentielles Fuzzing über 100 randomisierte Unicode-Ziele."""
        import random
        from fuzz_percent_encoding import generate_random_unicode_string
        rng = random.Random(2026)

        for i in range(100):
            target = generate_random_unicode_string(rng, max_len=15)
            # Strip invalid link chars like parenthesis or newlines
            target = target.replace("(", "").replace(")", "").replace("[", "").replace("]", "").replace('"', "").replace("'", "")
            if not target:
                target = f"node-{i}"

            # 50% defined, 50% omitted
            is_defined = (i % 2 == 0)
            encoded = urllib.parse.quote(target) if (i % 3 == 0) else target

            doc = f"# Test Parity Iteration {i}\n"
            if is_defined:
                doc += f'<a id="{target}"></a>\n'

            doc += f"[MD Link](#{encoded})\n\n"
            doc += "```mermaid\ngraph TD\n"
            doc += f'click A href "#{encoded}"\n'
            doc += "```\n"

            code, out, err = self.run_verifier(doc)
            if is_defined:
                self.assertEqual(code, 0, f"Parity differential iteration {i} should be valid, got:\n{err}")
            else:
                self.assertEqual(code, 1, f"Parity differential iteration {i} should be broken")
                self.assertIn("[Markdown link]", err)
                self.assertIn("[Mermaid click]", err)

if __name__ == "__main__":
    unittest.main()
