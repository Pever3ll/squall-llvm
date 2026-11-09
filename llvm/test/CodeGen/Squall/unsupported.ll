; RUN: split-file %s %t
; RUN: not llc -mtriple=squallv2 -filetype=null %t/atomic.ll 2>&1 | FileCheck %s --check-prefix=ATOMIC
; RUN: not llc -mtriple=squallv2 -filetype=null %t/external.ll 2>&1 | FileCheck %s --check-prefix=EXTERNAL
; RUN: not llc -mtriple=squallv2 -filetype=null %t/half.ll 2>&1 | FileCheck %s --check-prefix=HALF
; RUN: not llc -mtriple=squallv2 %t/entry.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ENTRY
; RUN: not llc -mtriple=squallv2 -filetype=null %t/fpintrinsic.ll 2>&1 | FileCheck %s --check-prefix=FPINTRINSIC
; RUN: not llc -mtriple=squallv2 -filetype=null %t/frameaddress.ll 2>&1 | FileCheck %s --check-prefix=FRAMEADDRESS

; Unsupported IR is diagnosed before instruction selection.

; ATOMIC: error: {{.*}}atomic operations are not supported on Squall
; ATOMIC-NOT: error
; EXTERNAL: error: {{.*}}call to 'f' has no definition in this program
; EXTERNAL-NOT: error
; HALF: error: {{.*}}Squall return values must be integers of at most 64 bits, pointers, float, or double
; ENTRY: error: Squall: entry function 'main' is not defined in this module
; FPINTRINSIC: error: {{.*}}floating-point intrinsic 'llvm.roundeven.f64' is not supported on Squall
; FRAMEADDRESS: error: {{.*}}'llvm.returnaddress.p0' is not supported on Squall
; FRAMEADDRESS: error: {{.*}}'llvm.frameaddress.p0' is not supported on Squall

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
;--- half.ll
define half @main() {
  ret half 0.0
}
;--- entry.ll
define i32 @other() {
  ret i32 0
}
;--- fpintrinsic.ll
define i32 @main(i32 %a) {
  %x = sitofp i32 %a to double
  %r = call double @llvm.roundeven.f64(double %x)
  %i = fptosi double %r to i32
  ret i32 %i
}
declare double @llvm.roundeven.f64(double)
;--- frameaddress.ll
define i32 @main() {
  %r = call ptr @llvm.returnaddress.p0(i32 0)
  %f = call ptr @llvm.frameaddress.p0(i32 0)
  %d = ptrtoint ptr %r to i32
  %e = ptrtoint ptr %f to i32
  %s = add i32 %d, %e
  ret i32 %s
}
declare ptr @llvm.returnaddress.p0(i32)
declare ptr @llvm.frameaddress.p0(i32)
