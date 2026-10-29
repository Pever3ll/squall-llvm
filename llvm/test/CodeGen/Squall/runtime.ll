; RUN: llc -mtriple=squallv2 -print-after=squall-lower-alloca -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=ALLOCA
; RUN: llc -mtriple=squallv2 -verify-machineinstrs -stop-before=squall-asm-printer %s -o - | FileCheck %s --check-prefix=VARARGS
; RUN: llc -mtriple=squallv2 %s -o - | FileCheck %s --check-prefix=TRAP

; __builtin_trap halts with status 4.
; TRAP:       __squall_trap:
; TRAP-NEXT:  dupr ({{-?[0-9]+}}).{{[lr]}} #4
; TRAP-LABEL: main:
; TRAP:       dupl __squall_trap.l #0
; TRAP:       .ascii {{.*}}"trap\":4

; Variadic arguments are read from the argument registers' frame words:
; va_start points at the first unnamed one (S1, frame word 4), and va_arg
; advances by four bytes for i32 and eight for i64, without realigning.
; VARARGS-LABEL: name: sum
; VARARGS:       [[P:\$s[0-9]+]] = FIADDR 4, 0
; VARARGS-NEXT:  [[NEXT:\$s[0-9]+]] = ADDri [[P]], 4
; VARARGS:       ADDri [[P]], 8
; VARARGS:       ADDri {{.*}}[[P]], 12

; Dynamic allocas come from the alloca area: aligned, bounds-checked, and
; released when the function returns.
; ALLOCA:      @__squall_alloca_area = internal global [1024 x i32] zeroinitializer, align 16
; ALLOCA:      @__squall_alloca_top = internal global ptr @__squall_alloca_area
; ALLOCA-LABEL: define i32 @vla(
; ALLOCA:      %alloca.saved = load ptr, ptr @__squall_alloca_top
; ALLOCA:      icmp ugt i32 [[END:%.*]], ptrtoint (ptr getelementptr ([1024 x i32], ptr @__squall_alloca_area, i32 0, i32 1024) to i32)
; ALLOCA:      call void @llvm.trap()
; ALLOCA:      store ptr {{%.*}}, ptr @__squall_alloca_top
; ALLOCA:      store ptr %alloca.saved, ptr @__squall_alloca_top
; ALLOCA-NEXT: ret i32

define i32 @main(i32 %n) {
  %c = icmp slt i32 %n, 0
  br i1 %c, label %bad, label %ok
bad:
  call void @llvm.trap()
  unreachable
ok:
  %s = call i32 (i32, ...) @sum(i32 2, i32 %n, i64 7)
  %v = call i32 @vla(i32 %n)
  %r = add i32 %s, %v
  ret i32 %r
}

define i32 @sum(i32 %count, ...) {
  %ap = alloca ptr
  call void @llvm.va_start.p0(ptr %ap)
  %a = va_arg ptr %ap, i32
  %b = va_arg ptr %ap, i64
  call void @llvm.va_end.p0(ptr %ap)
  %b32 = trunc i64 %b to i32
  %r = add i32 %a, %b32
  ret i32 %r
}

define i32 @vla(i32 %n) {
  %p = alloca i32, i32 %n, align 4
  store i32 9, ptr %p
  %v = load i32, ptr %p
  ret i32 %v
}
declare void @llvm.trap()
declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_end.p0(ptr)
