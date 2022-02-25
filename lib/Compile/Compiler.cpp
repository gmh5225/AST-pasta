/*
 * Copyright (c) 2020 Trail of Bits, Inc.
 */

#include "Compiler.h"

#include <llvm/Support/Host.h>
#include <pasta/Util/FileSystem.h>

namespace pasta {

Compiler::~Compiler(void) {}

Compiler::Compiler(std::shared_ptr<CompilerImpl> impl_)
    : impl(std::move(impl_)) {}

// Name/variant of this compiler.
CompilerName Compiler::Name(void) const {
  return impl->compiler_name;
}

// Target language for this compiler.
::pasta::TargetLanguage Compiler::TargetLanguage(void) const {
  return impl->target_lang;
}

std::string Compiler::HostTargetTriple(void) noexcept {
  return llvm::sys::getDefaultTargetTriple();
}

// Return the default target triple for this compiler.
std::string Compiler::TargetTriple(void) const noexcept {
  return impl->triple;
}

// Path to the executable.
std::filesystem::path Compiler::ExecutablePath(void) const {
  return impl->compiler_exe;
}

// Resource directory of the compiler, i.e. where you find compiler-specific
// header files.
std::filesystem::path Compiler::ResourceDirectory(void) const {
  return impl->resource_dir;
}

// Directory to treat as the system root. Useful for cross-compilation
// toolchains.
std::filesystem::path Compiler::SystemRootDirectory(void) const {
  return impl->sysroot_dir.empty() ? impl->install_dir : impl->sysroot_dir;
}

// Directory to treat as the system root for inclusions. Useful for cross-
// compilation toolchains.
std::filesystem::path Compiler::SystemRootIncludeDirectory(void) const {
  return impl->isysroot_dir;
}

// Directory where the compiler is installed.
std::filesystem::path Compiler::InstallationDirectory(void) const {
  return impl->install_dir;
}

// Return the file system associated with this compiler's paths.
std::shared_ptr<FileSystem> Compiler::FileSystem(void) const {
  return impl->file_manager.FileSystem();
}

// Return the file manager associated with files that will be opened and
// read by this compiler.
FileManager Compiler::FileManager(void) const {
  return impl->file_manager;
}

// Invoke a callback `cb` for each system include directory. Think `-isystem`.
void Compiler::ForEachSystemIncludeDirectory(
    std::function<void(const std::filesystem::path &,
                       IncludePathLocation)> cb) const {
  for (const auto &entry : impl->system_includes) {
    if (!entry.first.empty()) {
      cb(entry.first, entry.second);
    }
  }
}

// Invoke a callback `cb` for each user include directory. Think `-I` or
// `-iquote`.
void Compiler::ForEachUserIncludeDirectory(
    std::function<void(const std::filesystem::path &,
                       IncludePathLocation)> cb) const {
  for (const auto &entry : impl->user_includes) {
    if (!entry.first.empty()) {
      cb(entry.first, entry.second);
    }
  }
}

// Invoke a callback `cb` for each user include directory. Think `-iframework`
// or `iframeworkwithsysroot`.
void Compiler::ForEachFrameworkDirectory(
    std::function<void(const std::filesystem::path &,
                       IncludePathLocation)> cb) const {
  for (const auto &entry : impl->frameworks) {
    if (!entry.first.empty()) {
      cb(entry.first, entry.second);
    }
  }
}

}  // namespace pasta
