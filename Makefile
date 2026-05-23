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
src/voices/acb_tb303_stage1.cpp \
src/voices/acb_tb303_stage2.cpp \
src/voices/acb_808_bd.cpp \
src/voices/acb_808_sd.cpp \
src/voices/acb_808_tom.cpp \
src/voices/acb_808_cp.cpp \
src/voices/acb_909_bd.cpp \
src/voices/acb_909_sd.cpp \
src/voices/acb_808_cb.cpp \

CXXFLAGS = -std=c++20 -Wall -Isrc

include third_party/nugget/psyqo/psyqo.mk
