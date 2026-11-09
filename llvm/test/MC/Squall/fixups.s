; RUN: llvm-mc -triple=squallv2 -filetype=obj %s -o %t.o
; RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS
start: add.r done.l (data)
extract done.r start.l
done: write 1
data: .slot 35
add external.l #0
extract external.r external.l
add.r external.l (external)
; DIS: add.r (2).l (3)
; DIS: extract (1).r (-1).l
; RELOC: 0x20 R_SQUALL_BRANCH20 external
; RELOC-DAG: 0x28 R_SQUALL_BRANCH20 external
; RELOC-DAG: 0x28 R_SQUALL_TARGET20 external
; RELOC-DAG: 0x30 R_SQUALL_BRANCH20 external
; RELOC-DAG: 0x30 R_SQUALL_CODE12 external
