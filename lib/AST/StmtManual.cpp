/*
 * Copyright (c) 2022 Trail of Bits, Inc.
 */

#include <pasta/AST/StmtManual.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#include <clang/AST/Expr.h>
#pragma clang diagnostic pop

#include "AST.h"
#include "Builder.h"

namespace pasta {

#ifndef PASTA_IN_BOOTSTRAP

// Is this a field designator?
bool Designator::IsFieldDesignator(void) const noexcept {
  // Cast the void pointers to `clang::DesignatedInitExpr::Designator`
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  return design->isFieldDesignator();
}

// Is this an array designator?
bool Designator::IsArrayDesignator(void) const noexcept {
  // Cast the void pointers to `clang::DesignatedInitExpr::Designator`
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  return design->isArrayDesignator();
}

// Is this an array range designator?
bool Designator::IsArrayRangeDesignator(void) const noexcept {
  // Cast the void pointers to `clang::DesignatedInitExpr::Designator`
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  return design->isArrayRangeDesignator();
}

// Returns the FieldDecl for the designator if it is field designator
std::optional<::pasta::FieldDecl> Designator::Field(void) const noexcept {

  // Cast the void pointers to `clang::DesignatedInitExpr::Designator`
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  if (!design->isFieldDesignator()) {
    return std::nullopt;
  }
  return DeclBuilder::Create<pasta::FieldDecl>(ast, design->getField());
}

// Returns the TokenRange for the designator.
::pasta::TokenRange Designator::Tokens(void) const noexcept {
  // Cast the void pointers to `clang::DesignatedInitExpr::Designator`
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  return ast->TokenRangeFrom(design->getSourceRange());
}

// Get the token for dot location
::pasta::Token Designator::DotToken(void) const noexcept {
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  if (!design->isFieldDesignator()) {
    // If this is not field designator; it will return empty token
    return ast->TokenAt(clang::SourceLocation());
  }
  return ast->TokenAt(design->getDotLoc());
}

// Get the token for field location
::pasta::Token Designator::FieldToken(void) const noexcept {
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  if (!design->isFieldDesignator()) {
    // If this is not a field designator; it will return empty token
    return ast->TokenAt(clang::SourceLocation());
  }
  return ast->TokenAt(design->getFieldLoc());
}

// Get the token for l-bracket location
::pasta::Token Designator::LeftBracketToken(void) const noexcept {
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  if (!(design->isArrayDesignator() || design->isArrayRangeDesignator())) {
    // If this is field designator it will return empty token
    return ast->TokenAt(clang::SourceLocation());
  }
  return ast->TokenAt(design->getLBracketLoc());
}

// Get the token for r-bracket location
::pasta::Token Designator::RightBracketToken(void) const noexcept {
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  if (!(design->isArrayDesignator() || design->isArrayRangeDesignator())) {
    // if the designator is of field type and has no right braces, it will return empty token
    return ast->TokenAt(clang::SourceLocation());
  }
  return ast->TokenAt(design->getRBracketLoc());
}

// Get the token for ellipsis location
::pasta::Token Designator::EllipsisToken(void) const noexcept {
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  if (!design->isArrayRangeDesignator()) {
    // if the designator is not an array range; it will not have ellipsis. Return empty token
    return ast->TokenAt(clang::SourceLocation());
  }
  return ast->TokenAt(design->getEllipsisLoc());
}

// Get the index for first designator expression. It will be only valid for
std::optional<unsigned> Designator::FirstExpressionIndex(void) const noexcept {
  auto design = reinterpret_cast<const clang::DesignatedInitExpr::Designator *>(spec);
  assert(design != nullptr);
  if (!(design->isArrayDesignator() || design->isArrayRangeDesignator())) {
    return std::nullopt;
  }
  return design->getFirstExprIndex();
}

#endif  // PASTA_IN_BOOTSTRAP

}  // namespace pasta
