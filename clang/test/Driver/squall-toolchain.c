// A Squall object from -c is a complete image of one module. Linking several
// translation units compiles them to bitcode and generates the image with
// llvm-lto at link time.

// RUN: %clang -### --target=squallv2 -c %s 2>&1 | FileCheck %s --check-prefix=COMPILE
// COMPILE: "-cc1" "-triple" "squallv2"
// COMPILE-SAME: "-emit-obj"
// COMPILE-NOT: "-flto
// COMPILE-NOT: llvm-lto

// RUN: %clang -### --target=squallv2 -O2 -mllvm -squall-entry=kernel %s %s -o image.o 2>&1 \
// RUN:   | FileCheck %s --check-prefix=LINK
// LINK: "-cc1" "-triple" "squallv2"
// LINK-SAME: "-flto=full"
// LINK: "-cc1" "-triple" "squallv2"
// LINK-SAME: "-flto=full"
// LINK: {{.*}}llvm-lto" "{{.*}}.o" "{{.*}}.o" "-o" "image.o" "-squall-entry=kernel" "-exported-symbol=kernel" "-O2"

// The entry defaults to main, and the link uses the compile's optimization
// level.
// RUN: %clang -### --target=squallv2 %s 2>&1 | FileCheck %s --check-prefix=DEFAULT
// DEFAULT: {{.*}}llvm-lto" "{{.*}}.o" "-o" "a.out" "-exported-symbol=main" "-O0"

// The entry may also be given as a separate argument.
// RUN: %clang -### --target=squallv2 -mllvm -squall-entry -mllvm kernel %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SPACED
// SPACED: {{.*}}llvm-lto" "{{.*}}.o" "-o" "a.out" "-squall-entry" "kernel" "-exported-symbol=kernel"

// RUN: not %clang -### --target=squallv2 -fno-lto %s 2>&1 | FileCheck %s --check-prefix=NOLTO
// NOLTO: error: Squall programs are linked as bitcode with full LTO; '-fno-lto' cannot be used when linking
// RUN: not %clang -### --target=squallv2 -flto=thin %s 2>&1 | FileCheck %s --check-prefix=THIN
// THIN: error: Squall programs are linked as bitcode with full LTO; '-flto=thin' cannot be used when linking

// -fno-lto still compiles a single file to an image with -c.
// RUN: %clang -### --target=squallv2 -fno-lto -c %s 2>&1 | FileCheck %s --check-prefix=COMPILE

// Headers come from the compiler and the Squall runtime, never the host; the
// runtime's bitcode libraries are linked unless -nostdlib or -nolibc says
// otherwise.
// RUN: %clang -### --target=squallv2 --sysroot=%S/Inputs/squall_runtime -c %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=INCLUDE
// INCLUDE: "-nostdsysteminc"
// INCLUDE-SAME: "-internal-isystem" "{{[^"]*}}{{/|\\}}include"
// INCLUDE-SAME: "-internal-isystem" "{{[^"]*}}squall_runtime{{/|\\}}include"
// INCLUDE-NOT: /usr/local/include
// RUN: %clang -### --target=squallv2 --sysroot=%S/Inputs/squall_runtime %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=RUNTIME
// RUNTIME: llvm-lto" {{.*}}"-O0" "{{[^"]*}}squall_runtime{{/|\\}}lib{{/|\\}}libc.bc" "{{[^"]*}}squall_runtime{{/|\\}}lib{{/|\\}}libclang_rt.builtins.bc"
// RUN: %clang -### --target=squallv2 --sysroot=%S/Inputs/squall_runtime -nolibc %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOLIBC
// NOLIBC: llvm-lto" {{.*}}"-O0" "{{[^"]*}}libclang_rt.builtins.bc"
// NOLIBC-NOT: libc.bc
// RUN: %clang -### --target=squallv2 --sysroot=%S/Inputs/squall_runtime -nostdlib %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=NOSTDLIB
// NOSTDLIB: llvm-lto"
// NOSTDLIB-NOT: .bc"

// RUN: not %clang -### --target=squallv2 %s -lm 2>&1 | FileCheck %s --check-prefix=LIBRARY
// LIBRARY: error: unsupported option '-lm' for target 'squallv2'

// RUN: %clang --target=squallv2 -c %s -o %t.image.o
// RUN: not %clang --target=squallv2 %t.image.o -o %t.out 2>&1 | FileCheck %s --check-prefix=IMAGE
// IMAGE: error: '{{.*}}.image.o' is a complete Squall image, not bitcode; compile files to link with -flto

int main(void) { return 0; }
