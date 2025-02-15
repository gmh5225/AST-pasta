/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 */

#include <fstream>
#include <ostream>
#include <string>

#include "BootstrapConfig.h"
#include "Globals.h"
#include "Util.h"

extern void DefineCppMethods(std::ostream &os, const std::string &class_name,
                              uint32_t class_id);

// Generate `lib/AST/Attr.cpp`.
void GenerateAttrCpp(void) {
  std::ofstream os(kASTAttrCpp);

  os
      << "/*\n"
      << " * Copyright (c) 2022 Trail of Bits, Inc.\n"
      << " */\n\n"
      << "// This file is auto-generated.\n\n"
      << "#define PASTA_IN_ATTR_CPP\n"
      << "#ifndef PASTA_IN_BOOTSTRAP\n"
      << "#pragma clang diagnostic push\n"
      << "#pragma clang diagnostic ignored \"-Wimplicit-int-conversion\"\n"
      << "#pragma clang diagnostic ignored \"-Wsign-conversion\"\n"
      << "#pragma clang diagnostic ignored \"-Wshorten-64-to-32\"\n"
      << "#include <clang/AST/Attr.h>\n"
      << "#include <clang/Frontend/CompilerInstance.h>\n"
      << "#pragma clang diagnostic pop\n\n"
      << "#include <pasta/AST/Attr.h>\n"
      << "#include \"AST.h\"\n"
      << "#include \"Builder.h\"\n\n"
      << "#define PASTA_DEFINE_BASE_OPERATORS(base, derived) \\\n"
      << "    std::optional<class derived> derived::From(const class base &that) { \\\n"
      << "      if (auto attr_ptr = clang::dyn_cast_or_null<clang::derived>(that.u.base)) { \\\n"
      << "        return AttrBuilder::Create<class derived>(that.ast, attr_ptr); \\\n"
      << "      } else { \\\n"
      << "        return std::nullopt; \\\n"
      << "      } \\\n"
      << "    }\n\n"
      << "#define PASTA_DEFINE_DERIVED_OPERATORS(base, derived) \\\n"
      << "    base::base(const class derived &that) \\\n"
      << "        : base(that.ast, that.u.Attr) {} \\\n"
      << "    base::base(class derived &&that) noexcept \\\n"
      << "        : base(std::move(that.ast), that.u.Attr) {} \\\n"
      << "    base &base::operator=(const class derived &that) { \\\n"
      << "      if (ast != that.ast) { \\\n"
      << "        ast = that.ast; \\\n"
      << "      } \\\n"
      << "      u.Attr = that.u.Attr; \\\n"
      << "      kind = that.kind; \\\n"
      << "      return *this; \\\n"
      << "    } \\\n"
      << "    base &base::operator=(class derived &&that) noexcept { \\\n"
      << "      class derived new_that(std::forward<class derived>(that)); \\\n"
      << "      ast = std::move(new_that.ast); \\\n"
      << "      u.Attr = new_that.u.Attr; \\\n"
      << "      kind = new_that.kind; \\\n"
      << "      return *this; \\\n"
      << "    }\n\n"
      << "namespace pasta {\n\n"
      << "namespace {\n"
      << "// Return the PASTA `AttrKind` for a Clang `Attr`.\n"
      << "static AttrKind KindOfAttr(const clang::Attr *attr) {\n"
      << "  switch (attr->getKind()) {\n"
      << "#define ATTR_RANGE(...)\n"
      << "#define ATTR(a) \\\n"
      << "    case clang::attr::Kind::a: \\\n"
      << "      return AttrKind::k ## a;\n"
      << "#include <clang/Basic/AttrList.inc>\n"
      << "  }\n"
      << "  __builtin_unreachable();\n"
      << "}\n\n"
      << "static const std::string_view kAttrNames[] = {\n"
      << "#define PASTA_ATTR_KIND_NAME(name) #name ,\n"
      << "PASTA_FOR_EACH_ATTR_IMPL(PASTA_ATTR_KIND_NAME, PASTA_IGNORE_ABSTRACT)\n"
      << "#undef PASTA_ATTR_KIND_NAME\n"
      << "};\n"
      << "}  // namespace\n\n"
      << "std::string_view Attr::KindName(void) const noexcept {\n"
      << "  return kAttrNames[static_cast<unsigned>(kind)];\n"
      << "}\n\n"
      // All Attributes will use the inherited function to get Tokens
      << "::pasta::TokenRange Attr::Tokens(void) const noexcept {\n"
      << "  return ast->TokenRangeFrom(u.Attr->getRange());\n"
      << "}\n\n";

  // Define them all.
  for (const auto &name : gTopologicallyOrderedAttrs) {
    llvm::StringRef name_ref(name);

    if (name != "Attr") {
      os
          << name << "::" << name << "(\n"
          << "    std::shared_ptr<ASTImpl> ast_,\n"
          << "    const ::clang::Attr *attr_)";

      auto sep = "\n    : ";
      const auto &base_classes = gBaseClasses[name];
      auto prefix = base_classes.size() == 1u ? "std::move(" : "";
      auto suffix = base_classes.size() == 1u ? ")" : "";
      for (const auto &parent_class : base_classes) {
        os << sep << parent_class << "(" << prefix << "ast_" << suffix
           << ", attr_)";
        sep = ",\n      ";
      }
      os << " {}\n\n";
    } else {
      os
          << "Attr::Attr(\n"
          << "    std::shared_ptr<ASTImpl> ast_,\n"
          << "    const ::clang::Attr *attr_)"
          << "    : Attr(std::move(ast_), attr_, KindOfAttr(attr_)) {}\n\n";
    }

    // Constructors from derived class -> base class.
    for (const auto &base_class : gTransitiveBaseClasses[name]) {
      os << "PASTA_DEFINE_BASE_OPERATORS(" << base_class << ", "
         << name << ")\n";
    }

    for (const auto &derived_class : gTransitiveDerivedClasses[name]) {
      os << "PASTA_DEFINE_DERIVED_OPERATORS("
         << name << ", " << derived_class << ")\n";
    }
    DefineCppMethods(os, name, gClassIDs[name]);
  }

  os
      << "}  // namespace pasta\n"
      << "#endif  // PASTA_IN_BOOTSTRAP\n";
}
