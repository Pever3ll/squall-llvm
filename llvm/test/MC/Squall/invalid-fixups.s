; RUN: not llvm-mc -triple=squallv2 -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s
.set odd, 1
add odd.l #0
; CHECK: error: Squall relative address must be eight-byte aligned
.set far, 8388608
add far.l #0
; CHECK: error: Squall relative address must be eight-byte aligned
