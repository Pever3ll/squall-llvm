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

; A call may run in one lane when the callee cannot reach the caller again;
; the region joins on reserved call join word 1 (absolute word 16), not the
; shared word 7. A recursive call stays outside the lanes. Frame objects are
; static slots, so stores to two of them may be in different lanes.
; MIR-LABEL: name: calls
; MIR:       FORK [[ID:[0-9]+]]
; MIR-NEXT:  {{\$s[0-9]+}} = CALLR @leaf, $s0
; MIR-NEXT:  LANE [[ID]]
; MIR:       JOIN [[ID]], 1
; MIR-LABEL: name: recursive
; MIR-NOT:   FORK
; MIR:       CALLR @recursive
; MIR-LABEL: name: frames
; MIR:       FORK [[FID:[0-9]+]]
; MIR:       STF {{.*}} :: (volatile store (s32) into %ir.x)
; MIR:       LANE [[FID]]
; MIR:       STF {{.*}} :: (store (s32) into %ir.y)
; MIR:       JOIN [[FID]], 0
; CHECK-LABEL: calls:
; CHECK:       .Lsquall_join{{[0-9]+}}:
; CHECK-NEXT:  dupl (1).{{[lr]}} 16
; The manifest lists both join words.
; CHECK:       .ascii {{.*}}join_words\":[7,16]{{.*}}parallel_lanes\":true

define internal i32 @leaf(i32 %x) noinline {
  %y = mul i32 %x, 3
  %z = add i32 %y, 1
  ret i32 %z
}

define i32 @calls(i32 %a, i32 %b) {
  %c = call i32 @leaf(i32 %a)
  %b1 = mul i32 %b, 22695477
  %b2 = lshr i32 %b1, 7
  %b3 = xor i32 %b2, %b1
  %r = add i32 %c, %b3
  ret i32 %r
}

define i32 @recursive(i32 %a, i32 %b) {
  %stop = icmp slt i32 %a, 1
  br i1 %stop, label %done, label %more
more:
  %a1 = sub i32 %a, 1
  %c = call i32 @recursive(i32 %a1, i32 %b)
  %b1 = mul i32 %b, 22695477
  %b2 = lshr i32 %b1, 7
  %b3 = xor i32 %b2, %b1
  %r = add i32 %c, %b3
  ret i32 %r
done:
  ret i32 0
}

define i32 @frames(i32 %a) {
  %x = alloca i32
  %y = alloca i32
  store volatile i32 %a, ptr %x
  %x1 = load volatile i32, ptr %x
  %x2 = mul i32 %x1, 5
  store i32 %x2, ptr %x
  %b = xor i32 %a, 85
  store i32 %b, ptr %y
  %y1 = load i32, ptr %y
  %y2 = mul i32 %y1, 7
  store i32 %y2, ptr %y
  %xv = load i32, ptr %x
  %yv = load i32, ptr %y
  %r = add i32 %xv, %yv
  ret i32 %r
}
