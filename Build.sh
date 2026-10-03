#!/bin/sh
#
# Build script for the orlyfix DLL which runs under MSYS2
# (see Build.ps1 for how to host and run this)
#
/mingw32/bin/gcc \
  -O2 -Wall -mincoming-stack-boundary=2 -shared -s -static-libgcc \
  -o winspool.dll \
  src/orlyfix.c src/winspool.def -luser32 -lgdi32 -lopengl32
