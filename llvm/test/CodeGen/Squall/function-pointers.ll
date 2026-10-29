; RUN: llc -mtriple=squallv2 -verify-machineinstrs -stop-after=finalize-isel %s -o - | FileCheck %s --check-prefix=MIR
; RUN: llc -mtriple=squallv2 %s -o - | FileCheck %s
; RUN: llc -mtriple=squallv2 -filetype=obj %s -o %t.o
; RUN: llc -mtriple=squallv2 %s -o %t.s
; RUN: llvm-mc -triple=squallv2 -filetype=obj %t.s -o %t.roundtrip.o
; RUN: llvm-objcopy --dump-section=.squall.image=%t.bin %t.o
; RUN: llvm-objcopy --dump-section=.squall.image=%t.roundtrip.bin %t.roundtrip.o
; RUN: cmp %t.bin %t.roundtrip.bin

; A function pointer is the callee's entry word address, which is a call tag
; with frame pointer 0. Initializers hold it as a layout-time expression.

; CHECK:      table:
; CHECK-NEXT: .quad ((.Lsquall.entry.inc-__squall_halt)/8)|-9223372036854775808
; CHECK-NEXT: .quad ((.Lsquall.entry.dec-__squall_halt)/8)|-9223372036854775808
@table = global [2 x ptr] [ptr @inc, ptr @dec]

define i32 @inc(i32 %x) {
  %r = add i32 %x, 1
  ret i32 %r
}

define i32 @dec(i32 %x) {
  %r = sub i32 %x, 1
  ret i32 %r
}

; Taking an address mints a tag for the entry and keeps its word address.
; MIR-LABEL: name: address
; MIR: MOVG @dec
; CHECK-LABEL: address:
; CHECK:      extract.r (1).l .Lsquall.entry.dec.l [0]
; CHECK-NEXT: and (1).{{[lr]}} #536870911
define ptr @address() {
  ret ptr @dec
}

; An indirect call ORs the callee frame pointer into the pointer and sends.
; MIR-LABEL: name: main
; MIR: CALLIR killed %{{[0-9]+}}, %{{[0-9]+}}
; CHECK-LABEL: main:
; CHECK:      add (1).l #{{[0-9]+}}
; CHECK-NEXT: shl (1).l #29
; CHECK-NEXT: or.r (1).l [{{[0-9]+}}]
; CHECK-NEXT: send.r __squall_halt.l [0]
define i32 @main(i32 %i, i32 %x) {
  %p = getelementptr ptr, ptr @table, i32 %i
  %f = load ptr, ptr %p
  %r = call i32 %f(i32 %x)
  ret i32 %r
}
