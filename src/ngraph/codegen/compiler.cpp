// ----------------------------------------------------------------------------
// Copyright 2017 Nervana Systems Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// ----------------------------------------------------------------------------

#include <iostream>

#include <clang/CodeGen/ObjectFilePCHContainerOperations.h>
#include <clang/Driver/DriverDiagnostic.h>
#include <clang/Driver/Options.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendDiagnostic.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Frontend/Utils.h>
#include <clang/FrontendTool/Utils.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/LinkAllPasses.h>
#include <llvm/Option/Arg.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Timer.h>
#include <llvm/Support/raw_ostream.h>

#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <llvm/Support/TargetSelect.h>

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>

#include "ngraph/codegen/compiler.hpp"
#include "ngraph/file_util.hpp"
#include "ngraph/log.hpp"
#include "ngraph/util.hpp"

// TODO: Fix leaks

// #define USE_CACHE

using namespace clang;
using namespace llvm;
using namespace llvm::opt;
using namespace std;

using namespace ngraph::codegen;

static HeaderCache s_header_cache;

static std::string GetExecutablePath(const char* Argv0)
{
    // This just needs to be some symbol in the binary; C++ doesn't
    // allow taking the address of ::main however.
    void* MainAddr = reinterpret_cast<void*>(GetExecutablePath);
    return llvm::sys::fs::getMainExecutable(Argv0, MainAddr);
}

execution_state::execution_state()
    : m_execution_engine{nullptr}
    , precompiled_headers_enabled(false)
    , debuginfo_enabled(false)
{
}

execution_state::~execution_state()
{
}

bool execution_state::is_version_number(const string& path)
{
    bool rc = true;
    vector<string> tokens = ngraph::split(path, '.');
    for (string s : tokens)
    {
        for (char c : s)
        {
            if (!isdigit(c))
            {
                rc = false;
            }
        }
    }
    return rc;
}

void execution_state::add_header_search_path(HeaderSearchOptions& hso, const string& path)
{
    static vector<string> valid_ext = {".h", ".hpp", ".tcc", ""};

#ifdef USE_CACHE
    string mapped_path = file_util::path_join("/$BUILTIN", path);
    mapped_path = path;
    s_header_cache.add_path(mapped_path);
    auto func = [&](const std::string& file, bool is_dir) {
        if (!is_dir)
        {
            string ext = file_util::get_file_ext(file);
            if (contains(valid_ext, ext))
            {
                // This is a header file
                string relative_name = file.substr(path.size() + 1);
                string mapped_name = file_util::path_join(mapped_path, relative_name);

                ErrorOr<unique_ptr<MemoryBuffer>> code = MemoryBuffer::getFile(file);
                if (error_code ec = code.getError())
                {
                    // throw up
                }

                s_header_cache.add_file(mapped_name, code.get());
            }
        }
    };
    file_util::iterate_files(path, func, true);
#else
    hso.AddPath(path, clang::frontend::System, false, false);
#endif
}

void execution_state::use_cached_files(std::unique_ptr<CompilerInstance>& Clang)
{
    HeaderSearchOptions& hso = Clang->getInvocation().getHeaderSearchOpts();
    for (const string& path : s_header_cache.get_include_paths())
    {
        hso.AddPath(path, clang::frontend::System, false, false);
    }
    for (auto& header : s_header_cache.get_header_map())
    {
        Clang->getPreprocessorOpts().addRemappedFile(header.first, header.second.get());
    }
}

std::unique_ptr<llvm::Module> execution_state::compile(const string& source, const string& name)
{
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllAsmParsers();

    // Prepare compilation arguments
    vector<const char*> args;
    args.push_back(name.c_str());

    // Prepare DiagnosticEngine
    DiagnosticOptions DiagOpts;
    TextDiagnosticPrinter* textDiagPrinter = new clang::TextDiagnosticPrinter(errs(), &DiagOpts);
    IntrusiveRefCntPtr<clang::DiagnosticIDs> pDiagIDs;
    DiagnosticsEngine* pDiagnosticsEngine =
        new DiagnosticsEngine(pDiagIDs, &DiagOpts, textDiagPrinter);

    // Create and initialize CompilerInstance
    std::unique_ptr<CompilerInstance> Clang(new CompilerInstance());
    Clang->createDiagnostics();

    // Initialize CompilerInvocation
    CompilerInvocation::CreateFromArgs(
        Clang->getInvocation(), &args[0], &args[0] + args.size(), *pDiagnosticsEngine);

    // Infer the builtin include path if unspecified.
    if (Clang->getHeaderSearchOpts().UseBuiltinIncludes &&
        Clang->getHeaderSearchOpts().ResourceDir.empty())
    {
        void* MainAddr = reinterpret_cast<void*>(GetExecutablePath);
        auto path = CompilerInvocation::GetResourcesPath(args[0], MainAddr);
        Clang->getHeaderSearchOpts().ResourceDir = path;
    }

    HeaderSearchOptions& hso = Clang->getInvocation().getHeaderSearchOpts();
    if (s_header_cache.is_valid() == false)
    {
        // Add base toolchain-supplied header paths
        // Ideally one would use the Linux toolchain definition in clang/lib/Driver/ToolChains.h
        // But that's a private header and isn't part of the public libclang API
        // Instead of re-implementing all of that functionality in a custom toolchain
        // just hardcode the paths relevant to frequently used build/test machines for now
        add_header_search_path(hso, CLANG_BUILTIN_HEADERS_PATH);
        add_header_search_path(hso, "/usr/include/x86_64-linux-gnu");
        add_header_search_path(hso, "/usr/include");

        // Search for headers in
        //    /usr/include/x86_64-linux-gnu/c++/N.N
        //    /usr/include/c++/N.N
        // and add them to the header search path

        file_util::iterate_files("/usr/include/x86_64-linux-gnu/c++/",
                                 [&](const std::string& file, bool is_dir) {
                                     if (is_dir)
                                     {
                                         string dir_name = file_util::get_file_name(file);
                                         if (is_version_number(dir_name))
                                         {
                                             add_header_search_path(hso, file);
                                         }
                                     }
                                 });

        file_util::iterate_files("/usr/include/c++/", [&](const std::string& file, bool is_dir) {
            if (is_dir)
            {
                string dir_name = file_util::get_file_name(file);
                if (is_version_number(dir_name))
                {
                    add_header_search_path(hso, file);
                }
            }
        });

        add_header_search_path(hso, EIGEN_HEADERS_PATH);
        add_header_search_path(hso, NGRAPH_HEADERS_PATH);
#ifdef USE_CACHE
        s_header_cache.set_valid();
#endif
    }

#ifdef USE_CACHE
    use_cached_files(Clang);
#endif

    // Language options
    // These are the C++ features needed to compile ngraph headers
    // and any dependencies like Eigen
    auto LO = Clang->getInvocation().getLangOpts();
    LO->CPlusPlus = 1;
    LO->CPlusPlus11 = 1;
    LO->Bool = 1;
    LO->Exceptions = 1;
    LO->CXXExceptions = 1;
    LO->WChar = 1;
    LO->RTTI = 1;
    // Enable OpenMP for Eigen
    LO->OpenMP = 1;
    LO->OpenMPUseTLS = 1;

    // CodeGen options
    auto& CGO = Clang->getInvocation().getCodeGenOpts();
    CGO.OptimizationLevel = 3;
    CGO.RelocationModel = "static";
    CGO.ThreadModel = "posix";
    CGO.FloatABI = "hard";
    CGO.OmitLeafFramePointer = 1;
    CGO.VectorizeLoop = 1;
    CGO.VectorizeSLP = 1;
    CGO.CXAAtExit = 0;

    if (debuginfo_enabled)
    {
        CGO.setDebugInfo(codegenoptions::FullDebugInfo);
    }

    if (precompiled_headers_enabled)
    {
        // Preprocessor options
        auto& PPO = Clang->getInvocation().getPreprocessorOpts();
        PPO.ImplicitPCHInclude = "ngcpu.pch";
        PPO.DisablePCHValidation = 1;
    }

    // Enable various target features
    // Most of these are for Eigen
    auto& TO = Clang->getInvocation().getTargetOpts();
    // TODO: This needs to be configurable and selected carefully
    TO.CPU = "broadwell";
    TO.FeaturesAsWritten.emplace_back("+sse");
    TO.FeaturesAsWritten.emplace_back("+sse2");
    TO.FeaturesAsWritten.emplace_back("+sse3");
    TO.FeaturesAsWritten.emplace_back("+ssse3");
    TO.FeaturesAsWritten.emplace_back("+sse4.1");
    TO.FeaturesAsWritten.emplace_back("+sse4.2");
    TO.FeaturesAsWritten.emplace_back("+avx");
    TO.FeaturesAsWritten.emplace_back("+avx2");
    TO.FeaturesAsWritten.emplace_back("+fma");

    // Map code filename to a memoryBuffer
    StringRef source_ref(source);
    unique_ptr<MemoryBuffer> buffer = MemoryBuffer::getMemBufferCopy(source_ref);
    Clang->getInvocation().getPreprocessorOpts().addRemappedFile(name, buffer.get());

    // Create and execute action
    CodeGenAction* compilerAction = new EmitCodeGenOnlyAction();
    std::unique_ptr<llvm::Module> rc;
    if (Clang->ExecuteAction(*compilerAction) == true)
    {
        rc = compilerAction->takeModule();
    }

    buffer.release();

    return rc;
}

bool execution_state::add_module(std::unique_ptr<llvm::Module>& module)
{
    if (module)
    {
        if (!m_execution_engine)
        {
            m_execution_engine = llvm::EngineBuilder(move(module))
                                     .setEngineKind(llvm::EngineKind::JIT)
                                     .setOptLevel(llvm::CodeGenOpt::Aggressive)
                                     .setErrorStr(&jit_error)
                                     .create();

            if (!m_execution_engine)
            {
                return false;
            }
        }
    }
    else
    {
        return false;
    }

    return true;
}

void execution_state::finalize()
{
    if (m_execution_engine)
    {
        m_execution_engine->finalizeObject();
        m_execution_engine->runStaticConstructorsDestructors(false);
    }
    else
    {
        throw std::runtime_error(
            "Error in finalize: " +
            (jit_error.empty() ? "Could not create an execution engine" : jit_error));
    }
}
