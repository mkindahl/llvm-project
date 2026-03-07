//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "HashCreateFlagsCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/StringSet.h"

using namespace clang::ast_matchers;

namespace clang::tidy::postgresql {

namespace {

struct FieldFlagMapping {
  StringRef FieldName;
  StringRef FlagName;
};

// Mapping from HASHCTL field names to their required flag macro names.
const FieldFlagMapping FieldToFlag[] = {
    {"num_partitions", "HASH_PARTITION"},
    {"ssize", "HASH_SEGMENT"},
    {"dsize", "HASH_DIRSIZE"},
    {"max_dsize", "HASH_DIRSIZE"},
    {"keysize", "HASH_ELEM"},
    {"entrysize", "HASH_ELEM"},
    {"hash", "HASH_FUNCTION"},
    {"match", "HASH_COMPARE"},
    {"keycopy", "HASH_KEYCOPY"},
    {"alloc", "HASH_ALLOC"},
    {"hcxt", "HASH_CONTEXT"},
    {"hctl", "HASH_SHARED_MEM"},
};

struct FlagFieldMapping {
  StringRef FlagName;
  SmallVector<StringRef, 2> FieldNames;
};

// Mapping from flag macro names to the fields they require.
const FlagFieldMapping FlagToFields[] = {
    {"HASH_PARTITION", {"num_partitions"}},
    {"HASH_SEGMENT", {"ssize"}},
    {"HASH_DIRSIZE", {"dsize", "max_dsize"}},
    {"HASH_ELEM", {"keysize", "entrysize"}},
    {"HASH_FUNCTION", {"hash"}},
    {"HASH_COMPARE", {"match"}},
    {"HASH_KEYCOPY", {"keycopy"}},
    {"HASH_ALLOC", {"alloc"}},
    {"HASH_CONTEXT", {"hcxt"}},
    {"HASH_SHARED_MEM", {"hctl"}},
};

struct MutuallyExclusivePair {
  StringRef Name1;
  StringRef Name2;
};

const MutuallyExclusivePair ExclusivePairs[] = {
    {"HASH_STRINGS", "HASH_BLOBS"},
    {"HASH_STRINGS", "HASH_FUNCTION"},
    {"HASH_BLOBS", "HASH_FUNCTION"},
};

// Recursively walk a bitwise-OR expression tree and collect the macro names
// of each leaf operand. E.g. for `HASH_ELEM | HASH_BLOBS | HASH_CONTEXT`,
// collects {"HASH_ELEM", "HASH_BLOBS", "HASH_CONTEXT"}.
void collectFlagMacroNames(const Expr *E, const SourceManager &SM,
                           const LangOptions &LO,
                           llvm::StringSet<> &FlagNames) {
  E = E->IgnoreParenImpCasts();

  // Recurse through bitwise-OR.
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_Or) {
      collectFlagMacroNames(BO->getLHS(), SM, LO, FlagNames);
      collectFlagMacroNames(BO->getRHS(), SM, LO, FlagNames);
      return;
    }
  }

  // Leaf node — check if it comes from a macro expansion.
  SourceLocation Loc = E->getBeginLoc();
  if (Loc.isMacroID()) {
    StringRef MacroName = Lexer::getImmediateMacroName(Loc, SM, LO);
    if (MacroName.starts_with("HASH_"))
      FlagNames.insert(MacroName);
  }
}

// Resolve the VarDecl from a hash_create argument (handles &ctl and ctl).
const VarDecl *resolveVarDecl(const Expr *Arg) {
  Arg = Arg->IgnoreParenImpCasts();
  if (const auto *UO = dyn_cast<UnaryOperator>(Arg)) {
    if (UO->getOpcode() == UO_AddrOf)
      Arg = UO->getSubExpr()->IgnoreParenImpCasts();
  }
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Arg))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}

// Collect field names from a VarDecl's initializer (e.g. designated
// initializers like `HASHCTL ctl = { .keysize = 4, .entrysize = 8 }`).
void collectFieldsFromInit(const VarDecl *Var, llvm::StringSet<> &Fields) {
  const auto *Init = Var->getInit();
  if (!Init)
    return;
  const auto *ILE = dyn_cast<InitListExpr>(Init->IgnoreParenImpCasts());
  if (!ILE)
    return;
  // Use the semantic form which has fields in declaration order.
  if (const auto *Semantic = ILE->getSemanticForm())
    ILE = Semantic;
  const auto *RD = dyn_cast_or_null<RecordDecl>(ILE->getType()->getAsTagDecl());
  if (!RD)
    return;
  unsigned Idx = 0;
  for (const auto *FD : RD->fields()) {
    if (Idx >= ILE->getNumInits())
      break;
    const Expr *InitExpr = ILE->getInit(Idx);
    // ImplicitValueInitExpr means the field was not explicitly set.
    if (!isa<ImplicitValueInitExpr>(InitExpr))
      Fields.insert(FD->getName());
    ++Idx;
  }
}

// Check if a CallExpr passes the given VarDecl as its 3rd argument (the
// HASHCTL pointer), meaning it's a hash_create call using this variable.
bool callPassesVar(const CallExpr *CE, const VarDecl *Var) {
  if (CE->getNumArgs() < 4)
    return false;
  const VarDecl *ArgVar = resolveVarDecl(CE->getArg(2));
  return ArgVar &&
         ArgVar->getCanonicalDecl() == Var->getCanonicalDecl();
}

// Collect field names assigned on a given VarDecl within a CompoundStmt,
// only considering assignments between the previous hash_create call (if any)
// that uses the same variable and the target call itself.
llvm::StringSet<> collectAssignedFields(const CompoundStmt *Body,
                                        const VarDecl *Var,
                                        const CallExpr *TargetCall) {
  llvm::StringSet<> Fields;
  collectFieldsFromInit(Var, Fields);
  SourceLocation TargetLoc = TargetCall->getBeginLoc();
  const SourceManager &SM = Var->getASTContext().getSourceManager();
  for (const Stmt *S : Body->body()) {
    // Stop when we reach or pass the target hash_create call.
    if (!SM.isBeforeInTranslationUnit(S->getBeginLoc(), TargetLoc))
      break;

    // If this is a prior hash_create call using the same variable, reset
    // accumulated fields — those were for the earlier call.
    // Match both bare calls and calls wrapped in assignments.
    const CallExpr *CE = dyn_cast<CallExpr>(S);
    if (!CE) {
      if (const auto *BO = dyn_cast<BinaryOperator>(S))
        if (BO->isAssignmentOp())
          CE = dyn_cast<CallExpr>(BO->getRHS()->IgnoreParenImpCasts());
    }
    if (CE && CE != TargetCall && callPassesVar(CE, Var))
      Fields.clear();

    // Look for member assignments: ctl.field = ...
    const BinaryOperator *BO = nullptr;
    if (const auto *ES = dyn_cast<ExprWithCleanups>(S))
      S = ES->getSubExpr();
    if (const auto *E = dyn_cast<Expr>(S))
      BO = dyn_cast<BinaryOperator>(E->IgnoreParenImpCasts());
    if (!BO || !BO->isAssignmentOp())
      continue;

    const auto *ME = dyn_cast<MemberExpr>(BO->getLHS()->IgnoreParenImpCasts());
    if (!ME)
      continue;

    const auto *Base =
        dyn_cast<DeclRefExpr>(ME->getBase()->IgnoreParenImpCasts());
    if (!Base)
      continue;

    if (Base->getDecl()->getCanonicalDecl() != Var->getCanonicalDecl())
      continue;

    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl()))
      Fields.insert(FD->getName());
  }
  return Fields;
}

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

} // namespace

HashCreateFlagsCheck::HashCreateFlagsCheck(StringRef Name,
                                           ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      HashCreateFunction(
          Options.get("HashCreateFunction", "hash_create").str()) {}

void HashCreateFlagsCheck::storeOptions(ClangTidyOptions::OptionMap &Opts) {
  Options.store(Opts, "HashCreateFunction", HashCreateFunction);
}

void HashCreateFlagsCheck::registerMatchers(MatchFinder *Finder) {
  Finder->addMatcher(
      callExpr(callee(functionDecl(hasName(HashCreateFunction))))
          .bind("call"),
      this);
}

void HashCreateFlagsCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("call");
  if (!Call || Call->getNumArgs() < 4)
    return;

  ASTContext &Ctx = *Result.Context;
  const SourceManager &SM = Ctx.getSourceManager();
  const LangOptions &LO = Ctx.getLangOpts();

  // 3rd arg (index 2) is the HASHCTL pointer, 4th arg (index 3) is flags.
  const Expr *CtlArg = Call->getArg(2);
  const Expr *FlagsArg = Call->getArg(3);

  // Symbolically collect HASH_* macro names from the flags expression.
  llvm::StringSet<> FlagNames;
  collectFlagMacroNames(FlagsArg, SM, LO, FlagNames);

  // If we couldn't extract any flag names, skip (e.g. flags passed as a
  // variable rather than literal OR of macros).
  if (FlagNames.empty())
    return;

  // Resolve the HASHCTL variable.
  const VarDecl *CtlVar = resolveVarDecl(CtlArg);
  if (!CtlVar)
    return;

  // Find the enclosing compound statement.
  const CompoundStmt *Body = findEnclosingCompound(Call, Ctx);
  if (!Body)
    return;

  // Collect all fields assigned on this variable.
  llvm::StringSet<> AssignedFields = collectAssignedFields(Body, CtlVar, Call);

  // Check 1: HASH_ELEM is always required.
  if (!FlagNames.count("HASH_ELEM")) {
    diag(Call->getBeginLoc(), "HASH_ELEM flag is required for hash_create");
  }

  // Check 2: Mutually exclusive flags.
  for (const auto &Pair : ExclusivePairs) {
    if (FlagNames.count(Pair.Name1) && FlagNames.count(Pair.Name2)) {
      diag(FlagsArg->getBeginLoc(),
           "mutually exclusive flags '%0' and '%1' are both passed to "
           "hash_create")
          << Pair.Name1 << Pair.Name2;
    }
  }

  // Check 3: Field assigned without corresponding flag.
  for (const auto &Mapping : FieldToFlag) {
    if (AssignedFields.count(Mapping.FieldName) &&
        !FlagNames.count(Mapping.FlagName)) {
      diag(Call->getBeginLoc(),
           "HASHCTL field '%0' is set but corresponding flag '%1' is not "
           "passed to hash_create")
          << Mapping.FieldName << Mapping.FlagName;
    }
  }

  // Check 4: Flag set without corresponding field assigned.
  for (const auto &Mapping : FlagToFields) {
    if (!FlagNames.count(Mapping.FlagName))
      continue;

    bool AnyFieldSet = false;
    for (StringRef Field : Mapping.FieldNames) {
      if (AssignedFields.count(Field)) {
        AnyFieldSet = true;
        break;
      }
    }

    if (!AnyFieldSet) {
      std::string FieldList;
      for (size_t I = 0; I < Mapping.FieldNames.size(); ++I) {
        if (I > 0)
          FieldList += ", ";
        FieldList += Mapping.FieldNames[I];
      }
      diag(FlagsArg->getBeginLoc(),
           "flag '%0' is passed to hash_create but corresponding HASHCTL "
           "field '%1' is not set")
          << Mapping.FlagName << FieldList;
    }
  }
}

} // namespace clang::tidy::postgresql
