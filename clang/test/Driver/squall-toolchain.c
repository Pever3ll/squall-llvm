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

// RUN: not %clang -### --target=squallv2 %s -lm 2>&1 | FileCheck %s --check-prefix=LIBRARY
// LIBRARY: error: unsupported option '-lm' for target 'squallv2'

// RUN: %clang --target=squallv2 -c %s -o %t.image.o
// RUN: not %clang --target=squallv2 %t.image.o -o %t.out 2>&1 | FileCheck %s --check-prefix=IMAGE
// IMAGE: error: '{{.*}}.image.o' is a complete Squall image, not bitcode; compile files to link with -flto

int main(void) { return 0; }
