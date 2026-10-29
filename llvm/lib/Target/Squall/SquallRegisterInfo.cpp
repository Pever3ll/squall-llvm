// SquallRegisterInfo.cpp
#include "SquallRegisterInfo.h"
#include "SquallSubtarget.h"

#define GET_REGINFO_TARGET_DESC
#include "SquallGenRegisterInfo.inc"

using namespace llvm;

SquallRegisterInfo::SquallRegisterInfo() : SquallGenRegisterInfo(0) {}
