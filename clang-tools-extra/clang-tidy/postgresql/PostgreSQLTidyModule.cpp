//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../ClangTidy.h"
#include "../ClangTidyModule.h"
#include "HashCreateFlagsCheck.h"
#include "PfreeNullCheck.h"

namespace clang::tidy {
namespace postgresql {
namespace {

class PostgreSQLModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<HashCreateFlagsCheck>(
        "postgresql-hash-create-flags");
    CheckFactories.registerCheck<PfreeNullCheck>("postgresql-pfree-null");
  }
};

} // namespace
} // namespace postgresql

// Register the PostgreSQLTidyModule using this statically initialized variable.
static ClangTidyModuleRegistry::Add<postgresql::PostgreSQLModule>
    X("postgresql-module", "Adds PostgreSQL-specific lint checks.");

// This anchor is used to force the linker to link in the generated object file
// and thus register the PostgreSQLModule.
volatile int PostgreSQLModuleAnchorSource = 0; // NOLINT(misc-use-internal-linkage)

} // namespace clang::tidy
