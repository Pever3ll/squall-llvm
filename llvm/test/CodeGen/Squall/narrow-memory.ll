; RUN: llc -mtriple=squallv2 -stop-after=squall-lower-memory %s -o - | FileCheck %s --check-prefix=IR
; RUN: llc -mtriple=squallv2 -verify-machineinstrs -stop-after=finalize-isel %s -o - | FileCheck %s --check-prefix=MIR

; Memory is four-byte little-endian cells. Partial-cell stores become cell
; updates in IR, where their order is fixed; narrow loads extract from a cell
; load during instruction selection.

@g = global [8 x i8] zeroinitializer

; A constant offset from a global has a known cell and byte position.
; IR-LABEL: define void @static_byte(
; IR: %[[OLD:.*]] = load i32, ptr getelementptr (i8, ptr @g, i32 4), align 4
; IR: %[[KEEP:.*]] = and i32 %[[OLD]], -16711681
; IR: %[[FIELD:.*]] = shl i32 %{{.*}}, 16
; IR: %[[NEW:.*]] = or i32 %[[KEEP]], %[[FIELD]]
; IR: store i32 %[[NEW]], ptr getelementptr (i8, ptr @g, i32 4), align 4
; IR-NOT: store i8
define void @static_byte(i8 %v) {
  store i8 %v, ptr getelementptr (i8, ptr @g, i32 6)
  ret void
}

; Through an unknown pointer, the byte position is computed.
; IR-LABEL: define void @dynamic_half(
; IR: %[[ADDR:.*]] = ptrtoint ptr %p to i32
; IR: %[[POS:.*]] = and i32 %[[ADDR]], 3
; IR: %[[BACK:.*]] = sub i32 0, %[[POS]]
; IR: %[[CELL:.*]] = getelementptr i8, ptr %p, i32 %[[BACK]]
; IR: load i32, ptr %[[CELL]], align 4
; IR: shl i32 65535,
; IR: store i32 %{{.*}}, ptr %[[CELL]], align 4
define void @dynamic_half(ptr %p, i16 %v) {
  store i16 %v, ptr %p, align 2
  ret void
}

; A misaligned word is written a byte at a time, each a cell update in order.
; IR-LABEL: define void @misaligned_word(
; IR-COUNT-4: store i32
; IR-NOT: store
; IR: ret void
define void @misaligned_word(ptr %p, i32 %v) {
  store i32 %v, ptr %p, align 1
  ret void
}

; Whole aligned cells are untouched.
; IR-LABEL: define void @whole_cell(
; IR-NEXT: store i32 %v, ptr %p, align 4
define void @whole_cell(ptr %p, i32 %v) {
  store i32 %v, ptr %p, align 4
  ret void
}

; MIR-LABEL: name: sign_extended_byte
; MIR: LDG @g + 4
; MIR: SHLri {{.*}}, 8
; MIR: SRAri {{.*}}, 24
define i32 @sign_extended_byte() {
  %v = load i8, ptr getelementptr (i8, ptr @g, i32 6)
  %r = sext i8 %v to i32
  ret i32 %r
}

; MIR-LABEL: name: zero_extended_half
; MIR: ANDri {{.*}}, -4
; MIR: LDP
; MIR: SRLrr
; MIR: ANDri {{.*}}, 65535
define i32 @zero_extended_half(ptr %p) {
  %v = load i16, ptr %p, align 2
  %r = zext i16 %v to i32
  ret i32 %r
}

define i32 @main() {
  ret i32 0
}
