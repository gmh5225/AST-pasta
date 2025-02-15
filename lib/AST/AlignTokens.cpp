/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 */

#include "AST.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclObjC.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Token.h>
#pragma clang diagnostic pop


#include <fstream>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>

#include "Builder.h"
#include "Printer/DeclStmtPrinter.h"
#include "Util.h"

#define PASTA_DEBUG_ALIGN 0
#define TK(...)

namespace pasta {
namespace {

static bool TokenHasLocationAndContext(const TokenImpl *impl) {
  return impl->opaque_source_loc != TokenImpl::kInvalidSourceLocation &&
         impl->context_index != kInvalidTokenContextIndex;
}

static bool TokenLocationsMatch(const TokenImpl *parsed,
                                const PrintedTokenImpl *printed) {
  if (parsed->opaque_source_loc == TokenImpl::kInvalidSourceLocation) {
    return false;
  } else if (printed->opaque_source_loc == TokenImpl::kInvalidSourceLocation) {
    return false;
  } else {
    return parsed->opaque_source_loc == printed->opaque_source_loc;
  }
}

static bool TokenCanBeAssignedContext(const TokenImpl *token) {
  switch (token->Kind()) {
    case clang::tok::unknown:
    case clang::tok::eod:
    case clang::tok::eof:
    case clang::tok::code_completion:
    case clang::tok::comment:
      return false;
    default:
      if (token->IsParsed()) {
        return token->data_len != 0;

      // Probably a printed token.
      } else {
        return token->Role() == TokenRole::kInvalid;
      }
  }
}

enum class RegionKind {
  kStatement,
  kSequence,
  kBalanced
};

struct Region {
 public:
  // Some context that is shared across all things in this region.
  std::optional<TokenContextIndex> common_context;

  // If we match this region with another, then we commit to that match and
  // record it here. If this is a parsed region, then this points at a printed
  // region. If it's a printed region, then this points at a parsed region.
  Region *matched_with{nullptr};

  virtual ~Region(void) = default;
  virtual RegionKind Kind(void) const noexcept = 0;

#if PASTA_DEBUG_ALIGN
  virtual void Print(std::ostream &os, std::string indent,
                     const ASTImpl &ast,
                     const PrintedTokenRangeImpl &range) const = 0;
  virtual void Print(std::ostream &os, std::string indent,
                     const PrintedTokenRangeImpl &range) const = 0;
#endif  // PASTA_DEBUG_ALIGN

  // Return the first token that has location and context information.
  virtual TokenImpl *FirstParsedToken(void) const = 0;
  virtual TokenImpl *LastParsedToken(void) const = 0;
  virtual TokenContextIndex CommonContext(
      const PrintedTokenRangeImpl &range,
      TokenContextIndex parent_context) = 0;
};

struct StatementRegion final : public Region {
  RegionKind Kind(void) const noexcept final {
    return RegionKind::kStatement;
  }

  TokenImpl *FirstParsedToken(void) const final {
    for (auto it = begin; it <= end; ++it) {
      if (TokenHasLocationAndContext(it) && TokenCanBeAssignedContext(it)) {
        return it;
      }
    }
    return nullptr;
  }

  TokenImpl *LastParsedToken(void) const final {
    for (auto it = end; it >= begin; --it) {
      if (TokenHasLocationAndContext(it) && TokenCanBeAssignedContext(it)) {
        return it;
      }
    }
    return nullptr;
  }

  TokenContextIndex CommonContext(const PrintedTokenRangeImpl &range,
                                  TokenContextIndex parent_context) final {
    if (common_context) {
      return common_context.value();
    }

    // Try to find the common ancestor of everything in this statement.
    const TokenContextImpl *prev = nullptr;
    for (auto tok = reinterpret_cast<PrintedTokenImpl *>(begin);
         tok <= end; ++tok) {
      if (TokenHasLocationAndContext(tok)) {
        if (tok->context_index < parent_context) {
          continue;
        }
        assert(TokenCanBeAssignedContext(tok));
        const TokenContextImpl *curr = &(range.contexts[tok->context_index]);
        if (!prev) {
          prev = curr;
        } else {
          prev = TokenContextImpl::CommonAncestor(prev, curr, range.contexts);
        }
      }
    }

    if (!prev) {
      common_context.emplace(parent_context);
      return parent_context;
    }

    // If we've got a common ancestor, then push its index, and double check
    // it against our parent. Otherwise, push our parent.
    auto index = static_cast<TokenContextIndex>(prev - range.contexts.data());
    assert(index < range.contexts.size());
    common_context.emplace(index);
    return index;
  }

#if PASTA_DEBUG_ALIGN
  void Print(std::ostream &os, std::string indent,
             const ASTImpl &ast, const PrintedTokenRangeImpl &range) const final {
    if (common_context.has_value()) {
      assert(common_context.value() < range.contexts.size());
    }
    os << indent << "------ST "
       << std::hex
       << (common_context ? common_context.value() : kInvalidTokenContextIndex)
       << std::dec << "------\n";
    for (TokenImpl *it = begin; it <= end; ++it) {
      if (it->Kind() == clang::tok::string_literal) {
        os << indent << "<str>";
      } else {
        os << indent << it->Data(ast);
      }
      TK( os << " " << clang::tok::getTokenName(it->Kind()); )
      if (it->opaque_source_loc != TokenImpl::kInvalidSourceLocation) {
        os << " l:" << std::hex << it->opaque_source_loc << std::dec;
      }
      if (it->context_index != kInvalidTokenContextIndex) {
        os << " c:" << std::hex << it->context_index << std::dec;
        const TokenContextImpl &context = range.contexts[it->context_index];
        if (context.kind == TokenContextKind::kDecl) {
          os << ' ' << reinterpret_cast<const clang::Decl *>(context.data)->getDeclKindName();

        } else if (context.kind == TokenContextKind::kStmt) {
          os << ' ' << reinterpret_cast<const clang::Stmt *>(context.data)->getStmtClassName();

        } else if (context.kind == TokenContextKind::kType) {
          os << ' ' << reinterpret_cast<const clang::Type *>(context.data)->getTypeClassName();
        }
      }
      os << '\n';
    }
  }

  void Print(std::ostream &os, std::string indent,
             const PrintedTokenRangeImpl &range) const final {
    if (common_context.has_value()) {
      assert(common_context.value() < range.contexts.size());
    }
    os << indent << "------ST "
       << std::hex
       << (common_context ? common_context.value() : kInvalidTokenContextIndex)
       << std::dec << "------\n";
    auto begin_ = reinterpret_cast<PrintedTokenImpl *>(begin);
    auto end_ = reinterpret_cast<PrintedTokenImpl *>(end);
    for (PrintedTokenImpl *it = begin_; it <= end_; ++it) {
      if (it->Kind() == clang::tok::string_literal) {
        os << indent << "<str>";
      } else {
        os << indent << it->Data(range);
      }
      TK( os << " " << clang::tok::getTokenName(it->Kind()); )
      if (it->opaque_source_loc != TokenImpl::kInvalidSourceLocation) {
        os << " l:" << std::hex << it->opaque_source_loc << std::dec;
      }
      if (it->context_index != kInvalidTokenContextIndex) {
        os << " c:" << std::hex << it->context_index << std::dec;

        const TokenContextImpl &context = range.contexts[it->context_index];
        if (context.kind == TokenContextKind::kDecl) {
          os << ' ' << reinterpret_cast<const clang::Decl *>(context.data)->getDeclKindName();

        } else if (context.kind == TokenContextKind::kStmt) {
          os << ' ' << reinterpret_cast<const clang::Stmt *>(context.data)->getStmtClassName();

        } else if (context.kind == TokenContextKind::kType) {
          os << ' ' << reinterpret_cast<const clang::Type *>(context.data)->getTypeClassName();
        }
      }
      os << '\n';
    }
  }
#endif  // PASTA_DEBUG_ALIGN

  TokenImpl *begin{nullptr};
  TokenImpl *end{nullptr};  // Inclusive.
};

struct SequenceRegion final : public Region {
  RegionKind Kind(void) const noexcept final {
    return RegionKind::kSequence;
  }

  TokenImpl *FirstParsedToken(void) const final {
    for (auto it = regions.rbegin(), end = regions.rend(); it != end; ++it) {
      Region *region = *it;
      if (TokenImpl *tok = region->FirstParsedToken()) {
        return tok;
      }
    }
    return nullptr;
  }

  TokenImpl *LastParsedToken(void) const final {
    for (Region *region : regions) {
      if (TokenImpl *tok = region->LastParsedToken()) {
        return tok;
      }
    }
    return nullptr;
  }

  TokenContextIndex CommonContext(const PrintedTokenRangeImpl &range,
                                  TokenContextIndex parent_context) final {
    if (common_context) {
      return common_context.value();
    }

    // Try to find the common ancestor of everything in this statement.
    const TokenContextImpl *prev = nullptr;
    for (Region *region : regions) {
      auto index = region->CommonContext(range, parent_context);
      if (index == kInvalidTokenContextIndex ||
          index < parent_context) {
        continue;
      }

      assert(index < range.contexts.size());
      const TokenContextImpl *curr = &(range.contexts[index]);
      if (!prev) {
        prev = curr;
      } else {
        prev = TokenContextImpl::CommonAncestor(prev, curr, range.contexts);
      }
    }

    if (!prev) {
      common_context.emplace(parent_context);
      return parent_context;
    }

    // If we've got a common ancestor, then push its index, and double check
    // it against our parent. Otherwise, push our parent.
    auto index = static_cast<TokenContextIndex>(prev - range.contexts.data());
    assert(index < range.contexts.size());
    common_context.emplace(index);
    return index;
  }

#if PASTA_DEBUG_ALIGN
  void Print(std::ostream &os, std::string indent,
             const ASTImpl &ast, const PrintedTokenRangeImpl &range) const final {
    if (common_context.has_value()) {
      assert(common_context.value() < range.contexts.size());
    }
    os << indent << "------SE "
       << std::hex
       << (common_context ? common_context.value() : kInvalidTokenContextIndex)
       << std::dec << "------\n";
    indent += "  ";
    for (auto it = regions.rbegin(), end = regions.rend(); it != end; ++it) {
      (*it)->Print(os, indent, ast, range);
    }
  }

  void Print(std::ostream &os, std::string indent,
             const PrintedTokenRangeImpl &range) const final {
    if (common_context.has_value()) {
      assert(common_context.value() < range.contexts.size());
    }
    os << indent << "------SE "
       << std::hex
       << (common_context ? common_context.value() : kInvalidTokenContextIndex)
       << std::dec << "------\n";
  indent += "  ";
    for (auto it = regions.rbegin(), end = regions.rend(); it != end; ++it) {
      (*it)->Print(os, indent, range);
    }
  }
#endif  // PASTA_DEBUG_ALIGN

  // NOTE(pag): These are in reverse order.
  std::vector<Region *> regions;
};

struct BalancedRegion final : public Region {

  StatementRegion *predecessor{nullptr};
  TokenImpl *leading_ident{nullptr};
  TokenImpl *begin{nullptr};
  SequenceRegion *statements{nullptr};
  TokenImpl *end{nullptr};

  RegionKind Kind(void) const noexcept final {
    return RegionKind::kBalanced;
  }

  TokenImpl *FirstParsedToken(void) const final {
    if (TokenHasLocationAndContext(begin)) {
      return begin;
    }
    if (statements) {
      return statements->FirstParsedToken();
    }
    if (TokenHasLocationAndContext(end)) {
      return end;
    }
    return nullptr;
  }

  TokenImpl *LastParsedToken(void) const final {
    if (TokenHasLocationAndContext(end)) {
      return end;
    }
    if (statements) {
      return statements->LastParsedToken();
    }
    if (TokenHasLocationAndContext(begin)) {
      return begin;
    }
    return nullptr;
  }

  TokenContextIndex CommonContext(const PrintedTokenRangeImpl &range,
                                  TokenContextIndex parent_context) final {
    if (common_context) {
      return common_context.value();
    }

    const TokenContextImpl *prev = nullptr;

    // Get it from the leading identifier.
    if (leading_ident) {
      auto tok = reinterpret_cast<PrintedTokenImpl *>(leading_ident);
      if (TokenHasLocationAndContext(tok)) {
        assert(tok->context_index < range.contexts.size());
        prev = &(range.contexts[tok->context_index]);
      }
    }

    // Get it from the brackets/braces/parens.
    if (!prev && begin && end) {
      auto tok = reinterpret_cast<PrintedTokenImpl *>(begin);
      if (TokenHasLocationAndContext(tok)) {
        prev = &(range.contexts[tok->context_index]);
      }

      tok = reinterpret_cast<PrintedTokenImpl *>(end);
      if (TokenHasLocationAndContext(tok)) {
        auto curr = &(range.contexts[tok->context_index]);
        if (!prev) {
          prev = curr;
        } else {
          prev = TokenContextImpl::CommonAncestor(prev, curr, range.contexts);
        }
      }
    }

    // Did we get it?
    if (prev) {
      parent_context = static_cast<TokenContextIndex>(
          prev - range.contexts.data());
      assert(parent_context < range.contexts.size());
    }

    // Top-down mutational update.
    if (statements) {
      parent_context = statements->CommonContext(range, parent_context);
    }

    assert(parent_context < range.contexts.size());
    common_context.emplace(parent_context);

    return parent_context;
  }

#if PASTA_DEBUG_ALIGN
  void Print(std::ostream &os, std::string indent,
             const ASTImpl &ast, const PrintedTokenRangeImpl &range) const final {
    if (common_context.has_value()) {
      assert(common_context.value() < range.contexts.size());
    }
    os << indent << "------BA "
       << std::hex
       << (common_context ? common_context.value() : kInvalidTokenContextIndex)
       << std::dec << "------\n";
    os << indent << begin->Data(ast) << " " << clang::tok::getTokenName(begin->Kind());
    if (begin->opaque_source_loc != TokenImpl::kInvalidSourceLocation) {
      os << " l:" << std::hex << begin->opaque_source_loc << std::dec;
    }
    if (begin->context_index != kInvalidTokenContextIndex) {
      os << " c:" << std::hex << begin->context_index << std::dec;
    }
    os << '\n';

    if (statements) {
      statements->Print(os, indent + "  ", ast, range);
    }

    os << indent << end->Data(ast) << " " << clang::tok::getTokenName(end->Kind());
    if (end->opaque_source_loc != TokenImpl::kInvalidSourceLocation) {
      os << " l:" << std::hex << end->opaque_source_loc << std::dec;
    }
    if (end->context_index != kInvalidTokenContextIndex) {
      os << " c:" << std::hex << end->context_index << std::dec;
    }
    os << '\n';
  }

  void Print(std::ostream &os, std::string indent,
             const PrintedTokenRangeImpl &range) const final {
    if (common_context.has_value()) {
      assert(common_context.value() < range.contexts.size());
    }
    os << indent << "------BA "
       << std::hex
       << (common_context ? common_context.value() : kInvalidTokenContextIndex)
       << std::dec << "------\n";
    os << indent << begin->Data(range) << " " << clang::tok::getTokenName(begin->Kind());
    if (begin->opaque_source_loc != TokenImpl::kInvalidSourceLocation) {
      os << " l:" << std::hex << begin->opaque_source_loc << std::dec;
    }
    if (begin->context_index != kInvalidTokenContextIndex) {
      os << " c:" << std::hex << begin->context_index << std::dec;
    }
    os << '\n';
    if (statements) {
      statements->Print(os, indent + "  ", range);
    }
    os << indent << end->Data(range) << " " << clang::tok::getTokenName(end->Kind());
    if (end->opaque_source_loc != TokenImpl::kInvalidSourceLocation) {
      os << " l:" << std::hex << end->opaque_source_loc << std::dec;
    }
    if (end->context_index != kInvalidTokenContextIndex) {
      os << " c:" << std::hex << end->context_index << std::dec;
    }
    os << '\n';
  }
#endif  // PASTA_DEBUG_ALIGN
};

static bool MergeToken(TokenImpl *parsed, PrintedTokenImpl *printed,
                       bool &changed, bool force=false) {

  if (parsed->context_index == printed->context_index) {
    return false;
  }

  assert(printed->context_index != kInvalidTokenContextIndex);
  if (force) {
    auto local_changed = (parsed->context_index != printed->context_index ||
                          parsed->opaque_source_loc !=
                              printed->opaque_source_loc);
    parsed->context_index = printed->context_index;
    assert(printed->opaque_source_loc == TokenImpl::kInvalidSourceLocation ||
           printed->opaque_source_loc == parsed->opaque_source_loc);
    printed->opaque_source_loc = parsed->opaque_source_loc;
    changed = changed || local_changed;
    return local_changed;

  } else if (parsed->context_index != kInvalidTokenContextIndex) {
    return false;
  }

  if (TokenImpl::kInvalidSourceLocation == printed->opaque_source_loc) {
    parsed->context_index = printed->context_index;
    assert(parsed->context_index != kInvalidTokenContextIndex);
    printed->opaque_source_loc = parsed->opaque_source_loc;
    changed = true;
    return true;

  } else if (TokenImpl::kInvalidSourceLocation == parsed->opaque_source_loc) {
    assert(false);
    return false;

  } else if (parsed->opaque_source_loc == printed->opaque_source_loc) {
    parsed->context_index = printed->context_index;
    assert(parsed->context_index != kInvalidTokenContextIndex);
    changed = true;
    return true;

  // Different locations.
  } else {
    return false;
  }
}

class Matcher {
 private:
  ASTImpl &ast;
  PrintedTokenRangeImpl &range;
  TokenImpl * const first_parsed;

  // Exclusive upper bound.
  TokenImpl * const last_parsed;

  std::vector<std::unique_ptr<Region>> parsed_regions;
  std::vector<std::unique_ptr<Region>> printed_regions;

  // Maps identifiers or keywords that precede a balanced region to the
  // token just after the balanced region. This is used to jump over
  // `__attribute__` tokens when doing forward matching.
  std::unordered_map<TokenImpl *, TokenImpl *> skip_balanced;

//  std::vector<std::pair<BalancedRegion *, BalancedRegion *>> failed_balanced;


 public:
  inline explicit Matcher(ASTImpl &ast_, PrintedTokenRangeImpl &range_,
                          TokenImpl *first_parsed_,
                          TokenImpl *last_parsed_)
      : ast(ast_),
        range(range_),
        first_parsed(first_parsed_),
        last_parsed(last_parsed_) {}

  // Organize the tokens into a tree, grouped by brace/bracket/paren-enclosed
  // regions, and comma/semicolon-separated regions.
  SequenceRegion *BuildRegions(
      std::vector<std::unique_ptr<Region>> &regions,
      std::stringstream &err, uint8_t *first, uint8_t *after_last,
      size_t tok_size_, const char *list_kind);

  bool DataEquals(TokenImpl *parsed, PrintedTokenImpl *printed);
  bool MatchToken(TokenImpl *parsed, PrintedTokenImpl *printed);
  bool MatchTokenByKindOrData(TokenImpl *parsed, PrintedTokenImpl *printed);
  bool MatchBalanced(BalancedRegion *parsed, BalancedRegion *printed,
                     bool &changed);
  bool MatchProduct(std::vector<Region *> &parsed_regions,
                    std::vector<Region *> &printed_regions,
                    bool &changed);
  bool MatchSequence(SequenceRegion *parsed, SequenceRegion *printed,
                     bool &changed);
  bool MatchStatement(StatementRegion *parsed, StatementRegion *printed,
                      bool &changed);
  bool MatchRegions(Region *parsed, Region *printed,
                    bool &changed);

  bool MergeForward(TokenImpl *parsed, PrintedTokenImpl *printed, bool &changed);
  bool MergeBackward(TokenImpl *parsed, PrintedTokenImpl *printed, bool &changed);

  void FixContexts(Region *parsed, std::vector<TokenContextIndex> &stack);
};

// Organize the tokens into a tree, grouped by brace/bracket/paren-enclosed
// regions, and comma/semicolon-separated regions.
SequenceRegion *Matcher::BuildRegions(
    std::vector<std::unique_ptr<Region>> &regions, std::stringstream &err,
    uint8_t *first, uint8_t *after_last, size_t tok_size_,
    const char *list_kind) {

  const ptrdiff_t tok_size = static_cast<ptrdiff_t>(tok_size_);
  std::vector<SequenceRegion *> region_stack;
  std::vector<std::pair<clang::tok::TokenKind, TokenImpl *>> match_stack;
  std::vector<std::tuple<clang::tok::TokenKind, clang::tok::TokenKind,
                         clang::tok::TokenKind>> stmt_stoppers;

  TokenImpl *unused_end = nullptr;

  auto push_empty_sequence = [&] (void) {
    const auto empty = new SequenceRegion;
    regions.emplace_back(empty);
    region_stack.emplace_back(empty);
    return empty;
  };

  auto add_uncollected_stmt = [&] (TokenImpl *begin) {
    if (unused_end) {
      auto seq = dynamic_cast<SequenceRegion *>(region_stack.back());
      assert(seq != nullptr);
      auto stmt = new StatementRegion;
      regions.emplace_back(stmt);
      stmt->begin = begin;
      stmt->end = unused_end;
      seq->regions.push_back(stmt);
      unused_end = nullptr;
    }
  };

  push_empty_sequence();
  stmt_stoppers.emplace_back(clang::tok::semi, clang::tok::comma,
                             clang::tok::eof);

  BalancedRegion *last_balanced = nullptr;

  // Traverse backwards through the printed tokens and try to match brackets,
  // braces, and parentheses with what is in the pre-processed code. We go
  // backward because the printed token ranges might start with declarations
  // for builtins, which are not present in the preprocessed code.
  auto prev_it = after_last;
  for (auto it = after_last, end = first; it != end; prev_it = it) {

    TokenImpl *next_tok_ptr = reinterpret_cast<TokenImpl *>(it);

    // Decrement by `sizeof(TokenImpl)` or `sizeof(PrintedTokenImpl)`.
    it = &(it[-tok_size]);

    assert(it < prev_it);
    (void) prev_it;

    TokenImpl &tok = *reinterpret_cast<TokenImpl *>(it);

//    if (auto hl_tok = ast.TokenAt(&tok)) {
//      for (auto i = 0u; i < match_stack.size(); ++i) {
//        std::cerr << ' ';
//      }
//      std::cerr << hl_tok.Index() << ' ' << hl_tok.KindName() << ' ' << hl_tok.Data() << '\n';
//    }

    // This is a macro expansion token; ignore it.
    if (!TokenCanBeAssignedContext(&tok)) {
      if (!unused_end) {
        unused_end = &tok;
      }
      continue;  // Skip.
    }

    const clang::tok::TokenKind tok_kind = tok.Kind();

    // If we just saw a balanced region, and now we're seeing an identifier,
    // then we want to use that identifier as part of our matching criteria.
    if (last_balanced &&
        (clang::tok::isAnyIdentifier(tok_kind) ||
         clang::tok::getKeywordSpelling(tok_kind) != nullptr)) {
      last_balanced->leading_ident = &tok;

      auto after_balanced = reinterpret_cast<TokenImpl *>(
          reinterpret_cast<uintptr_t>(last_balanced->end) + tok_size_);
      if (reinterpret_cast<uint8_t *>(after_balanced) < after_last) {
        skip_balanced.emplace(&tok, after_balanced);
      }
    }
    last_balanced = nullptr;

    switch (tok_kind) {

      // We have found the beginning of a nested region, in terms of the ending
      // token of that nested region.
      case clang::tok::r_paren:
        add_uncollected_stmt(next_tok_ptr);
        push_empty_sequence();
        match_stack.emplace_back(clang::tok::l_paren, &tok);
        stmt_stoppers.emplace_back(clang::tok::semi,
                                   clang::tok::comma  /* parameter lists */,
                                   clang::tok::colon  /* for loops */);
        break;
      case clang::tok::r_square:
        add_uncollected_stmt(next_tok_ptr);
        push_empty_sequence();
        match_stack.emplace_back(clang::tok::l_square, &tok);
        stmt_stoppers.emplace_back(clang::tok::semi, clang::tok::semi,
                                   clang::tok::comma  /* comma expressions */);
        break;
      case clang::tok::r_brace:
        add_uncollected_stmt(next_tok_ptr);
        push_empty_sequence();
        match_stack.emplace_back(clang::tok::l_brace, &tok);
        stmt_stoppers.emplace_back(clang::tok::semi, clang::tok::semi,
                                   clang::tok::colon  /* `public:`, etc. */);
        break;

      // We have found the end of a nested region, in terms of the beginning
      // token of that nested region.
      case clang::tok::l_paren:
      case clang::tok::l_square:
      case clang::tok::l_brace:
        add_uncollected_stmt(next_tok_ptr);

        // If this happens, it's likely a bug in `Bounds.cpp`.
        if (match_stack.empty()) {
          err << "Unable to match opening "
              << clang::tok::getTokenName(tok_kind)
              << "; match stack is empty for " << list_kind << " tokens";

          if (Token hl_tok = ast.TokenAt(&tok)) {
            err << " (index " << hl_tok.Index() << ')';
            if (auto ft = hl_tok.FileLocation()) {
              auto file = File::Containing(ft.value());
              err << " at " << file.Path().generic_string()
                  << ':' << ft->Line() << ':' << ft->Column();
            }
          }
          return nullptr;
        } else {
          auto [opening_kind, r_tok] = match_stack.back();

          match_stack.pop_back();
          stmt_stoppers.pop_back();

          if (opening_kind != tok_kind) {
            err << "Unbalanced "
                << clang::tok::getTokenName(tok_kind)
                << " (starting index " << ((it - first) / tok_size)
                << "); expected a "
                << clang::tok::getTokenName(opening_kind)
                << " (ending index "
                << ((reinterpret_cast<uint8_t *>(r_tok) - first) / tok_size)
                << ") in " << list_kind << " tokens";

            if (Token hl_tok_begin = ast.TokenAt(&tok)) {
              if (auto ft = hl_tok_begin.FileLocation()) {
                auto file = File::Containing(ft.value());
                err << " starting at " << file.Path().generic_string()
                    << ':' << ft->Line() << ':' << ft->Column();
              }
            }

            if (Token hl_tok_end = ast.TokenAt(r_tok)) {
              if (auto ft = hl_tok_end.FileLocation()) {
                auto file = File::Containing(ft.value());
                err << " ending at " << file.Path().generic_string()
                    << ':' << ft->Line() << ':' << ft->Column();
              }
            }
            return nullptr;

          } else {
            assert(!region_stack.empty());
            auto br = new BalancedRegion;
            regions.emplace_back(br);

            br->begin = &tok;
            br->end = r_tok;
            br->statements = region_stack.back();
            region_stack.pop_back();
            assert(!region_stack.empty());
            region_stack.back()->regions.push_back(br);
            last_balanced = br;
          }
          break;
        }

      // We've found the end of a statement.
      case clang::tok::semi:
      case clang::tok::comma:
      case clang::tok::colon:
        if (tok_kind == std::get<0>(stmt_stoppers.back()) ||
            tok_kind == std::get<1>(stmt_stoppers.back()) ||
            tok_kind == std::get<2>(stmt_stoppers.back())) {
          add_uncollected_stmt(next_tok_ptr);
          unused_end = &tok;
          break;
        }
        [[clang::fallthrough]];

      default:
        if (!unused_end) {
          unused_end = &tok;
        }
        break;
    }
  }

  // Flush out empty regions. This seems to happen in the following case from
  // the XNU kernel:
  //
  //        _Atomic(struct thread_group *) *
  //        kqr_preadopt_thread_group_addr(workq_threadreq_t req);
  while (1u < region_stack.size()) {
    if (region_stack.back()->regions.empty()) {
      region_stack.pop_back();
    }
  }

  if (region_stack.size() != 1u) {
    err
        << "Region stack for " << list_kind << " tokens has "
        << region_stack.size() << " regions";
    return nullptr;
  }

  add_uncollected_stmt(reinterpret_cast<TokenImpl *>(first));

  return region_stack.back();
}

static const std::hash<std::string_view> kHasher;

// Strip off leading and trailing underscores, then hash. This is to deal with
// things like `asm` vs. `__asm`.
static std::string_view HashableData(std::string_view view) {
  while (!view.empty() && view.front() == '_') {
    view = view.substr(1u, view.size() - 1u);
  }

  while (!view.empty() && view.back() == '_') {
    view = view.substr(0, view.size() - 1u);
  }

  return view;
}

// Strip off leading and trailing underscores, then hash. This is to deal with
// things like `asm` vs. `__asm`.
static uint64_t Hash(clang::tok::TokenKind kind, std::string_view view) {
  if (clang::tok::isLiteral(kind)) {
    return static_cast<uint64_t>(kind);
  } else {
    return kHasher(HashableData(view));
  }
}

bool Matcher::DataEquals(TokenImpl *parsed, PrintedTokenImpl *printed) {
  auto parsed_data = parsed->Data(ast);
  auto printed_data = printed->Data(range);
  if (clang::tok::getKeywordSpelling(parsed->Kind())) {
    return HashableData(parsed_data) == HashableData(printed_data);
  } else {
    return parsed_data == printed_data;
  }
}

static bool IsAttributeLikeKeword(clang::tok::TokenKind tk) {
  switch (tk) {
    case clang::tok::kw___attribute:
    case clang::tok::kw___declspec:
      return true;
    default:
      return false;
  }
}

bool Matcher::MergeForward(TokenImpl *parsed, PrintedTokenImpl *printed,
                           bool &changed) {
  auto merged = false;

  std::vector<TokenImpl *> backtrack_locs;

  const auto last_printed = &(range.tokens.back());
  while (parsed < last_parsed && printed <= last_printed) {
    if (!TokenCanBeAssignedContext(parsed)) {
      ++parsed;
      continue;
    }

    if (!TokenCanBeAssignedContext(printed)) {
      ++printed;
      continue;
    }

    const auto parsed_kind = parsed->Kind();
    const auto printed_kind = printed->Kind();

    // Try to skip over `__attribute__` sections when matching.
    if (parsed_kind != printed_kind) {
      if (IsAttributeLikeKeword(parsed_kind)) {
        if (auto skip_it = skip_balanced.find(parsed);
            skip_it != skip_balanced.end()) {
          parsed = skip_it->second;
          continue;
        }

      } else if (IsAttributeLikeKeword(printed_kind)) {
        if (auto skip_it = skip_balanced.find(printed);
            skip_it != skip_balanced.end()) {
          printed = reinterpret_cast<PrintedTokenImpl *>(skip_it->second);
          continue;
        }
      }
    }

    if (!MatchTokenByKindOrData(parsed, printed)) {
      break;
    }

    if (!MergeToken(parsed, printed, changed) && merged) {
      break;
    } else {
      merged = true;
      ++parsed;
      ++printed;
    }
  }

  return merged;
}

bool Matcher::MergeBackward(TokenImpl *parsed, PrintedTokenImpl *printed,
                            bool &changed) {

  auto merged = false;
  const auto first_printed = &(range.tokens.front());
  while (parsed >= first_parsed && printed >= first_printed) {
    if (!TokenCanBeAssignedContext(parsed)) {
      --parsed;
      continue;
    }

    if (!TokenCanBeAssignedContext(printed)) {
      --printed;
      continue;
    }

    if (!MatchTokenByKindOrData(parsed, printed)) {
      break;
    }

    if (!MergeToken(parsed, printed, changed) && merged) {
      break;

    } else {
      merged = true;
      --parsed;
      --printed;
    }
  }

  return merged;
}

bool Matcher::MatchToken(TokenImpl *parsed, PrintedTokenImpl *printed) {
  if (TokenLocationsMatch(parsed, printed)) {
    return true;

  } else {
    return MatchTokenByKindOrData(parsed, printed);
  }
}

bool Matcher::MatchTokenByKindOrData(TokenImpl *parsed,
                                     PrintedTokenImpl *printed) {
  const auto parsed_kind = parsed->Kind();
  const auto kinds_equal = parsed_kind == printed->Kind();
  if (clang::tok::isLiteral(parsed_kind)) {
    return kinds_equal;

  } else if (kinds_equal) {
    if (clang::tok::getKeywordSpelling(parsed_kind) ||
        !clang::tok::isAnyIdentifier(parsed_kind)) {
      return true;
    } else {
      return DataEquals(parsed, printed);
    }

  } else {
    return DataEquals(parsed, printed);
  }
}

bool Matcher::MatchBalanced(BalancedRegion *parsed, BalancedRegion *printed,
                            bool &changed) {
  if (parsed->matched_with && parsed->matched_with != printed) {
    return false;
  } else if (parsed->begin->Kind() != printed->begin->Kind()) {
    return false;
  }

  if (!parsed->leading_ident != !printed->leading_ident) {
    return false;
  }

  if (parsed->leading_ident &&
      !MatchToken(
          parsed->leading_ident,
          reinterpret_cast<PrintedTokenImpl *>(printed->leading_ident))) {
    return false;
  }

  assert(parsed->begin->Kind() == printed->begin->Kind());
  assert(parsed->end->Kind() == printed->end->Kind());

  auto printed_begin = reinterpret_cast<PrintedTokenImpl *>(printed->begin);
  auto begin_loc_matches = TokenLocationsMatch(parsed->begin, printed_begin);
  if (TokenHasLocationAndContext(printed_begin) && !begin_loc_matches) {
    return false;
  }

  auto printed_end = reinterpret_cast<PrintedTokenImpl *>(printed->end);
  auto end_loc_matches = TokenLocationsMatch(parsed->end, printed_end);
  if (TokenHasLocationAndContext(printed_end) && !end_loc_matches) {
    return false;
  }

  // Look just before and just after the opening and/or closing tokens to
  // see if we can match on those.
  if (!begin_loc_matches && !end_loc_matches) {
    const auto first_parsed_tok = this->first_parsed;
    const auto first_printed_tok = &(range.tokens.front());

    // Look one before.
    if (parsed->begin > first_parsed_tok &&
        printed_begin > first_printed_tok) {
      auto before_parsed = &(parsed->begin[-1]);
      auto before_printed = &(printed_begin[-1]);
      if (TokenLocationsMatch(before_parsed, before_printed)) {
        begin_loc_matches = true;
      }
    }

    // Look one beyond.
    const auto last_parsed_tok = this->last_parsed;
    const auto last_printed_tok = &(range.tokens.back());
    if (parsed->end < last_parsed_tok &&
        printed_end < last_printed_tok) {
      auto after_parsed = &(parsed->end[1]);
      auto after_printed = &(printed_end[1]);
      if (TokenLocationsMatch(after_parsed, after_printed)) {
        end_loc_matches = true;
      }
    }
  }

  auto did_recurse = false;
  if (!begin_loc_matches && !end_loc_matches) {
    if (parsed->predecessor && printed->predecessor &&
        parsed->predecessor->matched_with == printed->predecessor) {

      if (parsed->statements && printed->statements &&
          MatchSequence(parsed->statements, printed->statements, changed)) {
        did_recurse = true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  if (!parsed->common_context) {
    parsed->common_context = printed->common_context;
  }

  // Commit to this match.
  parsed->matched_with = printed;

  if (!printed->matched_with) {
    printed->matched_with = parsed;
  }

  MergeToken(parsed->begin, printed_begin, changed, true);
  MergeToken(parsed->end, printed_end, changed, true);

  // If we pivoted from a match on the ending to injecting a match on the
  // beginning, then apply linear matching around the beginning of the range.
  MergeForward(parsed->begin, printed_begin, changed);
  MergeBackward(parsed->begin, printed_begin, changed);

  // If we pivoted from a match on the beginning to injecting a match on the
  // ending, then apply linear matching around the ending of the range.
  MergeForward(parsed->end, printed_end, changed);
  MergeBackward(parsed->end, printed_end, changed);

  // If we have statements, then commit to them too.
  if (!did_recurse && parsed->statements && printed->statements) {

    if (parsed->statements->matched_with != printed->statements) {
      changed = true;
      parsed->statements->matched_with = printed->statements;
      printed->statements->matched_with = parsed->statements;
    }

    (void) MatchSequence(parsed->statements, printed->statements, changed);
  }

  return parsed->matched_with == printed;
}

template <typename T>
static std::vector<Region *> FilterRegionsInto(
    const std::vector<Region *> &regions) {
  std::vector<Region *> filtered;
  for (auto it = regions.rbegin(), end = regions.rend(); it != end; ++it) {
    if (auto f = dynamic_cast<T *>(*it)) {
      filtered.push_back(f);
    }
  }
  return filtered;
}

bool Matcher::MatchProduct(std::vector<Region *> &parsed_regions,
                           std::vector<Region *> &printed_regions,
                           bool &changed) {
  bool matched = false;
  for (Region *&parsed_sub : parsed_regions) {
    if (!parsed_sub) {
      continue;
    }
    for (Region *&printed_sub : printed_regions) {
      if (printed_sub &&
          parsed_sub->matched_with != printed_sub &&
          MatchRegions(parsed_sub, printed_sub, changed)) {

        if (!parsed_sub->common_context) {
          parsed_sub->common_context = printed_sub->common_context;
        }

        if (!parsed_sub->matched_with) {
          changed = true;
          parsed_sub->matched_with = printed_sub;
        }

        if (!printed_sub->matched_with) {
          changed = true;
          printed_sub->matched_with = parsed_sub;
        }

        parsed_sub = nullptr;
        printed_sub = nullptr;
        matched = true;
        break;
      }
    }
  }
  return matched;
}

// NOTE(pag): The elements in a sequence are already in reverse order.
bool Matcher::MatchSequence(SequenceRegion *parsed, SequenceRegion *printed,
                            bool &changed) {
  if (parsed->regions.empty() != printed->regions.empty()) {
    return false;  // One is empty, the other isn't.

  } else if (parsed->regions.empty()) {
    return true;  // Both are empty.
  }

  if (parsed->matched_with && parsed->matched_with != printed) {
    return false;
  }

  auto matched = false;

  {
    auto parsed_regions = FilterRegionsInto<StatementRegion>(parsed->regions);
    auto printed_regions = FilterRegionsInto<StatementRegion>(printed->regions);
    if (MatchProduct(parsed_regions, printed_regions, changed)) {
      matched = true;
    }
  }
  {
    auto parsed_regions = FilterRegionsInto<SequenceRegion>(parsed->regions);
    auto printed_regions = FilterRegionsInto<SequenceRegion>(printed->regions);
    if (MatchProduct(parsed_regions, printed_regions, changed)) {
      matched = true;
    }
  }
  {
    auto parsed_regions = FilterRegionsInto<BalancedRegion>(parsed->regions);
    auto printed_regions = FilterRegionsInto<BalancedRegion>(printed->regions);
    if (MatchProduct(parsed_regions, printed_regions, changed)) {
      matched = true;
    }
  }

  if (matched) {

    if (!parsed->common_context) {
      parsed->common_context = printed->common_context;
    }

    if (parsed->matched_with != printed) {
      changed = true;
      parsed->matched_with = printed;
    }

    if (!printed->matched_with) {
      changed = true;
      printed->matched_with = parsed;
    }
  }

  // Allow ourselves to locally fail on this run, but benefit from a prior
  // matching.
  return parsed->matched_with == printed;
}

bool Matcher::MatchStatement(StatementRegion *parsed, StatementRegion *printed,
                             bool &changed) {

  if (parsed->matched_with && parsed->matched_with != printed) {
    return false;
  }

  const auto parsed_begin = parsed->begin;
  auto parsed_end = parsed->end;

  const auto printed_begin =
      reinterpret_cast<PrintedTokenImpl *>(printed->begin);
  auto printed_end = reinterpret_cast<PrintedTokenImpl *>(printed->end);

  std::unordered_map<uint64_t, std::vector<TokenImpl *>> parsed_toks;
  std::unordered_map<uint64_t, std::vector<PrintedTokenImpl *>> printed_toks;

  for (TokenImpl *it = parsed_begin; it <= parsed_end; ++it) {
    if (TokenCanBeAssignedContext(it)) {
      parsed_toks[Hash(it->Kind(), it->Data(ast))].emplace_back(it);
    }
  }

  for (PrintedTokenImpl *it = printed_begin; it <= printed_end; ++it) {
    if (TokenCanBeAssignedContext(it)) {
      printed_toks[Hash(it->Kind(), it->Data(range))].emplace_back(it);
    }
  }

  auto matched = false;
  for (auto &[hash, parsed_tok_group] : parsed_toks) {
    auto printed_toks_it = printed_toks.find(hash);
    if (printed_toks_it == printed_toks.end()) {
      continue;
    }

    for (TokenImpl *&parsed_tok : parsed_tok_group) {
      for (PrintedTokenImpl *&printed_tok : printed_toks_it->second) {
        if (!printed_tok) {
          continue;

        } else if (TokenLocationsMatch(parsed_tok, printed_tok)) {
          matched = true;
          break;

        } else if (!TokenHasLocationAndContext(printed_tok) &&
                   MatchTokenByKindOrData(parsed_tok, printed_tok)) {
          MergeForward(parsed_tok, printed_tok, changed);
          MergeBackward(parsed_tok, printed_tok, changed);
          matched = true;
          printed_tok = nullptr;
          break;
        }
      }
    }
  }

  if (matched) {
    if (!parsed->common_context) {
      parsed->common_context = printed->common_context;
    }

    if (parsed->matched_with != printed) {
      changed = true;
      parsed->matched_with = printed;
    }

    if (!printed->matched_with) {
      changed = true;
      printed->matched_with = parsed;
    }
  }

  // Allow ourselves to locally fail on this run, but benefit from a prior
  // matching.
  return parsed->matched_with == printed;
}

bool Matcher::MatchRegions(Region *parsed, Region *printed,
                           bool &changed) {
  if (!parsed != !printed) {
    return false;
  }

  if (parsed->matched_with) {
    return parsed->matched_with == printed;
  }

  if (parsed->common_context && printed->common_context &&
      parsed->common_context.value() != printed->common_context.value()) {
    return false;
  }

  const auto kind = parsed->Kind();
  if (kind != printed->Kind()) {
    return false;
  }

  switch (kind) {
    case RegionKind::kBalanced:
      return MatchBalanced(dynamic_cast<BalancedRegion *>(parsed),
                           dynamic_cast<BalancedRegion *>(printed), changed);
    case RegionKind::kSequence:
      return MatchSequence(dynamic_cast<SequenceRegion *>(parsed),
                           dynamic_cast<SequenceRegion *>(printed), changed);
    case RegionKind::kStatement:
      return MatchStatement(dynamic_cast<StatementRegion *>(parsed),
                            dynamic_cast<StatementRegion *>(printed), changed);
  }
  return false;
}

// Make sure that every parsed token is assigned *some* kind of context. We try
// to benefit from existing matches and common ancestors to apply contexts.
void Matcher::FixContexts(
    Region *parsed, std::vector<TokenContextIndex> &stack) {

  TokenContextIndex prev_context = stack.back();

  if (auto bal = dynamic_cast<BalancedRegion *>(parsed)) {
    assert(!stack.empty());

    // If we have a predecessor context from an identifier or keyword, take
    // it.
    if (bal->leading_ident && TokenHasLocationAndContext(bal->leading_ident)) {
      prev_context = bal->leading_ident->context_index;
      bal->common_context.emplace(prev_context);

    } else if (bal->common_context.has_value()) {
      prev_context = bal->common_context.value();
    }

    // Happens when we match the `)` of an `__attribute__` against the `)`
    // of the parameter list of the function to which the attribute applies.
    if (TokenHasLocationAndContext(bal->begin) !=
        TokenHasLocationAndContext(bal->end)) {
      bal->begin->context_index = prev_context;
      bal->end->context_index = prev_context;
    }

    // Take the parent context.
    if (!TokenHasLocationAndContext(bal->begin)) {
      assert(!TokenHasLocationAndContext(bal->end));
      bal->begin->context_index = prev_context;
      bal->end->context_index = prev_context;
      stack.push_back(prev_context);

    // Get the context from the balanced tokens.
    } else {
      stack.push_back(bal->begin->context_index);
    }

    // Fix the contexts of any tokens inside of this balanced range.
    if (bal->statements) {
      FixContexts(bal->statements, stack);
    }

  } else if (auto stmt = dynamic_cast<StatementRegion *>(parsed)) {

    const TokenContextImpl *prev = nullptr;
    if (stmt->common_context.has_value()) {
      prev = &(range.contexts[stmt->common_context.value()]);
    }

    // Try to find the common ancestor of everything in this statement.
    if (!prev) {
      for (auto tok = stmt->begin; tok <= stmt->end; ++tok) {
        if (TokenHasLocationAndContext(tok)) {
          assert(TokenCanBeAssignedContext(tok));
          const TokenContextImpl *curr = &(range.contexts[tok->context_index]);
          if (!prev) {
            prev = curr;
          } else {
            prev = TokenContextImpl::CommonAncestor(prev, curr, range.contexts);
          }
        }
      }
    }

    // If we've got a common ancestor, then push its index, and double check
    // it against our parent. Otherwise, push our parent.
    if (prev) {
      auto index = static_cast<TokenContextIndex>(prev - range.contexts.data());
      assert(index < range.contexts.size());

      stmt->common_context.emplace(index);

      if (prev_context != kInvalidTokenContextIndex) {
        auto parent = &(range.contexts[prev_context]);
        auto ancestor = TokenContextImpl::CommonAncestor(
            prev, parent, range.contexts);
        if (!ancestor || ancestor->depth < parent->depth) {
          index = prev_context;  // Fixup.
        }
      }

      stack.push_back(index);

    } else {
      stack.push_back(prev_context);
    }

    // Assign the top context on the stack to any token lacking the context.
    for (auto tok = stmt->begin; tok <= stmt->end; ++tok) {
      if (!TokenHasLocationAndContext(tok) && TokenCanBeAssignedContext(tok)) {
        tok->context_index = stack.back();
      }
    }

  } else if (auto seq = dynamic_cast<SequenceRegion *>(parsed)) {

    // Try to find the common ancestor of everything in this sequence.
    const TokenContextImpl *prev = nullptr;
    if (seq->common_context.has_value()) {
      prev = &(range.contexts[seq->common_context.value()]);
    }

    if (!prev) {
      for (Region *region : seq->regions) {
        if (TokenImpl *tok = region->FirstParsedToken()) {
          assert(TokenHasLocationAndContext(tok));
          assert(TokenCanBeAssignedContext(tok));
          const TokenContextImpl *curr = &(range.contexts[tok->context_index]);
          if (!prev) {
            prev = curr;
          } else {
            prev = TokenContextImpl::CommonAncestor(prev, curr, range.contexts);
          }
        }
        if (TokenImpl *tok = region->LastParsedToken()) {
          assert(TokenHasLocationAndContext(tok));
          assert(TokenCanBeAssignedContext(tok));
          const TokenContextImpl *curr = &(range.contexts[tok->context_index]);
          if (!prev) {
            prev = curr;
          } else {
            prev = TokenContextImpl::CommonAncestor(prev, curr, range.contexts);
          }
        }
      }
    }

    // If we've got a common ancestor, then push its index, and double check
    // it against our parent. Otherwise, push our parent.
    if (prev) {
      auto index = static_cast<TokenContextIndex>(prev - range.contexts.data());
      assert(index < range.contexts.size());

      seq->common_context.emplace(index);

      if (prev_context != kInvalidTokenContextIndex) {
        auto parent = &(range.contexts[prev_context]);
        auto ancestor = TokenContextImpl::CommonAncestor(
            prev, parent, range.contexts);
        if (!ancestor || ancestor->depth < parent->depth) {
          index = prev_context;  // Fixup.
        }
      }

      stack.push_back(index);

    } else {
      stack.push_back(prev_context);
    }

    // Fix the contexts of any tokens inside of this sequence.
    for (auto region : seq->regions) {
      FixContexts(region, stack);
    }
  }

  stack.pop_back();
}

static bool HasNotBeenMatched(Region *r) {
  return !r->matched_with;
}

// Recursively migrate token contexts.
static TokenContextIndex MigrateContexts(
    TokenContextIndex id,
    const std::vector<TokenContextImpl> &from_contexts,
    std::vector<TokenContextImpl> &to_contexts,
    std::unordered_multimap<const void *, TokenContextIndex> &data_to_context,
    std::vector<TokenContextIndex> &context_map) {

  if (id == kInvalidTokenContextIndex || !id) {
    return 0u;  // Return the index of the AST node.
  }

  assert(id < from_contexts.size());
  const TokenContextImpl *from_c = &(from_contexts[id]);
  TokenContextIndex &ret_id = context_map[id];
  if (ret_id) {
#ifndef NDEBUG
    TokenContextImpl *to_c = &(to_contexts[ret_id]);
    assert(to_c->kind == from_c->kind);
    assert(to_c->depth == from_c->depth);
    assert(to_c->data == from_c->data || to_c->kind == TokenContextKind::kAlias);
#endif
    return ret_id;
  }

  TokenContextIndex parent_id = MigrateContexts(
      from_c->parent_index, from_contexts, to_contexts, data_to_context,
      context_map);

  if (from_c->kind == TokenContextKind::kAlias) {
    TokenContextIndex aliasee_id = MigrateContexts(
        static_cast<TokenContextIndex>(reinterpret_cast<uintptr_t>(from_c->data)),
        from_contexts, to_contexts, data_to_context, context_map);

    ret_id = static_cast<TokenContextIndex>(to_contexts.size());
    (void) to_contexts.emplace_back(parent_id, from_c->depth - 1u, aliasee_id);

  } else {

    // Search for the matching one.
    for (auto [it, end] = data_to_context.equal_range(from_c->data);
         it != end; ++it) {
      TokenContextIndex maybe_id = it->second;
      if (maybe_id == kInvalidTokenContextIndex ||
          maybe_id >= to_contexts.size()) {
        assert(maybe_id < to_contexts.size());
        continue;
      }

      TokenContextImpl *to_c = &(to_contexts[maybe_id]);
      if (to_c->data == from_c->data && to_c->parent_index == parent_id &&
          to_c->depth == from_c->depth && to_c->kind == from_c->kind) {
        ret_id = maybe_id;
        assert(ret_id != 0u);
        return maybe_id;
      }
    }

    // Didn't find it.
    ret_id = static_cast<TokenContextIndex>(to_contexts.size());
    (void) to_contexts.emplace_back(from_c->data, parent_id, from_c->depth,
                                    from_c->kind);
    data_to_context.emplace(from_c->data, ret_id);
  }

  assert(ret_id != 0u);
  return ret_id;
}

}  // namespace

Result<std::monostate, std::string> ASTImpl::AlignTokens(
      const std::shared_ptr<ASTImpl> &ast_,
      TokenImpl *parsed_begin, TokenImpl *parsed_end,
      PrintedTokenRangeImpl &range,
      TokenContextIndex decl_context_id) {

  ASTImpl * const ast = ast_.get();
  auto printed_begin = &(range.tokens[0]);
  auto printed_end = &(printed_begin[range.tokens.size()]);
  assert(parsed_begin < parsed_end);
  assert(printed_begin < printed_end);
  assert(parsed_end <= &(ast->tokens.back()));

  std::stringstream err;
  std::vector<std::unique_ptr<Region>> parsed_regions;
  std::vector<std::unique_ptr<Region>> printed_regions;

  Matcher matcher(*ast, range, parsed_begin, parsed_end);

  std::unordered_map<OpaqueSourceLoc, TokenImpl *> loc_to_toks;
  for (auto tok = parsed_begin; tok < parsed_end; ++tok) {
    if (TokenCanBeAssignedContext(tok)) {
      loc_to_toks.emplace(tok->opaque_source_loc, tok);
    }
  }

//  std::cerr << "\n\n----------------------------------------- "
//            << ast->TokenAt(parsed_begin).Index() << ' '
//            << ast->TokenAt(&(parsed_end[-1])).Index() << "\n";
//  auto sep = "";
//  for (auto t = parsed_begin; t < parsed_end; ++t) {
//    if (TokenCanBeAssignedContext(t)) {
//      std::cerr << sep << t->Data(*ast);
//      sep = " ";
//    }
//  }
//  std::cerr << '\n';

  // Join on the shared source locations, and linearly "spread"
  // outward from there. This should generally cover a bunch.
  bool changed = false;
  for (PrintedTokenImpl *printed = printed_begin;
       printed < printed_end; ++printed) {

    if (printed->opaque_source_loc == TokenImpl::kInvalidSourceLocation) {
      continue;
    }

    auto loc_it = loc_to_toks.find(printed->opaque_source_loc);
    if (loc_it == loc_to_toks.end()) {
      continue;
    }

    TokenImpl *parsed = loc_it->second;
    if (!parsed || TokenHasLocationAndContext(parsed)) {
      continue;
    }

    if (MergeToken(parsed, printed, changed, false)) {

      // These might change
      matcher.MergeBackward(parsed, printed, changed);
      matcher.MergeForward(parsed, printed, changed);
    }
  }

  auto parsed_tree = matcher.BuildRegions(
      parsed_regions, err, reinterpret_cast<uint8_t *>(parsed_begin),
      reinterpret_cast<uint8_t *>(parsed_end), sizeof(*parsed_begin),
      "parsed");
  if (!parsed_tree) {
//    for (auto tok = parsed_begin; tok < parsed_end; ++tok) {
//      std::cerr
//          << clang::tok::getTokenName(tok->Kind())
//          << '\t' << tok->Data(*ast) << '\n';
//    }
    return err.str();
  }

  auto printed_tree = matcher.BuildRegions(
      printed_regions, err, reinterpret_cast<uint8_t *>(printed_begin),
      reinterpret_cast<uint8_t *>(printed_end), sizeof(*printed_begin),
      "printed");
  if (!printed_tree) {

//    std::ofstream parsed_os("/tmp/tree.parsed");
//    parsed_tree->Print(parsed_os, "", *ast, range);
//
//    std::ofstream printed_os("/tmp/tree.printed");
//    for (auto t = printed_begin; t < printed_end; ++t) {
//      printed_os << t->Data(range);
////      if (t->opaque_source_loc != TokenImpl::kInvalidSourceLocation) {
////        printed_os << " l:" << std::hex << t->opaque_source_loc << std::dec;
////      }
////      if (t->context_index != kInvalidTokenContextIndex) {
////        printed_os << " c:" << std::hex << t->context_index << std::dec;
////      }
//
////      if ((t - printed_begin) == 18025) {
////        printed_os << "  /* HERE */";
////      }
//
//      printed_os << '\n';
//    }
//
//    parsed_os.flush();
//    printed_os.flush();
//    assert(false);
//
////    std::ofstream printed_os("/tmp/tree.printed");
////    printed_tree->Print(printed_os, "", range);

    return err.str();
  }

  // Initialize the printed regions with the contexts.
  (void) printed_tree->CommonContext(range, decl_context_id);

//  if (log) {
//    auto c = &(ast->tokens[69125].context_index);
//    assert(*c == kInvalidTokenContextIndex);
//    asm volatile ("nop;" ::"m"(c));
//  }

  // Assign the predecessor sequences of balanced regions. These can be helpful
  // for matching.
  auto assign_preds = [] (Region *region) {
    if (auto seq = dynamic_cast<SequenceRegion *>(region)) {
      StatementRegion *pred_stmt = nullptr;
      for (auto it = seq->regions.rbegin(), end = seq->regions.rend();
           it != end; ++it) {
        Region *curr = *it;
        if (auto balanced = dynamic_cast<BalancedRegion *>(curr)) {
          balanced->predecessor = pred_stmt;
        }
        pred_stmt = dynamic_cast<StatementRegion *>(curr);
      }
    }
  };

  for (const auto &printed_region : printed_regions) {
    assign_preds(printed_region.get());
  }

  std::vector<BalancedRegion *> parsed_balanced;
  std::vector<StatementRegion *> parsed_statements;

  std::vector<BalancedRegion *> printed_balanced;
  std::vector<StatementRegion *> printed_statements;

  for (const auto &region_ref : parsed_regions) {
    const auto region = region_ref.get();
    if (auto balanced = dynamic_cast<BalancedRegion *>(region)) {
      parsed_balanced.push_back(balanced);
    } else if (auto statement = dynamic_cast<StatementRegion *>(region)) {
      parsed_statements.push_back(statement);
    }
  }

  for (const auto &region_ref : printed_regions) {
    const auto region = region_ref.get();
    if (auto balanced = dynamic_cast<BalancedRegion *>(region)) {
      printed_balanced.push_back(balanced);
    } else if (auto statement = dynamic_cast<StatementRegion *>(region)) {
      printed_statements.push_back(statement);
    }
  }

  std::unordered_multimap<OpaqueSourceLoc, BalancedRegion *>
      printed_loc_to_balanced;
  std::unordered_multimap<OpaqueSourceLoc, StatementRegion *>
      printed_end_loc_to_statement;

  // Join on the shared source locations of balanced and statement regions.
  // This only joins unmatched ones. We generally have decent-ish ability in
  // the token printer (due to clang source locations) to find things like the
  // location of a `}` or a `{`, or a `;`.
  auto join_based_region_merge = [&] (bool &changed) {
    printed_loc_to_balanced.clear();
    printed_end_loc_to_statement.clear();

    {
      auto it = std::partition(parsed_balanced.begin(), parsed_balanced.end(),
                               HasNotBeenMatched);
      parsed_balanced.erase(it, parsed_balanced.end());
    }
    {
      auto it = std::partition(printed_balanced.begin(), printed_balanced.end(),
                               HasNotBeenMatched);
      printed_balanced.erase(it, printed_balanced.end());
    }
    {
      auto it = std::partition(parsed_statements.begin(),
                               parsed_statements.end(), HasNotBeenMatched);
      parsed_statements.erase(it, parsed_statements.end());
    }
    {
      auto it = std::partition(printed_statements.begin(),
                               printed_statements.end(), HasNotBeenMatched);
      printed_statements.erase(it, printed_statements.end());
    }

#if PASTA_DEBUG_ALIGN
    std::cerr
        << "parsed_balanced="<<parsed_balanced.size()
        << " printed_balanced="<<printed_balanced.size()
        << " parsed_statements="<<parsed_statements.size()
        << " printed_statements="<<printed_statements.size()
        <<'\n';
#endif

    for (auto region : parsed_balanced) {
      auto loc = region->begin->opaque_source_loc;
      if (loc != TokenImpl::kInvalidSourceLocation) {
        printed_loc_to_balanced.emplace(loc, region);
      }

      loc = region->end->opaque_source_loc;
      if (loc != TokenImpl::kInvalidSourceLocation) {
        printed_loc_to_balanced.emplace(loc, region);
      }
    }

    for (auto region : printed_balanced) {
      if (const auto begin_loc = region->begin->opaque_source_loc;
          begin_loc != TokenImpl::kInvalidSourceLocation) {

        auto [bb_it, bb_end] = printed_loc_to_balanced.equal_range(begin_loc);
        for (; bb_it != bb_end; ++bb_it) {
          matcher.MatchBalanced(bb_it->second, region, changed);
        }
      }

      if (const auto end_loc = region->end->opaque_source_loc;
          end_loc != TokenImpl::kInvalidSourceLocation) {

        auto [bb_it, bb_end] = printed_loc_to_balanced.equal_range(end_loc);
        for (; bb_it != bb_end; ++bb_it) {
          matcher.MatchBalanced(bb_it->second, region, changed);
        }
      }
    }

    for (auto region : parsed_statements) {
      auto loc = region->end->opaque_source_loc;
      if (loc != TokenImpl::kInvalidSourceLocation) {
        printed_end_loc_to_statement.emplace(loc, region);
      }
    }

    for (auto region : printed_statements) {
      if (const auto end_loc = region->end->opaque_source_loc;
          end_loc != TokenImpl::kInvalidSourceLocation) {

        auto [bb_it, bb_end] =
            printed_end_loc_to_statement.equal_range(end_loc);
        for (; bb_it != bb_end; ++bb_it) {
          matcher.MatchStatement(bb_it->second, region, changed);
        }
      }
    }
  };

  parsed_tree->matched_with = printed_tree;
  printed_tree->matched_with = parsed_tree;

  auto max_iters = (parsed_regions.size() / 2) + 1u;

  join_based_region_merge(changed);

#if PASTA_DEBUG_ALIGN
  auto i = 0u;
  for (PrintedTokenImpl *tok = printed_begin; tok < printed_end; ++tok) {
    std::cerr
        << (i++) << '\t'
        << '\t' << std::hex << tok->opaque_source_loc << std::dec
        << '\t' << std::hex << tok->context_index << std::dec
        << '\t' << clang::tok::getTokenName(tok->Kind())
        << '\t' << tok->Data(range) << '\n';
  }
  std::cerr << "---------------------------------\n";
  i = 0u;
  for (TokenImpl *tok = parsed_begin; tok < parsed_end; ++tok) {
    if (!TokenCanBeAssignedContext(tok)) {
      ++i;
      continue;
    }
    std::cerr
        << (i++) << '\t'
        << '\t' << std::hex << tok->opaque_source_loc << std::dec
        << '\t' << std::hex << tok->context_index << std::dec
        << '\t' << clang::tok::getTokenName(tok->Kind())
        << '\t' << tok->Data(*ast) << '\n';
  }

#endif

  for (changed = true; changed; ) {
    changed = false;
    matcher.MatchRegions(parsed_tree, printed_tree, changed);
  }

  for (size_t i = 0u; i < max_iters; ++i) {
    changed = false;
    join_based_region_merge(changed);
    if (!changed) {
      break;
    }
  }

  std::vector<TokenContextIndex> context_stack;
  context_stack.push_back(decl_context_id);
  matcher.FixContexts(parsed_tree, context_stack);

#if PASTA_DEBUG_ALIGN
  std::ofstream parsed_os("/tmp/tree.parsed", std::ios_base::trunc | std::ios_base::out);
  parsed_tree->Print(parsed_os, "", *ast, range);

  std::ofstream printed_os("/tmp/tree.printed", std::ios_base::trunc | std::ios_base::out);
  printed_tree->Print(printed_os, "", range);

  parsed_os.flush();
  printed_os.flush();
//    assert(false);
//
//  if (log) {
//    auto t = &(parsed_end[-1]);
//    for (auto c = t->Context(ast->contexts); c; c = c->Parent(ast->contexts)) {
//      auto c_id = static_cast<uint64_t>(c - &(ast->contexts[0]));
//      std::cerr << std::hex << c_id << std::dec << '\t' << c->KindName(ast->contexts) << ' ';
//      if (c->kind == TokenContextKind::kDecl) {
//        std::cerr << reinterpret_cast<const clang::Decl *>(c->data)->getDeclKindName();
//      }
//      std::cerr << '\n';
//    }
//
//    assert(false);
//  }

#endif   // PASTA_DEBUG_ALIGN

//  for (const auto &parsed_region : parsed_regions) {
//    auto balanced = dynamic_cast<BalancedRegion *>(parsed_region.get());
//    if (!balanced) {
//      continue;
//    }
//
//  }
  return std::monostate{};
}

// Try to align parsed tokens with printed tokens. See `AlignTokens.cpp`.
Result<AST, std::string> ASTImpl::AlignTokens(std::shared_ptr<ASTImpl> ast) {
  std::vector<clang::Decl *> work_list;
  std::vector<clang::Decl *> tlds;
  std::unordered_set<const clang::Decl *> ignore_decls;

  assert(ast->contexts.empty());

  ast->contexts.reserve(ast->tokens.size() * 32u);
  assert(static_cast<uint32_t>(ast->contexts.capacity()) ==
         ast->contexts.capacity());

  // Add a dummy context at the beginning. This is nifty so that we can use a
  // vector as a `context_map` below instead of an map.
  //
  // This is pretty sketchy, but place a raw reference back to the AST at the
  // beginning of the contexts list. Because `ASTImpl` extends
  // `std::enable_shared_from_this`, and because references to this vector in
  // token contexts alias the lifetime of the `ASTImpl`, we can safely go and
  // find the AST given any token context.
  (void) ast->contexts.emplace_back(*ast);

  work_list.push_back(ast->tu);
  while (!work_list.empty()) {
    clang::Decl * const decl = work_list.back();
    work_list.pop_back();
    switch (decl->getKind()) {
      case clang::Decl::TranslationUnit:
      case clang::Decl::LinkageSpec:
      case clang::Decl::ExternCContext:
      case clang::Decl::Namespace:
        for (auto sub_decl : clang::Decl::castToDeclContext(decl)->decls()) {
          work_list.push_back(sub_decl);
        }
        break;

//      // If it's something like `extern "C" int foo;` then we want to treat it
//      // as top-level, otherwise, it's more like `extern "C" { ... }` and so we
//      // want to find the top-level decls in the `...`.
//      case clang::Decl::LinkageSpec:
//        if (auto lsp = llvm::dyn_cast<clang::LinkageSpecDecl>(decl);
//            !lsp->hasBraces() && !lsp->isImplicit()) {
//
//          // Compute bounds of top-level decls. This will fill out
//          // `ast->lexically_containing_decl`.
//          (void) ast->DeclBounds(decl);
//          tlds.push_back(decl);
//
//        } else {
//          for (auto sub_decl : clang::Decl::castToDeclContext(decl)->decls()) {
//            work_list.push_back(sub_decl);
//          }
//        }
//        break;

      default:
        if (auto ftpl = clang::dyn_cast<clang::FunctionTemplateDecl>(decl)) {
          for (clang::Decl *spec : ftpl->specializations()) {
            ignore_decls.insert(Canonicalize(spec));
          }

        } else if (auto ctpl = clang::dyn_cast<clang::ClassTemplateDecl>(decl)) {
          for (clang::Decl *spec : ctpl->specializations()) {
            ignore_decls.insert(Canonicalize(spec));
          }

        } else if (auto vtpl = clang::dyn_cast<clang::VarTemplateDecl>(decl)) {
          for (clang::Decl *spec : vtpl->specializations()) {
            ignore_decls.insert(Canonicalize(spec));
          }
        }

        if (!decl->isImplicit()) {
          // Compute bounds of top-level decls. This will fill out
          // `ast->lexically_containing_decl`.
          (void) ast->DeclBounds(decl);
          tlds.push_back(decl);
        }
        break;
    }
  }

  // File explicit, user-written explicit template specializations, and ignore
  // all other specializations.
  auto should_keep = [&ignore_decls] (clang::Decl *decl) {
    auto tsk = clang::TSK_Undeclared;
    bool has_spec_or_partial = false;
    if (auto cspec = clang::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl);
        cspec && !clang::isa<clang::ClassTemplatePartialSpecializationDecl>(cspec)) {
      tsk = cspec->getSpecializationKind();
      has_spec_or_partial = !cspec->getSpecializedTemplateOrPartial().isNull();

    } else if (auto vspec = clang::dyn_cast<clang::VarTemplateSpecializationDecl>(decl);
               vspec && !clang::isa<clang::VarTemplatePartialSpecializationDecl>(vspec)) {
      tsk = vspec->getSpecializationKind();
      has_spec_or_partial = !vspec->getSpecializedTemplateOrPartial().isNull();

    } else if (auto fdecl = clang::dyn_cast<clang::FunctionDecl>(decl)) {
      tsk = fdecl->getTemplateSpecializationKind();

    } else if (auto vdecl = clang::dyn_cast<clang::VarDecl>(decl)) {
      tsk = vdecl->getTemplateSpecializationKind();

    } else if (auto ta = clang::dyn_cast<clang::TypeAliasDecl>(decl)) {
      if (ta->getDescribedAliasTemplate()) {
        tsk = clang::TSK_ImplicitInstantiation;  // Fake it.
      }

    } else {
      has_spec_or_partial = ignore_decls.count(Canonicalize(decl));
    }

    return IsExplicitInstantiation(tsk, has_spec_or_partial);
  };

  // Strip out template specializations.
  tlds.erase(
      std::partition(tlds.begin(), tlds.end(), should_keep),
      tlds.end());

  tlds.erase(std::unique(tlds.begin(), tlds.end()), tlds.end());

  std::stable_sort(
      tlds.begin(), tlds.end(),
      [ast = ast.get()] (clang::Decl *a, clang::Decl *b) {
        auto a_bounds = ast->DeclBounds(a);
        auto b_bounds = ast->DeclBounds(b);

        // If `a` starts first, put it first.
        if (a_bounds.first < b_bounds.first) {
          return true;

        } else if (a_bounds.first > b_bounds.first) {
          return false;

        // If `b` encloses `a`, sort `b` first.
        } else if (a_bounds.second < b_bounds.second) {
          return false;

        // If `a` encloses `b`, then sort `a` first.
        } else if (a_bounds.second > b_bounds.second) {
          return true;

        // Keep the relative order from `tlds`.
        } else {
          return false;
        }
      });

  std::unordered_multimap<const void *, TokenContextIndex> data_to_context;
  std::vector<TokenContextIndex> context_map;
  std::vector<clang::Decl *> tld_group;
  std::vector<const clang::Decl *> parentage;
  std::vector<TokenPrinterContext> context_stack;
  std::string data;
  auto &ast_context = ast->tu->getASTContext();

  for (auto tld_it = tlds.begin(), tld_end = tlds.end(); tld_it != tld_end; ) {
    clang::Decl *decl = *tld_it;
    clang::Decl *&containing_decl = ast->lexically_containing_decl[decl];
    if (!containing_decl) {
      containing_decl = decl;
    }

    tld_group.clear();
    tld_group.push_back(containing_decl);
    for (; tld_it != tld_end; ++tld_it) {
      clang::Decl *next_decl = *tld_it;
      if (containing_decl == ast->lexically_containing_decl[next_decl]) {
        if (next_decl != containing_decl) {
          tld_group.push_back(next_decl);
        }
      } else {
        break;
      }
    }

    assert(1u <= tld_group.size());

    // Go find the parentage of the containing decl. It is the path of lexical
    // declaration contexts down to the one containing the `containing_decl`
    // for this TLD group.
    parentage.clear();
    context_stack.clear();
    parentage.push_back(containing_decl);
    for (auto dc = containing_decl->getLexicalDeclContext(); dc;
         dc = dc->getLexicalParent()) {
      if (auto dc_decl = clang::dyn_cast<clang::Decl>(dc)) {
        parentage.push_back(dc_decl);
      }
    }

    // Initialize a new printed token range.
    data.clear();
    raw_string_ostream out(data, 0);
    PrintedTokenRangeImpl range(ast_context);
    range.ast = ast;

    clang::PrintingPolicy pp = *(ast->printing_policy);
    DeclPrinter printer(out, pp, ast_context, range);

    // Build up a stack of the parentage for these decls. There should at least
    // be the translation unit. This mimicks the call stack initialization of
    // the printers.
    const auto num_parents = parentage.size();
    assert(1u <= num_parents);
    context_stack.reserve(num_parents);
    for (auto pit = parentage.rbegin(), pend = parentage.rend();
        pit != pend; ++pit) {
      const clang::Decl *dc_decl = *pit;
      (void) context_stack.emplace_back(out, dc_decl, range);
    }

    // Force our current top of stack to be the canonical version of the
    // first decl in our decl group.
    assert(!context_stack.empty());
    const TokenContextIndex decl_context_id = context_stack.back().context_index;
    assert(decl_context_id != kInvalidTokenContextIndex);
    assert(decl_context_id < range.contexts.size());

    for (clang::Decl *tld_decl : tld_group) {
      printer.Visit(tld_decl);
    }

    // Unwind the tokenizer contexts (just in case the destructors still have
    // work to do).
    while (!context_stack.empty()) {
      context_stack.pop_back();
    }

    if (range.tokens.empty()) {
      continue;
    }

//      // Figure out the context for the declaration itself.
//      TokenContextIndex decl_context_id = kInvalidTokenContextIndex;
//
//      // Go find the context ID of the primary declaration for our first
//      // top-level decl in this group.
//      auto cdecl = Canonicalize(containing_decl);
//      for (auto [dc_context_it, dc_context_end] = data_to_context.equal_range(cdecl);
//           dc_context_it != dc_context_end; ++dc_context_it) {
//        TokenContextIndex cid = dc_context_it->second;
//        const auto &c = ast->contexts[cid];
//
//        // Make sure we find the right instance of this decl, at the right
//        // depth. It can easily happen that we find a version of this decl
//        // at a depth related to some internal usage.
//        if (c.data == cdecl && c.kind == TokenContextKind::kDecl &&
//            c.depth == (num_parents + 1u)) {
//          decl_context_id = cid;
//          break;
//        }
//      }

//      if (decl_context_id == kInvalidTokenContextIndex) {
//        assert(false);
//        decl_context_id = kTranslationUnitTokenContextIndex;
//      }
//
//      assert(decl_context_id != kInvalidTokenContextIndex);


//    if (auto tdecl = clang::dyn_cast<clang::ClassTemplateSpecializationDecl>(containing_decl);
//        tdecl && tdecl->getNameAsString() == "pair" &&
//        tdecl->isThisDeclarationADefinition()) {
//      log = true;
//    }

    // Figure out the parsed bounds. If we don't have bounds then we are
    // probably dealing with something like a namespace / linkage spec /
    // extern C, or an implicit declaration.
    auto decl_bounds = ast->DeclBounds(decl);
    if (!decl_bounds.first) {
      continue;
    }

//    auto first_tok = &(ast->tokens.front());
//    auto begin_offset = decl_bounds.first - first_tok;
//    auto end_offset = decl_bounds.second - first_tok;
//    auto num = (decl_bounds.second - decl_bounds.first) + 1;

//    pasta::Token ft(ast, ast->RawTokenAt(containing_decl->getLocation()));
//    if (auto fl = ft.FileLocation()) {
//      auto f = pasta::File::Containing(*fl);
//      std::cerr << f.Path().generic_string() << ':' << fl->Line() << ':' << fl->Column() << ": ";
//    }
//
//    std::cerr
//        << reinterpret_cast<const void *>(containing_decl)
//        << " [" << begin_offset << ", " << end_offset << "] (size="
//        << num << ") group=" << tld_group.size() << "\n";

    // Clear out the old token contexts. We'll possibly detect issues here.
    for (TokenImpl *t = decl_bounds.first; t <= decl_bounds.second; ++t) {
//      if (t->context_index != kInvalidTokenContextIndex) {
//        for (auto tt = decl_bounds.first; tt <= decl_bounds.second; ++tt) {
//          if (tt->Kind() == clang::tok::string_literal) {
//            std::cerr << "\"str\"";
//          } else {
//            std::cerr << tt->Data(*ast);
//          }
//          if (tt->context_index != kInvalidTokenContextIndex) {
//            std::cerr << "  <<<< " << tt->context_index;
//          }
//          std::cerr << '\n';
//        }
//        assert(false);
//      }
      assert(t->context_index == kInvalidTokenContextIndex);
      t->context_index = kInvalidTokenContextIndex;
    }

    auto res = AlignTokens(ast, decl_bounds.first, &(decl_bounds.second[1]),
                           range, decl_context_id);
    if (!res.Succeeded()) {
      return res.TakeError();
    }

    // Migrate the token contexts into the AST. This will migrate only the
    // contexts that were actually propagated into the parsed tokens, as
    // opposed to all token contexts.
    context_map.clear();
    context_map.resize(range.contexts.size());
    for (TokenImpl *t = decl_bounds.first; t <= decl_bounds.second; ++t) {
      t->context_index = MigrateContexts(
          t->context_index, range.contexts, ast->contexts,
          data_to_context, context_map);
      assert(t->context_index != kInvalidTokenContextIndex);
    }
  }

  ast->LinkMacroTokenContexts();

//  for (auto decl : tlds) {
//    auto &containing_decl = ast->lexically_containing_decl[decl];
//    if (!containing_decl) {
//      containing_decl = decl;
//    }
//    if (containing_decl == decl) {
//      auto res = AlignTokens(ast, decl, data_to_context);
//      if (!res.Succeeded()) {
//        return res.Error();
//      }
//    }
//  }

  return AST(std::move(ast));
}

}  // namespace pasta
