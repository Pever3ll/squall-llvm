; RUN: llc -mtriple=squallv2 %s -o - | FileCheck %s
; RUN: llc -mtriple=squallv2 -filetype=obj %s -o %t.o
; RUN: llvm-readobj --file-headers --sections --relocations %t.o | FileCheck %s --check-prefix=OBJ
; RUN: llc -mtriple=squallv2 %s -o %t.s
; RUN: llvm-mc -triple=squallv2 -filetype=obj %t.s -o %t.roundtrip.o
; RUN: llvm-objcopy --dump-section=.squall.image=%t.bin %t.o
; RUN: llvm-objcopy --dump-section=.squall.image=%t.roundtrip.bin %t.roundtrip.o
; RUN: cmp %t.bin %t.roundtrip.bin

; The module becomes one dense memory image: low words, globals, runtime
; stubs at word 256, functions, the constant-initialized frame arena and the
; token stack, plus a JSON layout description.

; CHECK:      .section .squall.image,"axw",@progbits
; CHECK-NEXT: __squall_halt:
; CHECK:      .org 128, 0
; CHECK-NEXT: [[G:.+]]:
; CHECK-NEXT: .quad -9223372036854775766
; CHECK:      .org 2048, 0
; CHECK-NEXT: __squall_host_return:
; CHECK-NEXT: dupl (2).l.l #0
; CHECK-NEXT: write 1
; CHECK-NEXT: dupr (1).l #1
; CHECK-NEXT: write 2
; CHECK:      __squall_start:
; CHECK-NEXT: dupr.r (1).r 8
; CHECK-NEXT: dupr.c (1).l [3]
; CHECK-NEXT: dupr.r (1).r 9
; CHECK-NEXT: dupr.c (1).l [4]
; CHECK-NEXT: dupl .Lsquall.entry.main.l #0
; CHECK:      main:
; CHECK:      .org 131072, 0
; CHECK-NEXT: .quad -9223372036854775552
; CHECK-NEXT: .rept 47101
; CHECK-NEXT: .quad -9223372036854775808
; CHECK-NEXT: .endr
; CHECK:      .org 524272, 0
; CHECK:      .section .squall.config
; CHECK:      \"abi\":\"squall-frame-slots-1\"
; CHECK-SAME: \"argument_words\":[8,9]
; CHECK-SAME: \"globals\":{\"g\":16}

; OBJ:      Format: elf32-squall
; OBJ:      Name: .squall.image
; OBJ:      Size: 524288
; OBJ:      Name: .squall.config
; OBJ:      Relocations [
; OBJ-NEXT: ]

@g = global i32 42

define i32 @main(i32 %a, i32 %b) {
  %v = load i32, ptr @g
  %s = add i32 %a, %b
  %r = add i32 %s, %v
  ret i32 %r
}
