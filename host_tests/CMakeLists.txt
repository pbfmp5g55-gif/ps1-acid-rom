cmake_minimum_required(VERSION 3.16)
project(ps1_acid_rom_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# DSP voice library — pure C++17, no PS1 dependencies. The exact same .hpp
# files get included in the PS1 build under src/dsp/, just compiled with the
# mips toolchain. This lets us:
#   - hear each voice (wav output) on the host before touching PS1 SPU
#   - regression-test that a code change didn't subtly break a coefficient
#   - tune envelope decays / mod amounts against a reference recording
add_library(dsp STATIC
    ../src/dsp/bd_tbridge.cpp
)
target_include_directories(dsp PUBLIC ../src)

# wav writer + per-voice "render N seconds to a file" CLI
add_executable(render_bd render_bd.cpp wav_writer.cpp)
target_link_libraries(render_bd PRIVATE dsp)

# ctest-driven sanity tests
enable_testing()

add_executable(test_bd_basic test_bd_basic.cpp)
target_link_libraries(test_bd_basic PRIVATE dsp)
add_test(NAME bd_basic COMMAND test_bd_basic)
