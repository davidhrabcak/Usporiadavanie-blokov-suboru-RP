# How `rp` finds the original chunk order - the whole process, explained

Branch `global-matching`, HEAD `60e8c4a`, written 2026-10-20.

This document explains, in plain language and in full, how the program `rp` puts a shuffled
MP3 file back together. It covers every stage, why each one exists, which choices were made
along the way and why, what was tried and thrown away, and what the stage costs or risks.
It is meant to be read from top to bottom by someone who has not followed the project day
by day. It is deliberately long. Where a number appears, it comes from a dated measurement
(the benchmark of 2026-10-18 in `tmp/bench/runs.csv`, or a run done for this document) -
not from memory.

If you only want the short version: chapter 1 (the problem), chapter 3 (the pipeline in
one picture), chapter 6 (results) and chapter 8 (the keep-or-roll-back decision).

---

## Contents

1. The problem in plain words
2. Words you need (a small glossary)
3. The pipeline in one picture
4. Stage by stage
   - 4.1 Cutting, pinning chunk 0, shuffling
   - 4.2 Reading each chunk on its own (`ChunkMeta`)
   - 4.3 Which chunk can follow which: the structural graph
   - 4.4 The bit reservoir: a second, stricter rule
   - 4.5 NEW - the reservoir arc filter
   - 4.6 Arc consistency (AC-3), and the bug the filter exposed
   - 4.7 Listening to the junction: the PCM flux score
   - 4.8 NEW - `LeftContext`: decoding the left side only once
   - 4.9 Locking the obvious edges (confident-edge locking)
   - 4.10 Gluing forced chains: supernodes
   - 4.11 The depth-first search (DFS)
   - 4.12 NEW - global matching: solving the whole puzzle at once
   - 4.13 Repair: fixing a nearly-right order with block moves
   - 4.14 NEW - the minimum-gain rule for repair
   - 4.15 The matching as a hint for the DFS
   - 4.16 Iterated matching (built, measured, switched off)
   - 4.17 Output, the provenance label and the stability gap
5. A worked example: reading one real log
6. Results
7. The wrong outputs: what they are and why they happen
8. Keep or roll back? The decision and its reasoning
9. Honest caveats
10. What comes next
11. Quick reference: the switches

---

## 1. The problem in plain words

Imagine an MP3 file cut into equal pieces - say 1024 bytes each - and the pieces thrown on
the floor in random order. Nothing on any piece says where it came from. There is no file
name, no index, no timestamp. The task is to pick the pieces back up in the original order,
using only what is inside the pieces themselves.

This is a real problem in digital forensics ("file carving"): when a disk is damaged or a
file is deleted, its blocks are often still on the disk but no longer linked together.

Two simplifications are made:

- **Chunk 0 is known.** The first piece is never shuffled. It contains the start of the
  stream (often an ID3 tag and the first frames), so it is easy to find in practice, and it
  gives us the stream's basic settings (sample rate, stereo/mono, MPEG version).
- **All pieces are present** and none is damaged. The answer is a reordering of exactly
  the pieces we have.

So the task is: find the one order of `n` pieces, starting from piece 0, that is the
original file. With `n` pieces there are `(n-1)!` possible orders. For a 30-second file at
1 KB chunks that is roughly 500 pieces - a number of orders with over a thousand digits. We
can not try them all. Everything below is about cutting that space down, and about
recognising the right answer when we see it.

**What "success" means.** A run ends in one of three ways:

- **exact** - the output is byte-for-byte the original file;
- **wrong** - the program produced a complete, internally consistent order, but it is not
  the original (usually it is *almost* the original, with a few pieces swapped);
- **failed** - the program gave up without a complete order (it still reports how far it
  got).

A second, finer measure is **junctions recovered**: of the `n-1` places where one piece
meets the next, how many are the true pair. This matters because a "wrong" output with 99%
of junctions right is a song with one skip in it, while a "failed" output is nothing.

> Note on "chunks in original position": `rp` also prints how many chunks sit at exactly
> their original index. This number is misleading: if one block of chunks near the start is
> moved, *every* chunk after it shifts by one place and counts as "wrong", even though the
> audio is nearly perfect. The junction count is the meaningful accuracy measure. Chapter 5
> shows an example where 610 of 657 junctions are right but only 5.3% of chunks are "in
> place".

---

## 2. Words you need (a small glossary)

**MP3 frame.** An MP3 file is a sequence of *frames*. Each frame holds 1152 audio samples
(about 26 ms at 44.1 kHz). A frame starts with a 4-byte *header* (a sync pattern plus the
bitrate, sample rate and so on), followed by *side information* (a small block describing
how the audio is coded), followed by the coded audio data.

**Frame length.** It follows from the header: for MPEG-1 Layer III it is
`144 * bitrate / sampleRate (+1 if padded)`. At 128 kbps / 44.1 kHz that is 417 or 418
bytes. So once you can read a header, you know exactly where the next frame must start.

**CBR / VBR.** *Constant bitrate*: every frame has the same bitrate and (nearly) the same
length. *Variable bitrate*: the encoder changes bitrate frame by frame, so frame lengths
vary (in our test songs from ~100 to ~835 bytes). This matters a lot: in CBR every header
looks the same, so the structure tells you very little; in VBR the varying lengths act like
a fingerprint.

**Chunk.** One of the equal-sized pieces (1, 2, 4 or 8 KB in the benchmark). Chunk
boundaries ignore frame boundaries, so most frames that touch a boundary are cut in two:
the first part is the *tail* of one chunk, the rest is the *head* of the next.

**Junction.** The place where one chunk ends and the next begins. A "true junction" is a
pair that was adjacent in the original file.

**Graph, node, arc (edge).** We draw every chunk as a dot (*node*) and an arrow (*arc*, or
*edge*) from `a` to `b` whenever `b` *could* follow `a`. The original order is then a path
through the arrows.

**Hamiltonian path.** A path that visits every node exactly once. The original file is a
Hamiltonian path that starts at chunk 0. Finding a Hamiltonian path is a famously hard
problem in general (NP-hard): no known method is fast on every input. That is why the whole
design is about making the graph small and using strong hints.

**Bit reservoir, `main_data_begin`, `part2_3_length`.** An MP3 encoder may store part of a
frame's audio data *inside earlier frames* that had spare room. Each frame says, in its
side information, how far back its data starts (`main_data_begin`, in bytes) and how many
bits of data it uses (`part2_3_length`). A frame can only reach back over bytes that really
exist and were not used yet. This gives a hard rule: in the true order, every frame's
reach-back must be satisfiable. In a wrong order it often is not. This is the *reservoir
check*. Section 4.4 explains it in detail.

**PCM.** The actual decoded audio samples - the sound wave. Decoding an MP3 to PCM is what
a music player does.

**Spectral flux.** Take the sound in short slices, compute each slice's frequency content
(a *spectrum*, via an FFT), and measure how much the spectrum changes from one slice to the
next. Music changes smoothly most of the time; a splice of two unrelated moments of a song
produces a sudden jump. The flux at a junction, divided by the typical flux just before it,
is the main "does this sound right?" score in this project.

**Supernode.** A run of chunks that are forced to follow each other (each has exactly one
possible successor, and that successor has exactly one possible predecessor). We glue such
a run into one node. The search then works on supernodes instead of chunks.

**AC-3 (arc consistency).** A standard constraint-propagation trick: if chunk `b` has only
one possible predecessor `a`, then `a` *must* be followed by `b`, so every other arrow out of
`a` can be deleted - which may leave another chunk with a single option, and so on.

**Assignment problem / matching.** Give every node exactly one successor and exactly one
predecessor, so that the total cost is as small as possible. Unlike the Hamiltonian path,
this can be solved exactly and fast. Its answer is a path plus possibly some separate loops
(*cycles*). Section 4.12 explains why that is useful anyway.

**Local search / repair moves.** Start from a complete order and improve it by small
changes: cut out a block of consecutive chunks and put it somewhere else (a *block move*),
or exchange two blocks (a *block swap*). Keep a change only if it makes the order better.

**Seed.** The random number used to shuffle. All benchmark numbers use seed 1, so every run
is reproducible.

---

## 3. The pipeline in one picture

```
 input.mp3
    |
    v
 [4.1] cut into chunks, pin chunk 0, shuffle the rest (fixed seed)
    |
    v
 [4.2] read every chunk on its own: where do frames start? what does the tail look like?
    |
    v
 [4.3] structural graph: arrow a->b if b's frame starts are where a's cut frame says they must be
    |
    v
 [4.5] NEW reservoir arc filter: delete arrows that break the bit reservoir in every possible case
    |
    v
 [4.6] AC-3: forced arrows remove competitors, repeat until nothing changes
    |
    v
 +--- lock attempt, tau = 1.5, then 1.25, then 1.1 (each starts again from the graph above) ---+
 |  [4.9] lock edges whose decoded audio is clearly smoother than every alternative; AC-3 again  |
 |  [4.10] glue forced chains into supernodes                                                     |
 |  [4.11] DFS from chunk 0: try candidates best-sounding first, check the reservoir as it goes  |
 |      found?  -> stop trying lock attempts                                                      |
 |      not found -> [4.12] NEW global matching (whole-file assignment + cycle splicing)          |
 |                   [4.13/4.14] repair it (breaks first, then flux, with a minimum gain)         |
 |                   valid?  -> done                                                              |
 |                   not valid -> [4.15] DFS again, with the matching's choices tried first       |
 +-----------------------------------------------------------------------------------------------+
    |
    v
 [4.13/4.14] repair the found order (block moves/swaps on the full graph, minimum gain 1.0)
    |
    v
 [4.17] write output.mp3, print provenance label + diagnostics
```

The stages marked NEW are the work on this branch. The rest existed before (on
`full-pcm-decode`, called "PCM final" in the results) but some of them were changed to fit -
those changes are described in place.

**The single most important idea of this branch** is the change from "search for a path,
step by step, from the start" to "solve for the whole file at once, then fix the few
mistakes". Section 4.12 explains why the step-by-step search hit a wall and why the global
view gets past it.

---

## 4. Stage by stage

### 4.1 Cutting, pinning chunk 0, shuffling

**What happens.** The file is read and cut into chunks of `RP_CHUNK_SIZE` bytes (default
1024). Chunk 0 stays first. The other chunks are shuffled with a random generator seeded
with `RP_SEED` (default 1). Alongside the shuffle, the program keeps a hidden list of where
each chunk *really* came from (`origIndexOf`).

**Why a fixed seed.** Early in the project the seed came from the clock, so two runs of the
same file gave different results and claims in the notes could not be re-checked (the
"CBR in 0.17 s" story in `CLAUDE.md` item 1 is the cautionary tale). Since 2026-09-07 the
seed is fixed; `RP_SEED=random` restores the old behaviour.

**What the hidden list is used for - and what it must never be used for.** `origIndexOf` is
used only to *report* how good the answer is ("610/657 true junctions", "[vs source: 36/37
correct]"). Every line in the log that says "vs source" is a report card, not an input. The
algorithm itself must not peek. There is one important exception, discussed honestly in
chapter 9: two safety gates look at the *unshuffled* file.

**Pre-shuffle diagnostics.** Before shuffling, the program runs its own rules on the true
order:

- *pre-shuffle check*: does every true pair pass the structural rule (4.3)? If not, the
  rule has a blind spot on this file (`test7.mp3` has 2 such pairs);
- *reservoir check on the true order*: does the true order pass the reservoir rule (4.4)?

These two answers are used as *gates*: the stages that would become unsafe if a rule had a
blind spot (the reservoir arc filter, AC-3, edge locking) only run when the rules passed the
true order. Chapter 9 explains why this is a research-prototype convenience.

### 4.2 Reading each chunk on its own (`ChunkMeta`)

**What happens.** For every chunk, `computeChunkMeta` (in `chunk_meta.cpp`) tries every byte
position as a possible frame start. A position counts if there is a valid header there that
matches the stream (same MPEG version, layer, sample rate, channel mode as chunk 0). From
each such position it walks forward frame by frame. The best starting position becomes the
chunk's `headOffset`. From that walk the chunk learns:

- `frameStarts` - every position in the chunk where a valid header sits (sorted);
- `frames` - the complete frames inside the chunk (offset, length, bitrate,
  `main_data_begin`);
- the *tail*: how many bytes of a cut frame hang off the end (`tailOverflow`), and if that
  frame's header is readable, its full length (`tailPartialLen`);
- the first bytes of the chunk and of a cut header, for the case where a header itself is
  split across two chunks.

**Why "best position" is ranked tail-first.** A random byte pattern can look like a valid
header. An earlier version picked the position with the most complete frames, which often
chose a false alignment with a messy tail. Ranking by "how clean is the tail" first and
"how many frames" second (`tailQuality`) removed most of those mistakes (`CLAUDE.md` item 2).

**Why chunks with zero complete frames are kept.** With large VBR frames (up to 835 bytes in
a 1024-byte chunk) a chunk may contain only the end of one frame and the start of the next.
An old rule threw such chunks away as "invalid", which cut the true path in 17 places on
`test7_shorter` (item 6). Now any chunk with a believable frame start is valid.

### 4.3 Which chunk can follow which: the structural graph

**What happens.** For every pair of chunks `a, b`, `canFollow(a, b)` (in `adjacency.cpp`)
asks a simple question: *if `b` came right after `a`, would the frames line up?*

- **Case 1** - `a` ends with a cut frame whose header is readable. Then we know the frame's
  full length, so we know exactly how many bytes of it must be at the start of `b`. Call
  that `rem`. Then `b` must have a valid header at byte `rem`. If it does, draw the arrow.
- **Case 2** - `a` ends with only 1-3 bytes of a header (the header itself is cut). Glue
  those bytes to the first bytes of `b`, decode the header, compute the frame length, and
  check `b` has a valid header where the next frame must start.

**Why this is not enough on its own.** The rule is necessary (the true successor always
passes it, when the metadata is right) but far from sufficient: many wrong chunks *also*
happen to have a valid header at the required position. In CBR all frames have the same
length, so the frame starts in a chunk repeat with a short period; a 30 s CBR file at 1 KB
gives each chunk dozens of possible successors (`CBR.mp3`: ~33 per chunk). VBR is better
but still ambiguous: short quiet frames of 104 bytes appear dozens of times.

**Tried and rejected.** Requiring one *more* complete frame after the required position
made things worse (many true successors start with a large frame that does not fit twice in
a chunk) - item 7. Comparing the bytes of `a`'s tail with `b`'s head does not work at all:
they are two different, consecutive parts of one frame, not two copies of the same bytes -
item 1.

### 4.4 The bit reservoir: a second, stricter rule

**What it is.** As described in the glossary: a frame may store its audio data in spare
room left by earlier frames. Each frame declares how far back its data starts
(`main_data_begin`) and how much data it uses (`part2_3_length`).

**How the program models it.** Walking along an order, the program keeps a number called the
reservoir *level*: how many bytes of spare room are available behind the current position.
For every frame it checks:

- *Can this frame reach back as far as it says?* If `main_data_begin` is larger than the
  level, the frame would read bytes that do not exist (or belong to someone else). The
  order is impossible at this point - a **reservoir break**.
- Then it updates the level: `level_after = main_data_begin + bytes this frame adds -
  bytes this frame uses`.

Note the update starts from the frame's own `main_data_begin`, not from the previous level.
This was a hard-won detail (checked byte by byte against the `minimp3` decoder): any spare
room the frame does not point back to is gone, it is not carried forward. The first
version got this wrong and rejected the true order by chunk 7 (`CLAUDE.md` TODO list).

When a frame is cut by a chunk boundary, its side information is rebuilt from the two
chunks (`decodeSplitFrameSideInfo`) and checked like any other frame - which is only
possible once we know which chunk comes next. This is why the reservoir check is a check
on *pairs and paths*, not on single chunks.

**Why it matters.** The reservoir check is the only rule that can say "this order is
*impossible*" beyond simple header alignment. It never rejects the true order (verified on
every fixture where the metadata is right). But it is a *path* rule: the level depends on
everything that came before, so it can not be turned into a simple per-arrow yes/no... or
so it seemed. The next section is about the part of it that *can*.

### 4.5 NEW - the reservoir arc filter

**What it does.** For every arrow `a -> b` the program asks: *if we arrive at `a` with the
reservoir completely full - the best possible situation - can `b` still follow `a` without a
reservoir break?* If the answer is no, the arrow is deleted.

**Why this is safe (the key argument).** It relies on one property of the reservoir check:
it is **monotone in the entry level**. In plain words, "arriving with more spare room never
hurts". Look at the check: the only place where the level matters is the test
`main_data_begin > level`. A higher level can only make that test easier to pass. And the
level *after* a frame does not depend on the level before it at all (it is
`main_data_begin + added - used`). So if a step fails from a full reservoir, it fails from
every reservoir - in every possible order, in every context. The DFS would reject this
arrow anyway, every time it tried it; the filter just rejects it once, up front.

**Why it was worth doing if the DFS rejects those arrows anyway.** Because several other
stages look at the graph *before* any reservoir check runs:

- AC-3 (4.6) counts how many options each chunk has. An impossible arrow still counts as
  an option, so it can hide the fact that a chunk really has only one choice.
- The supernode gluing (4.10) needs chunks with exactly one option.
- The global matching (4.12) picks one successor per chunk by cost. An impossible arrow
  with a low cost is a trap.
- Every arrow costs PCM decodes to score (4.7, 4.9).

**Effect.** The filter removes 40-85% of arrows on CBR files and **zero true arrows on
every fixture** (the log line `[vs source: 0 true arcs removed]` is the report card). It
lets AC-3 cascade much further: on `cbr128_30s` at 1 KB the graph goes from 11 003 arrows
to 1 437. Graphs became 5-20x smaller overall. This is the single biggest reason the new
pipeline is faster.

**Its side effect.** The filter never removes a true arrow, but it changes *which* valid order
the DFS meets first. On a very small graph the first valid order can be slightly wrong. This
is what happened in the 3 cells that used to be exact and are now wrong (chapter 6). With the
filter off, all three are exact again.

**Its gate.** The argument above assumes the reservoir model is correct for this file. If
the model wrongly rejected a true pair, the filter would delete a true arrow, and AC-3 could
then *force* a wrong one. So the filter only runs when the reservoir check passed the true
order (see 4.1 and chapter 9). `RP_RESV_FILTER=0` turns it off, `=1` forces it past the
gate.

### 4.6 Arc consistency (AC-3), and the bug the filter exposed

**What it does.** Repeat until nothing changes:

- if a chunk `b` has exactly one possible predecessor `a`, then `a -> b` is forced: delete
  every other arrow out of `a` (they would leave `b` without a predecessor);
- if a chunk `a` has exactly one possible successor `b`, then `a -> b` is forced: delete
  every other arrow into `b`;
- nothing may come before chunk 0.

Each deletion can create new single-option chunks, so it cascades.

**Its gate.** Same reason as the filter: if the structural rule has a blind spot (a true
pair missing from the graph), a chunk can look like it has "only one option" when the real
option is missing, and AC-3 would force a wrong arrow. So AC-3 only runs when the
pre-shuffle check found zero broken true pairs. `test7.mp3`, which has 2, correctly skips
it.

**The bug (fixed in `844fe4b`).** The successor-side rule ("only one successor, so it is
forced") was applied to every chunk - including the *last* chunk of the file, which has no
true successor at all. Before the filter, every chunk had many arrows, so this never
mattered. After the filter made graphs very sparse, it happened: the last chunk had one
leftover (wrong) arrow, AC-3 forced it, and deleted a *true* arrow into that chunk. Benchmark
runs made with this bug (`devg025`, `devg100`) are marked as contaminated and were rerun.
Now the successor-side rule only applies once we know which chunk is the end (the final
fragment, when the file does not end exactly on a chunk boundary).

**Lesson.** Making one stage stronger can wake up a dormant bug in the next. This one was
caught because every run prints how many true arrows each stage removed.

### 4.7 Listening to the junction: the PCM flux score

This stage existed before this branch, but everything new builds on it, so here is how it
works.

**The idea.** If `b` really follows `a`, then decoding `a` followed by `b` gives smooth,
continuous music. If not, the decoder produces a frame made of two unrelated halves, plus a
jump to a different moment in the song. That sounds like a click, and it shows up as a spike
in spectral flux.

**How it is measured** (`pcm_junction.cpp`):

1. Take a window of chunks: up to three chunks of the already-placed path (the *left
   side*), then the candidate `b` (the *right side*).
2. Decode it all the way to sound with `minimp3` (a small, public-domain MP3 decoder that is
   included in the project), starting with a fresh decoder. The earlier left chunks are
   warm-up: the decoder needs some frames before it produces the same samples as a decoder
   that played the whole file. With enough warm-up the window decode is *sample-exact*
   against decoding the full file - this was verified on every fixture.
3. Slice the sound into overlapping pieces (1024-sample FFT, hop of 576 samples), compute
   the spectral flux between consecutive pieces.
4. Score = the highest flux just after the junction, divided by the median flux in the
   audio before it. Dividing by the "normal" flux makes the score fair across quiet and loud,
   calm and busy music.

**Window size is fixed in bytes, not in chunks.** Decoding starts at least 3 KB / 8 frames
before the junction and stops at least 1 KB / 4 frames after it. At 1 KB chunks that is
exactly the whole window; at 8 KB chunks it avoids decoding seconds of audio per score,
which would make every score expensive and let normal musical changes far from the
junction drown the spike.

**How good is it?** `pcm_probe` measured it on the true orders: the true successor gets the
best score among all candidates in 68-96% of cases (older header-only signals: 11-51%).
AUC 0.77-0.94. It is the strongest signal the project has.

**What it is not.** It is not a yes/no test. Some true junctions *do* spike (a drum hit
right at the cut), so the score is used to *rank* and to *add up*, never to reject an arrow
by itself. Measurements that looked tempting - loudness jumps, peak level, high-frequency
bursts - were noise.

**Why `log1p(flux)` everywhere.** Flux ratios are very uneven: most true junctions are
around 1, wrong ones around 3, but some reach 50. Taking `log(1 + flux)` squeezes the big
values so one huge spike does not outweigh many small differences. Adding up logs is also
the same as multiplying the ratios, which is the natural way to combine independent pieces
of evidence.

**Cost.** One window decode takes about 0.3-0.8 ms at 128 kbps and above, ~1.8 ms at
32 kbps (more frames per byte). With tens of thousands of arrows and several contexts each,
decoding is the main cost of the whole program. That is why the next section matters.

### 4.8 NEW - `LeftContext`: decoding the left side only once

**The observation.** When the program scores several candidates `b1, b2, b3...` after the
same path, the left side of every window is identical. And the decoder's work on the left
side does not depend on the candidate at all: everything up to the frame that straddles the
junction is the same.

**What `LeftContext` does.** Decode the left side once, keep the decoder's internal state at
the junction frame, keep the pre-junction spectra and the "normal flux" baseline. To score a
candidate, copy that state and decode only the right side (about 1 KB / 4 frames instead of
4 KB).

**Why it is safe.** The result is bit-identical to decoding the whole window
(`test/left_context_check.cpp` checks this). It is a pure speed-up, 1.5-2.9x cheaper
scoring.

**A small cache** keeps the 64 most recent left sides, because the DFS and the repair tend
to score many candidates after the same few paths.

### 4.9 Locking the obvious edges (confident-edge locking)

Existed before; described because the new stages run inside its loop.

**The idea.** Some junctions are obvious: `a -> b` sounds smooth while every other option
for `a`, and every other option for `b`, sounds terrible. Such an arrow can be *locked*
(forced), deleting all its competitors before the search starts - like AC-3, but driven by
sound instead of structure.

**The difficulty: context.** The flux of `a -> b` depends on what comes *before* `a` (the
warm-up chunk), which is not known yet. So `a -> b` is scored once for each possible
predecessor `p` of `a` (window `[p, a, b]`), up to 8 of them (an even sample if there are
more), and the median is taken.

**The rule.** Lock `a -> b` when its median flux beats every other successor of `a` *and*
every other predecessor of `b` by a factor of at least `tau`. Then run AC-3 again, and
repeat (up to 8 rounds) - each lock makes the graph smaller, which can make more locks
obvious.

**Why a schedule of thresholds (1.5, then 1.25, then 1.1).** A wrong lock is fatal for the
DFS: it deletes the true arrow. So the first attempt uses a strict threshold (1.5), where
measurements showed zero wrong locks on every completable fixture. If that attempt does not
find a complete order, the whole thing restarts from the unlocked graph with 1.25, then 1.1.
Lower thresholds lock more and make the search easier, but risk wrong locks. The repair
stage (4.13) works on the graph *before* locking, so it can undo a wrong lock.

**Why the decode cap counts decodes instead of measuring time.** On very dense graphs, one
locking round would need hundreds of thousands of decodes. Locking is skipped when the
estimate exceeds 640 000 (`RP_LOCK_MAX_DECODES`). This cap is a *count*, deliberately not a
time limit: a time limit would make the result depend on how many CPU cores the machine
has (more cores = more decodes in the same time = maybe a different answer). A count-based
cap gives the same answer on every machine; only the wall-clock time differs. Scoring runs
in parallel (`RP_THREADS`) and gives identical results with 1 or 16 threads.

**Memo.** A window's flux depends only on its three chunks, so every result is stored in a
table (`FluxMemo`) shared by all lock rounds, all tau attempts, and the global matching.
That is why the CBR example in chapter 5 says the matching had "0 new windows" to score -
locking had already scored them all.

### 4.10 Gluing forced chains: supernodes

**What happens.** After filtering, AC-3 and locking, many chunks have exactly one successor,
which has exactly one predecessor. Such runs are glued into one *supernode*. The search
treats a supernode as a single piece.

**Why.** Every glued chunk is one less decision. On `test7_shorter` locking brought 164
supernodes down to 49, and the search went from "never finishes" to "finds the exact order
after 55 steps". In the benchmark, the reservoir filter + AC-3 + locking often leave fewer
than 40 supernodes for a 30 s file (the three runs analysed in chapter 6 had 9, 10 and 39).

### 4.11 The depth-first search (DFS)

**What it does.** Start with chunk 0's supernode. At each step, list the supernodes that may
come next, sort them from best-sounding to worst, try the best one: place it, update the
reservoir level, check for breaks; if the reservoir breaks, try the next candidate; if no
candidate works, step back (backtrack) and try the previous step's next candidate. The first
time all supernodes are placed, stop - this is "first-valid" search.

**How candidates are sorted.** A combination of three scores: the change in loudness
(`global_gain`) and coded-bandwidth (`big_values`) across the junction, a coarse spectral
comparison (from a partial decoder, "Milestone 2"), and the PCM flux (4.7), which is
calibrated to dominate. The weights are set per file by sampling random arrows and scaling
each score by its spread, so no magic constant has to fit every file.

**Bounding it.** On large graphs an exhaustive DFS never finishes. So for more than 120
supernodes it uses *iterative broadening*: first try only the single best candidate at each
step (beam 1, i.e. greedy), then the best 2, 3, 4, 6, 9, then unlimited - within a time
budget (45 s, 30 s for lock attempts that still have a fallback). The program always
terminates.

**Why first-valid and not "best of all".** Keeping the search going to find the *cheapest*
complete order sounds better, but was measured worse: with the older header scores the
cheapest order was *not* the true one (only 25% correct vs 76-100% for first-valid,
`CLAUDE.md` item 7). A cheap score is good for ranking nearby choices, not necessarily as a
global goal. This lesson matters again in chapter 7.

**The wall the DFS hit.** Even with PCM ranking, on hard files the DFS took one wrong turn
early (in the recorded `test7_short` case, ~26 chunks in), and then spent its whole budget exploring every possible
continuation of that wrong start. A DFS can only undo its *latest* decisions; a mistake made
early is buried under all the later ones. The notes of 2026-10-08 say it plainly: *"The
bottleneck is now the search, not the signal."* That sentence is the reason for the next
stage.

### 4.12 NEW - global matching: solving the whole puzzle at once

**The idea in one sentence.** Instead of building the order from left to right, give every
supernode its best-sounding successor *all at the same time*, under the rule that no
supernode may be chosen as successor twice - and solve that exactly.

**Why this helps where the DFS failed.** The DFS's problem is order: a choice at step 26 is
made before anything about step 300 is known, and can not be revisited cheaply. The matching
has no order. Every choice is made knowing every other choice. A chunk that sounds best
after two different chunks gets assigned to the one where it matters more, globally.

**What exactly is solved.** This is the classic *assignment problem*:

- left side: every supernode in its role as "predecessor";
- right side: every supernode in its role as "successor";
- a line from left `s` to right `t` for every arrow `s -> t`, with cost =
  `log1p(median flux)` of that junction;
- choose lines so every left node and every right node is used exactly once, with the
  smallest total cost.

Two details make it fit our problem:

- **The start.** Chunk 0's supernode can not be anyone's successor. Its "successor" slot on
  the right side is reused as a virtual **END**: the supernode assigned to END is the last
  one of the file. If the file has a *final fragment* (a last chunk shorter than the rest),
  only that one may end; otherwise any may, at a neutral cost.
- **Context.** As in locking, the flux of `s -> t` depends on what comes before. If the
  supernode has 2 or more chunks, its own second-to-last chunk is the warm-up - known for
  sure. If it is a single chunk, the median over up to 8 of its possible predecessors is
  used. Junctions that can not be measured (e.g. silence before the cut) get the median
  cost of all measured ones - "no evidence either way".

**How it is solved.** With the *successive shortest path* method (Dijkstra's algorithm with
"potentials"). In plain words: the problem is drawn as a network where one unit of "flow"
goes from a source through each left node, across one line, through one right node, to a
sink. The program sends units one at a time, each along the currently cheapest route,
allowing earlier choices to be rerouted when a cheaper overall arrangement appears. After
`N` units, every node is matched and the total is provably the minimum. Costs are converted
to integers (x 10 000) so the arithmetic is exact. On our graphs (sparse after the filter) it
takes about 0.01-0.1 s.

**Why the answer is not yet an order: cycles.** The assignment only promises "everyone has
one successor and one predecessor". That describes a path from chunk 0 to END *plus* any
number of closed loops, e.g. `x -> y -> z -> x`. (The assignment is what mathematicians call
a *relaxation* of the Hamiltonian path: it drops the "one single path" requirement, which is
exactly what makes it easy to solve.) In practice there are only 0-3 cycles.

**Cycle splicing.** Each cycle is cut open at one of its arrows and inserted into the path
between two neighbours `x` and `y`, where both new junctions (`x -> start of cycle` and
`end of cycle -> y`) must be real arrows in the graph. Among all places and all cut points,
the one that adds the least cost is chosen; then the next cycle, until none is left. If a
cycle can not be inserted anywhere, the matching reports failure and the program moves on.

**How good is the raw matching?** Very good at the junction level: 92-98% of junctions right
in a few hundredths of a second (e.g. `cbr128_30s` 2 KB: 230/234; `bohemoan` 4 KB:
344/351). But it ignores the bit reservoir completely, and a handful of wrong junctions are
usually enough to cause a reservoir break somewhere. So it is almost always followed by
repair (4.13), which starts by fixing the breaks.

**When it runs** (`RP_MATCH`):

- `1` (default) - only when a lock attempt's DFS failed to find a complete order.
- `2` - before the DFS.

Why `1` is the default: both were measured on the first matching version (`57965af`, before
the reservoir filter existed). Matching-first was slightly worse: 41 vs 42 exact on main and
39 vs 41 on held-out, and every cell where the two differed went from exact (DFS first) to
wrong (matching first). Note that this does *not* mean a DFS result is always safe - chapter 6
shows three wrong DFS results.
- `0` - never.

**Decode cap.** On very dense graphs (a 6-minute 32 kbps file at 1-2 KB: ~700 supernodes x
~140 successors x 8 contexts), just scoring all junctions took over 15 minutes. So the
number of predecessor contexts is halved (8, 4, 2, 1) until the decodes still needed fit
under 100 000 (`RP_MATCH_MAX_DECODES`); if even one context is too many, the matching is
skipped. A count, not a time, for the same reason as in 4.9.

### 4.13 Repair: fixing a nearly-right order with block moves

The repair existed before this branch (it ran after the DFS). It was substantially
rewritten here, because the matching needed it to be fast and needed it to fix reservoir
breaks, not just lower flux.

**What a move is.**

- *Block move*: take chunks `i..j` out of the order and put them back after position `k`.
  This changes 3 junctions.
- *Block swap*: exchange two separate blocks. This changes 4 junctions. It is needed for the
  shape `A-X-Y-B` that should be `A-B-Y-X`, where every single block move on the way would
  pass through an impossible order.

Only moves where *every new junction is an arrow of the full graph* are considered - the
graph after the reservoir filter but **before AC-3 and locking**. This is deliberate: a
wrong lock (or a wrong AC-3 force) deleted the true arrow from the search graph; repair can
still use it, so repair can undo those mistakes.

**What "better" means - a ranked cost.** Orders are compared by three numbers, in this order:

1. **reservoir breaks** (only when repairing an invalid order, e.g. from the matching): an
   order with fewer breaks is always better, whatever it sounds like;
2. **unmeasured junctions**: how many junctions the flux window can not score;
3. **summed `log1p(flux)`** over the measured junctions.

Why breaks first: an order with a reservoir break is *impossible*; no amount of smooth sound
makes it right. Why unmeasured second: when unmeasured junctions counted at a neutral value,
the repair learned to "win" by turning a high-flux junction into an unscorable one
(`test7_short`), hiding evidence instead of improving the order.

On a valid order, a move must keep it valid. On an invalid order, a move must reduce the
number of breaks.

**Best improvement, one move per pass.** Each pass scans all allowed moves, takes the single
best, applies it, and starts again (up to 50 passes).

**Making it fast enough - the rewrite.** A naive repair rebuilds and re-scores the whole
order for every candidate move: tens of thousands of candidates x hundreds of junctions,
each needing decodes. The new version is about 10-50x faster through three ideas:

- **Suspects first.** Wrong junctions are usually high-flux (true ones have median ~1.05,
  wrong ones ~3). So each pass first looks only at moves that cut one of the worst
  `max(16, n/20)` junctions (plus, on an invalid order, the junctions just before each
  break). Only if none of those helps does it scan every move.
- **Segments instead of copies.** A candidate is described as a list of pieces of the
  current order ("0..k, then i..j, then k+1..i-1, then j+1..end"). Within a piece, a
  junction's window is the same as in the current order once you are 3 chunks in, so its
  flux comes from a running total (prefix sums). Only the first 3 junctions of each piece
  need a fresh lookup.
- **Reservoir re-sync.** After a cut, the reservoir level usually matches the current
  order's level again within a frame or two (because the level after a frame depends only on
  that frame). From that point on, the break count also comes from a running total.

Results: `test7_short` repair 0.67 s (was ~5 s), `cbr320_10s` 4 KB 0.06 s (was 3.4 s). Same
moves, same costs as before.

**Time budget.** 60 s by default (`RP_REPAIR_BUDGET`); when it runs out, the best move found
so far is still applied.

### 4.14 NEW - the minimum-gain rule for repair

**The problem.** Lowering summed flux is *usually* good. But when the program already has a
valid order, tiny improvements are mostly noise, and they can turn the true order into a
cheaper wrong one. Example: on `cbr192_10s` 2 KB a move that lowered the sum by 0.037 turned
a correct order into a wrong one. In contrast, the move that fixed `test7_short` lowered it
by 3.4.

**The study.** All 104 flux-only moves that the repair made on valid orders in the
development benchmark were collected, with the change in true junctions each caused
(`tmp/bench/move_study.txt`). Moves below 0.25 gain were harmful 30 times and helpful 4
times.

**The rule.** On a valid order, a move that only lowers flux (same breaks, same unmeasured
count) must lower the sum by at least `RP_REPAIR_MIN_GAIN` = **1.0**. Moves that fix breaks
or unmeasured junctions are not affected.

**How 1.0 was chosen, honestly.** 1.0 was set as a provisional value before the final
comparison. Then 0.25 and 1.0 were compared on both the main and the held-out set: 49 vs 48
exact on main (a tie, in practice) and 44 vs 41 on held-out. So the held-out set helped pick
the value - it is no longer a clean, untouched test for this one setting. See chapter 9.

**Its cost.** A threshold always cuts both ways. Chapter 6 shows the three cells where the
new pipeline is wrong but the old one was exact: in all three, the move that would have fixed
the order was found and rejected because it gained only 0.26, 0.51 and 0.72.

**Consequence for the old "skip repair" rule.** Previously, an order found by the strictest
lock attempt was never repaired (one such repair had broken an exact result). With the
minimum gain in place, repair always runs; `RP_REPAIR_MIN_GAIN=0` restores the old skip.

### 4.15 The matching as a hint for the DFS

If the repaired matching is still invalid, its choices are not thrown away: the DFS is run
again (with a fresh time budget), and at every step the matched successor is tried *first*,
ahead of the normal ranking. Everything else (reservoir checks, backtracking) is unchanged.
This combines the global view of the matching with the DFS's guarantee of a valid order. A
result found this way is labelled `DFS search (matching-ranked)` and treated like any DFS
result.

### 4.16 Iterated matching (built, measured, switched off)

**The idea.** The matching ignores the reservoir. So: find where its order breaks the
reservoir, forbid the arrow at each break, and solve again; repeat a few times, keep the
answer with the fewest breaks.

**Result.** In the benchmark it changed *no* outcome (`RP_MATCH_ITERS`, default 0). The
reason was not investigated; a likely one (a guess, not measured) is that the repair already
fixes the breaks, so the extra rounds have nothing left to add. Kept in the code for experiments, off by default - the rule followed throughout
this project is that a change that does not move a measured outcome does not become a
default.

### 4.17 Output, the provenance label and the stability gap

`output.mp3` is the chunks in the found order. Then the log prints:

- `Result provenance:` - *where* the order came from: `DFS search`, `DFS search
  (matching-ranked)` or `best-effort (global matching + repair)`, plus `+ N repair move(s)`
  when repair changed it after it was found.
- `stability gap` - how much the summed flux would rise under the cheapest valid move the
  repair rejected. A small gap means a nearly equally good, different order is one move
  away.
- With the source available (benchmarks only): chunks in place, true junctions, and with
  `RP_SCORE_ORDER=1` the summed flux of the found order vs the true order.

**What the label does *not* mean.** It was introduced so that outputs from the matching
would be marked "best-effort" and DFS outputs could be trusted. With the reservoir filter,
that separation broke: the filtered graph is so small that the DFS's *first* valid order can
itself be wrong (3 cases in the benchmark). And the stability gap did not separate right
from wrong either (wrong outputs had gaps of 0.013 and 0.037, but an exact one had 0.046).
So today **no label means "verified"**. This is the main open problem, chapter 7.

---

## 5. A worked example: reading one real log

`CBR.mp3`: a 658-chunk constant-bitrate song at 1 KB - the hardest of the old fixtures.
The committed regression baseline (`test/regression_baseline.tsv`) records it as FAILED. Run for this document with `RP_SEED=1
RP_TIME_BUDGET=20 build/rp test/CBR.mp3` (as `test/regression.sh` does). Key lines:

```
Edge locking total: 37 edges (20434 decodes, 0.34s) [vs source: 36/37 correct]
```
Locking found 37 confident edges; one of them is wrong (we know only because of the hidden
source list). The repair will be able to undo it, because repair uses the pre-lock graph.

```
Adjacency graph: 2844 edges, 456/658 chunks have exactly one valid successor
Supernodes after chain collapse: 202
```
After filter + AC-3 + locks, 658 chunks became 202 supernodes. (The old notes recorded 1/658
chunks with one successor and ~21 000 arrows on this file.)

```
  beam=1 ... 44 nodes, 0.09s (exhausted, no ordering)
  beam=2 ... 15591552 nodes, 19.91s (stopped: budget)
```
The DFS fails: greedy dead-ends at once, and beam 2 runs out of time - the "wall" of 4.11.

```
Global matching: scoring 0 new windows (<= 8 contexts per edge) ...
Global matching: 202 supernodes, 0 decodes (8 ctx), 1 cycle(s), 1 spliced, cost 180.199,
  0.01s - reservoir OK [vs source: 622/657 true junctions]
```
The matching needed no new decodes (all junctions were in the memo from locking), took
0.01 s, produced one cycle which was spliced in, and the result is reservoir-valid with
622 of 657 junctions right (94.7%).

```
  repair pass 1 (suspects): swap [200..259] <-> [300..619], cost (unmeasured, summed flux)
    (6, 656.104) -> (6, 653.996) ... [vs source: true junctions 622 -> 619]
  ... passes 2-5, each lowering the flux by 1.07-2.11 ...
  repair pass 6: best flux-only move lowers the summed flux by only 0.987 (< 1.000) - stopping
Matching repair: 5 move(s), 2.94s - reservoir OK [vs source: 610/657 true junctions]
```
This is instructive. Each repair move lowered the summed flux by *more* than the minimum gain,
yet each one *removed* true junctions: 622 -> 610. On this file, **the order with lower flux
is further from the truth**. No threshold on flux can fix that - the objective itself points
the wrong way here. This is an "objective failure" (chapter 7).

```
Result provenance: best-effort (global matching + repair) + 5 repair move(s)
Reconstruction vs source: 35/658 chunks in original position (5.3%)
Reconstruction junctions: 610/657 are true successor pairs
```
The output is complete, valid and wrong. 92.8% of junctions are right, yet only 5.3% of
chunks are "in place", because an early block moved and shifted everything after it - which
is why junctions are the measure to trust.

`test/regression.sh` reports this file as "OK" (it only flags a drop below the baseline, and
the baseline was FAILED). Counted honestly, it is one more wrong output: before, the program
said "I could not do it"; now it gives an answer that is 93% right and does not say it is
wrong.

---

## 6. Results

### The benchmark

- **Main set (64 cells):** 16 files - CBR at 32, 64, 128, 192, 320 kbps; VBR at quality 2
  and 5; 10 s and 30 s clips (taken from 120 s into a source track); a 6-minute 32 kbps CBR
  (`bohemoan-rhapsody-2k`) and a 6-minute VBR (`Queen ... VBR`) - each at chunk sizes 1, 2, 4
  and 8 KB.
- **Held-out set (56 cells):** the same kinds of files, cut from 200 s into the source -
  material the algorithm was not developed on (with the one exception in 4.14).
- Seed 1, one run per cell. Timeout 900 s.
- Compared configurations:
  - **PCM final** - `1c5abe1`, the pipeline before this branch (DFS + locking + repair);
  - **global matching v2** - `57965af`, the first matching version (no filter, no
    `LeftContext`, no minimum gain);
  - **current** - `844fe4b` defaults. HEAD (`60e8c4a`) is the same algorithm: its only code
    change since `844fe4b` is the `+ N repair move(s)` text in the provenance line (checked
    with `git diff 844fe4b HEAD`).

### Totals

| set | config | exact | wrong | failed | junctions recovered | total wall time |
|---|---|---|---|---|---|---|
| main | PCM final | 35 | 0 | 29 | 80.3% | 60 min |
| main | global matching v2 | 42 | 8 | 14 | 89.7% | 80 min |
| main | **current** | **49** | 8 | **7** | **93.9%** | **27 min** |
| held-out | PCM final | 33 | 0 | 23 | 82.7% | 37 min |
| held-out | global matching v2 | 41 | 9 | 6 | 91.9% | 42 min |
| held-out | **current** | **44** | 8 | **4** | **94.3%** | **16 min** |

Speed: total time is 2.2-3x lower; per cell (paired median) the current pipeline is 4-5x
faster (quartiles 1x-30x). The median run takes 0.2-0.3 s, against 6.5-32 s before. (Eight
old runs had wall times inflated by the machine going to sleep; they were rerun and the
table uses the reruns.)

### Cell by cell (what changed where)

Totals can hide things: a gain of 15 exact cells could come with 10 exact cells lost
somewhere else. So each cell was paired with itself (same file, chunk size, seed) - PCM
final vs current:

| PCM final -> current | main (64) | held-out (56) |
|---|---|---|
| exact -> exact | 34 | 31 |
| **failed -> exact** | **15** | **13** |
| failed -> wrong | 7 | 6 |
| failed -> failed | 7 | 4 |
| **exact -> wrong** | **1** | **2** |
| exact -> failed | 0 | 0 |

So, in plain words: **28 cells that used to fail are now exact; 3 cells that used to be
exact are now wrong; 13 cells that used to fail now give a wrong (but mostly near-complete)
answer; nothing that worked now fails.**

**The 3 regressions, looked at closely.** All three are CBR at 4-8 KB, solved by the DFS
on the strictest lock attempt in well under a second:

| cell | DFS order | rejected repair move | found vs true summed flux |
|---|---|---|---|
| main `cbr320_10s` 4 KB | 93/97 junctions | gain 0.262 < 1.0 | 81.053 vs 80.790 |
| held-out `cbr192_30s` 8 KB | 85/88 | gain 0.510 < 1.0 | 72.684 vs 72.174 |
| held-out `cbr320_30s` 8 KB | 143/146 | gain 0.718 < 1.0 | 121.344 vs 120.626 |

In each case the true order is cheaper, it is exactly one block swap away, the repair found
that swap - and the minimum-gain rule (4.14) rejected it, because the gain was below 1.0.
**The reservoir filter causes these three.** Rerunning the same cells today with
`RP_RESV_FILTER=0` (everything else unchanged) gives EXACT on all three (run 2026-10-20). On the
much smaller filtered graph the DFS's first valid order is slightly wrong (3-4 junctions), and
the safety rule that protects against noisy moves blocks the fix. With
threshold 0.25 all three of these were exact - but 0.25 lost more cells elsewhere, where
small moves did harm (paired, 0.25 vs 1.0: main 1 cell better at 0.25 and 2 worse; held-out
3 better and 6 worse).

**The 13 "failed -> wrong" cells** are mostly near-misses: e.g. `bohemoan` 2 KB 685/702
junctions, Queen VBR 1 KB 5292/5321, `cbr320_30s` 2 KB 579/586. Where PCM final reported
"no order" (with its deepest partial path), the new pipeline delivers a complete file whose
junctions are 94.9% to 99.7% right (median 98.9%).

---

## 7. The wrong outputs: what they are and why they happen

16 of the 120 cells end "wrong" (8 main, 8 held-out), plus `CBR.mp3` from chapter 5. They
fall into two very different groups.

### Search failures (9 cells)

The true order is *cheaper* than the output, but the program did not get there:

- the fix needs more than one repair move at once (the repair only looks one move ahead,
  and the intermediate order may be invalid or more expensive), or
- the fix is one move away but gains less than 1.0 (the three regressions above).

These are fixable in principle with a better search: a repair that can make a temporarily
worse move to escape a dead end (perturbation / "iterated local search"), or an exact
solver.

### Objective failures (7 cells)

The *output* is cheaper than the true order: Queen VBR 2 KB and 4 KB, `bohemoan` 2 KB and
4 KB, held-out `cbr32_30s` 1 KB, `cbr192_30s` 2 KB, `cbr320_30s` 2 KB (and `CBR.mp3`, where
repair moved further from the truth with every move).

This is a fundamentally different problem. The program's definition of "best" -
lowest summed flux, among valid orders - simply prefers a wrong order on these files. A
perfect search would find the same wrong order. No repair tuning, no threshold, no extra
search time can fix this. Only a better definition of "best" can: a per-junction score that
is better calibrated (so that a sum of scores means something), or more evidence per
junction.

This is the same lesson as the "best-of" experiment in 4.11, one level up: a good *ranking*
signal is not automatically a good *global objective*. PCM flux is much closer to being one
(in 97-100% of tested swaps the true pair had the lower flux) - but over hundreds of
junctions, a few percent of exceptions add up to a cheaper wrong order.

### Why the label can not warn about them (yet)

The matching's outputs were meant to be the risky ones and are labelled "best-effort". But
with the filter, 5 wrong outputs come from the DFS (3 wrong as first found, 2 changed by
repair), and the stability gap does not separate wrong from right. So currently the program
can not tell a user "this one is probably wrong". That is the honest cost of this branch:
PCM final *never* produced a wrong order; it said "failed" instead.

---

## 8. Keep or roll back? The decision and its reasoning

**The question asked was:** finish the algorithm improvements, present them, and roll them
back if they are not enough.

**Verdict: keep them.** The reasons:

1. **The gains are large and on both sets.** Exact results +14 on main (35 -> 49) and +11 on
   held-out (33 -> 44). Failures 29 -> 7 and 23 -> 4. Junctions recovered from ~81% to ~94%.
2. **They are not bought with lost successes.** The paired comparison shows 28 cells
   failed -> exact and 0 cells exact -> failed. Only 3 of 120 cells went exact -> wrong, each
   by 3-4 junctions, each one rejected repair move away from exact.
3. **It is much faster.** 2-3x less total time, 4-5x per cell, median run ~0.25 s.
4. **The risk is understood and documented.** The wrong outputs are explained (chapter 7),
   reproducible, and split into a fixable kind and a kind that needs a better signal.

**What keeping them costs.** The program can now return a complete-but-wrong order where it
used to admit failure, and its provenance label does not reliably warn about it. If, for
the intended use, *a wrong answer is worse than no answer*, that is a real argument for the
old behaviour. Two cheaper alternatives to a full rollback exist:

- `RP_MATCH=0` turns off the matching. This is **not** a cheap fix: the matching is the
  source of 11 of the 16 wrong outputs, but also of 16 exact results (10 main, 6 held-out,
  provenance `best-effort (global matching + repair)`). Turning it off could lose up to 16
  exact results to remove 11 wrong ones. "Up to", because a later lock attempt's DFS might
  still solve some of them; a 120-cell rerun would give the exact number. Not measured;
- treat every output as "best-effort" until a confidence measure exists (chapter 10).

**If a rollback is wanted anyway,** it should be done by *not merging* the branch, not by
deleting or resetting it: `global-matching` exists only on this machine (it was never
pushed), so its commits are the only copy of this work.

---

## 9. Honest caveats

- **One seed.** Every benchmark number is from seed 1. Earlier work saw seed-dependent
  outcomes on some files (e.g. Queen 4 KB was wrong on all 3 seeds; `test7_short` finished
  at different lock attempts on different seeds). Differences of 1-3 cells are within noise.
- **The held-out set is not fully clean.** It helped choose `RP_REPAIR_MIN_GAIN=1.0`
  (4.14). For every other setting it is untouched.
- **Two gates look at the unshuffled file.** The reservoir filter, AC-3 and edge locking only
  run if the program's own rules pass the *true* order (4.1). In this research setup we have
  the original file, so we can check that. In a real carving situation we would not. In
  practice the gate is about the *file type* (e.g. "does the reservoir model hold for this
  encoder?"), and could be replaced by a check that does not need the answer - for example,
  whether the shuffled graph still has a valid path after the filter - but that has not been
  built.
- **The fixtures are few.** Two source tracks for the benchmark plus the older fixtures.
  Different music (silence, speech, very dense electronic music) may behave differently -
  long silent passages, for example, are ambiguous by nature (any order of identical silent
  frames is "right").
- **MPEG-1 Layer III only** for the decoding-based parts (the partial decoder and side-info
  parsing paths are tested only on MPEG-1 files).
- **"Junctions recovered" counts complete outputs and partial paths alike.** For failed runs,
  the deepest partial path's junctions are counted.

---

## 10. What comes next

In rough order of expected value per effort:

1. **A confidence signal for outputs.** Without it, a wrong output can not be told from a
   right one. Candidates: the margin between the found order and the best *different* order
   found by a few perturbed repairs; or calibrated probabilities (item 4).
2. **Repair escapes (iterated local search).** 9 of the 16 wrong outputs are search failures.
   Allow a few "kick" moves (random block moves that may worsen the cost) followed by a new
   repair, and keep the best order seen. The three regressions in chapter 6 would likely be
   fixed by a smarter acceptance rule for tiny gains (for example, accept a sub-threshold move
   when it also lowers the maximum single-junction flux).
3. **Cheaper scoring at low bitrate.** `bohemoan` at 2 KB spends 221 s just on locking
   decodes; a frame-capped window would cut that.
4. **Edge probabilities and a most-probable path** - `PLAN_probabilistic_hamiltonian_path.md`.
   Turn each junction's evidence into a calibrated probability, then search for the *most
   probable* complete order with an exact or near-exact solver, handling the reservoir with
   lazy constraints. This is the route for the objective failures, and it gives a natural
   confidence measure. It is the largest piece of work.

---

## 11. Quick reference: the switches

The full table is in `CLAUDE.md` ("Environment variables"). The ones introduced or changed on
this branch:

| variable | default | what it does |
|---|---|---|
| `RP_RESV_FILTER` | auto (on if the reservoir model passes the true order) | 4.5 reservoir arc filter. `0` off, `1` force. |
| `RP_MATCH` | 1 | 4.12 global matching. `1` after a failed DFS, `2` before it, `0` off. |
| `RP_MATCH_MAX_DECODES` | 100000 | 4.12 decode cap for the matching. |
| `RP_MATCH_ITERS` | 0 | 4.16 iterated matching rounds (off: changed nothing). |
| `RP_REPAIR_MIN_GAIN` | 1.0 | 4.14 minimum summed-flux gain for a flux-only move on a valid order. `0` = old behaviour. |
| `RP_REPAIR_SUSPECTS` | max(16, n/20) | 4.13 how many worst junctions the first repair phase looks at. |
| `RP_REPAIR_BUDGET` | 60 | 4.13 repair time budget in seconds. |
| `RP_SCORE_ORDER` | off | print the found vs true summed flux (diagnostic, needs the source). |

Unchanged but central: `RP_SEED` (1), `RP_CHUNK_SIZE` (1024), `RP_LOCK_TAU` (1.5,1.25,1.1),
`RP_THREADS` (all cores), `RP_TIME_BUDGET`, `RP_PCM`, `RP_REPAIR`.

**Where the code lives:**

- `main.cpp` - the whole pipeline; `globalMatch` (matching + splicing), `repairOrdering`
  (repair), the reservoir filter block after `buildAdjacency`, `tryMatch` (matching ->
  repair -> hint rerun), the provenance output.
- `pcm_junction.cpp` - window decode, flux, `LeftContext`.
- `edge_lock.cpp` - locking and `scoreWindows` (parallel scoring shared with the matching).
- `adjacency.cpp` - `canFollow`, `buildAdjacency`, `propagateArcConsistency` (with the
  end-of-stream fix).
- `test/left_context_check.cpp` - proves `LeftContext` is bit-identical.
- Benchmark data and scripts: `tmp/bench/runs.csv`, `tmp/bench/move_study.txt`,
  `tmp/scripts/` (not committed).
