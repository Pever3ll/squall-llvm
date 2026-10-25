; RUN: split-file %s %t
; RUN: not llc -mtriple=squallv2 -filetype=null %t/byte.ll 2>&1 | FileCheck %s --check-prefix=BYTE
; RUN: not llc -mtriple=squallv2 -filetype=null %t/external.ll 2>&1 | FileCheck %s --check-prefix=EXTERNAL
; RUN: not llc -mtriple=squallv2 -filetype=null %t/float.ll 2>&1 | FileCheck %s --check-prefix=FLOAT
; RUN: not llc -mtriple=squallv2 %t/entry.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ENTRY

; Unsupported IR is diagnosed before instruction selection.

; BYTE: error: {{.*}}Squall memory accesses must be i32, i64 or pointer values
; EXTERNAL: error: {{.*}}call to 'f' has no definition in this module
; FLOAT: error: {{.*}}Squall return values must be integers
; ENTRY: error: Squall: entry function 'main' is not defined in this module

;--- byte.ll
define i32 @main(ptr %p) {
  %v = load i8, ptr %p
  %r = zext i8 %v to i32
  ret i32 %r
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
