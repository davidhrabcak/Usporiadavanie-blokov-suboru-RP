"""
evaluate_order.py
=================
Compares an original (correct-order) MP3 against your algorithm's output
and prints a per-metric score — no composite grade, just the numbers.

HOW TO REPLACE PSEUDO-FUNCTIONS
--------------------------------
Replace the two functions marked PSEUDO-FUNCTION with your real ones.

Usage:
    python evaluate_order.py original.mp3 algorithm_output.mp3 --chunk-size 4096
"""

import argparse
from dataclasses import dataclass
from typing import List, Optional


# ══════════════════════════════════════════════════════════════════════════════
# ── MP3 INTERFACE  (replace these with your real ones) ───────────────────────
# ══════════════════════════════════════════════════════════════════════════════

def read_chunks(path: str) -> List[bytes]:
    """
    Reads MP3 frames using Mp3FrameScanner (frame_scanner.hpp / .cpp).
    Each frame's rawBits vector becomes one chunk.
    The chunk_size argument is ignored — frame boundaries are used instead.

    Requires the Mp3FrameScanner Python bindings to be importable.
    Make sure the compiled extension (e.g. frame_scanner.so / .pyd) is on
    sys.path or in the same directory as this script.
    """
    from frame_scanner import Mp3FrameScanner   # your compiled C++ extension

    scanner = Mp3FrameScanner(path)
    return [bytes(frame.rawBits) for frame in scanner.frame_data]


def chunk_id(chunk: bytes) -> bytes:
    return chunk


# ══════════════════════════════════════════════════════════════════════════════
# ── MATCHING ──────────────────────────────────────────────────────────────────
# ══════════════════════════════════════════════════════════════════════════════

@dataclass
class ChunkMatch:
    orig_index: int           # position in original  (0-based)
    algo_index: Optional[int] # position in algorithm output, None if missing


def match_chunks(orig_chunks: List[bytes],
                 algo_chunks: List[bytes]) -> List[ChunkMatch]:
    """For each original chunk, find where it ended up in the algo output."""
    algo_index: dict = {}
    for i, c in enumerate(algo_chunks):
        algo_index.setdefault(chunk_id(c), []).append(i)

    used = set()
    matches = []
    for orig_idx, chunk in enumerate(orig_chunks):
        candidates = [p for p in algo_index.get(chunk_id(chunk), [])
                      if p not in used]
        if candidates:
            best = min(candidates, key=lambda p: abs(p - orig_idx))
            used.add(best)
            matches.append(ChunkMatch(orig_idx, best))
        else:
            matches.append(ChunkMatch(orig_idx, None))
    return matches


# ══════════════════════════════════════════════════════════════════════════════
# ── METRICS ───────────────────────────────────────────────────────────────────
# ══════════════════════════════════════════════════════════════════════════════

def metric_presence(matches: List[ChunkMatch]):
    found = sum(1 for m in matches if m.algo_index is not None)
    return found, len(matches), found / len(matches) * 100


def metric_exact_position(matches: List[ChunkMatch]):
    exact = sum(1 for m in matches
                if m.algo_index is not None and m.algo_index == m.orig_index)
    return exact, len(matches), exact / len(matches) * 100


def metric_kendall_tau(matches: List[ChunkMatch]):
    """
    Kendall's tau: +1 = perfect order, 0 = random, -1 = fully reversed.
    Only counts chunks that were found in the output.
    """
    found = [m for m in matches if m.algo_index is not None]
    if len(found) < 2:
        return None
    positions = [m.algo_index for m in found]
    n = len(positions)
    concordant = discordant = 0
    for i in range(n):
        for j in range(i + 1, n):
            if positions[i] < positions[j]:
                concordant += 1
            else:
                discordant += 1
    total = concordant + discordant
    return (concordant - discordant) / total if total else 0.0


def metric_correct_sequences(matches: List[ChunkMatch], min_run: int = 2):
    """
    Finds all runs where consecutive original chunks are also consecutive
    in the algo output (correct relative order AND adjacency).
    Returns: list of run lengths, total chunks covered, % of all chunks covered.
    """
    found = sorted([m for m in matches if m.algo_index is not None],
                   key=lambda m: m.orig_index)
    runs = []
    current = 1
    for i in range(1, len(found)):
        prev, cur = found[i - 1], found[i]
        if cur.orig_index == prev.orig_index + 1 and \
           cur.algo_index == prev.algo_index + 1:
            current += 1
        else:
            if current >= min_run:
                runs.append(current)
            current = 1
    if current >= min_run:
        runs.append(current)

    covered = sum(runs)
    pct = covered / len(matches) * 100 if matches else 0.0
    return runs, covered, pct


def metric_avg_displacement(matches: List[ChunkMatch]):
    """
    Average number of chunk positions each chunk drifted from its correct slot.
    Also returns it normalised to [0, 1] relative to file length.
    """
    found = [m for m in matches if m.algo_index is not None]
    if not found:
        return None, None
    n = len(matches)
    displacements = [abs(m.algo_index - m.orig_index) for m in found]
    avg = sum(displacements) / len(displacements)
    avg_norm = avg / max(n - 1, 1)   # as a fraction of max possible displacement
    return avg, avg_norm


# ══════════════════════════════════════════════════════════════════════════════
# ── REPORT ────────────────────────────────────════════════════════════════════
# ══════════════════════════════════════════════════════════════════════════════

def print_report(orig_path, algo_path, matches):
    n = len(matches)

    found, total, pres_pct       = metric_presence(matches)
    exact, _,     exact_pct      = metric_exact_position(matches)
    tau                          = metric_kendall_tau(matches)
    runs, covered, seq_pct       = metric_correct_sequences(matches)
    avg_disp, avg_disp_norm      = metric_avg_displacement(matches)

    W = 60
    print("=" * W)
    print(" MP3 CHUNK ORDER EVALUATION".center(W))
    print("=" * W)
    print(f"  Original : {orig_path}")
    print(f"  Algorithm: {algo_path}")
    print("-" * W)

    print(f"\n  Presence")
    print(f"    {found}/{total} chunks found in output")
    print(f"    → {pres_pct:.1f}%")

    print(f"\n  Exact position")
    print(f"    {exact}/{total} chunks in the correct slot")
    print(f"    → {exact_pct:.1f}%")

    print(f"\n  Relative order  (Kendall's τ)")
    if tau is None:
        print("    → not enough matched chunks")
    else:
        print(f"    τ ranges from -1 (fully reversed) to +1 (perfect order)")
        print(f"    → τ = {tau:+.4f}")

    print(f"\n  Correct adjacent sequences  (runs of ≥2 consecutive chunks)")
    if not runs:
        print("    No correct runs found")
    else:
        runs_str = ", ".join(str(r) for r in runs)
        print(f"    Run lengths: [{runs_str}]")
        print(f"    {covered}/{total} chunks covered by correct runs")
    print(f"    → {seq_pct:.1f}%")

    print(f"\n  Average displacement per chunk")
    if avg_disp is None:
        print("    → no matched chunks")
    else:
        print(f"    Average: {avg_disp:.2f} chunk positions off")
        print(f"    → {avg_disp_norm * 100:.1f}% of file length")

    print("\n" + "=" * W)

    # Chunk map
    print("\n  CHUNK MAP  [original → algo position]  (? = missing)\n")
    col = 0
    for m in sorted(matches, key=lambda x: x.orig_index):
        a = str(m.algo_index) if m.algo_index is not None else "?"
        cell = f"[{m.orig_index}→{a}]"
        if col + len(cell) + 1 > W - 2:
            print()
            col = 0
        print(cell, end=" ")
        col += len(cell) + 1
    print("\n")


# ══════════════════════════════════════════════════════════════════════════════
# ── ENTRY POINT ───────────────────────────────────────────────────────────────
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="Score how well an MP3 chunk-unshuffler performed."
    )
    parser.add_argument("original",  help="Original correct-order MP3")
    parser.add_argument("algorithm", help="Algorithm output MP3")
    args = parser.parse_args()

    orig_chunks = read_chunks(args.original)
    algo_chunks = read_chunks(args.algorithm)
    matches = match_chunks(orig_chunks, algo_chunks)

    print_report(args.original, args.algorithm, matches)


if __name__ == "__main__":
    main()