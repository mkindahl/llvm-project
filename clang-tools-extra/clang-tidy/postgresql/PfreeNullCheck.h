//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_POSTGRESQL_PFREENULLCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_POSTGRESQL_PFREENULLCHECK_H

#include "../ClangTidyCheck.h"

namespace clang::tidy::postgresql {

/// Warns when pfree() is called with a NULL or potentially NULL argument.
/// Unlike free(), pfree() does not accept NULL pointers.
///
/// For the user-facing documentation see:
/// https://clang.llvm.org/extra/clang-tidy/checks/postgresql/pfree-null.html
class PfreeNullCheck : public ClangTidyCheck {
public:
  PfreeNullCheck(StringRef Name, ClangTidyContext *Context);
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
  void storeOptions(ClangTidyOptions::OptionMap &Opts) override;

private:
  const std::string PfreeFunction;
};

} // namespace clang::tidy::postgresql

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_POSTGRESQL_PFREENULLCHECK_H
