; RUN: not llvm-mc -triple=squallv2 -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s
add.r (1).l [4096]
; CHECK: error: Squall instruction field out of range
add.r (1).l (2048)
; CHECK: error: Squall instruction field out of range
add (1).l #2147483648
; CHECK: error: Squall instruction field out of range
write -1
; CHECK: error: Squall instruction field out of range
send.r (1).l.r [0]
; CHECK: error: send/extract cannot have a secondary ALU destination
.slot 4611686018427387904
; CHECK: error: constant slot payload must fit unsigned 62 bits
.slot -1
; CHECK: error: constant slot payload must fit unsigned 62 bits
extract.r (1).l (2).l
; CHECK: error: extract matches no slot and takes no matching suffix
write 1 256
; CHECK: error: Squall instruction field out of range
write.x 1
; CHECK: error: write suffix must be .a
dupl .l #0
; CHECK: error: expected a label before the destination port
dupl .l.r #0
; CHECK: error: expected a label before the destination port
