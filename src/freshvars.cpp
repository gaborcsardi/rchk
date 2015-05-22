
#include "freshvars.h"
#include "guards.h"
#include "exceptions.h"

#include <llvm/IR/CallSite.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

const bool REPORT_FRESH_ARGUMENTS = true;
 // now disabled as this is a common source of false alarms (many functions are callee-protect)

using namespace llvm;

static void pruneFreshVars(Instruction *in, FreshVarsTy& freshVars, LiveVarsTy& liveVars) {

  // clean up freshVars -- remove entries and conditional messages for dead variables
  for (FreshVarsVarsTy::iterator fi = freshVars.vars.begin(), fe = freshVars.vars.end(); fi != fe;) {
    AllocaInst *var = fi->first;
      
    auto lsearch = liveVars.find(in);
    assert(lsearch != liveVars.end());
      
    VarsSetTy& lvars = lsearch->second;
    if (lvars.find(var) == lvars.end()) {
      fi = freshVars.vars.erase(fi);
      freshVars.condMsgs.erase(var);
      continue;
    }
    ++fi;
  }
}

static void unprotectAll(FreshVarsTy& freshVars) {
  freshVars.pstack.clear();
  for (FreshVarsVarsTy::iterator fi = freshVars.vars.begin(), fe = freshVars.vars.end(); fi != fe; ++fi) {
    fi->second = 0; // zero protect count
  }
}

static void handleCall(Instruction *in, CalledModuleTy *cm, SEXPGuardsTy *sexpGuards, FreshVarsTy& freshVars,
    LineMessenger& msg, unsigned& refinableInfos, LiveVarsTy& liveVars) {
  
  const CalledFunctionTy *tgt = cm->getCalledFunction(in, sexpGuards);
  if (!tgt) {
    return;
  }
  CallSite cs(cast<Value>(in));
  assert(cs);
  assert(cs.getCalledFunction());


  // handle protect
  
  // TODO: handle PreserveObject(x = alloc())
  
  Function *f = tgt->fun;
  
    // FIXME: get rid of copy paste between PreserveObject and PROTECT handling
  if (f->getName() == "R_PreserveObject") {
    Value* arg = cs.getArgument(0);
    AllocaInst* var = NULL;
    
    if (LoadInst* li = dyn_cast<LoadInst>(arg)) { // PreserveObject(x)
      var = dyn_cast<AllocaInst>(li->getPointerOperand()); 
      if (msg.debug()) msg.debug("PreserveObject of variable " + varName(var), in); 
    }
    if (!var) { // PreserveObject(x = foo())
      for(Value::user_iterator ui = arg->user_begin(), ue = arg->user_end(); ui != ue; ++ui) { 
        User *u = *ui;
        if (StoreInst* si = dyn_cast<StoreInst>(u)) {
          var = dyn_cast<AllocaInst>(si->getPointerOperand()); 
          if (msg.debug()) msg.debug("indirect PreserveObject of variable PreserveObject(x = foo())" + varName(var), in); 
          // FIXME: there could be multiple variables and not all of them fresh
          break; 
        }
      }
    }
    if (!var) { // x = PreserveObject(foo())
      for(Value::user_iterator ui = in->user_begin(), ue = in->user_end(); ui != ue; ++ui) { 
        User *u = *ui;
        if (StoreInst* si = dyn_cast<StoreInst>(u)) {
          var = dyn_cast<AllocaInst>(si->getPointerOperand()); 
          if (msg.debug()) msg.debug("implied PreserveObject of variable x = PreserveObject(foo())" + varName(var), in); 
          // FIXME: there could be multiple uses, some possibly conflicting
          break; 
        }
      }
    }  

    if (var) {
      freshVars.vars.erase(var);
      if (msg.debug()) msg.debug("Variable " + varName(var) + " given to PreserveObject and thus no longer fresh", in);
    }
    return;
  }
  
  if (f->getName() == "Rf_protect" || f->getName() == "R_ProtectWithIndex") {
    // note, we don't support REPROTECT
    // it also does not have much sense as the checker is identifying protected objects by local variables
    //   which is on its own imprecise
    
    Value* arg = cs.getArgument(0);
    AllocaInst* var = NULL;
    
    if (LoadInst* li = dyn_cast<LoadInst>(arg)) { // PROTECT(x)
      var = dyn_cast<AllocaInst>(li->getPointerOperand()); 
      if (msg.debug()) msg.debug("PROTECT of variable " + varName(var), in); 
    }
    if (!var) { // PROTECT(x = foo())
      for(Value::user_iterator ui = arg->user_begin(), ue = arg->user_end(); ui != ue; ++ui) { 
        User *u = *ui;
        if (StoreInst* si = dyn_cast<StoreInst>(u)) {
          var = dyn_cast<AllocaInst>(si->getPointerOperand()); 
          if (msg.debug()) msg.debug("indirect PROTECT of variable PROTECT(x = foo())" + varName(var), in); 
          // FIXME: there could be multiple variables and not all of them fresh
          break; 
        }
      }
    }
    if (!var) { // x = PROTECT(foo())
      for(Value::user_iterator ui = in->user_begin(), ue = in->user_end(); ui != ue; ++ui) { 
        User *u = *ui;
        if (StoreInst* si = dyn_cast<StoreInst>(u)) {
          var = dyn_cast<AllocaInst>(si->getPointerOperand()); 
          if (msg.debug()) msg.debug("implied PROTECT of variable x = PROTECT(foo())" + varName(var), in); 
          // FIXME: there could be multiple uses, some possibly conflicting
          break; 
        }
      }
    }  

    if (freshVars.pstack.size() == MAX_PSTACK_SIZE) {
      unprotectAll(freshVars);
      refinableInfos++;
      msg.info("protect stack is too deep, unprotecting all variables", in);
      return;
    }
    
    if (var) {
      freshVars.pstack.push_back(var);
      if (msg.debug()) msg.debug("pushed variable " + varName(var) + " to the protect stack (size " + std::to_string(freshVars.pstack.size()) + ")", in);

      // NOTE: the handling of PROTECT(x = foo()) only will increment x's protectcount correctly
      // if the store x = %tmpvalue is done _before_ the call PROTECT(%tmpvalue)
      //   (otherwise the store would normally set the protectcount to zero)
        
      auto vsearch = freshVars.vars.find(var);
      if (vsearch != freshVars.vars.end()) {
        int nProtects = vsearch->second;
        vsearch->second = ++nProtects;
        if (msg.debug()) msg.debug("incremented protect count of variable " + varName(var) + " to " + std::to_string(nProtects), in); 
      } else {
        // the variable is not currently fresh, but the fact that it is being protected actually means
        //   that there is probably a reason to protect it, so when unprotected, it should be then treated
        //   as fresh again... so lets add it with protect count of 1
        
        freshVars.vars.insert({var, 1});
        if (msg.debug()) msg.debug("non-fresh variable " + varName(var) + " is being protected, inserting it as fresh with protectcount 1", in); 
      }
      return;
    }

    freshVars.pstack.push_back(NULL);
    if (msg.debug()) msg.debug("pushed anonymous value to the protect stack (size " + std::to_string(freshVars.pstack.size()) + ")", in);
    return;
  }
  
  if (f->getName() == "Rf_unprotect") {
    Value* arg = cs.getArgument(0);
    if (ConstantInt* ci = dyn_cast<ConstantInt>(arg)) {
      uint64_t val = ci->getZExtValue();
      if (val > freshVars.pstack.size()) {
        msg.info("attempt to unprotect more items (" + std::to_string(val) + ") than protected ("
          + std::to_string(freshVars.pstack.size()) + "), results will be incorrect", in);
          
        refinableInfos++;
        return;
      }
      while(val-- > 0) {
        AllocaInst* var = freshVars.pstack.back();
        if (!var) {
          continue;
        }
        auto vsearch = freshVars.vars.find(var);
        if (vsearch != freshVars.vars.end()) {  // decrement protect count of a possibly fresh variable
          int nProtects = vsearch->second;
          nProtects--;
          if (nProtects < 0) {
            msg.info("protect count of variable " + varName(var) + " went negative, set to zero (error?)", in);
            nProtects = 0;
            refinableInfos++;
          } else {
            if (msg.debug()) msg.debug("decremented protect count of variable " + varName(var) + " to " + std::to_string(nProtects), in);
          }
          vsearch->second = nProtects;
        }
        if (msg.debug()) msg.debug("unprotected variable " + varName(var), in);
        freshVars.pstack.pop_back();
      }
      
    } else {
      // unsupported forms of unprotect
      if (msg.debug()) msg.debug("unsupported form of unprotect, unprotecting all variables", in);
      unprotectAll(freshVars);
      return;
    }
    return;
  }
  
  if (!cm->isCAllocating(tgt)) {
    return;
  }
  
  // calling an allocating function
  
  if (REPORT_FRESH_ARGUMENTS && !protectsArguments(tgt)) {
    for(CallSite::arg_iterator ai = cs.arg_begin(), ae = cs.arg_end(); ai != ae; ++ai) {
      Value *arg = *ai;
      const CalledFunctionTy *src = cm->getCalledFunction(arg, sexpGuards);
      if (!src || !cm->isPossibleCAllocator(src)) {
        continue;
      }
      msg.info("calling allocating function " + funName(tgt) + " with argument allocated using " + funName(src), in);
      refinableInfos++;
    }
  }
  
  pruneFreshVars(in, freshVars, liveVars);
  if (freshVars.vars.size() > 0) {
  
    // compute all variables passed to the call
    //   (if a fresh variable is passed to a function, it is not to be reported here as error)
    // FIXME: perhaps should do this only for callee-protect functions?
    
    VarsSetTy passedVars;
    FunctionType* ftype = f->getFunctionType();
    unsigned nParams = ftype->getNumParams();
    
    unsigned i = 0;
    for(CallSite::arg_iterator ai = cs.arg_begin(), ae = cs.arg_end(); ai != ae; ++ai, ++i) {
      Value *arg = *ai;
      
      if (i < nParams && !isSEXP(ftype->getParamType(i))) {
        // note i can be >= nParams when the function accepts varargs (...)
        continue;
      }
      
      if (LoadInst *li = dyn_cast<LoadInst>(arg)) {  // foo(x)
        if (AllocaInst *lvar = dyn_cast<AllocaInst>(li->getPointerOperand())) {
          passedVars.insert(lvar);
        }
        continue;
      }
      if (arg->hasOneUse()) {
        continue;
      }
      // foo(x = bar())
      //   handling this is, sadly, quite slow
      for(Value::user_iterator ui = arg->user_begin(), ue = arg->user_end(); ui != ue; ++ui) {
        User *u = *ui;
        if (StoreInst* si = dyn_cast<StoreInst>(u)) {
          if (AllocaInst *svar = dyn_cast<AllocaInst>(si->getPointerOperand())) {
            passedVars.insert(svar);
          }
        }
      }
    }
  
    for (FreshVarsVarsTy::iterator fi = freshVars.vars.begin(), fe = freshVars.vars.end(); fi != fe; ++fi) {
      AllocaInst *var = fi->first;
      
      int nProtects = fi->second;
      if (nProtects > 0) { // the variable is not really currently fresh, it is protected
        return;
      }
      
      if (passedVars.find(var) != passedVars.end()) {
        // this fresh variable is in fact being passed to the function, so don't report it
        continue;
      }
      
      std::string message = "unprotected variable " + varName(var) + " while calling allocating function " + funName(tgt);
    
      // prepare a conditional message
      auto vsearch = freshVars.condMsgs.find(var);
      if (vsearch == freshVars.condMsgs.end()) {
        DelayedLineMessenger dmsg(&msg);
        dmsg.info(message, in);
        freshVars.condMsgs.insert({var, dmsg});
        if (msg.debug()) msg.debug("created conditional message \"" + message + "\" first for variable " + varName(var), in);
      } else {
        DelayedLineMessenger& dmsg = vsearch->second;
        dmsg.info(message, in);
        if (msg.debug()) msg.debug("added conditional message \"" + message + "\" for variable " + varName(var) + "(size " + std::to_string(dmsg.size()) + ")", in);
      }
    }
  }
}

static void handleLoad(Instruction *in, CalledModuleTy *cm, SEXPGuardsTy *sexpGuards, FreshVarsTy& freshVars, LineMessenger& msg, unsigned& refinableInfos) {
  if (!LoadInst::classof(in)) {
    return;
  }
  LoadInst *li = cast<LoadInst>(in);
  if (!AllocaInst::classof(li->getPointerOperand())) {
    return;
  }
  AllocaInst *var = cast<AllocaInst>(li->getPointerOperand());
  // a variable is being loaded
  
  // check for conditional messages
  auto msearch = freshVars.condMsgs.find(var);
  if (msearch != freshVars.condMsgs.end()) {
    msearch->second.flush();
    refinableInfos++;
    freshVars.condMsgs.erase(msearch);
    if (msg.debug()) msg.debug("printed conditional messages on use of variable " + varName(var), in);
  }
  
  auto vsearch = freshVars.vars.find(var);
  if (vsearch == freshVars.vars.end()) { 
    return;
  }
  int nProtects = vsearch->second;
  
  // a fresh variable is being loaded

  for(Value::user_iterator ui = li->user_begin(), ue = li->user_end(); ui != ue; ++ui) {
    User *u = *ui;
    
    CallSite cs(u);  // variable passed to a call as argument
    if (cs) {
      Function *tgt = cs.getCalledFunction();
      if (tgt) {

        // heuristic - handle functions usually protecting like setAttrib(x, ...) where x is protected (say, non-fresh, as approximation)
        if (cs.arg_size() > 1 && isSetterFunction(tgt)) {
          if (LoadInst* firstArgLoad = dyn_cast<LoadInst>(cs.getArgument(0))) {
            if (AllocaInst* firstArg = dyn_cast<AllocaInst>(firstArgLoad->getPointerOperand())) {
              if (freshVars.vars.find(firstArg) == freshVars.vars.end()) {
                if (msg.debug()) msg.debug("fresh variable " + varName(var) + " passed to known setter function (possibly implicitly protecting) " + funName(tgt) + " and thus no longer fresh" , in);  
                
                break;
              }
            }
          }
        }
      }
      continue;
    }
    
    if (StoreInst *sinst = dyn_cast<StoreInst>(u)) { // variable stored
      if (sinst->getValueOperand() == u) {
        if (!AllocaInst::classof(sinst->getPointerOperand())) {
          // variable stored into a non-local variable (e.g. into a global or into a location derived from a local variable, e.g. setting an attribute
          // of an SEXP in a local variable)
          
          // the heuristic is that these stores are usually implicitly protecting
          if (msg.debug()) msg.debug("fresh variable " + varName(var) + " stored into a global or derived local, and thus no longer fresh" , in);
          freshVars.vars.erase(var); // implicit protection, remove from map
          break;
        }
      }
      continue;
    }
  }

  if (REPORT_FRESH_ARGUMENTS) {
    if (!li->hasOneUse()) { // too restrictive? should look at other uses too?
      return;
    }

    // fresh variable passed to an allocating function - but this may be ok if it is callee-protect function
    //   or if the function allocates only after the fresh argument is no longer needed    
    
    const CalledFunctionTy* tgt = cm->getCalledFunction(li->user_back(), sexpGuards);
    if (!tgt || !cm->isCAllocating(tgt) || protectsArguments(tgt)) {
      return;
    }
  
    if (nProtects > 0) { // the variable is not really fresh now, it is protected
      return;
    }    

    std::string nameSuffix = "";
    if (var->getName().str().empty()) {
      unsigned i;
      CallSite cs(cast<Value>(li->user_back()));
      assert(cs);
      for(i = 0; i < cs.arg_size(); i++) {
        if (cs.getArgument(i) == li) {
          nameSuffix = " <arg " + std::to_string(i+1) + ">";
          break;
        }
      }
    }
    msg.info("calling allocating function " + funName(tgt) + " with a fresh pointer (" + varName(var) + nameSuffix + ")", in);
    refinableInfos++;
  }
}

static void handleStore(Instruction *in, CalledModuleTy *cm, SEXPGuardsTy *sexpGuards, FreshVarsTy& freshVars, LineMessenger& msg, unsigned& refinableInfos) {
  if (!StoreInst::classof(in)) {
    return;
  }
  Value* storePointerOp = cast<StoreInst>(in)->getPointerOperand();
  Value* storeValueOp = cast<StoreInst>(in)->getValueOperand();

  if (!AllocaInst::classof(storePointerOp)) {
    return;
  }
  AllocaInst *var = cast<AllocaInst>(storePointerOp);
  
  // a variable is being killed by the store, erase conditional messages if any
  if (freshVars.condMsgs.erase(var)) {
    if (msg.debug()) msg.debug("removed conditional messages as variable " + varName(var) + " is rewritten.", in);
  }
  
  const CalledFunctionTy *srcFun = cm->getCalledFunction(storeValueOp, sexpGuards);
  if (srcFun) { 
    // only allowing single use, the other use can be and often is PROTECT
    
    Function* sf = srcFun->fun;
    if (sf->getName() == "Rf_protect" || sf->getName() == "Rf_protectWithIndex") {
      // this case is being handled in handleCall
      return;
    }
    
    if (cm->isPossibleCAllocator(srcFun)) { // FIXME: this is very approximative -- we would rather need to know guaranteed allocators, but we have _maybe_ allocators
      // the store (re-)creates a fresh variable
      
      // check if the value stored is also being protected (e.g. PROTECT(x = allocVector())

      for(Value::user_iterator ui = storeValueOp->user_begin(), ue = storeValueOp->user_end(); ui != ue; ++ui) {
        User *u = *ui;
        CallSite cs(u);
        if (cs && cs.getCalledFunction()) {
          Function* otherFun = cs.getCalledFunction();
          if (otherFun->getName() == "Rf_protect" || otherFun->getName() == "Rf_protectWithIndex") {
            // this case is handled in handleCall
            return;
          }
          if (otherFun->getName() == "R_Reprotect") {
            // do nothing, assume the protectcount of the variable should not change
            // and that is in freshvars
            return;
          }
        }
      }
      
      int nProtects = 0;
      auto vsearch = freshVars.vars.find(var);
      if (vsearch == freshVars.vars.end()) {
        freshVars.vars.insert({var, nProtects}); // remember, insert won't ovewrite std:map value for an existing key
      } else {
        vsearch->second = nProtects;
      }
      if (msg.debug()) msg.debug("initialized fresh SEXP variable " + varName(var) + " with protect count " + std::to_string(nProtects), in);
      return;
    }
    
    // FIXME: handle x = PROTECT(allocVector)
  }
  
  // the store turns a variable into non-fresh  
  if (freshVars.vars.find(var) != freshVars.vars.end()) {
    freshVars.vars.erase(var);
    if (msg.debug()) msg.debug("fresh variable " + varName(var) + " rewritten and thus no longer fresh", in);
  }
}

void handleFreshVarsForNonTerminator(Instruction *in, CalledModuleTy *cm, SEXPGuardsTy *sexpGuards,
    FreshVarsTy& freshVars, LineMessenger& msg, unsigned& refinableInfos, LiveVarsTy& liveVars) {

  handleCall(in, cm, sexpGuards, freshVars, msg, refinableInfos, liveVars);
  handleLoad(in, cm, sexpGuards, freshVars, msg, refinableInfos);
  handleStore(in, cm, sexpGuards, freshVars, msg, refinableInfos);
}

void handleFreshVarsForTerminator(Instruction *in, FreshVarsTy& freshVars, LiveVarsTy& liveVars) {

  // this does not pay off here (it is enough during handleCall)
  //pruneFreshVars(in, freshVars, liveVars);
}

void StateWithFreshVarsTy::dump(bool verbose) {

  errs() << "=== fresh vars: " << &freshVars << "\n";
  for(FreshVarsVarsTy::iterator fi = freshVars.vars.begin(), fe = freshVars.vars.end(); fi != fe; ++fi) {
    AllocaInst *var = fi->first;
    errs() << "   " << var->getName();
    if (verbose) {
      errs() << " " << *var;
    }
    
    int depth = fi->second;
    errs() << " " << std::to_string(depth);
    
    auto vsearch = freshVars.condMsgs.find(var);
    if (vsearch != freshVars.condMsgs.end()) {
      errs() << " conditional messages: \n";
      DelayedLineMessenger& dmsg = vsearch->second;
      dmsg.print("    ");
    }
    
    errs() << "\n";
  }
  errs() << " protect stack:";

  for(VarsVectorTy::iterator vi = freshVars.pstack.begin(), ve = freshVars.pstack.end(); vi != ve; ++vi) {
    AllocaInst* var = *vi;

    errs() << " ";
    if (var) {
      errs() << varName(var);
    } else {
      errs() << "(ANON)";
    }
  }
  errs() << "\n";
}
