//===--- ModuleLoader.h - Module Loader Interface ---------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements an abstract interface for loading modules.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_MODULE_LOADER_H
#define SWIFT_AST_MODULE_LOADER_H

#include "swift/AST/Identifier.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "swift/AST/ModuleDependencies.h"

namespace llvm {
class FileCollector;
}

namespace clang {
class DependencyCollector;
}

namespace swift {

class AbstractFunctionDecl;
struct AutoDiffConfig;
class ClangImporterOptions;
class ClassDecl;
class FileUnit;
class ModuleDecl;
class ModuleDependencies;
class ModuleDependenciesCache;
class NominalTypeDecl;
class SourceFile;
class TypeDecl;
class CompilerInstance;

enum class KnownProtocolKind : uint8_t;

enum class Bridgeability : unsigned {
  /// This context does not permit bridging at all.  For example, the
  /// target of a C pointer.
  None,

  /// This context permits all kinds of bridging.  For example, the
  /// imported result of a method declaration.
  Full
};

/// Specifies which dependencies the intermodule dependency tracker records.
enum class IntermoduleDepTrackingMode {
  /// Records both system and non-system dependencies.
  IncludeSystem,

  /// Records only non-system dependencies.
  ExcludeSystem,
};

/// Records dependencies on files outside of the current module;
/// implemented in terms of a wrapped clang::DependencyCollector.
class DependencyTracker {
  std::shared_ptr<clang::DependencyCollector> clangCollector;
public:
  explicit DependencyTracker(
      IntermoduleDepTrackingMode Mode,
      std::shared_ptr<llvm::FileCollector> FileCollector = {});

  /// Adds a file as a dependency.
  ///
  /// The contents of \p File are taken literally, and should be appropriate
  /// for appearing in a list of dependencies suitable for tooling like Make.
  /// No path canonicalization is done.
  void addDependency(StringRef File, bool IsSystem);

  /// Fetches the list of dependencies.
  ArrayRef<std::string> getDependencies() const;

  /// Return the underlying clang::DependencyCollector that this
  /// class wraps.
  std::shared_ptr<clang::DependencyCollector> getClangCollector();
};

struct SubCompilerInstanceInfo {
  StringRef CompilerVersion;
  CompilerInstance* Instance;
  StringRef Hash;
  ArrayRef<StringRef> BuildArguments;
  ArrayRef<StringRef> ExtraPCMArgs;
};

/// Abstract interface to run an action in a sub ASTContext.
struct InterfaceSubContextDelegate {
  virtual bool runInSubContext(StringRef moduleName,
                               StringRef interfacePath,
                               StringRef outputPath,
                               SourceLoc diagLoc,
  llvm::function_ref<bool(ASTContext&,ArrayRef<StringRef>,
                          ArrayRef<StringRef>, StringRef)> action) = 0;
  virtual bool runInSubCompilerInstance(StringRef moduleName,
                                        StringRef interfacePath,
                                        StringRef outputPath,
                                        SourceLoc diagLoc,
                    llvm::function_ref<bool(SubCompilerInstanceInfo&)> action) = 0;

  virtual ~InterfaceSubContextDelegate() = default;
};

/// Abstract interface that loads named modules into the AST.
class ModuleLoader {
  virtual void anchor();

protected:
  DependencyTracker * const dependencyTracker;
  ModuleLoader(DependencyTracker *tracker) : dependencyTracker(tracker) {}

public:
  virtual ~ModuleLoader() = default;

  /// Collect visible module names.
  ///
  /// Append visible module names to \p names. Note that names are possibly
  /// duplicated, and not guaranteed to be ordered in any way.
  virtual void collectVisibleTopLevelModuleNames(
      SmallVectorImpl<Identifier> &names) const = 0;

  /// Check whether the module with a given name can be imported without
  /// importing it.
  ///
  /// Note that even if this check succeeds, errors may still occur if the
  /// module is loaded in full.
  virtual bool canImportModule(Located<Identifier> named) = 0;

  /// Import a module with the given module path.
  ///
  /// \param importLoc The location of the 'import' keyword.
  ///
  /// \param path A sequence of (identifier, location) pairs that denote
  /// the dotted module name to load, e.g., AppKit.NSWindow.
  ///
  /// \returns the module referenced, if it could be loaded. Otherwise,
  /// emits a diagnostic and returns NULL.
  virtual
  ModuleDecl *loadModule(SourceLoc importLoc,
                         ArrayRef<Located<Identifier>> path) = 0;

  /// Load extensions to the given nominal type.
  ///
  /// \param nominal The nominal type whose extensions should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains extensions loaded from any generation up to and including this
  /// one.
  virtual void loadExtensions(NominalTypeDecl *nominal,
                              unsigned previousGeneration) { }

  /// Load the methods within the given class that produce
  /// Objective-C class or instance methods with the given selector.
  ///
  /// \param classDecl The class in which we are searching for @objc methods.
  /// The search only considers this class and its extensions; not any
  /// superclasses.
  ///
  /// \param selector The selector to search for.
  ///
  /// \param isInstanceMethod Whether we are looking for an instance method
  /// (vs. a class method).
  ///
  /// \param previousGeneration The previous generation with which this
  /// callback was invoked. The list of methods will already contain all of
  /// the results from generations up and including \c previousGeneration.
  ///
  /// \param methods The list of @objc methods in this class that have this
  /// selector and are instance/class methods as requested. This list will be
  /// extended with any methods found in subsequent generations.
  virtual void loadObjCMethods(
                 ClassDecl *classDecl,
                 ObjCSelector selector,
                 bool isInstanceMethod,
                 unsigned previousGeneration,
                 llvm::TinyPtrVector<AbstractFunctionDecl *> &methods) = 0;

  /// Load derivative function configurations for the given
  /// AbstractFunctionDecl.
  ///
  /// \param originalAFD The declaration whose derivative function
  /// configurations should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains derivative function configurations loaded from any generation up
  /// to and including this one.
  ///
  /// \param results The result list of derivative function configurations.
  /// This list will be extended with any methods found in subsequent
  /// generations.
  virtual void loadDerivativeFunctionConfigurations(
      AbstractFunctionDecl *originalAFD, unsigned previousGeneration,
      llvm::SetVector<AutoDiffConfig> &results) {};

  /// Verify all modules loaded by this loader.
  virtual void verifyAllModules() { }

  /// Discover overlays declared alongside this file and add infomation about
  /// them to it.
  void findOverlayFiles(SourceLoc diagLoc, ModuleDecl *module, FileUnit *file);

  /// Retrieve the dependencies for the given, named module, or \c None
  /// if no such module exists.
  virtual Optional<ModuleDependencies> getModuleDependencies(
      StringRef moduleName,
      ModuleDependenciesCache &cache,
      InterfaceSubContextDelegate &delegate) = 0;
};

} // namespace swift

#endif
