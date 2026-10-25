; RUN: llc -mtriple=squallv2 -verify-machineinstrs -squall-entry=caller %s -o - | FileCheck %s

; Each pseudo expands to a chain of encoded words whose final token continues
; at the next word. Registers are frame words: s0 is [3], s1 is [4].

; A register operand is read directly from its slot; 32-bit results are
; sign-extended in the 64-bit token. Reading back the stored result is
; unnecessary because the store's token still carries it.
; CHECK-LABEL: add:
; CHECK:      dupr.r (1).l [3]
; CHECK-NEXT: add.r (1).l [4]
; CHECK-NEXT: shl (1).l #32
; CHECK-NEXT: shra (1).r #32
; CHECK-NEXT: dupr.c (1).r [3]
; CHECK-NEXT: send.r __squall_halt.l [0]
define i32 @add(i32 %a, i32 %b) {
  %r = add i32 %a, %b
  ret i32 %r
}

; beql's primary result means "different"; its secondary token takes the
; next word. Conditional branches name the block label.
; CHECK-LABEL: branch:
; CHECK:      beql.r (2).l.l [4]
; CHECK-NEXT: dupl (2).l #0
; CHECK-NEXT: dupr (2).r #1
; CHECK-NEXT: dupr (1).r #0
; CHECK-NEXT: dupr.c (1).l [4]
; CHECK-NEXT: beqz (2).l.l #0
; CHECK-NEXT: dupl [[ZERO:.LBB[0-9_]+]].l #0
; CHECK:      [[ZERO]]:
; CHECK-NEXT: dupr (1).r #1
define i32 @branch(i32 %a) {
  %c = icmp eq i32 %a, 0
  br i1 %c, label %zero, label %other
zero:
  ret i32 1
other:
  ret i32 %a
}

; The caller checks the frame arena, writes the arguments into the callee's
; s0/s1 at its own frame size (8 words) plus 3, stores the continuation tag
; as the callee's return tag, and sends a token with the frame pointer
; advanced by its frame size.
; CHECK-LABEL: caller:
; CHECK:      extract.r (1).l __squall_halt.l [0]
; CHECK-NEXT: shrl (1).l #29
; CHECK-NEXT: sub (2).l.r #61431
; CHECK-NEXT: dupr.c __squall_halt.l [1]
; CHECK-NEXT: dupr.r (1).l [1]
; CHECK-NEXT: beqz (2).l.l #0
; CHECK-NEXT: dupl __squall_depth_error.l #0
; CHECK-NEXT: dupr.r (1).r [3]
; CHECK-NEXT: dupr.c (1).l [11]
; CHECK-NEXT: dupr.r (1).r [4]
; CHECK-NEXT: dupr.c (1).l [12]
; CHECK-NEXT: extract.r (1).r (6).r [0]
; CHECK-NEXT: dupr.c (1).l [8]
; CHECK-NEXT: extract.r (1).l .Lsquall.entry.add.l [0]
; CHECK-NEXT: add.r (1).l (2)
; CHECK-NEXT: send.r __squall_halt.l [0]
; CHECK-NEXT: .quad -9223372032559808512
; CHECK-NEXT: dupr.c (1).r [3]
; CHECK-NEXT: send.r __squall_halt.l [0]
define i32 @caller(i32 %a) {
  %r = call i32 @add(i32 %a, i32 5)
  ret i32 %r
}
