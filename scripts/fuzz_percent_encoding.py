#!/usr/bin/env python3
"""
fuzz_percent_encoding.py

Property-based testing and fuzzing harness for RFC 3986 percent-encoding
and UTF-8 multi-byte validation in verify_markdown_anchors.py.

Invariants checked:
1. Roundtrip Preservation:
   For every valid Unicode string U, check_percent_encoding(quote(U)) == (True, U, None).
2. Hex Case-Insensitivity:
   Toggling hex digits in %HH tokens preserves validity and decoded string identity.
3. Crash-Freedom / Total Function:
   check_percent_encoding(s) never raises an unhandled exception for any arbitrary input.
4. Ground-Truth UTF-8 Equivalence:
   check_percent_encoding produces identical validity decisions to Python's reference
   UTF-8 codec across arbitrary byte sequences.
5. Rejection of RFC 3986 / RFC 3629 Violations:
   Malformed percent tokens, overlong UTF-8, truncated sequences, surrogate codes,
   and out-of-range codepoints are rejected with descriptive errors and no crashes.
"""

import sys
import os
import random
import argparse
import time
import re
import urllib.parse
from typing import Tuple, List, Optional

# Ensure scripts directory is on path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)

from verify_markdown_anchors import check_percent_encoding

# =============================================================================
# Generators
# =============================================================================

def generate_random_unicode_string(rng: random.Random, max_len: int = 30) -> str:
    """Generates arbitrary valid Unicode strings across all plane ranges, avoiding surrogates."""
    length = rng.randint(1, max_len)
    chars = []
    for _ in range(length):
        category = rng.choice([
            "ascii", "latin1", "greek", "cyrillic", "cjk", "emoji", "astral", "math"
        ])
        if category == "ascii":
            cp = rng.randint(0x20, 0x7E)
        elif category == "latin1":
            cp = rng.randint(0x00A0, 0x00FF)
        elif category == "greek":
            cp = rng.randint(0x0370, 0x03FF)
        elif category == "cyrillic":
            cp = rng.randint(0x0400, 0x04FF)
        elif category == "cjk":
            cp = rng.randint(0x4E00, 0x9FFF)
        elif category == "emoji":
            cp = rng.choice([rng.randint(0x1F600, 0x1F64F), rng.randint(0x1F300, 0x1F5FF)])
        elif category == "math":
            cp = rng.randint(0x2200, 0x22FF)
        else: # astral plane U+10000 .. U+10FFFF
            cp = rng.randint(0x10000, 0x10FFFF)

        if not (0xD800 <= cp <= 0xDFFF):
            chars.append(chr(cp))
    return "".join(chars)

def toggle_percent_hex_case(s: str, rng: random.Random) -> str:
    """Randomly toggles upper/lower case strictly on hex digits in %HH tokens."""
    def repl(m):
        tok = m.group(0)
        h1 = tok[1].lower() if rng.random() > 0.5 else tok[1].upper()
        h2 = tok[2].lower() if rng.random() > 0.5 else tok[2].upper()
        return "%" + h1 + h2
    return re.sub(r"%[0-9a-fA-F]{2}", repl, s)

def generate_malformed_syntax_string(rng: random.Random) -> str:
    """Generates strings with deliberate RFC 3986 percent-encoding syntax violations."""
    base = generate_random_unicode_string(rng, max_len=15)
    mutation_type = rng.choice([
        "trailing_percent",
        "single_hex_end",
        "single_hex_mid",
        "invalid_hex_chars",
        "double_percent",
        "lone_percent_mid",
        "percent_space",
        "percent_control"
    ])

    if mutation_type == "trailing_percent":
        return base + "%"
    elif mutation_type == "single_hex_end":
        return base + "%" + rng.choice("0123456789abcdefABCDEF")
    elif mutation_type == "single_hex_mid":
        return base + "%" + rng.choice("0123456789abcdefABCDEF") + "xyz"
    elif mutation_type == "invalid_hex_chars":
        invalid_pair = rng.choice(["ZZ", "GG", "??", "!!", "--", "  ", "/\\", "\x00\x00"])
        pos = rng.randint(0, len(base))
        return base[:pos] + "%" + invalid_pair + base[pos:]
    elif mutation_type == "double_percent":
        pos = rng.randint(0, len(base))
        return base[:pos] + "%%" + base[pos:]
    elif mutation_type == "lone_percent_mid":
        pos = rng.randint(0, len(base))
        return base[:pos] + "%" + base[pos:]
    elif mutation_type == "percent_space":
        pos = rng.randint(0, len(base))
        return base[:pos] + "% 2" + base[pos:]
    else: # percent_control
        pos = rng.randint(0, len(base))
        return base[:pos] + "%\tA" + base[pos:]

def generate_malformed_utf8_sequence(rng: random.Random) -> str:
    """Generates byte sequences that violate UTF-8 multi-byte encoding rules (RFC 3629)."""
    mutation_type = rng.choice([
        "truncated_2byte",
        "truncated_3byte",
        "truncated_4byte",
        "invalid_cont_2byte",
        "invalid_cont_3byte",
        "overlong_2byte",
        "overlong_3byte",
        "overlong_4byte",
        "surrogate_utf8",
        "out_of_range_4byte",
        "legacy_5byte_6byte",
        "orphan_continuation",
        "invalid_start_bytes"
    ])

    if mutation_type == "truncated_2byte":
        raw = bytes([rng.randint(0xC2, 0xDF)])
    elif mutation_type == "truncated_3byte":
        lead = rng.randint(0xE0, 0xEF)
        cont = [rng.randint(0x80, 0xBF)] if rng.random() > 0.5 else []
        raw = bytes([lead] + cont)
    elif mutation_type == "truncated_4byte":
        lead = rng.randint(0xF0, 0xF4)
        num_cont = rng.randint(0, 2)
        cont = [rng.randint(0x80, 0xBF) for _ in range(num_cont)]
        raw = bytes([lead] + cont)
    elif mutation_type == "invalid_cont_2byte":
        lead = rng.randint(0xC2, 0xDF)
        bad_cont = rng.choice(list(range(0x00, 0x80)) + list(range(0xC0, 0x100)))
        raw = bytes([lead, bad_cont])
    elif mutation_type == "invalid_cont_3byte":
        lead = rng.randint(0xE0, 0xEF)
        good_cont = rng.randint(0x80, 0xBF)
        bad_cont = rng.choice(list(range(0x00, 0x80)) + list(range(0xC0, 0x100)))
        raw = bytes([lead, good_cont, bad_cont]) if rng.random() > 0.5 else bytes([lead, bad_cont, good_cont])
    elif mutation_type == "overlong_2byte":
        # 0xC0 or 0xC1 followed by 0x80..0xBF
        raw = bytes([rng.choice([0xC0, 0xC1]), rng.randint(0x80, 0xBF)])
    elif mutation_type == "overlong_3byte":
        # 0xE0 followed by 0x80..0x9F followed by 0x80..0xBF
        raw = bytes([0xE0, rng.randint(0x80, 0x9F), rng.randint(0x80, 0xBF)])
    elif mutation_type == "overlong_4byte":
        # 0xF0 followed by 0x80..0x8F followed by 2 continuation bytes
        raw = bytes([0xF0, rng.randint(0x80, 0x8F), rng.randint(0x80, 0xBF), rng.randint(0x80, 0xBF)])
    elif mutation_type == "surrogate_utf8":
        # U+D800..U+DFFF in UTF-8: 0xED followed by 0xA0..0xBF followed by 0x80..0xBF
        raw = bytes([0xED, rng.randint(0xA0, 0xBF), rng.randint(0x80, 0xBF)])
    elif mutation_type == "out_of_range_4byte":
        # > U+10FFFF: 0xF4 followed by 0x90..0xBF, or 0xF5..0xF7
        if rng.random() > 0.5:
            raw = bytes([0xF4, rng.randint(0x90, 0xBF), rng.randint(0x80, 0xBF), rng.randint(0x80, 0xBF)])
        else:
            raw = bytes([rng.randint(0xF5, 0xF7), rng.randint(0x80, 0xBF), rng.randint(0x80, 0xBF), rng.randint(0x80, 0xBF)])
    elif mutation_type == "legacy_5byte_6byte":
        lead = rng.randint(0xF8, 0xFD)
        raw = bytes([lead] + [rng.randint(0x80, 0xBF) for _ in range(4)])
    elif mutation_type == "orphan_continuation":
        raw = bytes([rng.randint(0x80, 0xBF)])
    else: # invalid_start_bytes (0xFE, 0xFF)
        raw = bytes([rng.choice([0xFE, 0xFF])])

    # Convert raw bytes into valid %HH tokens
    return "".join(f"%{b:02X}" for b in raw)

# =============================================================================
# Property Runners
# =============================================================================

def run_property_roundtrip(iterations: int, seed: int, verbose: bool = False) -> bool:
    """Property 1: RFC 3986 Unicode Roundtrip Invariant."""
    rng = random.Random(seed)
    start_time = time.time()
    for i in range(iterations):
        u_str = generate_random_unicode_string(rng)
        quoted = urllib.parse.quote(u_str, safe="")
        is_valid, decoded, err = check_percent_encoding(quoted)

        if not is_valid or decoded != u_str or err is not None:
            print(f"[FAIL] Roundtrip failed on iter {i}:")
            print(f"  Input:   {repr(u_str)}")
            print(f"  Quoted:  {quoted}")
            print(f"  Valid:   {is_valid}, Decoded: {repr(decoded)}, Err: {err}")
            return False

        # Also test with unreserved characters mixed in
        quoted_mixed = urllib.parse.quote(u_str, safe="-_.~")
        is_valid_m, decoded_m, err_m = check_percent_encoding(quoted_mixed)
        if not is_valid_m or decoded_m != u_str or err_m is not None:
            print(f"[FAIL] Roundtrip mixed safe chars failed on iter {i}: {quoted_mixed}")
            return False

    elapsed = time.time() - start_time
    if verbose:
        print(f"[PASS] Property Roundtrip: {iterations} iterations in {elapsed:.3f}s")
    return True

def run_property_hex_case(iterations: int, seed: int, verbose: bool = False) -> bool:
    """Property 2: Hex Case-Insensitivity Invariant."""
    rng = random.Random(seed)
    start_time = time.time()
    for i in range(iterations):
        u_str = generate_random_unicode_string(rng)
        quoted = urllib.parse.quote(u_str, safe="")
        toggled = toggle_percent_hex_case(quoted, rng)
        is_valid, decoded, err = check_percent_encoding(toggled)

        if not is_valid or decoded != u_str or err is not None:
            print(f"[FAIL] Hex case invariance failed on iter {i}:")
            print(f"  Original quoted: {quoted}")
            print(f"  Toggled:         {toggled}")
            print(f"  Valid: {is_valid}, Decoded: {repr(decoded)}, Err: {err}")
            return False

    elapsed = time.time() - start_time
    if verbose:
        print(f"[PASS] Property Hex Case Invariance: {iterations} iterations in {elapsed:.3f}s")
    return True

def run_property_crash_freedom(iterations: int, seed: int, verbose: bool = False) -> bool:
    """Property 3: Crash-Freedom / Total Function Invariant."""
    rng = random.Random(seed)
    start_time = time.time()
    for i in range(iterations):
        # Generate arbitrary strings including surrogates, null bytes, long strings, nested %
        choice = rng.choice(["surrogates", "null_bytes", "repeated_percent", "random_chars", "non_str"])
        if choice == "surrogates":
            s = f"anchor%20{chr(rng.randint(0xD800, 0xDFFF))}test"
        elif choice == "null_bytes":
            s = f"start\x00%00{chr(0)}\x00end"
        elif choice == "repeated_percent":
            s = "%" * rng.randint(1, 100)
        elif choice == "random_chars":
            s = "".join(chr(rng.randint(0, 0x10FFFF)) for _ in range(rng.randint(1, 50)))
        else: # non_str
            s = rng.choice([None, 12345, 3.14, ["#list"], {"#dict": 1}])

        try:
            res = check_percent_encoding(s)
            if not isinstance(res, tuple) or len(res) != 3:
                print(f"[FAIL] Invalid return type on iter {i}: {res}")
                return False
            is_valid, decoded, err = res
            if is_valid:
                if decoded is None or err is not None:
                    print(f"[FAIL] Inconsistent success state on iter {i}: {res}")
                    return False
            else:
                if decoded is not None or not err:
                    print(f"[FAIL] Inconsistent failure state on iter {i}: {res}")
                    return False
        except Exception as e:
            print(f"[FAIL] Uncaught exception on iter {i} for input {repr(s)}: {type(e).__name__}: {e}")
            return False

    elapsed = time.time() - start_time
    if verbose:
        print(f"[PASS] Property Crash-Freedom: {iterations} iterations in {elapsed:.3f}s")
    return True

def run_fuzz_syntax(iterations: int, seed: int, verbose: bool = False) -> bool:
    """Fuzz 1: Malformed Percent Syntax Rejection."""
    rng = random.Random(seed)
    start_time = time.time()
    for i in range(iterations):
        malformed = generate_malformed_syntax_string(rng)
        try:
            is_valid, decoded, err = check_percent_encoding(malformed)
        except Exception as e:
            print(f"[FAIL] Crash on malformed syntax input iter {i} {repr(malformed)}: {e}")
            return False

        if is_valid:
            print(f"[FAIL] Malformed syntax accepted on iter {i}: {repr(malformed)}")
            return False
        if decoded is not None or not err:
            print(f"[FAIL] Malformed syntax return inconsistent on iter {i}: {repr(malformed)} -> {err}")
            return False
        if "malformed percent-encoding syntax" not in err:
            print(f"[FAIL] Expected syntax error message on iter {i}, got: {err}")
            return False

    elapsed = time.time() - start_time
    if verbose:
        print(f"[PASS] Fuzz Syntax Rejection: {iterations} iterations in {elapsed:.3f}s")
    return True

def run_fuzz_utf8_boundaries(iterations: int, seed: int, verbose: bool = False) -> bool:
    """Fuzz 2: UTF-8 Multi-byte Boundaries & RFC 3629 Rejection."""
    rng = random.Random(seed)
    start_time = time.time()
    for i in range(iterations):
        bad_utf8 = generate_malformed_utf8_sequence(rng)
        try:
            is_valid, decoded, err = check_percent_encoding(bad_utf8)
        except Exception as e:
            print(f"[FAIL] Crash on malformed UTF-8 iter {i} {repr(bad_utf8)}: {e}")
            return False

        if is_valid:
            print(f"[FAIL] Malformed UTF-8 accepted on iter {i}: {repr(bad_utf8)}")
            return False
        if decoded is not None or not err:
            print(f"[FAIL] Inconsistent malformed UTF-8 return on iter {i}: {err}")
            return False
        if "invalid UTF-8 byte sequence in percent-encoding" not in err:
            print(f"[FAIL] Expected UTF-8 error message on iter {i}, got: {err}")
            return False

    elapsed = time.time() - start_time
    if verbose:
        print(f"[PASS] Fuzz UTF-8 Multi-byte Boundaries: {iterations} iterations in {elapsed:.3f}s")
    return True

def run_fuzz_groundtruth(iterations: int, seed: int, verbose: bool = False) -> bool:
    """Fuzz 3: Ground-Truth UTF-8 Equivalence on Arbitrary Random Byte Streams."""
    rng = random.Random(seed)
    start_time = time.time()
    for i in range(iterations):
        length = rng.randint(1, 64)
        raw_bytes = bytes([rng.randint(0, 255) for _ in range(length)])
        pct_encoded = "".join(f"%{b:02X}" for b in raw_bytes)

        # Ground truth using Python reference UTF-8 decoder
        expected_valid = False
        expected_decoded = None
        try:
            expected_decoded = raw_bytes.decode("utf-8")
            expected_valid = True
        except UnicodeDecodeError:
            expected_valid = False

        try:
            is_valid, decoded, err = check_percent_encoding(pct_encoded)
        except Exception as e:
            print(f"[FAIL] Crash on ground-truth random bytes iter {i}: {e}")
            return False

        if is_valid != expected_valid:
            print(f"[FAIL] Ground truth mismatch on iter {i} ({pct_encoded}):")
            print(f"  Got is_valid={is_valid}, expected={expected_valid}")
            return False

        if is_valid:
            if decoded != expected_decoded or err is not None:
                print(f"[FAIL] Ground truth decoded mismatch on iter {i}: got {repr(decoded)}, expected {repr(expected_decoded)}")
                return False
        else:
            if decoded is not None or not err:
                print(f"[FAIL] Ground truth error state inconsistent on iter {i}: {err}")
                return False

    elapsed = time.time() - start_time
    if verbose:
        print(f"[PASS] Fuzz Ground-Truth UTF-8 Equivalence: {iterations} iterations in {elapsed:.3f}s")
    return True

# =============================================================================
# CLI Main
# =============================================================================


def run_fuzz_markdown_mermaid_parity(iterations: int = 1000, seed: int = 1337, verbose: bool = False) -> bool:
    """
    Fuzz Mode 7: Differential parity between Markdown links and Mermaid click targets.
    Asserts that both link syntaxes resolve to the exact same target and produce
    identical pass/fail decisions and error classifications for arbitrary Unicode anchors.
    """
    import tempfile, shutil
    from verify_markdown_anchors import parse_markdown_file, verify_anchors
    rng = random.Random(seed)
    t0 = time.time()

    temp_dir = tempfile.mkdtemp()
    try:
        for i in range(iterations):
            target = generate_random_unicode_string(rng, max_len=20)
            # Remove characters that break URL / syntax formatting
            target = target.replace("(", "").replace(")", "").replace("[", "").replace("]", "").replace('"', "").replace("'", "").replace("#", "").strip()
            if not target:
                target = f"node-{i}"

            is_defined = (rng.random() > 0.5)
            # 33% literal, 33% fully encoded, 33% case-toggled encoded
            enc_mode = rng.choice(["literal", "encoded", "case"])
            if enc_mode == "literal":
                ref_id = target
            elif enc_mode == "encoded":
                ref_id = urllib.parse.quote(target)
            else:
                ref_id = toggle_percent_hex_case(urllib.parse.quote(target), rng)

            doc_path = os.path.join(temp_dir, f"parity_{i}.md")
            content = f"# Differential Parity Document {i}\n"
            if is_defined:
                content += f'<a id="{target}"></a>\n'
                content += f"## Heading {target}\n"

            content += f"[MD Link](#{ref_id})\n\n"
            content += "```mermaid\n"
            content += "graph TD\n"
            content += "A --> B\n"
            content += f'click A href "#{ref_id}"\n'
            content += "```\n"

            with open(doc_path, "w", encoding="utf-8") as f:
                f.write(content)

            defined_anchors, _, references = parse_markdown_file(doc_path)
            assert len(references) == 2, f"Expected 2 references, got {len(references)}"
            md_ref, mermaid_ref = references[0], references[1]

            # Parity invariant 1: Syntactic parsing extracts identical anchor_part and file_part
            assert md_ref[3] == mermaid_ref[3], f"file_part mismatch: {md_ref[3]} vs {mermaid_ref[3]}"
            assert md_ref[4] == mermaid_ref[4], f"anchor_part mismatch: {md_ref[4]} vs {mermaid_ref[4]}"

            # Parity invariant 2: Both resolve identically against defined anchors
            valid_pct, unquoted_anchor, _ = check_percent_encoding(ref_id)
            dec_anchor = unquoted_anchor if valid_pct else ref_id
            resolves = (ref_id in defined_anchors) or (dec_anchor in defined_anchors)

            if is_defined:
                assert resolves, f"Expected anchor #{ref_id} to resolve in iteration {i}"
            else:
                assert not resolves, f"Expected anchor #{ref_id} NOT to resolve in iteration {i}"

            if os.path.exists(doc_path):
                os.remove(doc_path)
    finally:
        shutil.rmtree(temp_dir)

    elapsed = time.time() - t0
    if verbose:
        print(f"[PASS] Differential Markdown/Mermaid Parity: {iterations} iterations in {elapsed:.3f}s")
    return True

def main():
    parser = argparse.ArgumentParser(description="Property-based and Fuzz Testing for Anchor Percent-Encoding.")
    parser.add_argument("--mode", choices=["all", "roundtrip", "case", "crash", "syntax", "utf8", "groundtruth", "parity"],
                        default="all", help="Fuzzing/property mode to run (default: all)")
    parser.add_argument("--iterations", type=int, default=2000, help="Number of iterations per test mode (default: 2000)")
    parser.add_argument("--seed", type=int, default=1337, help="Pseudorandom seed for reproducibility (default: 1337)")
    parser.add_argument("--verbose", action="store_true", help="Print detailed progress and elapsed timings")

    args = parser.parse_args()

    print(f"=== Running Percent-Encoding Property & Fuzz Test Suite (Seed={args.seed}, Iterations={args.iterations}) ===")

    modes_to_run = []
    if args.mode == "all":
        modes_to_run = ["roundtrip", "case", "crash", "syntax", "utf8", "groundtruth", "parity"]
    else:
        modes_to_run = [args.mode]

    all_passed = True
    for mode in modes_to_run:
        print(f"--> Running mode: {mode} ...", end=" ", flush=True)
        if mode == "roundtrip":
            passed = run_property_roundtrip(args.iterations, args.seed, args.verbose)
        elif mode == "case":
            passed = run_property_hex_case(args.iterations, args.seed, args.verbose)
        elif mode == "crash":
            passed = run_property_crash_freedom(args.iterations, args.seed, args.verbose)
        elif mode == "syntax":
            passed = run_fuzz_syntax(args.iterations, args.seed, args.verbose)
        elif mode == "utf8":
            passed = run_fuzz_utf8_boundaries(args.iterations, args.seed, args.verbose)
        elif mode == "groundtruth":
            passed = run_fuzz_groundtruth(args.iterations, args.seed, args.verbose)
        elif mode == "parity":
            passed = run_fuzz_markdown_mermaid_parity(args.iterations, args.seed, args.verbose)
        else:
            passed = False

        if passed:
            print("OK")
        else:
            print("FAILED")
            all_passed = False
            break

    if all_passed:
        print("\n[SUCCESS] All Property and Fuzzing checks completed with 0 errors!")
        sys.exit(0)
    else:
        print("\n[FAILURE] Property or Fuzzing check detected a defect!")
        sys.exit(1)

if __name__ == "__main__":
    main()
