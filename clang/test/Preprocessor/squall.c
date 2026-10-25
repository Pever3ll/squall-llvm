// RUN: %clang_cc1 -triple squallv2-unknown-none -E -dM %s | FileCheck %s
// CHECK: #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
// CHECK: #define __POINTER_WIDTH__ 32
// CHECK: #define __Squall__ 1
