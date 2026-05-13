cmake_minimum_required(VERSION 4.1)
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
        main.cpp
        main.hpp)
