// rp: reconstruct the order of a shuffled, chunked MP3 file from its bytes alone.
//
//   rp [input.mp3]      writes output.mp3; options are RP_* environment variables (config.hpp)
//
// The pipeline, one stage per call below:
//   1. loadProblem     cut the file into chunks, check the true order (diagnostic), shuffle,
//                      find the first chunk, derive the stream profile and per-chunk metadata
//   2. JunctionScorer  how well one chunk continues another (side info, spectrum, PCM flux)
//   3. buildGraph      which chunk can follow which (structure, bit reservoir, arc consistency)
//   4. reconstruct     lock confident edges, collapse forced chains, search (DFS, global
//                      matching), repair
//   5. writeOutput     write output.mp3 and score it against the source order
// docs/HOW_IT_WORKS.md explains each stage; CLAUDE.md has the measurements behind them.

#include "config.hpp"
#include "graph/chunk_graph.hpp"
#include "pipeline/input.hpp"
#include "pipeline/output.hpp"
#include "run_report.hpp"
#include "scoring/junction_scorer.hpp"
#include "search/reconstruct.hpp"

int main(int argc, char* argv[]) {
    const Config cfg = Config::fromEnv(argc, argv);

    // RP_REPORT (PLAN_test_automation.md): starts the clock; written on any return from main.
    RunReport::set("file", cfg.input);
    RunReport::set("status", "error");
    RunReport::recordEnvironment();

    Problem p;
    if (!loadProblem(cfg, p)) return 1;

    // References p.chunks and p.metas, which stay in place from here on.
    JunctionScorer scorer(p.chunks, p.metas, p.profile, p.chunkSize, cfg);
    printChunkTable(p.metas);

    const ChunkGraph graph = buildGraph(p.metas, p.chunks, p.pool, p.profile, p.chunkSize, p.splitTails,
                                        p.truth, cfg, p.origIndexOf);
    scorer.calibrateSpectral();
    scorer.calibratePcm(graph.adj);

    Reconstruction r;
    if (!reconstruct(p, graph, scorer, cfg, r)) return 1;
    if (!r.found) {
        reportFailure(p, r);
        return 0;
    }
    return writeOutput(p, scorer, r, cfg);
}
