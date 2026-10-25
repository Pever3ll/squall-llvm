; RUN: llc -mtriple=squallv2 -squall-fork-join -verify-machineinstrs -stop-before=squall-asm-printer %s -o - | FileCheck %s --check-prefix=MIR
; RUN: llc -mtriple=squallv2 -squall-fork-join %s -o - | FileCheck %s
; RUN: llc -mtriple=squallv2 %s -o - | FileCheck %s --check-prefix=SERIAL

; Two independent chains in one block become two lanes. The temporary that
; register allocation gave both chains is renamed, so the chains share no
; register.

; MIR-LABEL: name: main
; MIR:      FORK 0
; MIR-NEXT: [[A1:\$s[0-9]+]] = MULri $s0, 1664525
; MIR-NEXT: [[A2:\$s[0-9]+]] = SRLri [[A1]], 13
; MIR-NEXT: [[A:\$s[0-9]+]] = XORrr [[A2]], [[A1]]
; MIR-NEXT: LANE 0
; MIR-NEXT: [[B1:\$s[0-9]+]] = MULri $s1, 22695477
; MIR-NEXT: [[B2:\$s[0-9]+]] = SRLri [[B1]], 7
; MIR-NEXT: [[B:\$s[0-9]+]] = XORrr [[B2]], [[B1]]
; MIR-NEXT: JOIN 0

; The fork's primary token jumps to lane B, its secondary starts lane A; lane
; A jumps to the join and lane B falls into it on the right port. The join is
; a Once match on absolute word 7.
; CHECK-LABEL: main:
; CHECK:      dupl .Lsquall_lane0.l.l #0
; CHECK:      dupl .Lsquall_join0.l #0
; CHECK-NEXT: .Lsquall_lane0:
; CHECK:      dupr.c (1).r [{{[0-9]+}}]
; CHECK-NEXT: .Lsquall_join0:
; CHECK-NEXT: dupl (1).{{[lr]}} 7
; CHECK:      .ascii {{.*}}join_words\":[7]{{.*}}parallel_lanes\":true

; SERIAL-NOT: dupl (1).{{[lr]}} 7
; SERIAL:     parallel_lanes\":false

define i32 @main(i32 %a, i32 %b) {
  %a1 = mul i32 %a, 1664525
  %a2 = lshr i32 %a1, 13
  %a3 = xor i32 %a2, %a1
  %b1 = mul i32 %b, 22695477
  %b2 = lshr i32 %b1, 7
  %b3 = xor i32 %b2, %b1
  %r = add i32 %a3, %b3
  ret i32 %r
}
