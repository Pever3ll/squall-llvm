; RUN: llc -mtriple=squallv2 -squall-arg=7 -squall-arg=35 %s -o - | FileCheck %s
; RUN: llc -mtriple=squallv2 -squall-arg=7 -squall-arg=35 -filetype=obj %s -o %t.o
; RUN: llvm-readobj --file-headers --sections %t.o | FileCheck %s --check-prefix=OBJ
; RUN: llc -mtriple=squallv2 -squall-arg=7 -squall-arg=35 %s -o %t.s
; RUN: llvm-mc -triple=squallv2 -filetype=obj %t.s -o %t.roundtrip.o
; RUN: llvm-objcopy --dump-section=.squall.image=%t.bin %t.o
; RUN: llvm-objcopy --dump-section=.squall.image=%t.roundtrip.bin %t.roundtrip.o
; RUN: cmp %t.bin %t.roundtrip.bin
; CHECK: .section .squall.image
; CHECK: host_return:
; CHECK: write 1
; CHECK: write 2
; CHECK: add.r
; CHECK: .section .squall.config
; OBJ: Format: elf32-squall
; OBJ: Name: .squall.image
; OBJ: Name: .squall.config

define i32 @main(i32 %a, i32 %b) {
  %sum = add i32 %a, %b
  ret i32 %sum
}
