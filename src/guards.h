#ifndef RCHK_GUARDS_H
#define RCHK_GUARDS_H

#include "common.h"
#include "callocators.h"
#include "linemsg.h"
#include "state.h"
#include "symbols.h"

#include <map>

#include <llvm/IR/Instruction.h>

using namespace llvm;

// integer variable used as a guard

enum IntGuardState {
  IGS_ZERO = 0,
  IGS_NONZERO,
  IGS_UNKNOWN
};

typedef std::map<AllocaInst*,IntGuardState> IntGuardsTy;
struct StateWithGuardsTy;

std::string igs_name(IntGuardState igs);
IntGuardState getIntGuardState(IntGuardsTy& intGuards, AllocaInst* var);
bool isIntegerGuardVariable(AllocaInst* var);
bool isIntegerGuardVariable(AllocaInst* var, VarBoolCacheTy& cache);
void handleIntGuardsForNonTerminator(Instruction* in, VarBoolCacheTy& intGuardVarsCache, IntGuardsTy& intGuards, LineMessenger& msg);
bool handleIntGuardsForTerminator(TerminatorInst* t, VarBoolCacheTy& intGuardVarsCache, StateWithGuardsTy& s, LineMessenger& msg);

// SEXP - an "R pointer" used as a guard

enum SEXPGuardState {
  SGS_NIL = 0, // R_NilValue
  SGS_SYMBOL,  // A specific symbol, name stored in symbolName
  SGS_NONNIL,
  SGS_UNKNOWN
};

struct SEXPGuardTy {
  SEXPGuardState state;
  std::string symbolName;
  
  SEXPGuardTy(SEXPGuardState state, std::string& symbolName): state(state), symbolName(symbolName) {}
  SEXPGuardTy(SEXPGuardState state): state(state), symbolName() { assert(state != SGS_SYMBOL); }
  SEXPGuardTy() : SEXPGuardTy(SGS_UNKNOWN) {};
  
  bool operator==(const SEXPGuardTy& other) const { return state == other.state && symbolName == other.symbolName; };
  
};

typedef std::map<AllocaInst*,SEXPGuardTy> SEXPGuardsTy;

std::string sgs_name(SEXPGuardState sgs);
SEXPGuardState getSEXPGuardState(SEXPGuardsTy& sexpGuards, AllocaInst* var);
bool isSEXPGuardVariable(AllocaInst* var, GlobalVariable* nilVariable, Function* isNullFunction);
bool isSEXPGuardVariable(AllocaInst* var, GlobalVariable* nilVariable, Function* isNullFunction, VarBoolCacheTy& cache);
void handleSEXPGuardsForNonTerminator(Instruction* in, VarBoolCacheTy& sexpGuardVarsCache, SEXPGuardsTy& sexpGuards,
  GlobalsTy& g, ArgInfosTy* argInfos, SymbolsMapTy* symbolsMap, LineMessenger& msg, FunctionsSetTy* possibleAllocators);
bool handleSEXPGuardsForTerminator(TerminatorInst* t, VarBoolCacheTy& sexpGuardVarsCache, StateWithGuardsTy& s, 
  GlobalsTy& g, ArgInfosTy* argInfos, SymbolsMapTy* symbolsMap, LineMessenger& msg);

// checking state with guards

struct StateWithGuardsTy : virtual public StateBaseTy {
  IntGuardsTy intGuards;
  SEXPGuardsTy sexpGuards;
  
  StateWithGuardsTy(BasicBlock *bb, IntGuardsTy& intGuards, SEXPGuardsTy& sexpGuards): StateBaseTy(bb), intGuards(intGuards), sexpGuards(sexpGuards) {};
  StateWithGuardsTy(BasicBlock *bb): StateBaseTy(bb), intGuards(), sexpGuards() {};
  
  virtual StateWithGuardsTy* clone(BasicBlock *newBB) = 0;
  
  void dump(bool verbose);
};

#endif