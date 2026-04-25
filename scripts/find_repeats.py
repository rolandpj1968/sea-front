#!/usr/bin/env python3
"""
Find repeated multi-line sequences across the codebase using a
line-level suffix array + LCP approach (BWT family).

Each line is stripped of leading/trailing whitespace and treated
as an atomic token. We concatenate all source lines into one
sequence, build the suffix array, compute LCP, then for each
(length L, count C) report the candidates ranked by L*C
(approximate "lines saved" by extracting a helper).

Usage: scripts/find_repeats.py [src_root]   (defaults to "src")
"""
import os, sys
from collections import defaultdict

ROOT = sys.argv[1] if len(sys.argv) > 1 else "src"

# 1. Collect all lines, with file/line metadata.
lines = []   # list of stripped text
meta  = []   # parallel list of (file, lineno)
for dirpath, dirnames, filenames in os.walk(ROOT):
    for fn in filenames:
        if not fn.endswith(('.c', '.h')): continue
        path = os.path.join(dirpath, fn)
        with open(path, 'r', errors='replace') as f:
            for i, raw in enumerate(f, 1):
                s = raw.strip()
                # Skip noise: empty, pure-comment dividers, single-char braces
                if not s: continue
                if s in ('{', '}', '*/', '/*'): continue
                if all(c in '/*-= ' for c in s): continue
                lines.append(s)
                meta.append((path, i))

n = len(lines)
print(f"# {n} non-trivial lines across the codebase", file=sys.stderr)

# 2. Map distinct lines to integers for fast comparison
line_id = {}
seq = []
for s in lines:
    if s not in line_id: line_id[s] = len(line_id)
    seq.append(line_id[s])
print(f"# {len(line_id)} distinct lines", file=sys.stderr)

# 3. Build suffix array (positions sorted by suffix).
# Naive O(n log n * avg-cmp) — sort positions by tuple-of-rest.
# n is ~25k so this is fine.
sa = sorted(range(n), key=lambda i: tuple(seq[i:i+20]))  # sort by first 20 lines

# 4. Compute LCP[i] = longest common prefix of suffix sa[i] and sa[i+1]
lcp = [0] * (n - 1)
for i in range(n - 1):
    a, b = sa[i], sa[i+1]
    j = 0
    while a + j < n and b + j < n and seq[a+j] == seq[b+j]:
        j += 1
    lcp[i] = j

# 5. For each suffix-array adjacent pair with lcp >= 3, that's a
# repeat. Find runs of consecutive lcps >= L for various L; count
# how many times sequences of length L repeat.
#
# Simple report: for each L >= 3, find groups of adjacent positions
# in sa where lcp[i..i+k] all >= L (means k+1 occurrences of an
# L-line sequence).
def find_groups(min_len):
    groups = []
    i = 0
    while i < len(lcp):
        if lcp[i] >= min_len:
            j = i
            while j < len(lcp) and lcp[j] >= min_len:
                j += 1
            count = j - i + 1   # number of suffixes sharing >= min_len prefix
            # Take the actual lines from one of them
            start = sa[i]
            text = lines[start:start + min_len]
            groups.append((count, min_len, text, [(meta[sa[k]][0], meta[sa[k]][1]) for k in range(i, j+1)]))
            i = j + 1
        else:
            i += 1
    return groups

# Score by lines-saved = (count - 1) * length (extracting a helper saves
# count-1 occurrences * length lines, minus the helper itself ~length).
results = []
for L in (3, 4, 5, 6, 8, 10, 15, 20):
    for count, length, text, locs in find_groups(L):
        if count >= 3:
            saved = (count - 1) * length
            results.append((saved, count, length, text, locs))

# Dedup overlapping reports: keep the longest at each starting fingerprint
results.sort(key=lambda r: -r[0])

print(f"\n# Top refactoring candidates: lines saved by extracting helper")
print(f"# (count - 1) * length, ranked\n")
seen_signatures = set()
for saved, count, length, text, locs in results:
    sig = '\n'.join(text)
    fingerprint = sig[:100]
    if fingerprint in seen_signatures: continue
    seen_signatures.add(fingerprint)

    print(f"=== {saved} lines saved | {count} occurrences | {length}-line block ===")
    for line in text:
        print(f"  {line[:90]}")
    print(f"  occurrences (first 5):")
    for path, ln in locs[:5]:
        print(f"    {path}:{ln}")
    print()

    if len(seen_signatures) >= 12: break
