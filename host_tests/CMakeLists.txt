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
    ../src/dsp/sd_twint.cpp
    ../src/dsp/tom_tbridge.cpp
    ../src/dsp/hh_six_squares.cpp
    ../src/dsp/cy_six_squares.cpp
    ../src/dsp/cp_burst.cpp
    ../src/dsp/cb_tbridge.cpp
    ../src/dsp/tb303_stage1.cpp
)
target_include_directories(dsp PUBLIC ../src)

# Per-voice "render N seconds to a file" CLIs.
foreach(voice bd sd tom hh cy cp cb tb303)
    add_executable(render_${voice} render_${voice}.cpp wav_writer.cpp)
    target_link_libraries(render_${voice} PRIVATE dsp)
endforeach()

# ctest-driven sanity tests.
enable_testing()
foreach(voice bd sd tom hh cy cp cb tb303)
    add_executable(test_${voice}_basic test_${voice}_basic.cpp)
    target_link_libraries(test_${voice}_basic PRIVATE dsp)
    add_test(NAME ${voice}_basic COMMAND test_${voice}_basic)
endforeach()
