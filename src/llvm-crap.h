#ifndef LLVM_CRAP_H
#define LLVM_CRAP_H
// ****************************************************************************
//  llvm-crap.h                                                    Tao project
// ****************************************************************************
//
//   File Description:
//
//     LLVM keeps breaking the API from release to release.
//     Of course, none of the choices are documented anywhere, and
//     the documentation is left out of date for years.
//
//     So this file is an attempt at reverse engineering all the API
//     changes over time to be able to compile with various versions of LLVM
//     Be prepared for the worst. It's so ugly I had to dispose of it by
//     putting it in its own separate file.
//
//
// ****************************************************************************
// This document is released under the GNU General Public License, with the
// following clarification and exception.
//
// Linking this library statically or dynamically with other modules is making
// a combined work based on this library. Thus, the terms and conditions of the
// GNU General Public License cover the whole combination.
//
// As a special exception, the copyright holders of this library give you
// permission to link this library with independent modules to produce an
// executable, regardless of the license terms of these independent modules,
// and to copy and distribute the resulting executable under terms of your
// choice, provided that you also meet, for each linked independent module,
// the terms and conditions of the license of that module. An independent
// module is a module which is not derived from or based on this library.
// If you modify this library, you may extend this exception to your version
// of the library, but you are not obliged to do so. If you do not wish to
// do so, delete this exception statement from your version.
//
// See http://www.gnu.org/copyleft/gpl.html and Matthew 25:22 for details
//  (C) 2014 Christophe de Dinechin <christophe@taodyne.com>
//  (C) 2014 Taodyne SAS
// ****************************************************************************

#ifndef LLVM_VERSION
#error "Sorry, no can do anything without knowing the LLVM version"
#elif LLVM_VERSION < 27
// At some point, I have only so much time to waste on this.
// Feel free to enhance if you care about earlier versions of LLVM.
#error "LLVM 2.6 and earlier are not supported in this code."
#endif


// ============================================================================
// 
//                          HEADER FILE ADJUSTMENTS 
// 
// ============================================================================

// Where any sane library would offer something like #include <llvm.h>,
// LLVM insists on you loading every single header file they have.
// And then changes their name regularly, just for fun. When they don't
// simply remove them! It's not like anybody is using header names, right?
// The wise man says: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARGH!
//
// SURGEON GENERAL WARNING: Do not read the code below, or else.

#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/PassManager.h>
#include "llvm/Support/raw_ostream.h"
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Target/TargetSelect.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/ADT/Statistic.h>

// Sometimes, headers magically disappear
#if LLVM_VERSION < 27
#include <llvm/ModuleProvider.h>
#endif

#if LLVM_VERSION < 30
#include <llvm/Support/StandardPasses.h>
#else
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#endif

// Sometimes, key headers magically appear. This one appeared in 2.6.
// Don't bother supporting anything earlier.
#if LLVM_VERSION < 33
#include <llvm/CallingConv.h>
#include <llvm/Constants.h>
#include <llvm/LLVMContext.h>
#else
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LLVMContext.h>
#endif


// It's also funny to move headers around for really no good reason at all,
// including core features that absolutely everybody has to use.
#if LLVM_VERSION < 33
#include <llvm/DerivedTypes.h>
#include <llvm/Module.h>
#include <llvm/GlobalValue.h>
#include "llvm/Instructions.h"
#else
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/GlobalValue.h>
#include "llvm/IR/Instructions.h"
#endif

// While we are at it, why not also change the name of headers providing a
// given feature, just for additional humorous obfuscation?
#if LLVM_VERSION < 32
#include <llvm/Support/IRBuilder.h>
#elif LLVM_VERSION < 33
#include <llvm/IRBuilder.h>
#else
#include <llvm/IR/IRBuilder.h>
#endif

// For example, knowing your target is completely unoptional.
// Is it IR? Well, everything is IR, since LLVM is an IR.
// IMHO, target data layout belongs more to 'target' than to 'IR',
// but who am I to say?
#if LLVM_VERSION < 32
#include <llvm/Target/TargetData.h>
#elif LLVM_VERSION < 33
#include <llvm/DataLayout.h>
#else
#include <llvm/IR/DataLayout.h>
#endif

// Repeat at nauseam. If you can figure out why the verifier moved
// from "analysis" to "IR", let me know.
#if LLVM_VERSION < 350
#include <llvm/Analysis/Verifier.h>
#else
#include <llvm/IR/Verifier.h>
#endif


// ============================================================================
// 
//               WORKAROUNDS FOR GRATUITOUS API BREAKAGE
// 
// ============================================================================

// It is not enough to break header files. It's also important to make
// changes that require distributed changes throughout the code. And to
// provide no help whatsoever fixing them, e.g. with macros or some kind
// of advanced warning that you will soon need to change the code.


// In 3.0, types suddenly became non-const, and you had to pass arguments
// using a newly created class called ArrayRef. Of course, ArrayRef could
// have been given one additional constructor taking a vector of const type
// pointers, and the compiler would have automagically adjusted things for you,
// even if that meant lying slightly about the const-ness of input types.
// But why bother when everybody can simply use a #ifdef every single time
// they call a function, build a structure, etc.
#if LLVM_VERSION < 30
typedef const llvm::Type *              llvm_type;
typedef const llvm::IntegerType *       llvm_integer_type;
#define LLVMS_ARGS(args)                args.begin(), args.end()
#else // 3.0 or later
typedef llvm::Type *                    llvm_type;
typedef llvm::IntegerType *             llvm_integer_type;
#define LLVMS_ARGS(args)                llvm::ArrayRef<llvm::Value *> (args)
#endif


// The opaque type went away in LLVM 3.0, although it still seems to be
// present in the textual form of the IR (if you trust the doc, which I don't)
// So now you need to create a structure type instead. It's actually better,
// too bad it was done without any typedef or macro to help users.
// So here are the typedefs or macros you need.
#if LLVM_VERSION < 30
typedef llvm::PATypeHolder llvm_struct;
#define LLVMS_getOpaqueType(llvm)       llvm::OpaqueType::get(llvm)
#else // LLVM_VERSION >= 30

typedef llvm::StructType *llvm_struct;
#define OpaqueType                      llvm::StructType
#define LLVMS_getOpaqueType(llvm)       llvm::StructType::create(llvm)

#endif // LLVM_VERSION



// ============================================================================
// 
//                      API STABILIZATION FUNCTIONS
// 
// ============================================================================

// All the functions below have a single purpose: to build an API that
// is semi-stable despite LLVM changing things underneath.

inline llvm::Constant *LLVMS_TextConstant(llvm::LLVMContext &llvm, text value)
// ----------------------------------------------------------------------------
//    Return a constant array of characters for the input text
// ----------------------------------------------------------------------------
{
#if LLVM_VERSION < 31
    return llvm::ConstantArray::get(llvm, value);
#else
    return llvm::ConstantDataArray::getString(llvm, value);
#endif
}


inline void LLVMS_SetName(llvm::Module *module, llvm_type type, text name)
// ----------------------------------------------------------------------------
//   Setting the name for a type
// ----------------------------------------------------------------------------
{
#if LLVM_VERSION < 30
    module->addTypeName(name, type);
#else
    // Not sure if it's possible to set the name for non-struct types anymore
    if (type->isStructTy())
    {
        llvm::StructType *stype = (llvm::StructType *) type;
        if (!stype->isLiteral())
            stype->setName(name);
    }
#endif
}


inline llvm::StructType *LLVMS_Struct(llvm::LLVMContext &llvm,
                                      llvm_struct old,
                                      std::vector<llvm_type> &elements)
// ----------------------------------------------------------------------------
//    Refine a forward-declared structure type
// ----------------------------------------------------------------------------
{
#if LLVM_VERSION < 30
    llvm::StructType *stype = llvm::StructType::get(llvm, elements);
    llvm::cast<llvm::OpaqueType>(old.get())->refineAbstractTypeTo(stype);
    return llvm::cast<llvm::StructType> (old.get());
#else
    old->setBody(elements);
    return old;
#endif
}


inline llvm::ExecutionEngine *LLVMS_InitializeJIT(llvm::LLVMContext &llvm,
                                                  text moduleName,
                                                  llvm::Module **module)
// ----------------------------------------------------------------------------
//    Initialization of LLVM parameters
// ----------------------------------------------------------------------------
{
    using namespace llvm;

#if LLVM_VERSION < 360
    // Create module where we will build the code
    *module = new Module(moduleName, llvm);
#else
    // WTF version of the above
    std::unique_ptr<Module> moduleOwner = make_unique<Module>(moduleName, llvm);
    *module = moduleOwner.get();
#endif


#if LLVM_VERSION >= 33
    // I get a crash on Linux if I don't do that. Unclear why.
    LLVMInitializeAllTargetMCs();
#endif
    // Initialize native target (new features)
    InitializeNativeTarget();

#if LLVM_VERSION < 31
    JITExceptionHandling = false;  // Bug #1026
    JITEmitDebugInfo = true;
    NoFramePointerElim = true;
#endif
#if LLVM_VERSION < 30
    UnwindTablesMandatory = true;
#endif

#if LLVM_VERSION < 360
    // Select the fast JIT
    EngineBuilder engineBuilder(*module);
#else
    // WTF version of the above
    EngineBuilder engineBuilder(std::move(moduleOwner));
#endif

#if LLVM_VERSION >= 31
    TargetOptions targetOpts;
    // targetOpts.JITEmitDebugInfo = true;
    engineBuilder.setEngineKind(EngineKind::JIT);
    engineBuilder.setTargetOptions(targetOpts);
#endif
    ExecutionEngine *runtime = engineBuilder.create();

    // Make sure that code is generated as early as possible
    runtime->DisableLazyCompilation(true);

    return runtime;
}


inline void LLVMS_SetupOpts(llvm::PassManager *moduleOptimizer,
                            llvm::FunctionPassManager *optimizer,
                            uint optLevel)
// ----------------------------------------------------------------------------
//   Setup the optimizations depending on the optimization level
// ----------------------------------------------------------------------------
{
#if LLVM_VERSION < 30
    createStandardModulePasses(moduleOptimizer, optLevel,
                               true,    /* Optimize size */
                               true,    /* Unit at a time */
                               true,    /* Unroll loops */
                               true,    /* Simplify lib calls */
                               false,   /* Have exception */
                               llvm::createFunctionInliningPass());
    createStandardLTOPasses(moduleOptimizer,
                            true, /* Internalize */
                            true, /* RunInliner */
                            true  /* Verify Each */);

    createStandardFunctionPasses(optimizer, optLevel);

#else // LLVM_VERSION after 3.0
    llvm::PassManagerBuilder Builder;
    Builder.OptLevel = optLevel;
    unsigned Threshold = optLevel > 2 ? 275 : 225;
    Builder.Inliner = llvm::createFunctionInliningPass(Threshold);
    Builder.populateFunctionPassManager(*optimizer);
    Builder.populateModulePassManager(*moduleOptimizer);
#endif // LLVM_VERSION
}

#endif // LLVM_CRAP_H
