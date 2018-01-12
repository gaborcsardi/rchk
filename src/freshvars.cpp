
#include "freshvars.h"
#include "guards.h"
#include "exceptions.h"
#include "patterns.h"

#include <llvm/IR/CallSite.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

const bool QUIET_WHEN_CONFUSED = true;
  // do not report any messages and don't do any check once confused by the code
  // in such case, in practice, the messages are almost always false alarms

const std::string CONFUSION_DISCLAIMER = QUIET_WHEN_CONFUSED ? "results will be incomplete" : "results will be incorrect";

const std::string MSG_PFX = "[UP] ";

using namespace llvm;

static void pruneFreshVars(Instruction *in, FreshVarsTy& freshVars, LiveVarsTy& liveVars, LineMessenger& msg, unsigned& refinableInfos) {

  // clean up freshVars
  //   remove entries and conditional messages for dead variables
  //   also print conditional messages for variables that are now definitely going to be used
    
  for (FreshVarsVarsTy::iterator fi = freshVars.vars.begin(), fe = freshVars.vars.end(); fi != fe;) {
    AllocaInst *var = fi->first;
      
    auto lsearch = liveVars.find(in);
    myassert(lsearch != liveVars.end());
      
    VarsLiveness& lvars = lsearch->second;
    if (!lvars.isPossiblyUsed(var)) {
      fi = freshVars.vars.erase(fi);
      freshVars.condMsgs.erase(var);
      continue;

    } else if (!lvars.isPossiblyKilled(var)) {
      auto msearch = freshVars.condMsgs.find(var);
      if (msearch != freshVars.condMsgs.end()) {
        msearch->second.flush();
        refinableInfos++;
        freshVars.condMsgs.erase(msearch);
        if (msg.debug()) msg.debug(MSG_PFX + "printed conditional messages as variable " + varName(var) + " is now definitely going to be used", in);
      }
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

static void issueConditionalMessage(Instruction *in, AllocaInst *var, FreshVarsTy& freshVars, LineMessenger& msg, unsigned& refinableInfos,
    LiveVarsTy& liveVars, std::string& message) {

  auto lsearch = liveVars.find(in);
  if (lsearch != liveVars.end()) {
    // there should be a record for all instructions
    VarsLiveness& vlive = lsearch->second;
    if (vlive.isDefinitelyUsed(var)) {
      msg.info(MSG_PFX + message, in);
      if (msg.trace()) msg.trace("issued an info directly because variable \"" + varName(var) + "\" is definitely live", in);
      refinableInfos++;
      return;
    }
  } 
    
  // prepare a conditional message - the variable may be live, but we don't know
  auto vsearch = freshVars.condMsgs.find(var);
  if (vsearch == freshVars.condMsgs.end()) {
    DelayedLineMessenger dmsg(&msg);
    dmsg.info(MSG_PFX + message, in);
    freshVars.condMsgs.insert({var, dmsg});
    if (msg.debug()) msg.debug(MSG_PFX + "created conditional message \"" + message + "\" first for variable " + varName(var), in);
  } else {
    DelayedLineMessenger& dmsg = vsearch->second;
    dmsg.info(MSG_PFX + message, in);
    if (msg.debug()) msg.debug(MSG_PFX + "added conditional message \"" + message + "\" for variable " + varName(var) + "(size " + std::to_string(dmsg.size()) + ")", in);
  }
}

static bool isVarCheckedFresh(AllocaInst *var) {

  for(Value::user_iterator ui = var->user_begin(), ue = var->user_end(); ui != ue; ++ui) {
    User *u = *ui;
    
    if (LoadInst::classof(u)) {
      continue;
    }
    if (StoreInst::classof(u)) {
      continue;
    }
    // this may e.g. be that an address of the variable is taken
    return false;
  }
  
  return true;
}

static bool isVarCheckedFresh(AllocaInst *var, VarBoolCacheTy& cache, LineMessenger& msg) {

  if (!isSEXP(var)) {
    return false;
  }

  auto vsearch = cache.find(var);
  if (vsearch != cache.end()) {
    return vsearch->second;
  }
  
  bool isChecked = isVarCheckedFresh(var);
  
  if (!isChecked) {
    // the message is here to make sure it is printed only once
    //   the line messenger mechanism for printing unique messages won't do in practice
    //   because generating the message is too expensive
    msg.info(MSG_PFX + "ignoring variable " + varName(var) + " as it has address taken, results will be incomplete ", NULL);  
  }
  
  cache.insert({var, isChecked});
  return isChecked;
}

static void unprotectOne(FreshVarsTy& freshVars, LineMessenger& msg, unsigned& refinableInfos, Instruction *in) {

  AllocaInst* var = freshVars.pstack.back();
  freshVars.pstack.pop_back();

  if (!var) {
    return;
  }
  
  auto vsearch = freshVars.vars.find(var);
  if (vsearch != freshVars.vars.end()) {  // decrement protect count of a possibly fresh variable
    int nProtects = vsearch->second;
    nProtects--;
    if (nProtects < 0) {
      // this happens quite commonly without necessarily being an error, e.g.
      // 
      // PROTECT(x);
      // x = foo(x);
      // UNPROTECT(1);
      // PROTECT(x);

    if (msg.debug()) msg.debug(MSG_PFX + "protect count of variable " + varName(var) + " went negative, set to zero (error?)", in);
      nProtects = 0;
      refinableInfos++;
    } else {
      if (msg.debug()) msg.debug(MSG_PFX + "decremented protect count of variable " + varName(var) + " to " + std::to_string(nProtects), in);
    }
    vsearch->second = nProtects;
  }
  if (msg.debug()) msg.debug(MSG_PFX + "unprotected variable " + varName(var), in);
}

static void handleCall(Instruction *in, CalledModuleTy *cm, SEXPGuardsChecker *sexpGuardsChecker, SEXPGuardsTy *sexpGuards, FreshVarsTy& freshVars,
    LineMessenger& msg, unsigned& refinableInfos, LiveVarsTy& liveVars, CProtectInfo& cprotect, BalanceStateTy* balance, VarBoolCacheTy& checkedVarsCache) {
  
  bool confused = QUIET_WHEN_CONFUSED && freshVars.confused;

  const CalledFunctionTy *tgt = cm->getCalledFunction(in, sexpGuardsChecker, sexpGuards, false);
  if (!tgt) {
    return;
  }
  CallSite cs(cast<Value>(in));
  myassert(cs);
  myassert(cs.getCalledFunction());
  Function *f = tgt->fun;

  // handle protect
  
  if (!confused) {
      // FIXME: get rid of copy paste between PreserveObject and PROTECT handling
    if (f->getName() == "R_PreserveObject") {
      Value* arg = cs.getArgument(0);
      AllocaInst* var = NULL;
    
      if (LoadInst* li = dyn_cast<LoadInst>(arg)) { // PreserveObject(x)
        var = dyn_cast<AllocaInst>(li->getPointerOperand()); 
        if (msg.debug()) msg.debug(MSG_PFX + "PreserveObject of variable " + varName(var), in); 
      }
      if (!var) { // PreserveObject(x = foo())
        for(Value::user_iterator ui = arg->user_begin(), ue = arg->user_end(); ui != ue; ++ui) { 
          User *u = *ui;
          if (StoreInst* si = dyn_cast<StoreInst>(u)) {
            var = dyn_cast<AllocaInst>(si->getPointerOperand()); 
            if (msg.debug()) msg.debug(MSG_PFX + "indirect PreserveObject of variable PreserveObject(x = foo())" + varName(var), in); 
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
            if (msg.debug()) msg.debug(MSG_PFX + "implied PreserveObject of variable x = PreserveObject(foo())" + varName(var), in); 
            // FIXME: there could be multiple uses, some possibly conflicting
            break; 
          }
        }
      }  

      if (var) {
        freshVars.vars.erase(var); // conditional messages, if any, should have been handled by load
        if (msg.debug()) msg.debug(MSG_PFX + "Variable " + varName(var) + " given to PreserveObject and thus no longer fresh", in);
      }
      // do not return, PreserveObject allocates
    }
  
    if (f->getName() == "Rf_protect" || f->getName() == "R_ProtectWithIndex" || f->getName() == "R_Reprotect") {
    
      Value* arg = cs.getArgument(0);
      AllocaInst* var = NULL;
    
      if (LoadInst* li = dyn_cast<LoadInst>(arg)) { // PROTECT(x)
        var = dyn_cast<AllocaInst>(li->getPointerOperand()); 
        if (msg.debug()) msg.debug(MSG_PFX + "PROTECT of variable " + varName(var), in); 
      }
      if (!var) { // PROTECT(x = foo())
        for(Value::user_iterator ui = arg->user_begin(), ue = arg->user_end(); ui != ue; ++ui) { 
          User *u = *ui;
          if (StoreInst* si = dyn_cast<StoreInst>(u)) {
            var = dyn_cast<AllocaInst>(si->getPointerOperand()); 
            if (msg.debug()) msg.debug(MSG_PFX + "indirect PROTECT of variable PROTECT(x = foo()) " + varName(var), in); 
            // FIXME: there could be multiple variables and not all of them fresh
            break; 
          }
        }
        if (var) {
          // found indirect PROTECT(x = foo()) - indirect protect of "x"
          // now check whether this indirect protect is also a part of a setter call, e.g.
          //
          // setAttrib(x, PROTECT(d = alloc()))
          //
          // yes, some code does it...
          //
          //        setAttrib(r, R_DimNamesSymbol, PROTECT(d = allocVector(VECSXP, 2)));
          //
          //  %197 = call %struct.SEXPREC* @Rf_allocVector(i32 19, i64 2), !dbg !1619 ; [#uses=2 type=%struct.SEXPREC*] [debug line = 489:33]
          // store %struct.SEXPREC* %197, %struct.SEXPREC** %22, align 8, !dbg !1619 ; [debug line = 489:33] <=========================================== stores result of allocVector into "d"
          // %198 = call %struct.SEXPREC* @Rf_protect(%struct.SEXPREC* %197), !dbg !1620 ; [#uses=1 type=%struct.SEXPREC*] [debug line = 489:33]  <====== PROTECT the source of "d"
          // %199 = call %struct.SEXPREC* @Rf_setAttrib(%struct.SEXPREC* %195, %struct.SEXPREC* %196, %struct.SEXPREC* %198), !dbg !1621 ; [#uses=0 type=%struct.SEXPREC*] [debug line = 489:2]
          
          for(Value::user_iterator ui = in->user_begin(), ue = in->user_end(); ui != ue; ++ui) { 
            User *u = *ui;
            CallSite cs2(u);
            if (!cs2) continue;
            Function *tgt = cs2.getCalledFunction();
            if (!tgt) continue;
            // heuristic - handle functions usually protecting like setAttrib(x, ...) where x is protected (say, non-fresh, as approximation)
            if (cs2.arg_size() > 1 && isSetterFunction(tgt)) {
              if (LoadInst* firstArgLoad = dyn_cast<LoadInst>(cs2.getArgument(0))) {
                if (AllocaInst* firstArg = dyn_cast<AllocaInst>(firstArgLoad->getPointerOperand())) {
                  if (firstArg != var) {
                    auto vsearch = freshVars.vars.find(firstArg);
                    if (vsearch == freshVars.vars.end() || (vsearch->second > 0)) {
                      // first argument of the setter is not fresh
                
                      if (msg.debug()) msg.debug(MSG_PFX + "fresh variable " + varName(var) + " passed to known setter function (possibly implicitly protecting) via setter(,PROTECT(x=foo())) " + funName(tgt) + " and thus no longer fresh" , in);
                      freshVars.vars.erase(var);
                      var = NULL;
                        // setting the variable to null because otherwise it would be by the code below
                        // inserted into the fresh list again with protect count 1, so will become fresh again
                        //   (by a heuristic that guess that if a variable is being protected, it is for a reason, and hence
                        //    perhaps should be treated as fresh again...)
                      
                      break;
                    }
                  }
                }
              }
            }            
          }
        }
      }
      if (!var) { // x = PROTECT(foo())
        for(Value::user_iterator ui = in->user_begin(), ue = in->user_end(); ui != ue; ++ui) { 
          User *u = *ui;
          if (StoreInst* si = dyn_cast<StoreInst>(u)) {
            var = dyn_cast<AllocaInst>(si->getPointerOperand()); 
            if (msg.debug()) msg.debug(MSG_PFX + "implied PROTECT of variable x = PROTECT(foo()) " + varName(var), in); 
            // FIXME: there could be multiple uses, some possibly conflicting
            break; 
          }
        }
      }
      
      if (var && !isVarCheckedFresh(var, checkedVarsCache, msg)) {
        var = NULL; // fall back below into pushing anonymous value on the stack
      }
    
      if (f->getName() == "R_Reprotect") {
      
        if (!var) {
          if (msg.debug()) msg.debug(MSG_PFX + "ignoring reprotect of unknown variable", in);
          return;
        }
      
        auto vsearch = freshVars.vars.find(var);
        if (vsearch != freshVars.vars.end()) {
          int nProtects = vsearch->second;
          if (nProtects > 0) {
            if (msg.debug()) msg.debug(MSG_PFX + "left alone protect count of variable " + varName(var) + " on " + std::to_string(nProtects) + " at REPROTECT", in);
          } else {
            // usually this means a protected variable has been modified and then re-protected
            // typically it was before protected just once, so lets set its protect count to 1
          
            nProtects = 1;
            vsearch->second = nProtects;
            if (msg.debug()) msg.debug(MSG_PFX + "set protect count of variable " + varName(var) + " to 1 at REPROTECT (heuristic)", in);
          }	
        } else {
          // this is rather strange
      
          // the variable is not currently fresh, but the fact that it is being reprotected actually means
          //   that there is probably a reason to protect it
          //   just a heuristic, it does not always work with implicit protection
        
          freshVars.vars.insert({var, 1});
          if (msg.debug()) msg.debug(MSG_PFX + "non-fresh variable " + varName(var) + " is being REPROTECTed, inserting it as fresh with protectcount 1", in); 
        }
        return;  
      }

      if (freshVars.pstack.size() == MAX_PSTACK_SIZE) {
        unprotectAll(freshVars);
        refinableInfos++;
        msg.info(MSG_PFX + "protect stack is too deep, unprotecting all variables, " + CONFUSION_DISCLAIMER, NULL);
        if (QUIET_WHEN_CONFUSED) freshVars.confused = true;
        return;
      }
    
      if (var) {
        freshVars.pstack.push_back(var);
        if (msg.debug()) msg.debug(MSG_PFX + "pushed variable " + varName(var) + " to the protect stack (size " + std::to_string(freshVars.pstack.size()) + ")", in);

        // NOTE: the handling of PROTECT(x = foo()) only will increment x's protectcount correctly
        // if the store x = %tmpvalue is done _before_ the call PROTECT(%tmpvalue)
        //   (otherwise the store would normally set the protectcount to zero)
        
        auto vsearch = freshVars.vars.find(var);
        if (vsearch != freshVars.vars.end()) {
          int nProtects = vsearch->second;
          vsearch->second = ++nProtects;
          if (msg.debug()) msg.debug(MSG_PFX + "incremented protect count of variable " + varName(var) + " to " + std::to_string(nProtects), in); 
        } else {
          // the variable is not currently fresh, but the fact that it is being protected actually means
          //   that there is probably a reason to protect it, so when unprotected, it should be then treated
          //   as fresh again... so lets add it with protect count of 1
          // just a heuristic, it does not always work with implicit protection
        
          freshVars.vars.insert({var, 1});
          if (msg.debug()) msg.debug(MSG_PFX + "non-fresh variable " + varName(var) + " is being protected, inserting it as fresh with protectcount 1", in); 
        }
        return;
      }

      freshVars.pstack.push_back(NULL);
      if (msg.debug()) msg.debug(MSG_PFX + "pushed anonymous value to the protect stack (size " + std::to_string(freshVars.pstack.size()) + ")", in);
    }
  
    if (f->getName() == "Rf_unprotect") {
      Value* arg = cs.getArgument(0);
      
      uint64_t unprotectCount = 0;
      bool haveCount = false;
      
      if (balance) { // UNPROTECT(nprotect)
        if (LoadInst* li = dyn_cast<LoadInst>(arg)) {
          if (AllocaInst *cvar = dyn_cast<AllocaInst>(li->getPointerOperand())) {
            if (balance->counterVar == cvar && balance->countState == CS_EXACT) {
              unprotectCount = balance->count;        
              haveCount = true;
            }
          }
        }
      }
      
      if (ConstantInt* ci = dyn_cast<ConstantInt>(arg)) { // UNPROTECT(const)
        unprotectCount = ci->getZExtValue();
        haveCount = true;
      }
      
      if (haveCount) {
        if (unprotectCount > freshVars.pstack.size()) {
          msg.info(MSG_PFX + "attempt to unprotect more items (" + std::to_string(unprotectCount) + ") than protected ("
            + std::to_string(freshVars.pstack.size()) + "), " + CONFUSION_DISCLAIMER, in);
          
          refinableInfos++;
          if (QUIET_WHEN_CONFUSED) freshVars.confused = true;
          return;
        }
        while(unprotectCount-- > 0) {
          unprotectOne(freshVars, msg, refinableInfos, in);
        }
      
      } else {
        // unsupported forms of unprotect
        // FIXME: this is not great
        msg.info(MSG_PFX + "unsupported form of unprotect, unprotecting all variables, " + CONFUSION_DISCLAIMER, in);
        unprotectAll(freshVars);
        if (QUIET_WHEN_CONFUSED) freshVars.confused = true;
        return;
      }
    }
  }
  
  if (!cm->isCAllocating(tgt)) {
    return;
  }
  
  // calling an allocating function
  
  if (!protectsArguments(tgt) && !cprotect.isCalleeSafe(tgt->fun, false)) {
    // this check can be done even when the tool is confused
    unsigned aidx = 0;
    for(CallSite::arg_iterator ai = cs.arg_begin(), ae = cs.arg_end(); ai != ae; ++ai, ++aidx) {
      Value *arg = *ai;
      const CalledFunctionTy *src = cm->getCalledFunction(arg, sexpGuardsChecker, sexpGuards, false);
      if (!src || !cm->isPossibleCAllocator(src)) {
        continue;
      }
      if (aidx < tgt->fun->arg_size() && cprotect.isCalleeSafe(tgt->fun, aidx, false)) {
        // we are directly passing an argument, so it does not matter the argument is destroyed by the call
        // (well, except that the value may be used again, in the LLVM bitcode -- it is an approximation that we ignore this)
        continue;
      }
      msg.info(MSG_PFX + "calling allocating function " + funName(tgt) + " with argument allocated using " + funName(src), in);
      refinableInfos++;
    }
  }
 
  if (confused) {
    return;
  }
  
  pruneFreshVars(in, freshVars, liveVars, msg, refinableInfos); // make sure messages are not emitted for (obviously) dead variables
  if (freshVars.vars.size() > 0) {
  
    if (msg.trace()) msg.trace(MSG_PFX + "checking freshvars at allocating call to " + funName(tgt), in);
  
    // compute all variables passed to the call
    //   (if a fresh variable is passed to a function, it is not to be reported here as error, but it is done at handleLoad)
    
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
        if (msg.trace()) msg.trace(MSG_PFX + "variable " + varName(var) + " has protect count " + std::to_string(nProtects) + " when passed to function " + funName(tgt) + " so not reported", in);
        continue;
      }
      
      if (passedVars.find(var) != passedVars.end()) {
        if (msg.trace()) msg.trace(MSG_PFX + "fresh variable " + varName(var) + " is passed to function " + funName(tgt) + " so not reported", in);
        // this fresh variable is in fact being passed to the function, so don't report it
        continue;
      }
      
      std::string message = "unprotected variable " + varName(var) + " while calling allocating function " + funName(tgt);
      issueConditionalMessage(in, var, freshVars, msg, refinableInfos, liveVars, message);
    }
  }
}

static void handleLoad(Instruction *in, CalledModuleTy *cm, SEXPGuardsChecker* sexpGuardsChecker, SEXPGuardsTy *sexpGuards, FreshVarsTy& freshVars, LineMessenger& msg,
    unsigned& refinableInfos, LiveVarsTy& liveVars, CProtectInfo& cprotect) {
    
  if (QUIET_WHEN_CONFUSED && freshVars.confused) {
    return;
  }
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
    if (msg.debug()) msg.debug(MSG_PFX + "printed conditional messages on use of variable " + varName(var), in);
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
            
              if (firstArg != var) {
                auto vsearch = freshVars.vars.find(firstArg);
                if (vsearch == freshVars.vars.end() || (vsearch->second > 0)) {
                  // first argument of the setter is not fresh
                
                  if (msg.debug()) msg.debug(MSG_PFX + "fresh variable " + varName(var) + " passed to known setter function (possibly implicitly protecting) " + funName(tgt) + " and thus no longer fresh" , in);
                  freshVars.vars.erase(var);
                  break;
                }
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
          if (msg.debug()) msg.debug(MSG_PFX + "fresh variable " + varName(var) + " stored into a global or derived local, and thus no longer fresh" , in);
          freshVars.vars.erase(var); // implicit protection, remove from map
          break;
        }
      }
      continue;
    }
  }

  if (!li->hasOneUse()) { // too restrictive? should look at other uses too?
    return;
  }

  // fresh variable passed to an allocating function - but this may be ok if it is callee-protect function
  //   or if the function allocates only after the fresh argument is no longer needed    
    
  const CalledFunctionTy* tgt = cm->getCalledFunction(li->user_back(), sexpGuardsChecker, sexpGuards, false);
  if (!tgt || !cm->isCAllocating(tgt) || protectsArguments(tgt) || cprotect.isCalleeProtect(tgt->fun, false)) {
    return;
  }
  
  if (nProtects > 0) { // the variable is not really fresh now, it is protected
    return;
  }    

  unsigned aidx = 0;
  CallSite cs(cast<Value>(li->user_back()));
  myassert(cs);
  
  for(aidx = 0; aidx < cs.arg_size(); aidx++) {
    if (cs.getArgument(aidx) == li) {
      break;
    }
  }
  myassert(aidx < cs.arg_size());

  if (aidx < tgt->fun->arg_size() && cprotect.isCalleeProtect(tgt->fun, aidx, false)) {
    return; // the variable is callee-protect for the given argument
  }

  std::string nameSuffix = "";
  if (var->getName().str().empty()) {
    nameSuffix = " <arg " + std::to_string(aidx+1) + ">";
  }
  
  if (aidx >= tgt->fun->arg_size()  || !cprotect.isCalleeSafe(tgt->fun, aidx, false)) {
    // passing an unprotected argument to a function parameter that is not callee-safe, this is always an error
    

    msg.info(MSG_PFX + "calling allocating function " + funName(tgt) + " with a fresh pointer (" + varName(var) + nameSuffix + ")", in);
    refinableInfos++;
  }
  
  // the function the argument is passed to is callee-safe for the respective parameter
  // a warning has to be reported if the value of this argument were to be used again
  
  Instruction *callIn = cs.getInstruction();
  myassert(callIn == li->user_back());
  
  std::string message = "allocating function " + funName(tgt) + " may destroy its unprotected argument ("
    + varName(var) + nameSuffix + "), which is later used.";

  issueConditionalMessage(in, var, freshVars, msg, refinableInfos, liveVars, message);
}

static void unfreshAliasedVars(Instruction *useInst, AllocaInst *useVar, FreshVarsTy& freshVars, LineMessenger& msg) {

  AllocaInst* origVar = NULL;
  if (aliasesVariable(useInst, useVar, origVar)) {
    freshVars.vars.erase(origVar);
    if (msg.debug()) msg.debug(MSG_PFX + "variable " + varName(origVar) + " indirectly saved to node stack and thus assumed not fresh", useInst);    

    // there may be multiple levels of aliases
    //   this is indeed a heuristic, the definingStore may not pre-dominate the use
    //   FIXME: this is quite rough
    
    bool checkAliases = true;
    
    while(checkAliases) {
      checkAliases = false;
      
      StoreInst *definingStore = NULL;
      if (findOnlyStoreTo(origVar, definingStore)) {
        if (LoadInst *li = dyn_cast<LoadInst>(definingStore->getValueOperand())) {
          if (AllocaInst *v = dyn_cast<AllocaInst>(li->getPointerOperand())) {
            freshVars.vars.erase(v);
            if (msg.debug()) msg.debug(MSG_PFX + "variable " + varName(v) + " indirectly saved to node stack and thus assumed not fresh", useInst);
            checkAliases = true;
            origVar = v;
          }
        }
      }
    }
  }
}

static void handleStore(Instruction *in, CalledModuleTy *cm, SEXPGuardsChecker *sexpGuardsChecker, SEXPGuardsTy *sexpGuards, 
  FreshVarsTy& freshVars, LineMessenger& msg, unsigned& refinableInfos, BalanceStateTy* balance, VarBoolCacheTy& checkedVarsCache) {
  
  if (QUIET_WHEN_CONFUSED && freshVars.confused) {
    return;
  }
  
  AllocaInst* protectedVar = NULL;
  if (isStoreToStructureElement(in, "struct.R_bcstack_t", "union.ieee_double", protectedVar)) { // this indicates store to the node stack
    freshVars.vars.erase(protectedVar);
    if (msg.debug()) msg.debug(MSG_PFX + "variable " + varName(protectedVar) + " saved to node stack and thus assumed not fresh", in);

      // there may be multiple layers of aliases due to macros that do a local copy of its argument, to avoid repeated evaluation
    unfreshAliasedVars(in, protectedVar, freshVars, msg);
    return ;
  }
  
  
  if (!StoreInst::classof(in)) {
    return;
  }
  Value* storePointerOp = cast<StoreInst>(in)->getPointerOperand();
  Value* storeValueOp = cast<StoreInst>(in)->getValueOperand();

  if (storePointerOp == cm->getGlobals()->ppStackTopVariable) {
    // R_PPStackTop = var
    
    if (LoadInst *li = dyn_cast<LoadInst>(storeValueOp)) {
      if (AllocaInst *saveVar = dyn_cast<AllocaInst>(li->getPointerOperand())) {
        if (balance && balance->topSaveVar == saveVar) {
          // handle restore of pointer protection stack top
      
          int newDepth = balance->savedDepth;
          myassert(newDepth >= 0);
      
          int curDepth = freshVars.pstack.size();
          if (newDepth > curDepth) {
            msg.info(MSG_PFX + "attempt to restore protection stack to higher depth than it has now, " + CONFUSION_DISCLAIMER, in);
            if (QUIET_WHEN_CONFUSED) freshVars.confused = true;
            return;
          }
          if (newDepth == curDepth) {
            if (msg.debug()) msg.debug(MSG_PFX + "restoring protection stack to the depth it has now (doing nothing)", in);
            return;
          }
      
          while((int)freshVars.pstack.size() != newDepth) {
            unprotectOne(freshVars, msg, refinableInfos, in);
          }
          return;
        }
      }
    }
  
    msg.info(MSG_PFX + "manipulates PPStackTop directly, " + CONFUSION_DISCLAIMER, in);
    if (QUIET_WHEN_CONFUSED) freshVars.confused = true;
    return;
  }
  if (!AllocaInst::classof(storePointerOp)) {
    return;
  }
  AllocaInst *var = cast<AllocaInst>(storePointerOp);
  if (!isVarCheckedFresh(var, checkedVarsCache, msg)) {
    return;
  }
  
  // a variable is being killed by the store, erase conditional messages if any
  if (freshVars.condMsgs.erase(var)) {
    if (msg.debug()) msg.debug(MSG_PFX + "removed conditional messages as variable " + varName(var) + " is rewritten.", in);
  }
  
  const CalledFunctionTy *srcFun = cm->getCalledFunction(storeValueOp, sexpGuardsChecker, sexpGuards, false);
  if (srcFun) { 
    // only allowing single use, the other use can be and often is PROTECT
    
    Function* sf = srcFun->fun;
    if (sf->getName() == "Rf_protect" || sf->getName() == "Rf_protectWithIndex" || sf->getName() == "Rf_Reprotect") {
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
          if (otherFun->getName() == "Rf_protect" || otherFun->getName() == "Rf_protectWithIndex" || otherFun->getName() == "R_Reprotect") {
            // this case is handled in handleCall
            return;
          }
            // handle setter calls with indirect loads, e.g.
            // SET_VECTOR_ELT(ans, 2, cosines = allocVector(REALSXP, shiftlen));
    
          if (cs.arg_size() > 1 && isSetterFunction(otherFun)) {
            if (LoadInst* firstArgLoad = dyn_cast<LoadInst>(cs.getArgument(0))) {
              if (AllocaInst* firstArg = dyn_cast<AllocaInst>(firstArgLoad->getPointerOperand())) {
            
                auto vsearch = freshVars.vars.find(firstArg);
                if (vsearch == freshVars.vars.end() || (vsearch->second > 0)) {
                  // first argument of the setter is not fresh

                  Value *protArg = cs.getArgument(1); // the argument being implicitly protected
                  if (protArg != storeValueOp) {
                    if (msg.debug()) msg.debug(MSG_PFX + "indirect protect using setter call for variable " + varName(var), in);
                    freshVars.vars.erase(var);
                    return ; // variable is not fresh as it is implicitly protected
                  }
                }
              }
            }
          }
        }
      }
      
      int nProtects = 0;
      auto vsearch = freshVars.vars.find(var);
      if (vsearch == freshVars.vars.end()) {
        freshVars.vars.insert({var, nProtects});
        // remember, insert won't overwrite std::map value for an existing key
      } else {
        vsearch->second = nProtects;
      }
      if (msg.debug()) msg.debug(MSG_PFX + "initialized fresh SEXP variable " + varName(var) + " with protect count " + std::to_string(nProtects) +
        " allocated by " + funName(srcFun), in);
      return;
    }
  }
  if (LoadInst *dli = dyn_cast<LoadInst>(storeValueOp)) {
    if (GetElementPtrInst *dgep  = dyn_cast<GetElementPtrInst>(dli->getPointerOperand())) {
      if (dgep->isInBounds()) {
        if (LoadInst *dlis = dyn_cast<LoadInst>(dgep->getOperand(0))) {
          if (AllocaInst *dvars = dyn_cast<AllocaInst>(dlis->getPointerOperand())) {
            if (isVarCheckedFresh(dvars, checkedVarsCache, msg)) {
              auto vssearch = freshVars.vars.find(dvars);
              if (vssearch != freshVars.vars.end() && vssearch->second == 0) {
                // handle var = ATTRIB(var1) where var1 is fresh
                // NOTE: this is only an approximation and can cause false alarms
                //   if var1 later is protected, var still will be deemed fresh
                int nProtects = 0;
                auto vsearch = freshVars.vars.find(var);
                if (vsearch == freshVars.vars.end()) {
                  freshVars.vars.insert({var, nProtects});
                  // remember, insert won't overwrite std::map value for an existing key
                } else {
                  vsearch->second = nProtects;
                }
                if (msg.debug()) msg.debug(MSG_PFX + "initialized fresh SEXP variable " + varName(var) + " with protect count " + std::to_string(nProtects) +
                  " based on derived assignment from fresh variable " + varName(dvars), in);
                return;
              }
            }
          }
        }
      }
    }
  }
  
  // the store turns a variable into non-fresh  
  if (freshVars.vars.find(var) != freshVars.vars.end()) {
    freshVars.vars.erase(var);
    if (msg.debug()) msg.debug(MSG_PFX + "fresh variable " + varName(var) + " rewritten and thus no longer fresh", in);
  }
}

void handleFreshVarsForNonTerminator(Instruction *in, CalledModuleTy *cm, SEXPGuardsChecker *sexpGuardsChecker, SEXPGuardsTy *sexpGuards,
    FreshVarsTy& freshVars, LineMessenger& msg, unsigned& refinableInfos, LiveVarsTy& liveVars, CProtectInfo& cprotect, BalanceStateTy* balance, VarBoolCacheTy& checkedVarsCache) {

  handleCall(in, cm, sexpGuardsChecker, sexpGuards, freshVars, msg, refinableInfos, liveVars, cprotect, balance, checkedVarsCache);
  handleLoad(in, cm, sexpGuardsChecker, sexpGuards, freshVars, msg, refinableInfos, liveVars, cprotect);
  handleStore(in, cm, sexpGuardsChecker, sexpGuards, freshVars, msg, refinableInfos, balance, checkedVarsCache);
}

void handleFreshVarsForTerminator(Instruction *in, FreshVarsTy& freshVars, LiveVarsTy& liveVars) {
}

void StateWithFreshVarsTy::dump(bool verbose) {

  errs() << "=== fresh vars: " << &freshVars << " confused: " << freshVars.confused << "\n";
  for(FreshVarsVarsTy::iterator fi = freshVars.vars.begin(), fe = freshVars.vars.end(); fi != fe; ++fi) {
    AllocaInst *var = fi->first;
    errs() << "   " << varName(var);
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
