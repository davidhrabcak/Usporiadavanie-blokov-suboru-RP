cmake_minimum_required(VERSION 3.16)
project(rp_text)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(text_core STATIC
        dictionary/base_dictionary.hpp
        dictionary/standard_dictionary.hpp
        dictionary/standard_dictionary.cpp
        dictionary/two_sequence_dictionary.hpp
        dictionary/two_sequence_dictionary.cpp
        dictionary/two_sequence_dictionary_creator.hpp
        dictionary/two_sequence_dictionary_creator.cpp
        dictionary/three_sequence_dictionary.hpp
        dictionary/three_sequence_dictionary.cpp
        dictionary/three_sequence_dict_creator.hpp
        dictionary/three_sequence_dict_creator.cpp
        validation/base_validator.hpp
        validation/standard_validator.hpp
        validation/standard_validator.cpp
        validation/frequency_2_words_validator.hpp
        validation/frequency_2_words_validator.cpp
        validation/frequency_3_words_validator.hpp
        validation/frequency_3_words_validator.cpp
        algorithm/base_algorithm.hpp
        algorithm/backtrack.hpp
        algorithm/backtrack.cpp
        segment.hpp
        segment.cpp
        text_util.hpp)
target_include_directories(text_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(main main.cpp)
target_link_libraries(main PRIVATE text_core)

add_executable(example_use example_use.cpp)
target_link_libraries(example_use PRIVATE text_core)

enable_testing()

add_executable(test_standard_dictionary_validator test/test_standard_dictionary_validator.cpp)
target_link_libraries(test_standard_dictionary_validator PRIVATE text_core)
add_test(NAME test_standard_dictionary_validator
        COMMAND test_standard_dictionary_validator
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_frequency_dictionary_validator test/test_frequency_dictionary_validator.cpp)
target_link_libraries(test_frequency_dictionary_validator PRIVATE text_core)
add_test(NAME test_frequency_dictionary_validator
        COMMAND test_frequency_dictionary_validator
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
