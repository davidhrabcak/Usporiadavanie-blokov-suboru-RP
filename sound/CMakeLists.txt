cmake_minimum_required(VERSION 3.16)
project(rp)

set(CMAKE_CXX_STANDARD 14)
find_package(Threads REQUIRED)

add_executable(rp
        frame_scanner.cpp
        Header.cpp
        Header.hpp
        frame_scanner.hpp
        chunk_meta.cpp
        chunk_meta.hpp
        adjacency.cpp
        adjacency.hpp
        huffman_tables.cpp
        huffman_tables.hpp
        main_data_decoder.cpp
        main_data_decoder.hpp
        pcm_junction.cpp
        pcm_junction.hpp
        pcm_decoder_impl.cpp
        edge_lock.cpp
        edge_lock.hpp
        main.cpp)

add_executable(evaluate_order
        test/evaluate_order.cpp
        frame_scanner.cpp
        frame_scanner.hpp
        Header.cpp
        Header.hpp)
target_compile_features(evaluate_order PRIVATE cxx_std_17)

# Side-info-only continuity probe (PLAN_sideinfo_mainbits.md Step 1). Dev/measurement tool;
# built in CI only so it can't bit-rot against chunk_meta/adjacency changes.
add_executable(sideinfo_probe
        test/sideinfo_probe.cpp
        frame_scanner.cpp
        frame_scanner.hpp
        Header.cpp
        Header.hpp
        chunk_meta.cpp
        chunk_meta.hpp
        adjacency.cpp
        adjacency.hpp
        huffman_tables.cpp
        huffman_tables.hpp
        main_data_decoder.cpp
        main_data_decoder.hpp)
target_include_directories(sideinfo_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(sideinfo_probe PRIVATE cxx_std_17)

# Full-PCM junction probe (PLAN_full_pcm_decode.md steps 1-2). Dev/measurement tool.
add_executable(pcm_probe
        test/pcm_probe.cpp
        frame_scanner.cpp
        Header.cpp
        chunk_meta.cpp
        adjacency.cpp
        huffman_tables.cpp
        main_data_decoder.cpp
        pcm_junction.cpp
        pcm_decoder_impl.cpp
        edge_lock.cpp)
target_include_directories(pcm_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pcm_probe PRIVATE cxx_std_17)

target_link_libraries(rp PRIVATE Threads::Threads)
target_link_libraries(pcm_probe PRIVATE Threads::Threads)
