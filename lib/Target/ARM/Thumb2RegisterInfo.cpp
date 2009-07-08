//===- Thumb2RegisterInfo.cpp - Thumb-2 Register Information -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the Thumb-2 implementation of the TargetRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMAddressingModes.h"
#include "ARMMachineFunctionInfo.h"
#include "ARMSubtarget.h"
#include "Thumb2InstrInfo.h"
#include "Thumb2RegisterInfo.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLocation.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
using namespace llvm;

static cl::opt<bool>
Thumb2RegScavenging("enable-thumb2-reg-scavenging",
                    cl::Hidden,
                    cl::desc("Enable register scavenging on Thumb-2"));

Thumb2RegisterInfo::Thumb2RegisterInfo(const ARMBaseInstrInfo &tii,
                                       const ARMSubtarget &sti)
  : ARMBaseRegisterInfo(tii, sti) {
}

/// emitLoadConstPool - Emits a load from constpool to materialize the
/// specified immediate.
void Thumb2RegisterInfo::emitLoadConstPool(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator &MBBI,
                                           const TargetInstrInfo *TII, DebugLoc dl,
                                           unsigned DestReg, int Val,
                                           ARMCC::CondCodes Pred,
                                           unsigned PredReg) const {
  MachineFunction &MF = *MBB.getParent();
  MachineConstantPool *ConstantPool = MF.getConstantPool();
  Constant *C = ConstantInt::get(Type::Int32Ty, Val);
  unsigned Idx = ConstantPool->getConstantPoolIndex(C, 4);

  BuildMI(MBB, MBBI, dl, TII->get(ARM::tLDRcp), DestReg)
    .addConstantPoolIndex(Idx);
}

const TargetRegisterClass*
Thumb2RegisterInfo::getPhysicalRegisterRegClass(unsigned Reg, MVT VT) const {
  if (isARMLowRegister(Reg))
    return ARM::tGPRRegisterClass;
  switch (Reg) {
   default:
    break;
   case ARM::R8:  case ARM::R9:  case ARM::R10:  case ARM::R11:
   case ARM::R12: case ARM::SP:  case ARM::LR:   case ARM::PC:
    return ARM::GPRRegisterClass;
  }

  return TargetRegisterInfo::getPhysicalRegisterRegClass(Reg, VT);
}

bool
Thumb2RegisterInfo::requiresRegisterScavenging(const MachineFunction &MF) const {
  return Thumb2RegScavenging;
}

bool Thumb2RegisterInfo::hasReservedCallFrame(MachineFunction &MF) const {
  const MachineFrameInfo *FFI = MF.getFrameInfo();
  unsigned CFSize = FFI->getMaxCallFrameSize();
  // It's not always a good idea to include the call frame as part of the
  // stack frame. ARM (especially Thumb) has small immediate offset to
  // address the stack frame. So a large call frame can cause poor codegen
  // and may even makes it impossible to scavenge a register.
  if (CFSize >= ((1 << 8) - 1) * 4 / 2) // Half of imm8 * 4
    return false;

  return !MF.getFrameInfo()->hasVarSizedObjects();
}

/// emitThumbRegPlusImmInReg - Emits a series of instructions to materialize
/// a destreg = basereg + immediate in Thumb code. Materialize the immediate
/// in a register using mov / mvn sequences or load the immediate from a
/// constpool entry.
static
void emitThumbRegPlusImmInReg(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator &MBBI,
                              unsigned DestReg, unsigned BaseReg,
                              int NumBytes, bool CanChangeCC,
                              const TargetInstrInfo &TII,
                              const Thumb2RegisterInfo& MRI,
                              DebugLoc dl) {
    bool isHigh = !isARMLowRegister(DestReg) ||
                  (BaseReg != 0 && !isARMLowRegister(BaseReg));
    bool isSub = false;
    // Subtract doesn't have high register version. Load the negative value
    // if either base or dest register is a high register. Also, if do not
    // issue sub as part of the sequence if condition register is to be
    // preserved.
    if (NumBytes < 0 && !isHigh && CanChangeCC) {
      isSub = true;
      NumBytes = -NumBytes;
    }
    unsigned LdReg = DestReg;
    if (DestReg == ARM::SP) {
      assert(BaseReg == ARM::SP && "Unexpected!");
      LdReg = ARM::R3;
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVlor2hir), ARM::R12)
        .addReg(ARM::R3, RegState::Kill);
    }

    if (NumBytes <= 255 && NumBytes >= 0)
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVi8), LdReg).addImm(NumBytes);
    else if (NumBytes < 0 && NumBytes >= -255) {
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVi8), LdReg).addImm(NumBytes);
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tNEG), LdReg)
        .addReg(LdReg, RegState::Kill);
    } else
      MRI.emitLoadConstPool(MBB, MBBI, &TII, dl, LdReg, NumBytes);

    // Emit add / sub.
    int Opc = (isSub) ? ARM::tSUBrr : (isHigh ? ARM::tADDhirr : ARM::tADDrr);
    const MachineInstrBuilder MIB = BuildMI(MBB, MBBI, dl,
                                            TII.get(Opc), DestReg);
    if (DestReg == ARM::SP || isSub)
      MIB.addReg(BaseReg).addReg(LdReg, RegState::Kill);
    else
      MIB.addReg(LdReg).addReg(BaseReg, RegState::Kill);
    if (DestReg == ARM::SP)
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVhir2lor), ARM::R3)
        .addReg(ARM::R12, RegState::Kill);
}

/// calcNumMI - Returns the number of instructions required to materialize
/// the specific add / sub r, c instruction.
static unsigned calcNumMI(int Opc, int ExtraOpc, unsigned Bytes,
                          unsigned NumBits, unsigned Scale) {
  unsigned NumMIs = 0;
  unsigned Chunk = ((1 << NumBits) - 1) * Scale;

  if (Opc == ARM::tADDrSPi) {
    unsigned ThisVal = (Bytes > Chunk) ? Chunk : Bytes;
    Bytes -= ThisVal;
    NumMIs++;
    NumBits = 8;
    Scale = 1;  // Followed by a number of tADDi8.
    Chunk = ((1 << NumBits) - 1) * Scale;
  }

  NumMIs += Bytes / Chunk;
  if ((Bytes % Chunk) != 0)
    NumMIs++;
  if (ExtraOpc)
    NumMIs++;
  return NumMIs;
}

/// emitThumbRegPlusImmediate - Emits a series of instructions to materialize
/// a destreg = basereg + immediate in Thumb code.
static
void emitThumbRegPlusImmediate(MachineBasicBlock &MBB,
                               MachineBasicBlock::iterator &MBBI,
                               unsigned DestReg, unsigned BaseReg,
                               int NumBytes, const TargetInstrInfo &TII,
                               const Thumb2RegisterInfo& MRI,
                               DebugLoc dl) {
  bool isSub = NumBytes < 0;
  unsigned Bytes = (unsigned)NumBytes;
  if (isSub) Bytes = -NumBytes;
  bool isMul4 = (Bytes & 3) == 0;
  bool isTwoAddr = false;
  bool DstNotEqBase = false;
  unsigned NumBits = 1;
  unsigned Scale = 1;
  int Opc = 0;
  int ExtraOpc = 0;

  if (DestReg == BaseReg && BaseReg == ARM::SP) {
    assert(isMul4 && "Thumb sp inc / dec size must be multiple of 4!");
    NumBits = 7;
    Scale = 4;
    Opc = isSub ? ARM::tSUBspi : ARM::tADDspi;
    isTwoAddr = true;
  } else if (!isSub && BaseReg == ARM::SP) {
    // r1 = add sp, 403
    // =>
    // r1 = add sp, 100 * 4
    // r1 = add r1, 3
    if (!isMul4) {
      Bytes &= ~3;
      ExtraOpc = ARM::tADDi3;
    }
    NumBits = 8;
    Scale = 4;
    Opc = ARM::tADDrSPi;
  } else {
    // sp = sub sp, c
    // r1 = sub sp, c
    // r8 = sub sp, c
    if (DestReg != BaseReg)
      DstNotEqBase = true;
    NumBits = 8;
    Opc = isSub ? ARM::tSUBi8 : ARM::tADDi8;
    isTwoAddr = true;
  }

  unsigned NumMIs = calcNumMI(Opc, ExtraOpc, Bytes, NumBits, Scale);
  unsigned Threshold = (DestReg == ARM::SP) ? 3 : 2;
  if (NumMIs > Threshold) {
    // This will expand into too many instructions. Load the immediate from a
    // constpool entry.
    emitThumbRegPlusImmInReg(MBB, MBBI, DestReg, BaseReg, NumBytes, true, TII,
                             MRI, dl);
    return;
  }

  if (DstNotEqBase) {
    if (isARMLowRegister(DestReg) && isARMLowRegister(BaseReg)) {
      // If both are low registers, emit DestReg = add BaseReg, max(Imm, 7)
      unsigned Chunk = (1 << 3) - 1;
      unsigned ThisVal = (Bytes > Chunk) ? Chunk : Bytes;
      Bytes -= ThisVal;
      BuildMI(MBB, MBBI, dl,TII.get(isSub ? ARM::tSUBi3 : ARM::tADDi3), DestReg)
        .addReg(BaseReg, RegState::Kill).addImm(ThisVal);
    } else {
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVr), DestReg)
        .addReg(BaseReg, RegState::Kill);
    }
    BaseReg = DestReg;
  }

  unsigned Chunk = ((1 << NumBits) - 1) * Scale;
  while (Bytes) {
    unsigned ThisVal = (Bytes > Chunk) ? Chunk : Bytes;
    Bytes -= ThisVal;
    ThisVal /= Scale;
    // Build the new tADD / tSUB.
    if (isTwoAddr)
      BuildMI(MBB, MBBI, dl, TII.get(Opc), DestReg)
        .addReg(DestReg).addImm(ThisVal);
    else {
      bool isKill = BaseReg != ARM::SP;
      BuildMI(MBB, MBBI, dl, TII.get(Opc), DestReg)
        .addReg(BaseReg, getKillRegState(isKill)).addImm(ThisVal);
      BaseReg = DestReg;

      if (Opc == ARM::tADDrSPi) {
        // r4 = add sp, imm
        // r4 = add r4, imm
        // ...
        NumBits = 8;
        Scale = 1;
        Chunk = ((1 << NumBits) - 1) * Scale;
        Opc = isSub ? ARM::tSUBi8 : ARM::tADDi8;
        isTwoAddr = true;
      }
    }
  }

  if (ExtraOpc)
    BuildMI(MBB, MBBI, dl, TII.get(ExtraOpc), DestReg)
      .addReg(DestReg, RegState::Kill)
      .addImm(((unsigned)NumBytes) & 3);
}

static void emitSPUpdate(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator &MBBI,
                         const TargetInstrInfo &TII, DebugLoc dl,
                         const Thumb2RegisterInfo &MRI,
                         int NumBytes) {
  emitThumbRegPlusImmediate(MBB, MBBI, ARM::SP, ARM::SP, NumBytes, TII,
                            MRI, dl);
}

void Thumb2RegisterInfo::
eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator I) const {
  if (!hasReservedCallFrame(MF)) {
    // If we have alloca, convert as follows:
    // ADJCALLSTACKDOWN -> sub, sp, sp, amount
    // ADJCALLSTACKUP   -> add, sp, sp, amount
    MachineInstr *Old = I;
    DebugLoc dl = Old->getDebugLoc();
    unsigned Amount = Old->getOperand(0).getImm();
    if (Amount != 0) {
      // We need to keep the stack aligned properly.  To do this, we round the
      // amount of space needed for the outgoing arguments up to the next
      // alignment boundary.
      unsigned Align = MF.getTarget().getFrameInfo()->getStackAlignment();
      Amount = (Amount+Align-1)/Align*Align;

      // Replace the pseudo instruction with a new instruction...
      unsigned Opc = Old->getOpcode();
      if (Opc == ARM::ADJCALLSTACKDOWN || Opc == ARM::tADJCALLSTACKDOWN) {
        emitSPUpdate(MBB, I, TII, dl, *this, -Amount);
      } else {
        assert(Opc == ARM::ADJCALLSTACKUP || Opc == ARM::tADJCALLSTACKUP);
        emitSPUpdate(MBB, I, TII, dl, *this, Amount);
      }
    }
  }
  MBB.erase(I);
}

/// emitThumbConstant - Emit a series of instructions to materialize a
/// constant.
static void emitThumbConstant(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator &MBBI,
                              unsigned DestReg, int Imm,
                              const TargetInstrInfo &TII,
                              const Thumb2RegisterInfo& MRI,
                              DebugLoc dl) {
  bool isSub = Imm < 0;
  if (isSub) Imm = -Imm;

  int Chunk = (1 << 8) - 1;
  int ThisVal = (Imm > Chunk) ? Chunk : Imm;
  Imm -= ThisVal;
  BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVi8), DestReg).addImm(ThisVal);
  if (Imm > 0)
    emitThumbRegPlusImmediate(MBB, MBBI, DestReg, DestReg, Imm, TII, MRI, dl);
  if (isSub)
    BuildMI(MBB, MBBI, dl, TII.get(ARM::tNEG), DestReg)
      .addReg(DestReg, RegState::Kill);
}

void Thumb2RegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                             int SPAdj, RegScavenger *RS) const{
  unsigned i = 0;
  MachineInstr &MI = *II;
  MachineBasicBlock &MBB = *MI.getParent();
  MachineFunction &MF = *MBB.getParent();
  ARMFunctionInfo *AFI = MF.getInfo<ARMFunctionInfo>();
  DebugLoc dl = MI.getDebugLoc();

  while (!MI.getOperand(i).isFI()) {
    ++i;
    assert(i < MI.getNumOperands() && "Instr doesn't have FrameIndex operand!");
  }

  unsigned FrameReg = ARM::SP;
  int FrameIndex = MI.getOperand(i).getIndex();
  int Offset = MF.getFrameInfo()->getObjectOffset(FrameIndex) +
               MF.getFrameInfo()->getStackSize() + SPAdj;

  if (AFI->isGPRCalleeSavedArea1Frame(FrameIndex))
    Offset -= AFI->getGPRCalleeSavedArea1Offset();
  else if (AFI->isGPRCalleeSavedArea2Frame(FrameIndex))
    Offset -= AFI->getGPRCalleeSavedArea2Offset();
  else if (hasFP(MF)) {
    assert(SPAdj == 0 && "Unexpected");
    // There is alloca()'s in this function, must reference off the frame
    // pointer instead.
    FrameReg = getFrameRegister(MF);
    Offset -= AFI->getFramePtrSpillOffset();
  }

  unsigned Opcode = MI.getOpcode();
  const TargetInstrDesc &Desc = MI.getDesc();
  unsigned AddrMode = (Desc.TSFlags & ARMII::AddrModeMask);

  if (Opcode == ARM::tADDrSPi) {
    Offset += MI.getOperand(i+1).getImm();

    // Can't use tADDrSPi if it's based off the frame pointer.
    unsigned NumBits = 0;
    unsigned Scale = 1;
    if (FrameReg != ARM::SP) {
      Opcode = ARM::tADDi3;
      MI.setDesc(TII.get(ARM::tADDi3));
      NumBits = 3;
    } else {
      NumBits = 8;
      Scale = 4;
      assert((Offset & 3) == 0 &&
             "Thumb add/sub sp, #imm immediate must be multiple of 4!");
    }

    if (Offset == 0) {
      // Turn it into a move.
      MI.setDesc(TII.get(ARM::tMOVhir2lor));
      MI.getOperand(i).ChangeToRegister(FrameReg, false);
      MI.RemoveOperand(i+1);
      return;
    }

    // Common case: small offset, fits into instruction.
    unsigned Mask = (1 << NumBits) - 1;
    if (((Offset / Scale) & ~Mask) == 0) {
      // Replace the FrameIndex with sp / fp
      MI.getOperand(i).ChangeToRegister(FrameReg, false);
      MI.getOperand(i+1).ChangeToImmediate(Offset / Scale);
      return;
    }

    unsigned DestReg = MI.getOperand(0).getReg();
    unsigned Bytes = (Offset > 0) ? Offset : -Offset;
    unsigned NumMIs = calcNumMI(Opcode, 0, Bytes, NumBits, Scale);
    // MI would expand into a large number of instructions. Don't try to
    // simplify the immediate.
    if (NumMIs > 2) {
      emitThumbRegPlusImmediate(MBB, II, DestReg, FrameReg, Offset, TII,
                                *this, dl);
      MBB.erase(II);
      return;
    }

    if (Offset > 0) {
      // Translate r0 = add sp, imm to
      // r0 = add sp, 255*4
      // r0 = add r0, (imm - 255*4)
      MI.getOperand(i).ChangeToRegister(FrameReg, false);
      MI.getOperand(i+1).ChangeToImmediate(Mask);
      Offset = (Offset - Mask * Scale);
      MachineBasicBlock::iterator NII = next(II);
      emitThumbRegPlusImmediate(MBB, NII, DestReg, DestReg, Offset, TII,
                                *this, dl);
    } else {
      // Translate r0 = add sp, -imm to
      // r0 = -imm (this is then translated into a series of instructons)
      // r0 = add r0, sp
      emitThumbConstant(MBB, II, DestReg, Offset, TII, *this, dl);
      MI.setDesc(TII.get(ARM::tADDhirr));
      MI.getOperand(i).ChangeToRegister(DestReg, false, false, true);
      MI.getOperand(i+1).ChangeToRegister(FrameReg, false);
    }
    return;
  } else {
    unsigned ImmIdx = 0;
    int InstrOffs = 0;
    unsigned NumBits = 0;
    unsigned Scale = 1;
    switch (AddrMode) {
    case ARMII::AddrModeT1_s: {
      ImmIdx = i+1;
      InstrOffs = MI.getOperand(ImmIdx).getImm();
      NumBits = (FrameReg == ARM::SP) ? 8 : 5;
      Scale = 4;
      break;
    }
    default:
      llvm_report_error("Unsupported addressing mode!");
      break;
    }

    Offset += InstrOffs * Scale;
    assert((Offset & (Scale-1)) == 0 && "Can't encode this offset!");

    // Common case: small offset, fits into instruction.
    MachineOperand &ImmOp = MI.getOperand(ImmIdx);
    int ImmedOffset = Offset / Scale;
    unsigned Mask = (1 << NumBits) - 1;
    if ((unsigned)Offset <= Mask * Scale) {
      // Replace the FrameIndex with sp
      MI.getOperand(i).ChangeToRegister(FrameReg, false);
      ImmOp.ChangeToImmediate(ImmedOffset);
      return;
    }

    bool isThumSpillRestore = Opcode == ARM::tRestore || Opcode == ARM::tSpill;
    if (AddrMode == ARMII::AddrModeT1_s) {
      // Thumb tLDRspi, tSTRspi. These will change to instructions that use
      // a different base register.
      NumBits = 5;
      Mask = (1 << NumBits) - 1;
    }
    // If this is a thumb spill / restore, we will be using a constpool load to
    // materialize the offset.
    if (AddrMode == ARMII::AddrModeT1_s && isThumSpillRestore)
      ImmOp.ChangeToImmediate(0);
    else {
      // Otherwise, it didn't fit. Pull in what we can to simplify the immed.
      ImmedOffset = ImmedOffset & Mask;
      ImmOp.ChangeToImmediate(ImmedOffset);
      Offset &= ~(Mask*Scale);
    }
  }

  // If we get here, the immediate doesn't fit into the instruction.  We folded
  // as much as possible above, handle the rest, providing a register that is
  // SP+LargeImm.
  assert(Offset && "This code isn't needed if offset already handled!");

  if (Desc.mayLoad()) {
    // Use the destination register to materialize sp + offset.
    unsigned TmpReg = MI.getOperand(0).getReg();
    bool UseRR = false;
    if (Opcode == ARM::tRestore) {
      if (FrameReg == ARM::SP)
        emitThumbRegPlusImmInReg(MBB, II, TmpReg, FrameReg,
                                 Offset, false, TII, *this, dl);
      else {
        emitLoadConstPool(MBB, II, &TII, dl, TmpReg, Offset);
        UseRR = true;
      }
    } else
      emitThumbRegPlusImmediate(MBB, II, TmpReg, FrameReg, Offset, TII,
                                *this, dl);
    MI.setDesc(TII.get(ARM::tLDR));
    MI.getOperand(i).ChangeToRegister(TmpReg, false, false, true);
    if (UseRR)
      // Use [reg, reg] addrmode.
      MI.addOperand(MachineOperand::CreateReg(FrameReg, false));
    else  // tLDR has an extra register operand.
      MI.addOperand(MachineOperand::CreateReg(0, false));
  } else if (Desc.mayStore()) {
    // FIXME! This is horrific!!! We need register scavenging.
    // Our temporary workaround has marked r3 unavailable. Of course, r3 is
    // also a ABI register so it's possible that is is the register that is
    // being storing here. If that's the case, we do the following:
    // r12 = r2
    // Use r2 to materialize sp + offset
    // str r3, r2
    // r2 = r12
    unsigned ValReg = MI.getOperand(0).getReg();
    unsigned TmpReg = ARM::R3;
    bool UseRR = false;
    if (ValReg == ARM::R3) {
      BuildMI(MBB, II, dl, TII.get(ARM::tMOVlor2hir), ARM::R12)
        .addReg(ARM::R2, RegState::Kill);
      TmpReg = ARM::R2;
    }
    if (TmpReg == ARM::R3 && AFI->isR3LiveIn())
      BuildMI(MBB, II, dl, TII.get(ARM::tMOVlor2hir), ARM::R12)
        .addReg(ARM::R3, RegState::Kill);
    if (Opcode == ARM::tSpill) {
      if (FrameReg == ARM::SP)
        emitThumbRegPlusImmInReg(MBB, II, TmpReg, FrameReg,
                                 Offset, false, TII, *this, dl);
      else {
        emitLoadConstPool(MBB, II, &TII, dl, TmpReg, Offset);
        UseRR = true;
      }
    } else
      emitThumbRegPlusImmediate(MBB, II, TmpReg, FrameReg, Offset, TII,
                                *this, dl);
    MI.setDesc(TII.get(ARM::tSTR));
    MI.getOperand(i).ChangeToRegister(TmpReg, false, false, true);
    if (UseRR)  // Use [reg, reg] addrmode.
      MI.addOperand(MachineOperand::CreateReg(FrameReg, false));
    else // tSTR has an extra register operand.
      MI.addOperand(MachineOperand::CreateReg(0, false));

    MachineBasicBlock::iterator NII = next(II);
    if (ValReg == ARM::R3)
      BuildMI(MBB, NII, dl, TII.get(ARM::tMOVhir2lor), ARM::R2)
        .addReg(ARM::R12, RegState::Kill);
    if (TmpReg == ARM::R3 && AFI->isR3LiveIn())
      BuildMI(MBB, NII, dl, TII.get(ARM::tMOVhir2lor), ARM::R3)
        .addReg(ARM::R12, RegState::Kill);
  } else
    assert(false && "Unexpected opcode!");
}

void Thumb2RegisterInfo::emitPrologue(MachineFunction &MF) const {
  MachineBasicBlock &MBB = MF.front();
  MachineBasicBlock::iterator MBBI = MBB.begin();
  MachineFrameInfo  *MFI = MF.getFrameInfo();
  ARMFunctionInfo *AFI = MF.getInfo<ARMFunctionInfo>();
  unsigned VARegSaveSize = AFI->getVarArgsRegSaveSize();
  unsigned NumBytes = MFI->getStackSize();
  const std::vector<CalleeSavedInfo> &CSI = MFI->getCalleeSavedInfo();
  DebugLoc dl = (MBBI != MBB.end() ?
                 MBBI->getDebugLoc() : DebugLoc::getUnknownLoc());

  // Check if R3 is live in. It might have to be used as a scratch register.
  for (MachineRegisterInfo::livein_iterator I =MF.getRegInfo().livein_begin(),
         E = MF.getRegInfo().livein_end(); I != E; ++I) {
    if (I->first == ARM::R3) {
      AFI->setR3IsLiveIn(true);
      break;
    }
  }

  // Thumb add/sub sp, imm8 instructions implicitly multiply the offset by 4.
  NumBytes = (NumBytes + 3) & ~3;
  MFI->setStackSize(NumBytes);

  // Determine the sizes of each callee-save spill areas and record which frame
  // belongs to which callee-save spill areas.
  unsigned GPRCS1Size = 0, GPRCS2Size = 0, DPRCSSize = 0;
  int FramePtrSpillFI = 0;

  if (VARegSaveSize)
    emitSPUpdate(MBB, MBBI, TII, dl, *this, -VARegSaveSize);

  if (!AFI->hasStackFrame()) {
    if (NumBytes != 0)
      emitSPUpdate(MBB, MBBI, TII, dl, *this, -NumBytes);
    return;
  }

  for (unsigned i = 0, e = CSI.size(); i != e; ++i) {
    unsigned Reg = CSI[i].getReg();
    int FI = CSI[i].getFrameIdx();
    switch (Reg) {
    case ARM::R4:
    case ARM::R5:
    case ARM::R6:
    case ARM::R7:
    case ARM::LR:
      if (Reg == FramePtr)
        FramePtrSpillFI = FI;
      AFI->addGPRCalleeSavedArea1Frame(FI);
      GPRCS1Size += 4;
      break;
    case ARM::R8:
    case ARM::R9:
    case ARM::R10:
    case ARM::R11:
      if (Reg == FramePtr)
        FramePtrSpillFI = FI;
      if (STI.isTargetDarwin()) {
        AFI->addGPRCalleeSavedArea2Frame(FI);
        GPRCS2Size += 4;
      } else {
        AFI->addGPRCalleeSavedArea1Frame(FI);
        GPRCS1Size += 4;
      }
      break;
    default:
      AFI->addDPRCalleeSavedAreaFrame(FI);
      DPRCSSize += 8;
    }
  }

  if (MBBI != MBB.end() && MBBI->getOpcode() == ARM::tPUSH) {
    ++MBBI;
    if (MBBI != MBB.end())
      dl = MBBI->getDebugLoc();
  }

  // Darwin ABI requires FP to point to the stack slot that contains the
  // previous FP.
  if (STI.isTargetDarwin() || hasFP(MF)) {
    MachineInstrBuilder MIB =
      BuildMI(MBB, MBBI, dl, TII.get(ARM::tADDrSPi), FramePtr)
      .addFrameIndex(FramePtrSpillFI).addImm(0);
  }

  // Determine starting offsets of spill areas.
  unsigned DPRCSOffset  = NumBytes - (GPRCS1Size + GPRCS2Size + DPRCSSize);
  unsigned GPRCS2Offset = DPRCSOffset + DPRCSSize;
  unsigned GPRCS1Offset = GPRCS2Offset + GPRCS2Size;
  AFI->setFramePtrSpillOffset(MFI->getObjectOffset(FramePtrSpillFI) + NumBytes);
  AFI->setGPRCalleeSavedArea1Offset(GPRCS1Offset);
  AFI->setGPRCalleeSavedArea2Offset(GPRCS2Offset);
  AFI->setDPRCalleeSavedAreaOffset(DPRCSOffset);

  NumBytes = DPRCSOffset;
  if (NumBytes) {
    // Insert it after all the callee-save spills.
    emitSPUpdate(MBB, MBBI, TII, dl, *this, -NumBytes);
  }

  if (STI.isTargetELF() && hasFP(MF)) {
    MFI->setOffsetAdjustment(MFI->getOffsetAdjustment() -
                             AFI->getFramePtrSpillOffset());
  }

  AFI->setGPRCalleeSavedArea1Size(GPRCS1Size);
  AFI->setGPRCalleeSavedArea2Size(GPRCS2Size);
  AFI->setDPRCalleeSavedAreaSize(DPRCSSize);
}

static bool isCalleeSavedRegister(unsigned Reg, const unsigned *CSRegs) {
  for (unsigned i = 0; CSRegs[i]; ++i)
    if (Reg == CSRegs[i])
      return true;
  return false;
}

static bool isCSRestore(MachineInstr *MI, const unsigned *CSRegs) {
  return (MI->getOpcode() == ARM::tRestore &&
          MI->getOperand(1).isFI() &&
          isCalleeSavedRegister(MI->getOperand(0).getReg(), CSRegs));
}

void Thumb2RegisterInfo::emitEpilogue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  MachineBasicBlock::iterator MBBI = prior(MBB.end());
  assert((MBBI->getOpcode() == ARM::tBX_RET ||
          MBBI->getOpcode() == ARM::tPOP_RET) &&
         "Can only insert epilog into returning blocks");
  DebugLoc dl = MBBI->getDebugLoc();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  ARMFunctionInfo *AFI = MF.getInfo<ARMFunctionInfo>();
  unsigned VARegSaveSize = AFI->getVarArgsRegSaveSize();
  int NumBytes = (int)MFI->getStackSize();

  if (!AFI->hasStackFrame()) {
    if (NumBytes != 0)
      emitSPUpdate(MBB, MBBI, TII, dl, *this, NumBytes);
  } else {
    // Unwind MBBI to point to first LDR / FLDD.
    const unsigned *CSRegs = getCalleeSavedRegs();
    if (MBBI != MBB.begin()) {
      do
        --MBBI;
      while (MBBI != MBB.begin() && isCSRestore(MBBI, CSRegs));
      if (!isCSRestore(MBBI, CSRegs))
        ++MBBI;
    }

    // Move SP to start of FP callee save spill area.
    NumBytes -= (AFI->getGPRCalleeSavedArea1Size() +
                 AFI->getGPRCalleeSavedArea2Size() +
                 AFI->getDPRCalleeSavedAreaSize());

    if (hasFP(MF)) {
      NumBytes = AFI->getFramePtrSpillOffset() - NumBytes;
      // Reset SP based on frame pointer only if the stack frame extends beyond
      // frame pointer stack slot or target is ELF and the function has FP.
      if (NumBytes)
        emitThumbRegPlusImmediate(MBB, MBBI, ARM::SP, FramePtr, -NumBytes,
                                  TII, *this, dl);
      else
        BuildMI(MBB, MBBI, dl, TII.get(ARM::tMOVlor2hir), ARM::SP)
          .addReg(FramePtr);
    } else {
      if (MBBI->getOpcode() == ARM::tBX_RET &&
          &MBB.front() != MBBI &&
          prior(MBBI)->getOpcode() == ARM::tPOP) {
        MachineBasicBlock::iterator PMBBI = prior(MBBI);
        emitSPUpdate(MBB, PMBBI, TII, dl, *this, NumBytes);
      } else
        emitSPUpdate(MBB, MBBI, TII, dl, *this, NumBytes);
    }
  }

  if (VARegSaveSize) {
    // Epilogue for vararg functions: pop LR to R3 and branch off it.
    // FIXME: Verify this is still ok when R3 is no longer being reserved.
    BuildMI(MBB, MBBI, dl, TII.get(ARM::tPOP)).addReg(ARM::R3);

    emitSPUpdate(MBB, MBBI, TII, dl, *this, VARegSaveSize);

    BuildMI(MBB, MBBI, dl, TII.get(ARM::tBX_RET_vararg)).addReg(ARM::R3);
    MBB.erase(MBBI);
  }
}
