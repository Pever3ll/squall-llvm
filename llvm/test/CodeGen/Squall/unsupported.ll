; RUN: split-file %s %t
; RUN: not llc -mtriple=squallv2 -filetype=null %t/atomic.ll 2>&1 | FileCheck %s --check-prefix=ATOMIC
; RUN: not llc -mtriple=squallv2 -filetype=null %t/external.ll 2>&1 | FileCheck %s --check-prefix=EXTERNAL
; RUN: not llc -mtriple=squallv2 -filetype=null %t/float.ll 2>&1 | FileCheck %s --check-prefix=FLOAT
; RUN: not llc -mtriple=squallv2 %t/entry.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ENTRY

; Unsupported IR is diagnosed before instruction selection.

; ATOMIC: error: {{.*}}atomic operations are not supported on Squall
; ATOMIC-NOT: error
; EXTERNAL: error: {{.*}}call to 'f' has no definition in this program
; EXTERNAL-NOT: error
; FLOAT: error: {{.*}}Squall return values must be integers
; ENTRY: error: Squall: entry function 'main' is not defined in this module

;--- atomic.ll
define i32 @main(ptr %p) {
  %v = atomicrmw add ptr %p, i32 1 seq_cst
  ret i32 %v
}
;--- external.ll
declare i32 @f()
define i32 @main() {
  %r = call i32 @f()
  ret i32 %r
}
;--- float.ll
define float @main() {
  ret float 0.0
}
;--- entry.ll
define i32 @other() {
  ret i32 0
}
