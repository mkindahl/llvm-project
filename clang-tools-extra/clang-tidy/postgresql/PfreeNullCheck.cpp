//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PfreeNullCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang::ast_matchers;

namespace clang::tidy::postgresql {

namespace {

// Find the enclosing CompoundStmt for a given statement.
const CompoundStmt *findEnclosingCompound(const Stmt *S, ASTContext &Ctx) {
  for (const auto &Parent : Ctx.getParents(*S)) {
    if (const auto *CS = Parent.get<CompoundStmt>())
      return CS;
    if (const auto *PS = Parent.get<Stmt>())
      return findEnclosingCompound(PS, Ctx);
  }
  return nullptr;
}

// Find the enclosing CompoundStmt that contains the declaration of Var.
const CompoundStmt *findCompoundContainingDecl(const Stmt *S,
                                                const VarDecl *Var,
                                                ASTContext &Ctx) {
  const CompoundStmt *CS = findEnclosingCompound(S, Ctx);
  while (CS) {
    for (const Stmt *Child : CS->body()) {
      if (const auto *DS = dyn_cast<DeclStmt>(Child)) {
        for (const Decl *D : DS->decls()) {
          if (const auto *VD = dyn_cast<VarDecl>(D))
            if (VD->getCanonicalDecl() == Var->getCanonicalDecl())
              return CS;
        }
      }
    }
    CS = findEnclosingCompound(CS, Ctx);
  }
  return nullptr;
}

// Check if a statement is an IfStmt that guards a pfree call with a NULL
// check on the given variable. Matches patterns like:
//   if (ptr) pfree(ptr);
//   if (ptr != NULL) pfree(ptr);
bool isNullGuardedPfree(const Stmt *S, const VarDecl *Var,
                        const CallExpr *TargetCall) {
  const auto *If = dyn_cast<IfStmt>(S);
  if (!If)
    return false;

  // Check if the then-branch (or its compound body) contains the target call.
  const Stmt *Then = If->getThen();
  if (!Then)
    return false;

  bool ContainsCall = false;
  if (Then == TargetCall) {
    ContainsCall = true;
  } else if (const auto *CS = dyn_cast<CompoundStmt>(Then)) {
    for (const Stmt *Child : CS->body()) {
      if (Child == TargetCall) {
        ContainsCall = true;
        break;
      }
    }
  }
  if (!ContainsCall)
    return false;

  // Check if the condition is a NULL check on the variable.
  const Expr *Cond = If->getCond();
  if (!Cond)
    return false;
  Cond = Cond->IgnoreParenImpCasts();

  // Match: if (ptr)
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Cond)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return VD->getCanonicalDecl() == Var->getCanonicalDecl();
  }

  // Match: if (ptr != NULL) or if (ptr != 0)
  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->getOpcode() == BO_NE) {
      const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
      const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(LHS);
      if (!DRE)
        DRE = dyn_cast<DeclRefExpr>(RHS);
      if (DRE) {
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
          return VD->getCanonicalDecl() == Var->getCanonicalDecl();
      }
    }
  }

  return false;
}

// Recursively check whether any sub-statement modifies the given variable
// (via assignment or by taking its address).
bool containsModificationOf(const Stmt *S, const VarDecl *Var) {
  if (!S)
    return false;
  if (const auto *BO = dyn_cast<BinaryOperator>(S)) {
    if (BO->isAssignmentOp()) {
      if (const auto *DRE =
              dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts())) {
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
          if (VD->getCanonicalDecl() == Var->getCanonicalDecl())
            return true;
      }
    }
  }
  // Check if the variable's address is taken (&var), which means a called
  // function could modify it.
  if (const auto *UO = dyn_cast<UnaryOperator>(S)) {
    if (UO->getOpcode() == UO_AddrOf) {
      if (const auto *DRE =
              dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts())) {
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
          if (VD->getCanonicalDecl() == Var->getCanonicalDecl())
            return true;
      }
    }
  }
  for (const Stmt *Child : S->children())
    if (containsModificationOf(Child, Var))
      return true;
  return false;
}

// Walk backward through the enclosing compound to find the most recent
// assignment to the variable. Returns true if the variable is definitely
// NULL at the point of the pfree call.
bool isDefinitelyNull(const VarDecl *Var, const CallExpr *Call,
                      ASTContext &Ctx) {
  const CompoundStmt *Body = findCompoundContainingDecl(Call, Var, Ctx);
  if (!Body)
    return false;

  const SourceManager &SM = Ctx.getSourceManager();
  SourceLocation CallLoc = Call->getBeginLoc();

  // Track the last known assignment value.
  // 0 = unknown, 1 = null, 2 = non-null/other
  int LastAssign = 0;

  // Check the initializer first.
  if (const Expr *Init = Var->getInit()) {
    if (Init->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull))
      LastAssign = 1;
    else
      LastAssign = 2;
  }

  for (const Stmt *S : Body->body()) {
    if (!SM.isBeforeInTranslationUnit(S->getBeginLoc(), CallLoc))
      break;

    // Look for top-level assignments to the variable: var = expr;
    bool HandledAsDirectAssign = false;
    const BinaryOperator *BO = nullptr;
    if (const auto *E = dyn_cast<Expr>(S))
      BO = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
    if (BO && BO->isAssignmentOp()) {
      if (const auto *DRE =
              dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts())) {
        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
          if (VD->getCanonicalDecl() == Var->getCanonicalDecl()) {
            HandledAsDirectAssign = true;
            const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
            if (RHS->isNullPointerConstant(Ctx,
                                           Expr::NPC_ValueDependentIsNotNull))
              LastAssign = 1;
            else
              LastAssign = 2;
          }
        }
      }
    }

    // DeclStmt with initializer: type *p = NULL;
    if (const auto *DS = dyn_cast<DeclStmt>(S)) {
      for (const Decl *D : DS->decls()) {
        if (const auto *VD = dyn_cast<VarDecl>(D)) {
          if (VD->getCanonicalDecl() == Var->getCanonicalDecl()) {
            HandledAsDirectAssign = true;
            if (const Expr *Init = VD->getInit()) {
              if (Init->isNullPointerConstant(
                      Ctx, Expr::NPC_ValueDependentIsNotNull))
                LastAssign = 1;
              else
                LastAssign = 2;
            }
          }
        }
      }
    }

    // For any statement not handled as a direct assignment, check if it
    // modifies the variable (via nested assignment, address-of, etc.).
    // If so, reset to unknown since we can't track the value.
    if (!HandledAsDirectAssign && containsModificationOf(S, Var))
      LastAssign = 0;
  }

  return LastAssign == 1;
}

} // namespace

PfreeNullCheck::PfreeNullCheck(StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      PfreeFunction(Options.get("PfreeFunction", "pfree").str()) {}

void PfreeNullCheck::storeOptions(ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "PfreeFunction", PfreeFunction);
}

void PfreeNullCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      callExpr(callee(functionDecl(hasName(PfreeFunction)))).bind("call"),
      this);
}

void PfreeNullCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  if (!Call || Call->getNumArgs() < 1)
    return;

  ASTContext &Ctx = *Result.Context;
  const Expr *Arg = Call->getArg(0);
  const Expr *Inner = Arg->IgnoreParenImpCasts();

  // Check 1: Direct NULL literal.
  if (Inner->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull)) {
    diag(Call->getBeginLoc(),
         "calling 'pfree' with a NULL argument; unlike free(), pfree() does "
         "not accept NULL");
    return;
  }

  // Check 2: Conditional expression with a NULL branch.
  if (const auto *CO = dyn_cast<ConditionalOperator>(Inner)) {
    bool TrueNull = CO->getTrueExpr()->IgnoreParenImpCasts()
                        ->isNullPointerConstant(
                            Ctx, Expr::NPC_ValueDependentIsNotNull);
    bool FalseNull = CO->getFalseExpr()->IgnoreParenImpCasts()
                         ->isNullPointerConstant(
                             Ctx, Expr::NPC_ValueDependentIsNotNull);
    if (TrueNull || FalseNull) {
      diag(Call->getBeginLoc(),
           "calling 'pfree' with a potentially NULL argument; unlike free(), "
           "pfree() does not accept NULL");
      return;
    }
  }

  // Check 3: Variable that is definitely NULL (assigned NULL, never reassigned).
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Inner)) {
    const auto *Var = dyn_cast<VarDecl>(DRE->getDecl());
    if (!Var || !Var->isLocalVarDecl())
      return;

    // Check if this pfree call is inside a NULL guard.
    const CompoundStmt *Body = findEnclosingCompound(Call, Ctx);
    if (Body) {
      for (const Stmt *S : Body->body()) {
        if (isNullGuardedPfree(S, Var, Call))
          return;
      }
    }

    // Check if the enclosing statement is itself an if-guard.
    // This handles: if (ptr) pfree(ptr); where pfree is directly the then-body.
    for (const auto &Parent : Ctx.getParents(*Call)) {
      if (const auto *If = Parent.get<IfStmt>()) {
        if (isNullGuardedPfree(If, Var, Call))
          return;
      }
    }

    if (isDefinitelyNull(Var, Call, Ctx)) {
      diag(Call->getBeginLoc(),
           "calling 'pfree' with a NULL argument; unlike free(), pfree() does "
           "not accept NULL");
    }
  }
}

} // namespace clang::tidy::postgresql
