cmake_minimum_required(VERSION 3.16)
project(rp)

set(CMAKE_CXX_STANDARD 14)

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
        main.cpp
        main.hpp)

add_executable(evaluate_order
        test/evaluate_order.cpp
        frame_scanner.cpp
        frame_scanner.hpp
        Header.cpp
        Header.hpp)
target_compile_features(evaluate_order PRIVATE cxx_std_17)
