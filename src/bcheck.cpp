/*
  Check protection stack balance for individual functions
  (and look for some other pointer protection bugs).

  Note that some functions have protection imbalance by design, most notable
  functions that manipulate the pointer protection stack and functions that
  are part of the parsers.

  The checking is somewhat path-sensitive and this sensitivity is adaptive.
  It increases when errors are found to validate they are not false alarms.

  The tool also looks for hints that there is an unprotected pointer while
  calling into a function that may allocate.  This is approximate only and
  has a lot of false alarms.
*/

#include "common.h"

#include <map>
#include <set>
#include <stack>
#include <unordered_set>
#include <unordered_map>

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Constants.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/CallGraph.h>

#include <llvm/Support/raw_ostream.h>

#include "errors.h"
#include "callocators.h"
#include "allocators.h"
#include "balance.h"
#include "freshvars.h"
#include "guards.h"
#include "linemsg.h"
#include "symbols.h"
#include "exceptions.h"
#include "liveness.h"

using namespace llvm;

const bool DEBUG = false;
const bool TRACE = false;

const bool DUMP_STATES = false;
const std::string DUMP_STATES_FUNCTION = "R_apply_dist_data_frame"; // only dump states in this function
const bool ONLY_FUNCTION = false; // only check one function (named ONLY_FUNCTION_NAME)
const std::string ONLY_FUNCTION_NAME = "R_apply_dist_data_frame";
const bool VERBOSE_DUMP = false;

const bool PROGRESS_MARKS = false;
const unsigned PROGRESS_STEP = 1000;

const bool SEPARATE_CHECKING = false;
  // check separate problems separately (e.g. balance, fresh SEXPs)
  //   separate checking could be faster for certain programs where the
  //   state space with join checking would be growing rapidly
  //   (but in the end it seems so far it is usually not the case)

const bool FULL_COMPARISON = true;
  // compare state precisely
  //   if disabled, only hashcodes are compared, which may cause some imprecision
  //   (some states will not be checked)
  //   yet there may be some speedups in some cases

const bool USE_ALLOCATOR_DETECTION = true;
  // use allocator detection to set SEXP guard variables to non-nill on allocation
  // this is optional, because it is not correct
  //   a function that would sometimes return a non-nill pointer, but at
  //   other times nil, will still be detected as an allocator [it would
  //   have been better to have a specific analysis for nullability]

// -------------------------
const bool UNIQUE_MSG = !DEBUG && !TRACE && !DUMP_STATES;
  // Do not write more than one identical messages per source line of code. 
  // This should be enable unless debugging.  When enabled, messages are
  // delayed until the next function, possibly even dropped in case of some
  // kind of adaptive checking.

const bool EXCLUDE_PROTECTION_FUNCTIONS = true;
  // if set to true, functions like protect, unprotect are not being checked (because they indeed cause imbalance)


// -------------------------------- basic block state -----------------------------------

const int MAX_STATES = BCHECK_MAX_STATES;        // maximum number of states visited per function

unsigned int nComparedEqual = 0;
unsigned int nComparedDifferent = 0;

struct BcheckStateTy : public StateWithGuardsTy, StateWithFreshVarsTy, StateWithBalanceTy {
  
  size_t hashcode;
  public:
    BcheckStateTy(BasicBlock *bb):
      StateBaseTy(bb), StateWithGuardsTy(bb), StateWithFreshVarsTy(bb), StateWithBalanceTy(bb), hashcode(0) {};

    BcheckStateTy(BasicBlock *bb, BalanceStateTy& balance, IntGuardsTy& intGuards, SEXPGuardsTy& sexpGuards, FreshVarsTy& freshVars):
      StateBaseTy(bb), StateWithGuardsTy(bb, intGuards, sexpGuards), StateWithFreshVarsTy(bb, freshVars), StateWithBalanceTy(bb, balance), hashcode(0) {};
      
    virtual BcheckStateTy* clone(BasicBlock *newBB) {
      return new BcheckStateTy(newBB, balance, intGuards, sexpGuards, freshVars);
    }
    
    virtual bool add();
    void hash() {
      size_t res = 0;
      hash_combine(res, bb);
      hash_combine(res, balance.depth);
      hash_combine(res, balance.count);
      hash_combine(res, balance.savedDepth);
      // not including topSaveVar
      hash_combine(res, (int) balance.countState);
      hash_combine(res, intGuards.size());
      for(IntGuardsTy::const_iterator gi = intGuards.begin(), ge = intGuards.end(); gi != ge; ++gi) {
        AllocaInst* var = gi->first;
        IntGuardState s = gi->second;
        hash_combine(res, (void *)var);
        hash_combine(res, (char) s);
      } // ordered map

      hash_combine(res, sexpGuards.size());
      for(SEXPGuardsTy::const_iterator gi = sexpGuards.begin(), ge = sexpGuards.end(); gi != ge; ++gi) {
        AllocaInst* var = gi->first;
        const SEXPGuardTy& g = gi->second;
        hash_combine(res, (void *) var);
        hash_combine(res, (char) g.state);
        if (g.state == SGS_SYMBOL) {
          hash_combine(res, g.symbolName);
        }
      } // ordered map

      hash_combine(res, freshVars.vars.size());
      for(FreshVarsVarsTy::iterator fi = freshVars.vars.begin(), fe = freshVars.vars.end(); fi != fe; ++fi) {
        AllocaInst* in = fi->first;
        int pcount = fi->second;
        hash_combine(res, (void *) in);
        hash_combine(res, pcount);
      } // ordered set

      hash_combine(res, freshVars.condMsgs.size());
      for(ConditionalMessagesTy::iterator mi = freshVars.condMsgs.begin(), me = freshVars.condMsgs.end(); mi != me; ++mi) {
        DelayedLineMessenger& msg = mi->second;
        hash_combine(res, msg.size());
        
        for(LineInfoPtrSetTy::const_iterator li = msg.delayedLineBuffer.begin(), le = msg.delayedLineBuffer.end(); li != le; ++li) {
          const LineInfoTy* l = *li;
          hash_combine(res, (const void *) l);
        }
      } // condMsgs is unordered

      hash_combine(res, freshVars.pstack.size());
      for(VarsVectorTy::iterator vi = freshVars.pstack.begin(), ve = freshVars.pstack.end(); vi != ve; ++vi) {
        AllocaInst* var = *vi;
        hash_combine(res, (void *) var);
      }
      hashcode = res;
    }

    void dump() {
      outs().flush();
      errs() << " vvvvvvvvvvvvvvvvvvvvvv  " << std::to_string(hashcode) << " vvvvvvvvvvvvvvvvvvvvvv";
      StateBaseTy::dump(VERBOSE_DUMP);
      StateWithGuardsTy::dump(VERBOSE_DUMP);
      StateWithFreshVarsTy::dump(VERBOSE_DUMP);
      StateWithBalanceTy::dump(VERBOSE_DUMP);
      errs() << " ^^^^^^^^^^^^^^^^^^^^^^  " << std::to_string(hashcode) << " ^^^^^^^^^^^^^^^^^^^^^^\n";
      errs().flush();
    }

};

// the hashcode is cached at the time of first hashing
//   (and indeed is not copied)

struct BcheckStateTy_hash {
  size_t operator()(const BcheckStateTy* t) const {
    return t->hashcode;
  }
};

struct BcheckStateTy_equal {
  bool operator() (const BcheckStateTy* lhs, const BcheckStateTy* rhs) const {

    if (!FULL_COMPARISON) {
      return lhs->hashcode == rhs->hashcode;
      // we could just return true, because the map will not call this for objects with
      // different hashcodes
    }
    
    bool res;
    if (lhs == rhs) {
      res = true;
    } else {
      res = lhs->bb == rhs->bb && 
      lhs->balance.depth == rhs->balance.depth && lhs->balance.savedDepth == rhs->balance.savedDepth && lhs->balance.count == rhs->balance.count &&
      lhs->balance.countState == rhs->balance.countState && lhs->balance.counterVar == rhs->balance.counterVar && lhs->balance.confused == rhs->balance.confused &&
      lhs->balance.topSaveVar == rhs->balance.topSaveVar &&
      lhs->intGuards == rhs->intGuards && lhs->sexpGuards == rhs->sexpGuards &&
      lhs->freshVars.vars == rhs->freshVars.vars && lhs->freshVars.condMsgs == rhs->freshVars.condMsgs && lhs->freshVars.pstack == rhs->freshVars.pstack
         && lhs->freshVars.confused == rhs->freshVars.confused;
    }
    
    if (PROGRESS_MARKS) {
      if (res) {
        nComparedEqual++;
      } else {
        nComparedDifferent++;
      }
    }
    return res;
  }
};

typedef std::stack<BcheckStateTy*> WorkListTy;
typedef std::unordered_set<BcheckStateTy*, BcheckStateTy_hash, BcheckStateTy_equal> DoneSetTy;

// ------------- helper functions --------------

DoneSetTy doneSet;
WorkListTy workList;   

bool BcheckStateTy::add() {
  hash(); // precompute hashcode
  auto sinsert = doneSet.insert(this);
  if (sinsert.second) {
    workList.push(this);
    if (DUMP_STATES && (DUMP_STATES_FUNCTION.empty() || DUMP_STATES_FUNCTION == bb->getParent()->getName())) {
      outs().flush();
      errs() << "\n -- dumping a new state being added -- \n";
      workList.top()->dump();
    }
    return true;
  } else {
    delete this; // NOTE: state suicide
    return false;
  }
}

unsigned long totalStates = 0;

void clearStates() {
  // clear the worklist and the doneset
  totalStates += doneSet.size();
  for(DoneSetTy::iterator ds = doneSet.begin(), de = doneSet.end(); ds != de; ++ds) {
    BcheckStateTy *old = *ds;
    delete old;
  }
  doneSet.clear();
  WorkListTy empty;
  std::swap(workList, empty);
  // all elements in worklist are also in doneset, so no need to call destructors
}

void handleUnprotectWithIntGuard(Instruction *in, BcheckStateTy& s, GlobalsTy& g, IntGuardsChecker& intGuardsChecker, LineMessenger& msg, unsigned& refinableInfos) { 
  
  // UNPROTECT(intguard ? 3 : 4)
  
  CallSite cs(cast<Value>(in));
  if (!cs) {
    return;
  }
  const Function* targetFunc = cs.getCalledFunction();
  if (!targetFunc || targetFunc != g.unprotectFunction) return;
          
  Value* unprotectValue = cs.getArgument(0);
  if (!SelectInst::classof(unprotectValue)) {
    return;
  } 
  
  SelectInst *si = cast<SelectInst>(unprotectValue);
              
  if (!CmpInst::classof(si->getCondition()) || !ConstantInt::classof(si->getTrueValue()) || !ConstantInt::classof(si->getFalseValue())) {
    return;
  }
                
  CmpInst *ci = cast<CmpInst>(si->getCondition());
  if (!ci->isEquality()) {
    return;
  }
  
  LoadInst *guardOp;
  ConstantInt *constOp;
  
  if (LoadInst::classof(ci->getOperand(0)) && ConstantInt::classof(ci->getOperand(1))) {
    guardOp = cast<LoadInst>(ci->getOperand(0));
    constOp = cast<ConstantInt>(ci->getOperand(1));
  } else if (ConstantInt::classof(ci->getOperand(0)) && LoadInst::classof(ci->getOperand(1))) {
    constOp = cast<ConstantInt>(ci->getOperand(0));
    guardOp = cast<LoadInst>(ci->getOperand(1));
  } else {
    return;
  }
  
  if (!constOp->isZero()) {
    return;
  }
  
  Value *guardValue = guardOp->getPointerOperand();
  if (!AllocaInst::classof(guardValue) || !intGuardsChecker.isGuard(cast<AllocaInst>(guardValue))) {
    return;
  }
                  
  IntGuardState gs = intGuardsChecker.getGuardState(s.intGuards, cast<AllocaInst>(guardValue));
                    
  if (gs != IGS_UNKNOWN) {
    uint64_t arg; 
    if ( (gs == IGS_ZERO && ci->isTrueWhenEqual()) || (gs == IGS_NONZERO && ci->isFalseWhenEqual()) ) {
      arg = cast<ConstantInt>(si->getTrueValue())->getZExtValue();
    } else {
      arg = cast<ConstantInt>(si->getFalseValue())->getZExtValue();
    }
    s.balance.depth -= (int) arg;
    msg.debug("unprotect call using constant in conditional expression on integer guard", in);              
    if (s.balance.countState != CS_DIFF && s.balance.depth < 0) {
      msg.info("has negative depth", in);
      refinableInfos++;
    }
  }
}

struct ModuleCheckingStateTy {
  FunctionsSetTy& possibleAllocators;
  FunctionsSetTy& allocatingFunctions;
  FunctionsSetTy& errorFunctions;
  GlobalsTy& gl;
  LineMessenger& msg;
  CalledModuleTy& cm;
  CProtectInfo& cprotect;
  
  ModuleCheckingStateTy(FunctionsSetTy& possibleAllocators, FunctionsSetTy& allocatingFunctions, FunctionsSetTy& errorFunctions,
      GlobalsTy& gl, LineMessenger& msg, CalledModuleTy& cm, CProtectInfo& cprotect):
    possibleAllocators(possibleAllocators), allocatingFunctions(allocatingFunctions), errorFunctions(errorFunctions), gl(gl), msg(msg), cm(cm), cprotect(cprotect) {};
};

class FunctionChecker {

  Function *fun;
  VarBoolCacheTy saveVarsCache;
  VarBoolCacheTy counterVarsCache;
  VarBoolCacheTy checkedVarsCache;
  IntGuardsChecker intGuardsChecker;
  SEXPGuardsChecker sexpGuardsChecker;
  BasicBlocksSetTy errorBasicBlocks;
  LiveVarsTy liveVars;

  ModuleCheckingStateTy& m;

  void checkFunction(bool intGuardsEnabled, bool sexpGuardsEnabled, bool balanceCheckingEnabled, bool freshVarsCheckingEnabled, unsigned& refinableInfos) {
  
    refinableInfos = 0;
    bool restartable = (!intGuardsEnabled && !avoidIntGuardsFor(fun)) || (!sexpGuardsEnabled && !avoidSEXPGuardsFor(fun));
    clearStates();
    {
      BcheckStateTy* initState = new BcheckStateTy(&fun->getEntryBlock());
      initState->add();
    }
    while(!workList.empty()) {
      if (restartable && refinableInfos > 0) {
        clearStates();
        return;
      }
      
      if (ONLY_FUNCTION && ONLY_FUNCTION_NAME != fun->getName()) {
        workList.pop();
        continue;
      }
      
      if (DUMP_STATES && (DUMP_STATES_FUNCTION.empty() || DUMP_STATES_FUNCTION == fun->getName())) {
        outs().flush();
        errs() << "\n -- dumping a state being visited -- \n";
        workList.top()->dump();
      }

      BcheckStateTy s(*workList.top());
      workList.pop();
      m.msg.trace("going to work on this state:", &*s.bb->begin());
      
      if (errorBasicBlocks.find(s.bb) != errorBasicBlocks.end()) {
        m.msg.debug("ignoring basic block on error path", &*s.bb->begin());
        continue;
      }
      
      if (doneSet.size() > MAX_STATES) {
        errs() << "ERROR: too many states (abstraction error?) in function " << funName(fun) << "\n";
        clearStates();
        return;
      }
      
      if (PROGRESS_MARKS) {
        if (doneSet.size() % PROGRESS_STEP == 0) {
          errs() << "current worklist:" << std::to_string(workList.size()) << " current function:" << funName(fun) <<
            " done:" << std::to_string(doneSet.size()) << " equal:" << nComparedEqual << " different:" << nComparedDifferent << "\n";
        }
      }      
      
      // process a single basic block
      for(BasicBlock::iterator ini = s.bb->begin(), ine = s.bb->end(); ini != ine; ++ini) {
        Instruction *in = &*ini;
        m.msg.trace("visiting", in);
   
        if (freshVarsCheckingEnabled) {
          handleFreshVarsForNonTerminator(in, &m.cm, sexpGuardsEnabled ? &sexpGuardsChecker : NULL, sexpGuardsEnabled ? &s.sexpGuards : NULL, s.freshVars, 
            m.msg, refinableInfos, liveVars, m.cprotect, balanceCheckingEnabled ? &s.balance : NULL, checkedVarsCache);
              // NOTE: must be called before balance handling
              //  because it uses some state of balance handling that will be removed by the call to
              //  handleBalanceForNonTerminator, e.g. re protection counter or topsave variable
            
          if (restartable && refinableInfos > 0) { clearStates(); return; }
        }
        if (balanceCheckingEnabled) {
          handleBalanceForNonTerminator(in, s.balance, m.gl, counterVarsCache, saveVarsCache, m.msg, refinableInfos);
          if (restartable && refinableInfos > 0) { clearStates(); return; }
        }
 
        if (intGuardsEnabled) {
          intGuardsChecker.handleForNonTerminator(in, s.intGuards);
          if (restartable && refinableInfos > 0) { clearStates(); return; }
          if (balanceCheckingEnabled) {
            handleUnprotectWithIntGuard(in, s, m.gl, intGuardsChecker, m.msg, refinableInfos);
            if (restartable && refinableInfos > 0) { clearStates(); return; }
          }
        }
        if (sexpGuardsEnabled) {
          sexpGuardsChecker.handleForNonTerminator(in, s.sexpGuards);
          if (restartable && refinableInfos > 0) { clearStates(); return; }
        }
      }
      
      TerminatorInst *t = s.bb->getTerminator();

      if (freshVarsCheckingEnabled) {
        handleFreshVarsForTerminator(t, s.freshVars, liveVars); // does nothing anyway
      }

      if (balanceCheckingEnabled && handleBalanceForTerminator(t, s, m.gl, counterVarsCache, m.msg, refinableInfos)) {
        // ignore successors in case important errors were already found, and hence further
        // errors found will just confuse the user
        continue;
      }

      if (sexpGuardsEnabled && sexpGuardsChecker.handleForTerminator(t, s)) {
        continue;
      }

        // int guards have to be after balance, so that "if (nprotect) UNPROTECT(nprotect)"
        // is handled in preference of int guard
      if (intGuardsEnabled && intGuardsChecker.handleForTerminator(t, s)) {
        continue;
      }
      
      // add conservatively all cfg successors
      for(int i = 0, nsucc = t->getNumSuccessors(); i < nsucc; i++) {
        BasicBlock *succ = t->getSuccessor(i);
        {
          BcheckStateTy* state = s.clone(succ);
          if (state->add()) {
            m.msg.trace("added (conservatively) successor of", t);
          }
        }
      }
    }
  }
  
  public:
    FunctionChecker(Function *fun, ModuleCheckingStateTy& moduleState): 
        fun(fun), saveVarsCache(), counterVarsCache(), checkedVarsCache(), intGuardsChecker(&moduleState.msg), 
        /* TODO: we would need "sure" allocators here instead of possible allocators! */
        sexpGuardsChecker(&moduleState.msg, &moduleState.gl, 
          USE_ALLOCATOR_DETECTION ? moduleState.cm.getContextSensitivePossibleAllocators() : NULL, moduleState.cm.getSymbolsMap(), NULL, moduleState.cm.getVrfState(), &moduleState.cm),
        errorBasicBlocks(), m(moduleState) {
        
      findErrorBasicBlocks(fun, &m.errorFunctions, errorBasicBlocks);
      liveVars = findLiveVariables(fun);
    }  
  
    // handles restarts
    void checkFunction(bool balanceCheckingEnabled, bool freshVarsCheckingEnabled, std::string checksName) {

      m.msg.newFunction(fun, checksName);
      bool intGuardsEnabled = false;
      bool sexpGuardsEnabled = false;
      unsigned refinableInfos;
    
      for(;;) {
        checkFunction(intGuardsEnabled, sexpGuardsEnabled, balanceCheckingEnabled, freshVarsCheckingEnabled, refinableInfos);
    
        bool restartable = (!intGuardsEnabled && !avoidIntGuardsFor(fun)) || (!sexpGuardsEnabled && !avoidSEXPGuardsFor(fun));
        if (restartable && refinableInfos>0) {
          // retry with more precise checking
          m.msg.clear();
          if (!intGuardsEnabled && !avoidIntGuardsFor(fun)) {
            intGuardsEnabled = true;
          } else if (!sexpGuardsEnabled && !avoidSEXPGuardsFor(fun)) {
            sexpGuardsEnabled = true;
          }
        } else {
          break;
        }
      }
    }
};


// -------------------------------- main  -----------------------------------

int main(int argc, char* argv[])
{
  LLVMContext context;
  FunctionsOrderedSetTy functionsOfInterestSet;
  FunctionsVectorTy functionsOfInterestVector;
  
  Module *m = parseArgsReadIR(argc, argv, functionsOfInterestSet, functionsOfInterestVector, context);
//  EXCLUDE_PROTECTION_FUNCTIONS = (argc == 3); // exclude when checking modules
  GlobalsTy gl(m);
  LineMessenger msg(context, DEBUG, TRACE, UNIQUE_MSG);
  
  FunctionsSetTy errorFunctions;
  findErrorFunctions(m, errorFunctions);

  FunctionsSetTy possibleAllocators;
  findPossibleAllocators(m, possibleAllocators);

  FunctionsSetTy allocatingFunctions;
  findAllocatingFunctions(m, allocatingFunctions);

  SymbolsMapTy symbolsMap;
  findSymbols(m, &symbolsMap);
  
  CalledModuleTy cm(m, &symbolsMap, &errorFunctions, &gl, &possibleAllocators, &allocatingFunctions);
  CProtectInfo cprotect = findCalleeProtectFunctions(m, *cm.getContextSensitiveAllocatingFunctions());
  
  ModuleCheckingStateTy mstate(possibleAllocators, allocatingFunctions, errorFunctions, gl, msg, cm, cprotect); 
    // FIXME: perhaps get rid of ModuleCheckingState now that we have CalledModule

  unsigned nAnalyzedFunctions = 0;
  for(FunctionsVectorTy::iterator FI = functionsOfInterestVector.begin(), FE = functionsOfInterestVector.end(); FI != FE; ++FI) {
    Function *fun = *FI;

    if (!fun) continue;
    if (!fun->size()) continue;
    
    if (EXCLUDE_PROTECTION_FUNCTIONS &&
      (fun == gl.protectFunction ||
      fun == gl.protectWithIndexFunction ||
      fun == gl.unprotectFunction ||
      fun == gl.unprotectPtrFunction)) {
      
      continue;
    }
    
    nAnalyzedFunctions++;
    FunctionChecker fchk(fun, mstate);

    if (SEPARATE_CHECKING) {
        // FIXME: it would make more sense to only print prefixes [BP] and [UP] with join checking
      fchk.checkFunction(true, false, " [protection balance]");
      fchk.checkFunction(false, true, " [unprotected pointers]");
    } else {
      fchk.checkFunction(true, true, "");  
    }
  }
  msg.flush();
  clearStates();
  delete m;

  outs().flush();
  errs() << "Analyzed " << nAnalyzedFunctions << " functions, traversed " << totalStates << " states.\n";
  return 0;
}
