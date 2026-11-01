#!/bin/sh
# Rebuild with LLVM (clang + lld), no Windows SDK needed
set -e
cd "$(dirname "$0")/src"
llvm-dlltool -m i386 -d kernel32.def -l kernel32.lib -k
llvm-dlltool -m i386 -d user32.def -l user32.lib -k
clang --target=i686-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-sse -c tdu_wonrace.c -o tdu_wonrace.o
clang --target=i686-pc-windows-msvc -c hooks.S -o hooks.o
lld-link /dll /machine:x86 /nodefaultlib /entry:DllMain /subsystem:windows /safeseh:no /out:../TDU_WonRaceFreedom.asi tdu_wonrace.o hooks.o kernel32.lib user32.lib
