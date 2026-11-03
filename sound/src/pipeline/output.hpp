#ifndef RP_OUTPUT_HPP
#define RP_OUTPUT_HPP

#include "config.hpp"
#include "pipeline/input.hpp"
#include "scoring/junction_scorer.hpp"
#include "search/reconstruct.hpp"

// Writes output.mp3 in the order found, with the headerless chunks put into the slots after
// their demanders (by PCM flux, SlotFill), and scores it against the source order. Returns the
// process exit code.
int writeOutput(const Problem& p, JunctionScorer& scorer, const Reconstruction& r, const Config& cfg);

// No complete ordering: how right the deepest partial path was.
void reportFailure(const Problem& p, const Reconstruction& r);

#endif
