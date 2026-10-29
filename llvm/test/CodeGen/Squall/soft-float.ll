; The module defines only some builtins, so code generation also reports the
; calls to the others.
; RUN: not llc -mtriple=squallv2 -print-after=squall-lower-float -o /dev/null %s 2>&1 | FileCheck %s
; RUN: not llc -mtriple=squallv2 -o /dev/null %s 2>&1 | FileCheck %s --check-prefix=UNDEFINED

; Floating point becomes calls to the compiler-rt soft-float builtins, and
; sign operations become bit operations. Runtime functions that LTO kept in
; llvm.compiler.used, and that nothing calls, are deleted with their state.

@llvm.compiler.used = appending global [3 x ptr] [ptr @__addsf3, ptr @__subsf3, ptr @rand], section "llvm.metadata"
@seed = internal global i32 1

; CHECK-LABEL: define i32 @main(
; CHECK:      %f = call float @__floatsisf(i32 %a)
; CHECK-NEXT: %g = call float @__mulsf3(float %f, float 1.500000e+00)
; CHECK-NEXT: %d = call double @__extendsfdf2(float %g)
; CHECK-NEXT: %h = call double @__adddf3(double %d, double 2.000000e+00)
; CHECK-NEXT: [[LT:%.*]] = call i32 @__ltdf2(double %h, double 1.000000e+01)
; CHECK-NEXT: %lt = icmp slt i32 [[LT]], 0
; CHECK-NEXT: [[UNORD:%.*]] = call i32 @__unordsf2(float %f, float %g)
; CHECK-NEXT: [[ISUNORD:%.*]] = icmp ne i32 [[UNORD]], 0
; CHECK-NEXT: [[EQ:%.*]] = call i32 @__eqsf2(float %f, float %g)
; CHECK-NEXT: [[ISEQ:%.*]] = icmp eq i32 [[EQ]], 0
; CHECK-NEXT: %ueq = or i1 [[ISUNORD]], [[ISEQ]]
; CHECK-NEXT: [[G:%.*]] = bitcast float %g to i32
; CHECK-NEXT: [[NEG:%.*]] = xor i32 [[G]], -2147483648
; CHECK-NEXT: %n = bitcast i32 [[NEG]] to float
; CHECK-NEXT: [[N:%.*]] = bitcast float %n to i32
; CHECK-NEXT: [[ABS:%.*]] = and i32 [[N]], 2147483647
; CHECK-NEXT: %abs = bitcast i32 [[ABS]] to float
; CHECK-NEXT: %s = call double @sqrt(double %h)
; fmuladd need not fuse: a separately rounded multiply and add.
; CHECK-NEXT: [[MUL:%.*]] = call float @__mulsf3(float %f, float %g)
; CHECK-NEXT: %m = call float @__addsf3(float [[MUL]], float %abs)
; CHECK-NEXT: %i = call i32 @__fixdfsi(double %s)
; CHECK-NEXT: [[J:%.*]] = call i32 @__fixunssfsi(float %m)
; CHECK-NEXT: %j = trunc i32 [[J]] to i16
; CHECK-NEXT: [[U:%.*]] = call double @__floatundidf(i64 %w)
; CHECK:      ret i32

; CHECK:      define internal float @__addsf3(
; CHECK-NOT:  @__subsf3(
; CHECK-NOT:  @rand(
; CHECK-NOT:  @seed

; UNDEFINED: error: {{.*}}in function undefined {{.*}}call to '__floatsisf' has no definition in this program; the Squall runtime library is linked only when clang links the program, without -c

define i32 @main(i32 %a, i32 %b, i64 %w) {
  %f = sitofp i32 %a to float
  %g = fmul float %f, 1.5
  %d = fpext float %g to double
  %h = fadd double %d, 2.0
  %lt = fcmp olt double %h, 10.0
  %ueq = fcmp ueq float %f, %g
  %n = fneg float %g
  %abs = call float @llvm.fabs.f32(float %n)
  %s = call double @llvm.sqrt.f64(double %h)
  %m = call float @llvm.fmuladd.f32(float %f, float %g, float %abs)
  %i = fptosi double %s to i32
  %j = fptoui float %m to i16
  %u = uitofp i64 %w to double
  %ub = fptosi double %u to i32
  %j32 = zext i16 %j to i32
  %c1 = zext i1 %lt to i32
  %c2 = zext i1 %ueq to i32
  %r1 = add i32 %i, %j32
  %r2 = add i32 %r1, %c1
  %r3 = add i32 %r2, %c2
  %r = add i32 %r3, %ub
  ret i32 %r
}

define internal float @__addsf3(float %a, float %b) {
  ret float %a
}

define internal float @__subsf3(float %a, float %b) {
  ret float %b
}

define internal i32 @rand() {
  %v = load i32, ptr @seed
  ret i32 %v
}

define i32 @undefined(i32 %a) {
  %f = sitofp i32 %a to float
  %i = fptosi float %f to i32
  ret i32 %i
}

declare float @llvm.fabs.f32(float)
declare double @llvm.sqrt.f64(double)
declare float @llvm.fmuladd.f32(float, float, float)
