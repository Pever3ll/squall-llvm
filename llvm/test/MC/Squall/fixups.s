; RUN: llvm-mc -triple=squallv2 -filetype=obj %s -o %t.o
; RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=DIS
start: add.r done.l (data)
extract.r done.r start.l [0]
done: write 1
data: .slot 35
add external.l #0
extract.r external.r external.l (external)
; DIS: add.r (2).l (3)
; DIS: extract.r (1).r (-1).l [0]
; RELOC: 0x20 R_SQUALL_BRANCH20 external
; RELOC-DAG: 0x28 R_SQUALL_BRANCH20 external
; RELOC-DAG: 0x28 R_SQUALL_TARGET20 external
; RELOC-DAG: 0x28 R_SQUALL_CODE11 external
