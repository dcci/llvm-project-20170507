//===-- PIC16AsmPrinter.h - PIC16 LLVM assembly writer ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to PIC16 assembly language.
//
//===----------------------------------------------------------------------===//

#ifndef PIC16ASMPRINTER_H
#define PIC16ASMPRINTER_H

#include "PIC16.h"
#include "PIC16TargetMachine.h"
#include "PIC16DebugInfo.h"
#include "PIC16MCAsmInfo.h"
#include "PIC16TargetObjectFile.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetMachine.h"
#include <list>
#include <string>

namespace llvm {
  class VISIBILITY_HIDDEN PIC16AsmPrinter : public AsmPrinter {
  public:
    explicit PIC16AsmPrinter(formatted_raw_ostream &O, TargetMachine &TM,
                             const MCAsmInfo *T, bool V);
  private:
    virtual const char *getPassName() const {
      return "PIC16 Assembly Printer";
    }
    
    PIC16TargetObjectFile &getObjFileLowering() const {
      return (PIC16TargetObjectFile &)AsmPrinter::getObjFileLowering();
    }

    bool runOnMachineFunction(MachineFunction &F);
    void printOperand(const MachineInstr *MI, int opNum);
    void printCCOperand(const MachineInstr *MI, int opNum);
    void printInstruction(const MachineInstr *MI); // definition autogenerated.
    bool printMachineInstruction(const MachineInstr *MI);
    void EmitFunctionDecls (Module &M);
    void EmitUndefinedVars (Module &M);
    void EmitDefinedVars (Module &M);
    void EmitIData (Module &M);
    void EmitUData (Module &M);
    void EmitAutos (std::string FunctName);
    void EmitRemainingAutos ();
    void EmitRomData (Module &M);
    void EmitFunctionFrame(MachineFunction &MF);
    void printLibcallDecls();
  protected:
    bool doInitialization(Module &M);
    bool doFinalization(Module &M);

    /// PrintGlobalVariable - Emit the specified global variable and its
    /// initializer to the output stream.
    virtual void PrintGlobalVariable(const GlobalVariable *GV) {
      // PIC16 doesn't use normal hooks for this.
    }
    
  private:
    PIC16TargetObjectFile *PTOF;
    PIC16TargetLowering *PTLI;
    PIC16DbgInfo DbgInfo;
    const PIC16MCAsmInfo *PTAI;
    std::list<const char *> LibcallDecls; // List of extern decls.
  };
} // end of namespace

#endif
