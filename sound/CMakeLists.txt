cmake_minimum_required(VERSION 3.16)
project(rp)

set(CMAKE_CXX_STANDARD 17)
find_package(Threads REQUIRED)

# Everything but main(): rp and the dev tools link the same code.
#   src/mp3      MPEG frame parsing, per-chunk metadata, Layer III side info / main data, bit reservoir
#   src/graph    which chunk can follow which (adjacency, AC-3, gap slots for headerless chunks),
#                staged into the search graph (chunk_graph), forced-chain collapse (supernodes)
#   src/scoring  junction costs for the ranking (junction_scorer: side info, spectrum, full-PCM
#                flux via the vendored minimp3) and confident-edge locking
#   src/search   ordering the supernodes: DFS with iterative broadening, global min-cost
#                successor matching, block-move repair on the summed PCM flux
add_library(rpcore STATIC
        src/config.cpp
        src/config.hpp
        src/run_report.hpp
        src/mp3/Header.cpp
        src/mp3/Header.hpp
        src/mp3/frame_scanner.cpp
        src/mp3/frame_scanner.hpp
        src/mp3/chunk_meta.cpp
        src/mp3/chunk_meta.hpp
        src/mp3/huffman_tables.cpp
        src/mp3/huffman_tables.hpp
        src/mp3/main_data_decoder.cpp
        src/mp3/main_data_decoder.hpp
        src/mp3/reservoir.cpp
        src/mp3/reservoir.hpp
        src/graph/adjacency.cpp
        src/graph/adjacency.hpp
        src/graph/slot_fill.cpp
        src/graph/slot_fill.hpp
        src/graph/supernodes.cpp
        src/graph/supernodes.hpp
        src/graph/chunk_graph.cpp
        src/graph/chunk_graph.hpp
        src/scoring/pcm_junction.cpp
        src/scoring/pcm_junction.hpp
        src/scoring/pcm_decoder_impl.cpp
        src/scoring/edge_lock.cpp
        src/scoring/edge_lock.hpp
        src/scoring/junction_scorer.cpp
        src/scoring/junction_scorer.hpp
        src/search/dfs.cpp
        src/search/dfs.hpp
        src/search/global_match.cpp
        src/search/global_match.hpp
        src/search/repair.cpp
        src/search/repair.hpp)
target_include_directories(rpcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
                                  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})   # third_party/minimp3.h
target_link_libraries(rpcore PUBLIC Threads::Threads)

add_executable(rp src/main.cpp)
target_link_libraries(rp PRIVATE rpcore)

# Dev / measurement tools (tools/). Built in CI so they can't bit-rot against the core.
function(rp_tool name)
    add_executable(${name} tools/${name}.cpp)
    target_link_libraries(${name} PRIVATE rpcore)
endfunction()

rp_tool(evaluate_order)       # compare an output .mp3 with the original, metric by metric
rp_tool(sideinfo_probe)       # side-info continuity signals on the true order (PLAN_sideinfo_mainbits.md)
rp_tool(pcm_probe)            # full-PCM junction flux on the true order (PLAN_full_pcm_decode.md)
rp_tool(left_context_check)   # PcmJunction::LeftContext == scoreJunction, bit for bit (+ timing)
rp_tool(graph_check)          # buildAdjacency agrees with canFollow on every pair (test/ci_sanity.sh)
