#!/usr/bin/env python3
"""
verify_markdown_anchors.py

Validates internal anchor links in Markdown files, specifically checking:
1. Standard Markdown links: [text](#anchor) and relative links [text](path/to/file.md#anchor)
2. Mermaid click directives: click Node href "#anchor" and relative click Node href "path#anchor"
3. URL-encoded anchors: e.g. #beispielpfad%2D1 or #stufe%2D1%2Dausf%C3%BChrung
4. Rejection of invalid percent-encoding syntax (%ZZ, %2, trailing %) or invalid UTF-8 byte sequences
5. Rejection of missing relative target files and directories targeted as files
6. Rejection of empty anchor references ([text](#), [text](file.md#), [text](), click Node href "#", click Node href "")
7. Explicit HTML anchors: <a id="anchor"></a>, <span id="anchor"></span>, or id="anchor"
8. Auto-generated heading slugs (GitHub style), including duplicate heading suffixing (-1, -2)
9. Duplicate anchor detection for explicit HTML IDs and collisions

Exit code:
  0: All internal anchors are valid and resolve.
  1: Broken internal anchor links, missing required anchors, or duplicate IDs (if --strict-duplicates).
  2: Input file not found or argument error.
"""

import sys
import re
import os
import urllib.parse
import argparse
from typing import Set, Dict, List, Tuple

RE_HTML_ANCHOR = re.compile(r'<[a-zA-Z0-9_-]+\s+[^>]*\b(?:id|name)=["\']([^"\']+)["\']', re.IGNORECASE)
RE_MARKDOWN_HEADING = re.compile(r'^(#{1,6})\s+(.+)$')
RE_MARKDOWN_LINK = re.compile(r'(?<!!)\[([^\]]+)\]\(([^)]*)\)')
RE_MERMAID_CLICK = re.compile(r'click\s+([a-zA-Z0-9_-]+)\s+href\s+["\']([^"\']*)["\']')

def github_slugify(text: str) -> str:
    """Generate GitHub-compatible anchor slug from heading text."""
    cleaned = re.sub(r'<[^>]+>', '', text)
    cleaned = cleaned.lower()
    cleaned = re.sub(r'[^\w\s\-]', '', cleaned, flags=re.UNICODE)
    cleaned = re.sub(r'\s+', '-', cleaned.strip())
    return cleaned

def strip_inline_code(line: str) -> str:
    """Remove inline code spans `...` from line so examples aren't parsed as active links/anchors."""
    return re.sub(r'`[^`]+`', ' ', line)

def check_percent_encoding(s: str) -> Tuple[bool, str, str]:
    """
    Validates RFC 3986 percent-encoding syntax (%HH) and UTF-8 byte decodability.
    Returns (is_valid, decoded_string, error_message).
    Total function invariant: Never raises unhandled exceptions on arbitrary input.
    """
    if not isinstance(s, str):
        return False, None, f"expected string, got {type(s).__name__}"

    if "%" not in s:
        try:
            s.encode("utf-8")
            return True, s, None
        except UnicodeEncodeError as e:
            return False, None, f"invalid Unicode character in anchor ({e})"

    bad_matches = list(re.finditer(r"%(?![0-9a-fA-F]{2})", s))
    if bad_matches:
        pos = bad_matches[0].start()
        snippet = s[pos:pos+3]
        return False, None, f'malformed percent-encoding syntax (expected %HH hex digits, got "{snippet}")'

    try:
        raw_bytes = urllib.parse.unquote_to_bytes(s)
        decoded = raw_bytes.decode("utf-8")
        return True, decoded, None
    except UnicodeEncodeError as e:
        return False, None, f"invalid Unicode character in anchor ({e})"
    except UnicodeDecodeError as e:
        return False, None, f"invalid UTF-8 byte sequence in percent-encoding ({e})"
    except Exception as e:
        return False, None, f"unexpected percent-decoding error ({e})"

def parse_markdown_file(file_path: str) -> Tuple[Set[str], Dict[str, List[int]], List[Tuple[int, str, str, str, str]]]:
    """
    Parses a markdown file and returns:
    - defined_anchors: set of all valid target IDs (explicit anchors + heading slugs, both raw and unquoted)
    - explicit_anchor_defs: mapping of id -> line numbers of explicit HTML anchors
    - referenced_anchors: list of (line_num, ref_type, link_source, file_part, anchor_part)
    """
    defined_anchors: Set[str] = set()
    explicit_anchor_defs: Dict[str, List[int]] = {}
    referenced_anchors: List[Tuple[int, str, str, str, str]] = []
    heading_counts: Dict[str, int] = {}

    with open(file_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    inside_fenced_code = False
    fenced_type = ""

    for line_idx, line in enumerate(lines, start=1):
        stripped = line.strip()

        # Handle fenced code block toggling
        if stripped.startswith("```"):
            if not inside_fenced_code:
                inside_fenced_code = True
                fenced_type = stripped[3:].strip().lower()
                continue
            else:
                inside_fenced_code = False
                fenced_type = ""
                continue

        # If inside a Mermaid block, look for Mermaid click directives
        if inside_fenced_code:
            if fenced_type.startswith("mermaid"):
                for cm in RE_MERMAID_CLICK.finditer(line):
                    node_id = cm.group(1)
                    raw_href = cm.group(2).strip()
                    if "#" in raw_href:
                        f_part, a_part = raw_href.split("#", 1)
                        referenced_anchors.append((line_idx, "Mermaid click", f"node {node_id}", f_part.strip(), a_part.strip()))
                    elif raw_href == "":
                        referenced_anchors.append((line_idx, "Mermaid click", f"node {node_id}", "", ""))
            continue

        # Outside code blocks: strip inline backticks
        active_line = strip_inline_code(line)

        # 1. Explicit HTML anchors
        for m in RE_HTML_ANCHOR.finditer(active_line):
            anchor_id = m.group(1).strip()
            if anchor_id and anchor_id != "...":
                valid_pct, unquoted_id, _ = check_percent_encoding(anchor_id)
                dec_id = unquoted_id if valid_pct else anchor_id
                defined_anchors.add(anchor_id)
                defined_anchors.add(dec_id)
                explicit_anchor_defs.setdefault(dec_id, []).append(line_idx)

        # 2. Markdown headings
        hm = RE_MARKDOWN_HEADING.match(active_line)
        if hm:
            heading_text = hm.group(2).strip()
            base_slug = github_slugify(heading_text)
            if base_slug:
                count = heading_counts.get(base_slug, 0)
                slug = base_slug if count == 0 else f"{base_slug}-{count}"
                heading_counts[base_slug] = count + 1
                defined_anchors.add(slug)
                valid_pct, dec_slug, _ = check_percent_encoding(slug)
                if valid_pct:
                    defined_anchors.add(dec_slug)

        # 3. Standard Markdown links [label](target)
        for lm in RE_MARKDOWN_LINK.finditer(active_line):
            label = lm.group(1).strip()
            target_url = lm.group(2).strip()

            # Ignore external protocols
            if target_url.startswith(("http://", "https://", "mailto:", "ftp:", "javascript:")):
                continue

            # Strip optional Markdown title: [label](url "title") or [label](url 'title')
            title_match = re.search(r"""\s+["'].*?["']\s*$""", target_url)
            if title_match:
                target_url = target_url[:title_match.start()].strip()

            if "#" in target_url:
                f_part, a_part = target_url.split("#", 1)
                referenced_anchors.append((line_idx, "Markdown link", f"[{label}]", f_part.strip(), a_part.strip()))
            elif target_url == "":
                referenced_anchors.append((line_idx, "Markdown link", f"[{label}]", "", ""))

    return defined_anchors, explicit_anchor_defs, referenced_anchors

def get_file_anchors(file_path: str, cache: Dict[str, Set[str]]) -> Set[str]:
    """Retrieve defined anchors for a given file path with caching."""
    norm_path = os.path.normpath(file_path)
    if norm_path in cache:
        return cache[norm_path]
    if not os.path.exists(norm_path) or os.path.isdir(norm_path):
        return set()
    anchors, _, _ = parse_markdown_file(norm_path)
    cache[norm_path] = anchors
    return anchors

def verify_anchors(file_path: str, required_anchors: List[str] = None, strict_duplicates: bool = False) -> bool:
    if not os.path.exists(file_path):
        print(f"[ERROR] Target file not found: {file_path}", file=sys.stderr)
        return False

    base_dir = os.path.dirname(os.path.abspath(file_path))
    file_cache: Dict[str, Set[str]] = {}

    try:
        defined_anchors, explicit_anchor_defs, references = parse_markdown_file(file_path)
    except Exception as e:
        print(f"[ERROR] Failed to parse {file_path}: {e}", file=sys.stderr)
        return False
    file_cache[os.path.normpath(file_path)] = defined_anchors

    has_errors = False

    # Check for duplicate explicit anchors
    duplicate_count = 0
    for anchor_id, line_numbers in explicit_anchor_defs.items():
        if len(line_numbers) > 1:
            duplicate_count += 1
            msg = f"[WARNING] Duplicate explicit anchor definition '{anchor_id}' on lines: {line_numbers}"
            if strict_duplicates:
                print(f"[ERROR - STRICT] {msg}", file=sys.stderr)
                has_errors = True
            else:
                print(msg)

    # Check required anchors
    if required_anchors:
        for req in required_anchors:
            valid_pct, req_unquoted, _ = check_percent_encoding(req)
            req_dec = req_unquoted if valid_pct else req
            if req not in defined_anchors and req_dec not in defined_anchors:
                print(f"[ERROR] Required anchor '{req}' is missing from {file_path}", file=sys.stderr)
                has_errors = True

    print(f"=== Verifying anchors in: {file_path} ===")
    print(f"Found {len(defined_anchors)} defined target anchors/slugs.")
    print(f"Found {len(references)} internal anchor references.")

    broken_refs = []
    for line_num, ref_type, source, file_part, anchor_part in references:
        # Check for empty anchor references
        if not anchor_part:
            if not file_part:
                broken_refs.append((line_num, ref_type, source, file_part, anchor_part, "Empty anchor reference: '#' (no anchor ID specified)"))
            else:
                broken_refs.append((line_num, ref_type, source, file_part, anchor_part, f"Empty anchor reference: '{file_part}#' (no anchor ID specified)"))
            continue

        # Check for invalid percent-encoding in anchor
        valid_pct, anchor_unquoted, pct_err = check_percent_encoding(anchor_part)
        if not valid_pct:
            broken_refs.append((line_num, ref_type, source, file_part, anchor_part, f"Invalid percent-encoding in anchor #{anchor_part}: {pct_err}"))
            continue

        # Case 1: Intra-file anchor (file_part is empty or refers to this file)
        target_file = None
        if not file_part:
            target_file = file_path
            target_anchors = defined_anchors
        else:
            file_part_clean = urllib.parse.unquote(file_part)
            resolved_target = os.path.normpath(os.path.join(base_dir, file_part_clean))
            if not os.path.exists(resolved_target):
                raw_target = os.path.normpath(os.path.join(base_dir, file_part))
                if os.path.exists(raw_target):
                    resolved_target = raw_target

            target_file = resolved_target

            if not os.path.exists(resolved_target):
                broken_refs.append((line_num, ref_type, source, file_part, anchor_part, f"Target file does not exist: {file_part}"))
                continue

            if os.path.isdir(resolved_target):
                broken_refs.append((line_num, ref_type, source, file_part, anchor_part, f"Target path is a directory, not a file: {file_part}"))
                continue

            target_anchors = get_file_anchors(resolved_target, file_cache)

        # Check if anchor resolves in target file
        if anchor_part not in target_anchors and anchor_unquoted not in target_anchors:
            broken_refs.append((line_num, ref_type, source, file_part, anchor_part, f"Anchor #{anchor_part} not found in {os.path.basename(target_file)}"))

    if broken_refs:
        print(f"\n[FAIL] Found {len(broken_refs)} broken anchor references:", file=sys.stderr)
        for line_num, ref_type, source, file_part, anchor_part, reason in broken_refs:
            full_ref = f"{file_part}#{anchor_part}" if file_part else (f"#{anchor_part}" if anchor_part else "#")
            print(f"  Line {line_num} [{ref_type}]: {source} -> {full_ref} ({reason})", file=sys.stderr)
        has_errors = True
    else:
        print(f"[OK] All {len(references)} internal anchor references successfully resolved to defined targets.")

    return not has_errors

def main():
    parser = argparse.ArgumentParser(description="Verify internal anchor links in Markdown documentation.")
    parser.add_argument("file", help="Path to markdown file (e.g. DIAGNOSTIC_METADATA_TESTING.md)")
    parser.add_argument("--required", nargs="*", default=[], help="List of critical anchor IDs that must be present")
    parser.add_argument("--strict-duplicates", action="store_true", help="Fail with exit code 1 if duplicate explicit anchor IDs are found")

    args = parser.parse_args()

    if not os.path.exists(args.file):
        print(f"[ERROR] Target file not found: {args.file}", file=sys.stderr)
        sys.exit(2)

    success = verify_anchors(args.file, args.required, args.strict_duplicates)
    if not success:
        sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
