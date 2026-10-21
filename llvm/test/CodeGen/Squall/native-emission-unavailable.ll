; REQUIRES: squall-registered-target
; RUN: not llc -mtriple=squallv2-unknown-none %s -o /dev/null 2>&1 | FileCheck %s
; CHECK: target does not support generation of this file type
; CHECK-NOT: Stack dump
;
; Discovery and IR processing are available before native instruction selection.
; An unsupported native emission request must fail without a null MCContext.
define i32 @answer() {
  ret i32 42
}
