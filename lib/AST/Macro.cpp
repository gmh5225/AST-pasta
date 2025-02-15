/*
 * Copyright (c) 2022 Trail of Bits, Inc.
 */

#include "Macro.h"

#include <cassert>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbitfield-enum-conversion"
#pragma GCC diagnostic ignored "-Wimplicit-int-conversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshorten-64-to-32"
#include <clang/AST/ASTContext.h>
#include <clang/Basic/IdentifierTable.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/TokenKinds.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/Token.h>
#pragma GCC diagnostic pop

#if 1
#include <pasta/Util/File.h>
#include <iostream>
#define DEBUG(...) __VA_ARGS__
#else
#define DEBUG(...)
#endif

#include "AST.h"
#include "Token.h"

namespace pasta {
namespace {

static MacroTokenImpl *FirstUseTokenImpl(const std::vector<Node> &nodes) {
  if (nodes.empty()) {
    return nullptr;
  }

  for (const Node &node : nodes) {
    if (std::holds_alternative<MacroTokenImpl *>(node)) {
      return std::get<MacroTokenImpl *>(node);
    } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
      MacroNodeImpl *sub_node = std::get<MacroNodeImpl *>(node);
      if (auto ret = sub_node->FirstUseToken()) {
        return ret;
      }
    }
  }

  return nullptr;
}

static MacroTokenImpl *FirstExpansionTokenImpl(const std::vector<Node> &nodes) {
  if (nodes.empty()) {
    return nullptr;
  }

  for (const Node &node : nodes) {
    if (std::holds_alternative<MacroTokenImpl *>(node)) {
      return std::get<MacroTokenImpl *>(node);
    } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
      MacroNodeImpl *sub_node = std::get<MacroNodeImpl *>(node);
      if (auto ret = sub_node->FirstExpansionToken()) {
        return ret;
      }
    }
  }

  return nullptr;
}


static const Node *FirstTokenImpl(const std::vector<Node> &nodes) {
  if (nodes.empty()) {
    return nullptr;
  }

  for (const Node &node : nodes) {
    if (std::holds_alternative<MacroTokenImpl *>(node)) {
      return &node;

    } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
      MacroNodeImpl *sub_node = std::get<MacroNodeImpl *>(node);
      if (auto ret = sub_node->FirstToken()) {
        return ret;
      }
    }
  }

  return nullptr;
}

static const Node *LastTokenImpl(const std::vector<Node> &nodes) {
  if (nodes.empty()) {
    return nullptr;
  }

  for (auto it = nodes.rbegin(), end = nodes.rend(); it != end; ++it) {
    const Node &node = *it;
    if (std::holds_alternative<MacroTokenImpl *>(node)) {
      return &node;

    } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
      MacroNodeImpl *sub_node = std::get<MacroNodeImpl *>(node);
      if (auto ret = sub_node->LastToken()) {
        return ret;
      }
    }
  }

  return nullptr;
}

}  // namespace

// Clone this token into the AST.
MacroTokenImpl *MacroTokenImpl::Clone(ASTImpl &ast,
                                      MacroNodeImpl *new_parent) const {
  TokenImpl ast_tok = ast.tokens[token_offset];
  size_t new_offset = ast.tokens.size();
  ast.tokens.emplace_back(std::move(ast_tok));

  MacroTokenImpl *clone = &(ast.root_macro_node.tokens.emplace_back());
  clone->token_offset = static_cast<uint32_t>(new_offset);
  assert(clone->token_offset == new_offset);
  clone->parent = new_parent;
  clone->kind_flags.kind = kind_flags.kind;
  clone->kind_flags.is_ignored_comma = kind_flags.is_ignored_comma;

  // Associate the clone node's token offset with the macro definition.
  if (ast_tok.is_macro_name) {
    auto old_node_it = ast.tokens_to_macro_definitions.find(token_offset);
    if (old_node_it != ast.tokens_to_macro_definitions.end()) {
      ast.tokens_to_macro_definitions.emplace(
          new_offset, old_node_it->second);
    }
  }

  ast.preprocessed_code.push_back('\n');
  ast.num_lines += 1u;
  return clone;
}

MacroNodeImpl *MacroDirectiveImpl::Clone(
    ASTImpl &ast, MacroNodeImpl *new_parent) const {

  MacroDirectiveImpl *clone = &(ast.root_macro_node.directives.emplace_back());
  clone->cloned_from = this;
  clone->defined_macro = defined_macro;
  clone->included_file = included_file;
  clone->kind = kind;
  clone->is_skipped = is_skipped;
  clone->parent = new_parent;

  CloneNodeList(
      ast, this, nodes, clone, clone->nodes,
      [=] (unsigned i, MacroTokenImpl *tok, MacroTokenImpl *cloned_tok) {
        if (std::holds_alternative<MacroTokenImpl *>(directive_name) &&
            std::get<MacroTokenImpl *>(directive_name) == tok) {
          clone->directive_name = cloned_tok;
        }

        if (std::holds_alternative<MacroTokenImpl *>(macro_name) &&
            std::get<MacroTokenImpl *>(macro_name) == tok) {
          clone->macro_name = cloned_tok;
        }
      },
      NoOnNodeCB);

  return clone;
}

MacroNodeImpl *MacroArgumentImpl::Clone(
    ASTImpl &ast, MacroNodeImpl *new_parent) const {

  assert(!has_been_cloned);
  has_been_cloned = true;

  MacroArgumentImpl *clone = &(ast.root_macro_node.arguments.emplace_back());
  clone->cloned_from = this;
  if (auto expansion = dynamic_cast<MacroExpansionImpl *>(new_parent)) {
    clone->index = static_cast<unsigned>(expansion->arguments.size());
    clone->offset = static_cast<unsigned>(expansion->nodes.size());
    clone->is_prearg_expansion = expansion->is_prearg_expansion;
    expansion->arguments.emplace_back(clone);
  } else {
    assert(false);
    clone->index = index;
    clone->offset = offset;
    clone->is_prearg_expansion = is_prearg_expansion;
  }
  clone->parent = new_parent;

  CloneNodeList(ast, this, nodes, clone, clone->nodes, NoOnTokenCB,
                NoOnNodeCB);

  return clone;
}

MacroNodeImpl *MacroParameterImpl::Clone(ASTImpl &, MacroNodeImpl *) const {
  assert(false);
  return nullptr;
}

MacroNodeImpl *MacroSubstitutionImpl::Clone(
    ASTImpl &ast, MacroNodeImpl *new_parent) const {

  MacroSubstitutionImpl *clone =
      &(ast.root_macro_node.substitutions.emplace_back());
  clone->cloned_from = this;
  clone->parent = new_parent;

  CloneNodeList(ast, this, nodes, clone, clone->nodes, NoOnTokenCB, NoOnNodeCB);
  CloneNodeList(ast, this, use_nodes, clone, clone->use_nodes, NoOnTokenCB,
                NoOnNodeCB);

  return clone;
}

MacroNodeImpl *MacroExpansionImpl::Clone(
    ASTImpl &ast, MacroNodeImpl *new_parent) const {

  MacroExpansionImpl *clone =
      &(ast.root_macro_node.expansions.emplace_back());
  clone->cloned_from = this;
  clone->parent = new_parent;
  clone->definition = definition;
  clone->defined_macro = defined_macro;
  if (auto new_parent_exp = dynamic_cast<MacroExpansionImpl *>(new_parent)) {
    if (parent_for_prearg) {
      clone->parent_for_prearg = new_parent_exp;
    }
  } else {
    assert(!parent_for_prearg);
  }

  clone->is_cancelled = is_cancelled;
  clone->is_prearg_expansion = is_prearg_expansion;

  CloneNodeList(ast, this, nodes, clone, clone->nodes, NoOnTokenCB,
                NoOnNodeCB);

  unsigned arg_num = 0u;

  CloneNodeList(
      ast, this, use_nodes, clone, clone->use_nodes,
      [=, &arg_num] (unsigned i, MacroTokenImpl *tok,
                     MacroTokenImpl *cloned_tok) {
        if (ident == tok) {
          clone->ident = cloned_tok;
        }

        if (l_paren == tok) {
          clone->l_paren = cloned_tok;
        }

        if (r_paren == tok) {
          clone->r_paren = cloned_tok;
          clone->r_paren_index = i;
        }

        if (arg_num < arguments.size() &&
            std::holds_alternative<MacroTokenImpl *>(arguments[arg_num]) &&
            std::get<MacroTokenImpl *>(arguments[arg_num]) == tok) {
          clone->arguments.emplace_back(cloned_tok);
          ++arg_num;
        }
      },
      [=, &arg_num] (unsigned i, MacroNodeImpl *node,
          MacroNodeImpl *cloned_node) {

        if (ident == node->FirstExpansionToken()) {
          clone->ident = cloned_node->FirstExpansionToken();
        }

        if (l_paren == node->FirstExpansionToken()) {
          clone->l_paren = cloned_node->FirstExpansionToken();
        }

        if (r_paren == node->FirstExpansionToken()) {
          clone->r_paren = cloned_node->FirstExpansionToken();
          clone->r_paren_index = i;
        }

        if (arg_num < arguments.size() &&
            std::holds_alternative<MacroNodeImpl *>(arguments[arg_num]) &&
            std::get<MacroNodeImpl *>(arguments[arg_num]) == node) {
          clone->arguments.emplace_back(cloned_node);
          ++arg_num;
        }
      });

  return clone;
}

MacroNodeImpl *RootMacroNode::Clone(ASTImpl &, MacroNodeImpl *) const {
  abort();
  __builtin_unreachable();
}

const Node *MacroNodeImpl::FirstToken(void) const {
  return FirstTokenImpl(nodes);
}

const Node *MacroSubstitutionImpl::FirstToken(void) const {
  if (auto tok = FirstTokenImpl(use_nodes)) {
    return tok;
  } else {
    return FirstTokenImpl(nodes);
  }
}

const Node *MacroNodeImpl::LastToken(void) const {
  return LastTokenImpl(nodes);
}

const Node *MacroSubstitutionImpl::LastToken(void) const {
  if (auto tok = LastTokenImpl(use_nodes)) {
    return tok;
  } else {
    return LastTokenImpl(nodes);
  }
}

MacroTokenImpl *MacroNodeImpl::FirstUseToken(void) const {
  return FirstUseTokenImpl(nodes);
}

MacroTokenImpl *MacroSubstitutionImpl::FirstUseToken(void) const {
  if (!use_nodes.empty()) {
    return FirstUseTokenImpl(use_nodes);
  } else {
    return FirstUseTokenImpl(nodes);  // An in-progress node.
  }
}


MacroTokenImpl *MacroNodeImpl::FirstExpansionToken(void) const {
  return FirstExpansionTokenImpl(nodes);
}

MacroTokenImpl *MacroSubstitutionImpl::FirstExpansionToken(void) const {
  if (!nodes.empty()) {
    return FirstExpansionTokenImpl(nodes);
  } else {
    return FirstExpansionTokenImpl(use_nodes);
  }
}

Macro::~Macro(void) {}

MacroKind Macro::Kind(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  if (std::holds_alternative<MacroTokenImpl *>(node)) {
    return MacroKind::kToken;
  } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
    return std::get<MacroNodeImpl *>(node)->kind;
  } else {
    assert(false);
    DEBUG( std::cerr << "Bad macro kind on main file: "
                     << ast->main_source_file.Path().generic_string() << '\n'; )
    abort();
    __builtin_unreachable();
  }
}

const void *Macro::RawMacro(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  if (std::holds_alternative<MacroTokenImpl *>(node)) {
    return &(ast->tokens[std::get<MacroTokenImpl *>(node)->token_offset]);
  } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
    auto ret = std::get<MacroNodeImpl *>(node);
    assert(ret != nullptr);
    return ret;
  } else {
    assert(false);
    DEBUG( std::cerr << "Bad macro kind on main file: "
                 << ast->main_source_file.Path().generic_string() << '\n'; )
    return nullptr;
  }
}

// Return the macro node containing this node.
std::optional<Macro> Macro::Parent(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  if (std::holds_alternative<MacroTokenImpl *>(node)) {
    return Macro(ast, &(std::get<MacroTokenImpl *>(node)->parent));
  }

  if (!std::holds_alternative<MacroNodeImpl *>(node)) {
    return std::nullopt;
  }

  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  if (!std::holds_alternative<MacroNodeImpl *>(node_impl->parent)) {
    return std::nullopt;
  }

  MacroNodeImpl *parent_node_impl = std::get<MacroNodeImpl *>(node_impl->parent);
  if (dynamic_cast<RootMacroNode *>(parent_node_impl)) {
    return std::nullopt;
  }

  return Macro(ast, &(node_impl->parent));
}

// Children of this macro. If this is a MacroToken then this is empty.
MacroRange Macro::Children(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  if (std::holds_alternative<MacroNodeImpl *>(node)) {
    MacroNodeImpl *impl = std::get<MacroNodeImpl *>(node);
    MacroSubstitutionImpl *sub_impl =
        dynamic_cast<MacroSubstitutionImpl *>(impl);
    if (sub_impl) {
      const auto first = sub_impl->use_nodes.data();
      return MacroRange(ast, first, &(first[sub_impl->use_nodes.size()]));
    } else {
      const auto first = impl->nodes.data();
      return MacroRange(ast, first, &(first[impl->nodes.size()]));
    }
  }
  return MacroRange(ast);
}

std::optional<MacroToken> Macro::BeginToken(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  if (std::holds_alternative<MacroTokenImpl *>(node)) {
    return reinterpret_cast<const MacroToken &>(*this);
  } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
    if (const Node *tok = std::get<MacroNodeImpl *>(node)->FirstToken()) {
      return MacroToken(ast, tok);
    }
  } else {
    assert(false);
    DEBUG( std::cerr << "Bad macro kind on main file: "
                 << ast->main_source_file.Path().generic_string() << '\n'; )
  }
  return std::nullopt;
}

std::optional<MacroToken> Macro::EndToken(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  if (std::holds_alternative<MacroTokenImpl *>(node)) {
    return reinterpret_cast<const MacroToken &>(*this);
  } else if (std::holds_alternative<MacroNodeImpl *>(node)) {
    if (const Node *tok = std::get<MacroNodeImpl *>(node)->LastToken()) {
      return MacroToken(ast, tok);
    }
  } else {
    assert(false);
    DEBUG( std::cerr << "Bad macro kind on main file: "
                 << ast->main_source_file.Path().generic_string() << '\n'; )
  }
  return std::nullopt;
}

enum TokenKind MacroToken::TokenKind(void) const noexcept {
  return ParsedLocation().Kind();
}

std::string_view MacroToken::TokenKindName(void) const noexcept {
  return clang::tok::getTokenName(
      static_cast<clang::tok::TokenKind>(TokenKind()));
}

// Return the data associated with this token.
std::string_view MacroToken::Data(void) const noexcept {
  return ParsedLocation().Data();
}

// Location of the token in a file.
std::optional<FileToken> MacroToken::FileLocation(void) const noexcept {
  return ParsedLocation().FileLocation();
}

// Location of the token as parsed.
Token MacroToken::ParsedLocation(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  auto offset = std::get<MacroTokenImpl *>(node)->token_offset;
  if (offset >= ast->tokens.size()) {
    assert(false);  // Not sure what's going on here.
    return Token(ast);
  }

  const TokenImpl &tok = ast->tokens[offset];
  if (!tok.HasMacroRole()) {
    assert(false);  // Not sure what's going on here.
    return Token(ast);
  }

  return Token(ast, &tok);
}

std::optional<MacroDirective> MacroDirective::From(const Macro &node) noexcept {
  switch (node.Kind()) {
#define PASTA_IGNORE(...)
#define PASTA_MAKE_DIRECTIVE(kind) \
    case MacroKind::k ## kind ## Directive:

PASTA_FOR_EACH_MACRO_IMPL(PASTA_IGNORE,
                          PASTA_IGNORE,
                          PASTA_MAKE_DIRECTIVE,
                          PASTA_MAKE_DIRECTIVE,
                          PASTA_MAKE_DIRECTIVE,
                          PASTA_MAKE_DIRECTIVE,
                          PASTA_IGNORE)
#undef PASTA_IGNORE
#undef PASTA_MAKE_DIRECTIVE
      return reinterpret_cast<const MacroDirective &>(node);
    default:
      return std::nullopt;
  }
}

// Return the hash token of the directive.
MacroToken MacroDirective::Hash(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);

  assert(!dir_impl->nodes.empty());
  assert(std::holds_alternative<MacroTokenImpl *>(dir_impl->nodes.front()));
  return MacroToken(ast, &(dir_impl->nodes.front()));
}

// The name of this directive, if any. Some GCC-specific macros don't have
// names. Directive names are macro tokens because the name may be a result
// of some other expansion, e.g. `_Pragma("...")` expanding into
// `#pragma ...`.
std::optional<MacroToken> MacroDirective::DirectiveName(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);

  if (!std::holds_alternative<MacroTokenImpl *>(dir_impl->directive_name)) {
    return std::nullopt;
  }

  return MacroToken(ast, &(dir_impl->directive_name));
}

// E.g. `...` in `args...`, or just `...`.
std::optional<MacroToken>
MacroParameter::VariadicDots(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroParameterImpl *param_impl = dynamic_cast<MacroParameterImpl *>(node_impl);
  if (param_impl->is_variadic) {
    return MacroToken(ast, &(param_impl->nodes.back()));
  } else {
    return std::nullopt;
  }
}

// The name of the macro parameter, if any.
std::optional<MacroToken> MacroParameter::Name(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroParameterImpl *param_impl = dynamic_cast<MacroParameterImpl *>(node_impl);
  if (param_impl->is_variadic) {
    return MacroToken(ast, &(param_impl->nodes.front()));
  } else {
    return std::nullopt;
  }
}

unsigned MacroParameter::Index(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroParameterImpl *param_impl = dynamic_cast<MacroParameterImpl *>(node_impl);
  return param_impl->index;
}

std::optional<DefineMacroDirective>
DefineMacroDirective::From(const Macro &node) {
  if (node.Kind() == MacroKind::kDefineDirective) {
    return reinterpret_cast<const DefineMacroDirective &>(node);
  } else {
    return std::nullopt;
  }
}

std::optional<MacroToken> DefineMacroDirective::Name(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  if (std::holds_alternative<MacroTokenImpl *>(dir_impl->macro_name)) {
    return MacroToken(ast, &(dir_impl->macro_name));
  } else {
    assert(std::holds_alternative<std::monostate>(dir_impl->macro_name));
    return std::nullopt;
  }
}

// Number of explicit, i.e. not variadic, parameters.
unsigned DefineMacroDirective::NumExplicitParameters(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  if (dir_impl->defined_macro) {

    // NOTE(pag): Clang includes the variadic parameter in `getNumParams()`.
    return dir_impl->defined_macro->getNumParams() - 1u;
  } else {
    assert(false);
    return 0u;
  }
}

// Is this a function-like macro? If so, then it could take zero-or-more
// arguments when used.
bool DefineMacroDirective::IsFunctionLike(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  if (dir_impl->defined_macro) {
    return dir_impl->defined_macro->isFunctionLike();
  } else {
    assert(false);
    return 0u;
  }
}

// Does this definition accept a variable number of arguments?
bool DefineMacroDirective::IsVariadic(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  if (dir_impl->defined_macro) {
    return dir_impl->defined_macro->isVariadic();
  } else {
    assert(false);
    return false;
  }
}

// Uses of this macro.
MacroRange DefineMacroDirective::Uses(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  if (!dir_impl->macro_uses.empty()) {
    const auto first = dir_impl->macro_uses.data();
    return MacroRange(
        ast, first, &(first[dir_impl->macro_uses.size()]));
  } else {
    return MacroRange(ast);
  }
}

// Body of the defined macro.
MacroRange DefineMacroDirective::Body(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  if (!dir_impl->nodes.empty()) {
    const auto first = dir_impl->nodes.data();
    const auto num_nodes = dir_impl->nodes.size();
    const auto body_offset = dir_impl->body_offset;
    if (body_offset >= num_nodes) {
      return MacroRange(ast);
    }
    return MacroRange(ast, &(first[body_offset]), &(first[num_nodes]));
  } else {
    return MacroRange(ast);
  }
}

// Parameters of this macro definition.
MacroRange DefineMacroDirective::Parameters(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  if (!dir_impl->parameters.empty()) {
    const auto first = dir_impl->parameters.data();
    return MacroRange(
        ast, first, &(first[dir_impl->parameters.size()]));
  } else {
    return MacroRange(ast);
  }
}

std::optional<IncludeLikeMacroDirective>
IncludeLikeMacroDirective::From(const Macro &node) {
  switch (node.Kind()) {
    case MacroKind::kIncludeDirective:
    case MacroKind::kIncludeNextDirective:
    case MacroKind::kIncludeMacrosDirective:
      return reinterpret_cast<const IncludeLikeMacroDirective &>(node);
    default:
      return std::nullopt;
  }
}

std::optional<ConditionalMacroDirective> ConditionalMacroDirective::From(
    const Macro &node) noexcept {
  switch (node.Kind()) {
    default: return std::nullopt;
#define PASTA_IGNORE(...)
#define PASTA_CMD_CAST(kind) case MacroKind::k ## kind ## Directive:
  PASTA_FOR_EACH_MACRO_IMPL(PASTA_IGNORE,
                            PASTA_IGNORE,
                            PASTA_IGNORE,
                            PASTA_CMD_CAST,
                            PASTA_IGNORE,
                            PASTA_IGNORE,
                            PASTA_IGNORE)
#undef PASTA_CMD_CAST
#undef PASTA_IGNORE
    return reinterpret_cast<const ConditionalMacroDirective &>(node);
  }
}

std::optional<File> IncludeLikeMacroDirective::IncludedFile(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroDirectiveImpl *dir_impl = dynamic_cast<MacroDirectiveImpl *>(node_impl);
  return dir_impl->included_file;
}

bool MacroArgument::IsVariadic(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroArgumentImpl *arg_impl = dynamic_cast<MacroArgumentImpl *>(node_impl);
  MacroExpansionImpl *exp_impl = dynamic_cast<MacroExpansionImpl *>(
      std::get<MacroNodeImpl *>(arg_impl->parent));
  if (exp_impl->defined_macro) {
    return exp_impl->defined_macro->getNumParams() <= arg_impl->index;
  }
  return false;
}

unsigned MacroArgument::Index(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroArgumentImpl *arg_impl = dynamic_cast<MacroArgumentImpl *>(node_impl);
  MacroNodeImpl *exp_node = std::get<MacroNodeImpl *>(arg_impl->parent);
  MacroExpansionImpl *exp_impl = dynamic_cast<MacroExpansionImpl *>(exp_node);
  auto i = 0u;
  for (const Node &arg_node : exp_impl->arguments) {
    if (std::get<MacroNodeImpl *>(arg_node) == node_impl) {
      return i;
    }
    ++i;
  }
  assert(false);
  return ~0u;
}

MacroRange MacroSubstitution::ReplacementChildren(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroSubstitutionImpl *sub_impl =
      dynamic_cast<MacroSubstitutionImpl *>(node_impl);
  if (!sub_impl->nodes.empty()) {
    const auto first = sub_impl->nodes.data();
    return MacroRange(
        ast, first, &(first[sub_impl->nodes.size()]));
  } else {
    return MacroRange(ast);
  }
}

MacroExpansion MacroExpansion::Containing(
    const MacroArgument &arg) noexcept {
  Node node = *reinterpret_cast<const Node *>(arg.impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroArgumentImpl *arg_impl = dynamic_cast<MacroArgumentImpl *>(node_impl);
  return MacroExpansion(arg.ast, &(arg_impl->parent));
}

// Returns the directive that led to the definition of this expansion.
std::optional<DefineMacroDirective> MacroExpansion::Definition(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroExpansionImpl *exp_impl = dynamic_cast<MacroExpansionImpl *>(node_impl);
  if (std::holds_alternative<MacroNodeImpl *>(exp_impl->definition)) {
    return DefineMacroDirective(ast, &(exp_impl->definition));
  } else {
    return std::nullopt;
  }
}

// Returns the list of arguments in the expansion if this was a use of a
// function-like macro.
std::vector<MacroArgument>
MacroExpansion::Arguments(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroExpansionImpl *exp_impl = dynamic_cast<MacroExpansionImpl *>(node_impl);

  std::vector<MacroArgument> ret;
  for (Node &arg : exp_impl->arguments) {
    ret.emplace_back(MacroArgument(ast, &arg));
  }

  return ret;
}

// Is this the argument pre-expansion phase of this expansion?
bool MacroExpansion::IsArgumentPreExpansion(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroExpansionImpl *exp_impl = dynamic_cast<MacroExpansionImpl *>(node_impl);
  return exp_impl->is_prearg_expansion;
}

// Return the "second" (if any) version of this expansion, where the
// arguments to this macro are subjected to pre-expansion prior to
// substituting the use of the macro with its body.
std::optional<MacroExpansion>
MacroExpansion::ArgumentPreExpansion(void) const noexcept {
  Node node = *reinterpret_cast<const Node *>(impl);
  MacroNodeImpl *node_impl = std::get<MacroNodeImpl *>(node);
  MacroExpansionImpl *exp_impl = dynamic_cast<MacroExpansionImpl *>(node_impl);

  if (exp_impl->nodes.empty()) {
    return std::nullopt;
  }

  Node &pa_node = exp_impl->nodes.front();
  if (!std::holds_alternative<MacroNodeImpl *>(pa_node)) {
    return std::nullopt;
  }

  MacroNodeImpl *pa_node_impl = std::get<MacroNodeImpl *>(pa_node);
  MacroExpansionImpl *pa_exp_impl =
      dynamic_cast<MacroExpansionImpl *>(pa_node_impl);

  if (!pa_exp_impl || pa_exp_impl->parent_for_prearg != exp_impl) {
    return std::nullopt;
  }

  return MacroExpansion(ast, &pa_node);
}

// Prefix increment operator.
MacroIterator &MacroIterator::operator++(void) noexcept {
  node.impl = reinterpret_cast<const Node *>(node.impl) + 1;
  return *this;
}

// Postfix increment operator.
MacroIterator MacroIterator::operator++(int) noexcept {
  auto ret = *this;
  node.impl = reinterpret_cast<const Node *>(node.impl) + 1;
  return ret;
}

// Prefix decrement operator.
MacroIterator &MacroIterator::operator--(void) noexcept {
  node.impl = reinterpret_cast<const Node *>(node.impl) - 1;
  return *this;
}

// Postfix decrement operator.
MacroIterator MacroIterator::operator--(int) noexcept {
  auto ret = *this;
  node.impl = reinterpret_cast<const Node *>(node.impl) - 1;
  return ret;
}

MacroIterator MacroIterator::operator-(size_t offset) const noexcept {
  return MacroIterator(
      node.ast, reinterpret_cast<const Node *>(node.impl) - offset);
}

MacroIterator &MacroIterator::operator+=(size_t offset) noexcept {
  node.impl = reinterpret_cast<const Node *>(node.impl) + offset;
  return *this;
}

MacroIterator &MacroIterator::operator-=(size_t offset) noexcept {
  node.impl = reinterpret_cast<const Node *>(node.impl) - offset;
  return *this;
}

Macro MacroIterator::operator[](size_t offset) const noexcept {
  auto begin = reinterpret_cast<const Node *>(node.impl);
  return Macro(node.ast, &(begin[offset]));
}

ptrdiff_t MacroIterator::operator-(const MacroIterator &that) const noexcept {
  return reinterpret_cast<const Node *>(node.impl) -
         reinterpret_cast<const Node *>(that.node.impl);
}

size_t MacroRange::Size(void) const noexcept {
  return static_cast<size_t>(reinterpret_cast<const Node *>(after_last) -
                             reinterpret_cast<const Node *>(first));
}

// Return the `index`th token in this range. If `index` is too big, then
// return nothing.
std::optional<Macro> MacroRange::At(size_t index) const noexcept {
  auto begin = reinterpret_cast<const Node *>(first);
  auto end = reinterpret_cast<const Node *>(after_last);
  if (auto it = &(begin[index]); it < end) {
    return Macro(ast, it);
  } else {
    return std::nullopt;
  }
}

// Unsafe indexed access into the token range.
Macro MacroRange::operator[](size_t index) const {
  auto begin = reinterpret_cast<const Node *>(first);
  return Macro(ast, &(begin[index]));
}

}  // namespace pasta
