; RUN: not llvm-mc -triple=squallv2 -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s
add.r (1).l [2048]
; CHECK: error: Squall instruction field out of range
add (1).l #2147483648
; CHECK: error: Squall instruction field out of range
write -1
; CHECK: error: Squall instruction field out of range
send.r (1).l.r [0]
; CHECK: error: send/extract cannot have a secondary ALU destination
.slot 2305843009213693952
; CHECK: error: constant slot payload must fit signed 62 bits
