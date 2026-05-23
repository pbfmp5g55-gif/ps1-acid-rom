# ps1-acid-rom — PS1 build entry point.
#
# Targets a ps-exe via the PSYQo framework. nugget submodule supplies the
# mips toolchain glue. Run `make` after `git submodule update --init` and
# having `mipsel-none-elf-gcc` on PATH.

TARGET = ps1-acid-rom
TYPE = ps-exe

SRCS = \
src/main.cpp \
src/audio/stream.cpp \

CXXFLAGS = -std=c++20 -Wall -Isrc

include third_party/nugget/psyqo/psyqo.mk
