; Every Squall option must coexist with the pass names that opt exposes as
; options; a duplicate aborts every tool that registers both.
; RUN: opt -S %s -o - | FileCheck %s
; RUN: llc -mtriple=squallv2 -squall-fork-join -o /dev/null %s
; CHECK: define i32 @main()

define i32 @main() {
  ret i32 0
}
