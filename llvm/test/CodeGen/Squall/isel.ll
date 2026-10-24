; RUN: llc -mtriple=squallv2 -verify-machineinstrs -stop-after=finalize-isel %s -o - | FileCheck %s

; Instruction selection produces frame-slot register pseudos; argument I
; arrives in $s<I>.

; CHECK-LABEL: name: arith
; CHECK: liveins: $s0, $s1
; CHECK: [[A:%[0-9]+]]:gpr = COPY $s0
; CHECK: [[S:%[0-9]+]]:gpr = ADDrr [[A]], %{{[0-9]+}}
; CHECK: [[M:%[0-9]+]]:gpr = MULri killed [[S]], 7
; CHECK: [[D:%[0-9]+]]:gpr = UDIVrr killed [[M]]
; CHECK: [[X:%[0-9]+]]:gpr = XORri killed [[D]], -1
; CHECK: RETV killed [[X]]
define i32 @arith(i32 %a, i32 %b) {
  %s = add i32 %a, %b
  %m = mul i32 %s, 7
  %d = udiv i32 %m, %b
  %x = xor i32 %d, -1
  ret i32 %x
}

; CHECK-LABEL: name: cmp
; CHECK: [[C:%[0-9]+]]:gpr = SETCC %0, %1, 6
; CHECK: SELECT killed [[C]], %0, %1
define i32 @cmp(i32 %a, i32 %b) {
  %c = icmp ult i32 %a, %b
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}

; 64-bit values are legalized into 32-bit halves; the i64 return is demoted
; to a hidden pointer argument.
; CHECK-LABEL: name: wide
; CHECK: liveins: $s0, $s1, $s2, $s3, $s4
; CHECK: MULHU
; CHECK: STP
; CHECK: STP
; CHECK: RET
define i64 @wide(i64 %a, i64 %b) {
  %r = mul i64 %a, %b
  ret i64 %r
}

@g = global [4 x i32] zeroinitializer

; Frame objects and globals in the low absolute range use static slots;
; other pointers use the dynamic access pseudos.
; CHECK-LABEL: name: mem
; CHECK: STF %1, %stack.0.f, 4
; CHECK: STG %1, @g + 8
; CHECK: LDP %0
; CHECK: LDF %stack.0.f, 4
define i32 @mem(ptr %p, i32 %i) {
  %f = alloca [2 x i32]
  %e = getelementptr [2 x i32], ptr %f, i32 0, i32 1
  store i32 %i, ptr %e
  %q = getelementptr [4 x i32], ptr @g, i32 0, i32 2
  store i32 %i, ptr %q
  %v = load i32, ptr %p
  %w = load volatile i32, ptr %e
  %r = add i32 %v, %w
  ret i32 %r
}

; CHECK-LABEL: name: main
; CHECK: [[R:%[0-9]+]]:gpr = CALLR @arith, %{{[0-9]+}}, killed %{{[0-9]+}}
; CHECK: RETV killed [[R]]
define i32 @main(i32 %a) {
  %r = call i32 @arith(i32 %a, i32 3)
  ret i32 %r
}
