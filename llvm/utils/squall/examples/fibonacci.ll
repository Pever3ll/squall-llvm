define i32 @main(i32 %n) {
entry:
  %small = icmp slt i32 %n, 2
  br i1 %small, label %base, label %recurse
base:
  ret i32 %n
recurse:
  %n1 = sub i32 %n, 1
  %a = call i32 @main(i32 %n1)
  %n2 = sub i32 %n, 2
  %b = call i32 @main(i32 %n2)
  %sum = add i32 %a, %b
  ret i32 %sum
}
