# Squall Compiler

Most the code for squall is in `llvm/lib/Target/Squall`

### Build Instructions

``` bash
cd build
cmake -G "Ninja" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="Squall" -DLLVM_TARGETS_TO_BUILD="" ../llvm
ninja llvm-mc
```
