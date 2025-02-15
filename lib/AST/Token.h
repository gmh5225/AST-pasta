/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 */

#pragma once

#include <pasta/AST/Token.h>
#include <pasta/AST/Forward.h>

#include <cstdint>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbitfield-enum-conversion"
#pragma GCC diagnostic ignored "-Wimplicit-int-conversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/TokenKinds.h>
#pragma GCC diagnostic pop

namespace clang {
class ASTContext;
class SourceManager;
class LangOptions;
class Token;
} // namespace clang

namespace pasta {

class ASTImpl;
class Token;
class PrintedTokenImpl;
class PrintedTokenRangeImpl;

using OpaqueSourceLoc = clang::SourceLocation::UIntTy;
using TokenContextIndex = uint32_t;
using DerivedTokenIndex = uint32_t;
using TokenDataOffset = int32_t;
using TokenDataIndex = uint32_t;
static constexpr DerivedTokenIndex kInvalidDerivedTokenIndex = ~0u;
static constexpr TokenContextIndex kInvalidTokenContextIndex = ~0u;
static constexpr TokenContextIndex kASTTokenContextIndex = 0u;
static constexpr TokenContextIndex kTranslationUnitTokenContextIndex = 1u;

inline static const clang::Decl *Canonicalize(const clang::Decl *decl) {
  return decl->getCanonicalDecl();
}

template <typename T>
inline static const T *Canonicalize(const T *other) {
  return other;
}

// Backing data for a token context.
class TokenContextImpl {
 public:
  const void *data;
  TokenContextIndex parent_index;
  uint16_t depth;
  TokenContextKind kind;

  // Return the common ancestor between two contexts. This focuses on the data
  // itself, so if there are two distinct contexts sharing the same data, or
  // aliasing the same data, the context associated with the second token is
  // returned.
  static const TokenContextImpl *CommonAncestor(
      const TokenContextImpl *a, const TokenContextImpl *b,
      const std::vector<TokenContextImpl> &contexts);

  // Return the common ancestor between two tokens. This focuses on the data
  // itself, so if there are two distinct contexts sharing the same data, or
  // aliasing the same data, the context associated with the second token is
  // returned.
  static const TokenContextImpl *CommonAncestor(
      TokenImpl *a, TokenImpl *b,
      const std::vector<TokenContextImpl> &contexts);

  const TokenContextImpl *Parent(
      const std::vector<TokenContextImpl> &contexts) const;

  const TokenContextImpl *Aliasee(
      const std::vector<TokenContextImpl> &contexts) const;

  const char *KindName(
      const std::vector<TokenContextImpl> &contexts) const;

  inline TokenContextImpl(const void *data_, TokenContextIndex parent_index_,
                          unsigned depth_, TokenContextKind kind_)
      : data(data_),
        parent_index(parent_index_),
        depth(static_cast<uint16_t>(depth_)),
        kind(kind_) {}

#define PASTA_DEFINE_TOKEN_CONTEXT_CONSTRUCTOR(cls) \
    inline TokenContextImpl(TokenContextIndex parent_index_, \
                            uint16_t parent_depth, \
                            const clang::cls *data_) \
        : TokenContextImpl(Canonicalize(data_), parent_index_, \
                           parent_depth + 1u, TokenContextKind::k ## cls) {}
  PASTA_FOR_EACH_TOKEN_CONTEXT_KIND(PASTA_DEFINE_TOKEN_CONTEXT_CONSTRUCTOR)
#undef PASTA_DEFINE_TOKEN_CONTEXT_CONSTRUCTOR

  inline TokenContextImpl(TokenContextIndex parent_index_,
                          uint16_t parent_depth,
                          const char *data_)
      : TokenContextImpl(data_, parent_index_,
                         parent_depth + 1u, TokenContextKind::kString) {}

  inline TokenContextImpl(TokenContextIndex parent_index_,
                          uint16_t parent_depth,
                          TokenContextIndex aliasee_)
      : TokenContextImpl(reinterpret_cast<const void *>(aliasee_),
                         parent_index_, parent_depth + 1u,
                         TokenContextKind::kAlias) {}

  inline TokenContextImpl(TokenContextIndex parent_index_, \
                          uint16_t parent_depth, \
                          const clang::DesignatedInitExpr::Designator *data_) \
      : TokenContextImpl(Canonicalize(data_), parent_index_, \
                         parent_depth + 1u, TokenContextKind::kDesignator) {} \

  // Special context that we place at the end of a vector.
  inline TokenContextImpl(ASTImpl &ast)
      : TokenContextImpl(reinterpret_cast<const void *>(&ast),
                         kInvalidTokenContextIndex,
                         0u,
                         TokenContextKind::kAST) {}
};

using TokenKindBase = std::underlying_type_t<clang::tok::TokenKind>;

// Backing implementation of a token.
class TokenImpl {
 public:
  static constexpr OpaqueSourceLoc kInvalidSourceLocation = 0u;

  static constexpr uint32_t kTokenSizeMask = ((1u << 20) - 1u);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
  inline TokenImpl(OpaqueSourceLoc opaque_source_loc_, int32_t data_offset_,
                   uint32_t data_len_, clang::tok::TokenKind kind_,
                   TokenRole role_, TokenContextIndex token_context_index_=kInvalidTokenContextIndex)
      : opaque_source_loc(opaque_source_loc_),
        context_index(token_context_index_),
        data_offset(data_offset_),
        data_len(static_cast<uint32_t>(data_len_ & kTokenSizeMask)),
        kind(static_cast<TokenKindBase>(kind_)),
        role(static_cast<TokenKindBase>(role_)),
        is_macro_name(0),
        is_in_pragma_directive(0) {}
#pragma GCC diagnostic pop

  // Return the source location of this token.
  inline clang::SourceLocation Location(void) const {
    return clang::SourceLocation::getFromRawEncoding(opaque_source_loc);
  }

  std::string_view Data(const ASTImpl &ast) const noexcept;
  std::string_view Data(const PrintedTokenRangeImpl &range) const noexcept;

  inline TokenRole Role(void) const noexcept {
    return static_cast<TokenRole>(role);
  }

  inline bool IsParsed(void) const noexcept {
    switch (Role()) {
      case TokenRole::kInvalid:
      case TokenRole::kBeginOfFileMarker:
      case TokenRole::kEndOfFileMarker:
      case TokenRole::kBeginOfMacroExpansionMarker:
      case TokenRole::kEndOfMacroExpansionMarker:
      case TokenRole::kInitialMacroUseToken:
      case TokenRole::kIntermediateMacroExpansionToken:
      case TokenRole::kEndOfInternalMacroEventMarker:
        return false;

      case TokenRole::kFinalMacroExpansionToken:
      case TokenRole::kFileToken:
        // Clang supports the following:
        //
        //    #pragma attribute push(__attribute__((....)), apply_to = (...))
        //    ...
        //
        // In this case, we don't want to let the locations of any of these
        // attributes influence the locations of the declarations enclosed by
        // this pragma.
        //
        // Although pragmas are indeed parsed, we "hide" their tokens from the
        // ASTs via some the `PatchedMacroTracker`: when a pragma directive is
        // finished, we inject a zero-length marker token, and also render the
        // full, macro-expanded directive into `ASTImpl::preprocessed_code`.
        // These pragmas are visible to Clang's Sema, but not to our parsed
        // token list.
        return !is_in_pragma_directive && data_len;
    }
  }

  inline bool HasMacroRole(void) const noexcept {
    switch (Role()) {
      case TokenRole::kBeginOfMacroExpansionMarker:
      case TokenRole::kInitialMacroUseToken:
      case TokenRole::kIntermediateMacroExpansionToken:
      case TokenRole::kFinalMacroExpansionToken:
      case TokenRole::kEndOfMacroExpansionMarker:
      case TokenRole::kEndOfInternalMacroEventMarker:
        return true;
      default:
        return false;
    }
  }

  inline clang::tok::TokenKind Kind(void) const noexcept {
    return static_cast<clang::tok::TokenKind>(kind);
  }

  // Return the context of this token, or `nullptr`.
  const TokenContextImpl *Context(
      const ASTImpl &ast,
      const std::vector<TokenContextImpl> &contexts) const noexcept;

  // If this number is positive, then it is the raw encoding of the source
  // location of the token, which references a `FileToken`. If this number is
  // negative, then this token was derived from a prior token in a macro
  // expansion. That prior token is at `ast->tokens[-opaque_source_loc]`. This
  // process is enacted by `PatchedMacroTracker::FixupDerivedLocations`. If the
  // index points to itself, then it's a macro token that makes it into the
  // final parse (and is thus relevant to token alignment), but that also
  // doesn't have any associated source location, e.g. how `__FILE__` expands to
  // a provenanceless string.
  //
  // TODO(pag): This is pretty terrible. There are at least three or four
  //            possible interpretations of this value, depending on the context
  //            (macro, not macro), timing (during expansion, after expansion),
  //            etc. This is a format error, where I should just store more data
  //            but stubbornly just leave things according to the old design.
  OpaqueSourceLoc opaque_source_loc{kInvalidSourceLocation};

  DerivedTokenIndex derived_index{kInvalidDerivedTokenIndex};

  // Index of the token context in either `ASTImpl::contexts` or
  // `PrintedTokenRangeImpl::contexts`.
  //
  // If `HasMacroRole()` is `true`, then the real token context index is stored
  // in `MacroTokenImpl::token_context` and this index references into
  // `ASTImple::root_macro_node::tokens`.
  TokenContextIndex context_index{kInvalidTokenContextIndex};

  // Offset and length of this token's data. If `data_offset` is positive, then
  // the data is located in `ast->preprocessed_code`, otherwise it's located in
  // `ast->backup_code`.
  TokenDataOffset data_offset{0u};

  // The Linux kernel has some *massive* comments, e.g. comments in
  // `tools/include/uapi/linux/bpf.h`.
  uint32_t data_len;

  // The original token kind.
  TokenKindBase kind;

  // The role of this token, e.g. parsed, printed, macro expansion, etc.
  TokenKindBase role:14;

  // Is this token associated with a macro definition? If so, then we have a
  // lookup mechanism in `ASTImpl` to go from the token index to the macro
  // definition.
  TokenKindBase is_macro_name:1;

  // Is this token part of a `#pragma` macro directive region?
  //
  // Clang supports the following:
  //
  //    #pragma attribute push(__attribute__((....)), apply_to = (...))
  //    ...
  //
  // In this case, we don't want to let the locations of any of these
  // attributes influence the locations of the declarations enclosed by
  // this `#pragma`.
  TokenKindBase is_in_pragma_directive:1;
};

// Strip off leading whitespace from a token that has been read.
void SkipLeadingWhitspace(clang::Token &tok, clang::SourceLocation &tok_loc,
                          std::string &tok_data);

// Read the data of the token into the passed in string pointer
bool TryReadRawToken(clang::SourceManager &source_manager,
                     const clang::LangOptions &lang_opts,
                     const clang::Token &tok, std::string *out);

// Try to lex the data at `loc` into the token `*out`.
bool TryLexRawToken(clang::SourceManager &source_manager,
                    const clang::LangOptions &lang_opts,
                    clang::SourceLocation loc, clang::Token *out);

// Try to lex the data at `loc` into the token `*out`.
bool TryLexRawToken(clang::ASTContext &ast_context,
                    clang::SourceLocation loc, clang::Token *out);

} // namespace pasta
