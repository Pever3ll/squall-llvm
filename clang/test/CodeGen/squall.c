// RUN: %clang_cc1 -triple squallv2-unknown-none -emit-llvm -o - %s | FileCheck %s
// CHECK: target datalayout = "e-m:e-p:32:32-i64:64-n32-S64"
// CHECK: target triple = "squallv2-unknown-none"
// CHECK-LABEL: define{{.*}} i32 @add(
// CHECK: add nsw i32
// CHECK: ret i32
int add(int a, int b) { return a + b; }
