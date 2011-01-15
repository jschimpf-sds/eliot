// ****************************************************************************
//  compiler-expred.cpp                                             XLR project
// ****************************************************************************
//
//   File Description:
//
//    Information required by the compiler for expression reduction
//
//
//
//
//
//
//
//
// ****************************************************************************
// This document is released under the GNU General Public License.
// See http://www.gnu.org/copyleft/gpl.html and Matthew 25:22 for details
//  (C) 1992-2010 Christophe de Dinechin <christophe@taodyne.com>
//  (C) 2010 Taodyne SAS
// ****************************************************************************

#include "compiler-expred.h"
#include "compiler-unit.h"
#include "compiler-arg.h"
#include "types.h"
#include "save.h"

#include <llvm/Support/IRBuilder.h>
#include <llvm/GlobalVariable.h>
#include <llvm/Function.h>

XL_BEGIN

using namespace llvm;



// ============================================================================
//
//    Compile an expression
//
// ============================================================================

llvm_value CompileExpression::DoInteger(Integer *what)
// ----------------------------------------------------------------------------
//   Compile an integer constant
// ----------------------------------------------------------------------------
{
    Compiler *compiler = unit->compiler;
    return ConstantInt::get(compiler->integerTy, what->value);
}


llvm_value CompileExpression::DoReal(Real *what)
// ----------------------------------------------------------------------------
//   Compile a real constant
// ----------------------------------------------------------------------------
{
    Compiler *compiler = unit->compiler;
    return ConstantFP::get(compiler->realTy, what->value);
}


llvm_value CompileExpression::DoText(Text *what)
// ----------------------------------------------------------------------------
//   Compile a text constant
// ----------------------------------------------------------------------------
{
    Compiler *compiler = unit->compiler;
    GlobalVariable *global = compiler->TextConstant(what->value);
    return unit->code->CreateConstGEP2_32(global, 0U, 0U);
}


llvm_value CompileExpression::DoName(Name *what)
// ----------------------------------------------------------------------------
//   Compile a name
// ----------------------------------------------------------------------------
{
    Context_p  where;
    Rewrite_p  rewrite;
    Context   *context  = unit->context;
    Tree      *existing = context->Bound(what, Context::SCOPE_LOOKUP,
                                         &where, &rewrite);
    assert(existing || !"Type checking didn't realize a name is missing");
    if (where == context)
        return unit->Known(rewrite->from);

    // Check true and false values
    if (existing == xl_true)
        return unit->code->getTrue();
    if (existing == xl_false)
        return unit->code->getFalse();

    // For now assume a global
    if (llvm_value global = unit->Global(existing))
        return global;

    return DoCall(what);
}


llvm_value CompileExpression::DoInfix(Infix *infix)
// ----------------------------------------------------------------------------
//   Compile infix expressions
// ----------------------------------------------------------------------------
{
    // Sequences
    if (infix->name == "\n" || infix->name == ";")
    {
        infix->left->Do(this);
        return infix->right->Do(this);
    }

    // Type casts - REVISIT: may need to do some actual conversion
    if (infix->name == ":")
    {
        return infix->left->Do(this);
    }

    // Declarations: enter a function
    if (infix->name == "->")
    {
        return NULL;
        Rewrite *rw = unit->context->RewriteFor(infix->left);
        assert(rw || !"Compiling declaration that was not seen before");
        // REVISIT: unit->inference is NOT the right inference here,
        // so we end up botching the compilation
        llvm_value function = unit->Compile(rw, unit->inference);
        assert(function || "Unable to compile function");
        return function;
    }

    // General case: expression
    return DoCall(infix);
}


llvm_value CompileExpression::DoPrefix(Prefix *what)
// ----------------------------------------------------------------------------
//   Compile prefix expressions
// ----------------------------------------------------------------------------
{
    if (Name *name = what->left->AsName())
    {
        if (name->value == "data" || name->value == "extern")
            return NULL;

        if (name->value == "opcode")
        {
            // This is a builtin, find if we write to code or data
            llvm_builder bld = unit->code;
            Tree *builtin = what->right;
            if (Prefix *prefix = builtin->AsPrefix())
            {
                if (Name *name = prefix->left->AsName())
                {
                    if (name->value == "data")
                    {
                        bld = unit->data;
                        builtin = prefix->right;
                    }
                }
            }

            // Take args list for current function as input
            std::vector<llvm_value> args;
            Function *function = unit->function;
            uint i, max = function->arg_size();
            Function::arg_iterator arg = function->arg_begin();
            for (i = 0; i < max; i++)
            {
                llvm_value inputArg = arg++;
                args.push_back(inputArg);
            }

            // Call the primitive (effectively creating a wrapper for it)
            Name *name = builtin->AsName();
            assert(name || !"Malformed primitive");
            Compiler *compiler = unit->compiler;
            text op = name->value;
            uint sz = args.size();
            llvm_value *a = &args[0];
            return compiler->Primitive(bld, op, sz, a);
        }
    }
    return DoCall(what);
}


llvm_value CompileExpression::DoPostfix(Postfix *what)
// ----------------------------------------------------------------------------
//   Compile postfix expressions
// ----------------------------------------------------------------------------
{
    return DoCall(what);
}


llvm_value CompileExpression::DoBlock(Block *block)
// ----------------------------------------------------------------------------
//   Compile blocks
// ----------------------------------------------------------------------------
{
    return block->child->Do(this);
}


llvm_value CompileExpression::DoCall(Tree *call)
// ----------------------------------------------------------------------------
//   Compile expressions into calls for the right expression
// ----------------------------------------------------------------------------
{
    llvm_value result = NULL;

    rcall_map &rcalls = unit->inference->rcalls;
    rcall_map::iterator found = rcalls.find(call);
    assert(found != rcalls.end() || !"Type analysis botched on expression");

    Function *function = unit->function;
    LLVMContext &llvm = *unit->llvm;
    RewriteCalls *rc = (*found).second;
    RewriteCandidates &calls = rc->candidates;

    // Optimize the frequent case where we have a single call candidate
    uint i, max = calls.size();
    if (max == 1)
    {
        // We now evaluate in that rewrite's type system
        RewriteCandidate &cand = calls[0];
        Save<TypeInference_p> saveTypes(unit->inference, cand.types);

        if (cand.conditions.size() == 0)
        {
            result = DoRewrite(cand);
            return result;
        }
    }

    // More general case: we need to generate expression reduction
    llvm_block isDone = BasicBlock::Create(llvm, "done", function);
    llvm_builder code = unit->code;
    llvm_value storage = unit->NeedStorage(call);

    for (i = 0; i < max; i++)
    {
        // Now evaluate in that candidate's type system
        RewriteCandidate &cand = calls[i];
        Save<TypeInference_p> saveTypes(unit->inference, cand.types);
        llvm_value condition = NULL;

        // Perform the tests to check if this candidate is valid
        std::vector<RewriteCondition> &conds = cand.conditions;
        std::vector<RewriteCondition>::iterator t;
        for (t = conds.begin(); t != conds.end(); t++)
        {
            llvm_value compare = Compare((*t).value, (*t).test);
            if (condition)
                condition = code->CreateAnd(condition, compare);
            else
                condition = compare;
        }

        if (condition)
        {
            llvm_block isBad = BasicBlock::Create(llvm, "bad", function);
            llvm_block isGood = BasicBlock::Create(llvm, "good", function);
            code->CreateCondBr(condition, isGood, isBad);
            code->SetInsertPoint(isGood);
            result = DoRewrite(cand);
            code->CreateStore(result, storage);
            code->CreateBr(isDone);
            code->SetInsertPoint(isBad);
        }
        else
        {
            // If this particular call was unconditional, we are done
            result = DoRewrite(cand);
            code->CreateStore(result, storage);
            code->CreateBr(isDone);
            code->SetInsertPoint(isDone);
            result = code->CreateLoad(storage);
            return result;
        }
    }

    // The final call to xl_form_error if nothing worked
    unit->CallFormError(call);
    code->CreateBr(isDone);
    code->SetInsertPoint(isDone);
    result = code->CreateLoad(storage);
    return result;
}


llvm_value CompileExpression::DoRewrite(RewriteCandidate &cand)
// ----------------------------------------------------------------------------
//   Generate code for a particular rewwrite candidate
// ----------------------------------------------------------------------------
{
    Rewrite *rw = cand.rewrite;
    llvm_value result = NULL;

    // Evaluate parameters
    std::vector<llvm_value> args;
    std::vector<RewriteBinding> &bnds = cand.bindings;
    std::vector<RewriteBinding>::iterator b;
    for (b = bnds.begin(); b != bnds.end(); b++)
    {
        Tree *tree = (*b).value;
        llvm_value value = Value(tree);
        args.push_back(value);
    }

    // Check if this is an LLVM builtin
    Tree *builtin = NULL;
    if (rw->to)
        if (Prefix *prefix = rw->to->AsPrefix())
            if (Name *name = prefix->left->AsName())
                if (name->value == "opcode")
                    builtin = prefix->right;

    if (builtin)
    {
        llvm_builder bld = unit->code;
        if (Prefix *prefix = builtin->AsPrefix())
        {
            if (Name *name = prefix->left->AsName())
            {
                if (name->value == "data")
                {
                    bld = unit->data;
                    builtin = prefix->right;
                }
            }
        }

        Name *name = builtin->AsName();
        assert(name || !"Malformed primitive");
        Compiler *compiler = unit->compiler;
        text op = name->value;
        uint sz = args.size();
        llvm_value *a = &args[0];
        result = compiler->Primitive(bld, op, sz, a);
    }
    else
    {
        llvm_value function = unit->Compile(rw, cand.types);
        if (function)
            result = unit->code->CreateCall(function, args.begin(), args.end());
    }

    return result;
}


llvm_value CompileExpression::Value(Tree *expr)
// ----------------------------------------------------------------------------
//   Evaluate an expression once
// ----------------------------------------------------------------------------
{
    llvm_value value = computed[expr];
    if (!value)
    {
        value = expr->Do(this);
        computed[expr] = value;
    }
    return value;
}


llvm_value CompileExpression::Compare(Tree *valueTree, Tree *testTree)
// ----------------------------------------------------------------------------
//   Perform a comparison between the two values and check if this matches
// ----------------------------------------------------------------------------
{
    llvm_value value = Value(valueTree);
    llvm_value test = Value(testTree);
    llvm_type valueType = value->getType();
    llvm_type testType = test->getType();

    CompiledUnit &u = *unit;
    Compiler &c = *u.compiler;
    llvm_builder code = u.code;

    // Comparison of boolean values
    if (testType == c.booleanTy)
    {
        if (valueType == c.treePtrTy || valueType == c.nameTreePtrTy)
        {
            value = u.Autobox(value, c.booleanTy);
            valueType = value->getType();
        }
        if (valueType != c.booleanTy)
            return code->getFalse();
        return code->CreateICmpEQ(test, value);
    }

    // Comparison of character values
    if (testType == c.characterTy)
    {
        if (valueType == c.textTreePtrTy)
        {
            value = u.Autobox(value, testType);
            valueType = value->getType();
        }
        if (valueType != c.characterTy)
            return code->getFalse();
        return code->CreateICmpEQ(test, value);
    }

    // Comparison of text constants
    if (testType == c.textTy)
    {
        test = u.Autobox(test, c.charPtrTy);
        testType = test->getType();
    }
    if (testType == c.charPtrTy)
    {
        if (valueType == c.textTreePtrTy)
        {
            value = u.Autobox(value, testType);
            valueType = value->getType();
        }
        if (valueType != c.charPtrTy)
            return code->getFalse();
        value = code->CreateCall2(c.strcmp_fn, test, value);
        test = ConstantInt::get(value->getType(), 0);
        value = code->CreateICmpEQ(value, test);
        return value;
    }

    // Comparison of integer values
    if (testType->isIntegerTy())
    {
        if (valueType == c.integerTreePtrTy)
        {
            value = u.Autobox(value, c.integerTy);
            valueType = value->getType();
        }
        if (!valueType->isIntegerTy())
            return code->getFalse();
        if (valueType != c.integerTy)
            value = code->CreateSExt(value, c.integerTy);
        if (testType != c.integerTy)
            test = code->CreateSExt(test, c.integerTy);
        return code->CreateICmpEQ(test, value);
    }

    // Comparison of floating-point values
    if (testType->isFloatingPointTy())
    {
        if (valueType == c.realTreePtrTy)
        {
            value = u.Autobox(value, c.realTy);
            valueType = value->getType();
        }
        if (!valueType->isFloatingPointTy())
            return code->getFalse();
        if (valueType != testType)
        {
            if (valueType != c.realTy)
            {
                value = code->CreateFPExt(value, c.realTy);
                valueType = value->getType();
            }
            if (testType != c.realTy)
            {
                test = code->CreateFPExt(test, c.realTy);
                testType = test->getType();
            }
            if (valueType != testType)
                return code->getFalse();
        }
        return code->CreateFCmpOEQ(test, value);
    }

    // Test our special types
    if (testType == c.treePtrTy         ||
        testType == c.integerTreePtrTy  ||
        testType == c.realTreePtrTy     ||
        testType == c.textTreePtrTy     ||
        testType == c.nameTreePtrTy     ||
        testType == c.blockTreePtrTy    ||
        testType == c.infixTreePtrTy    ||
        testType == c.prefixTreePtrTy   ||
        testType == c.postfixTreePtrTy)
    {
        if (testType != c.treePtrTy)
        {
            test = code->CreateBitCast(test, c.treePtrTy);
            testType = test->getType();
        }

        // Convert value to a Tree * if possible
        if (valueType->isIntegerTy() ||
            valueType->isFloatingPointTy() ||
            valueType == c.charPtrTy ||
            valueType == c.textTy ||
            valueType == c.integerTreePtrTy  ||
            valueType == c.realTreePtrTy     ||
            valueType == c.textTreePtrTy     ||
            valueType == c.nameTreePtrTy     ||
            valueType == c.blockTreePtrTy    ||
            valueType == c.infixTreePtrTy    ||
            valueType == c.prefixTreePtrTy   ||
            valueType == c.postfixTreePtrTy)
        {
            value = u.Autobox(value, c.treePtrTy);
            valueType = value->getType();
        }

        if (testType != valueType)
            return code->getFalse();

        // Call runtime function to perform tree comparison
        return code->CreateCall2(c.xl_same_shape, value, test);
    }

    // Other comparisons fail for now
    return code->getFalse();
}



// ============================================================================
//
//    Expression reduction
//
// ============================================================================
//   An expression reduction typically compiles as:
//     if (cond1) if (cond2) if (cond3) invoke(T)
//   However, we may determine during compilation of if(cond2) that the
//   call is statically not valid. So we save the initial basic block,
//   and decide at the end to connect it or not. Let LLVM optimize branches
//   and dead code away...

ExpressionReduction::ExpressionReduction(CompiledUnit &u, Tree *src)
// ----------------------------------------------------------------------------
//    Snapshot current basic blocks in the compiled unit
// ----------------------------------------------------------------------------
    : unit(u), source(src), llvm(u.llvm),
      savedfailbb(NULL),
      entrybb(NULL), savedbb(NULL), successbb(NULL)
{
    // We need storage for the various cases to store the result
    u.NeedStorage(src);
    u.NeedLazy(src);

    // Save compile unit's data
    savedfailbb = u.failbb;
    u.failbb = NULL;

    // Create the end-of-expression point if we find a match or had it already
    successbb = u.BeginLazy(src);
}


ExpressionReduction::~ExpressionReduction()
// ----------------------------------------------------------------------------
//   Destructor restores the prevous sucess and fail points
// ----------------------------------------------------------------------------
{
    CompiledUnit &u = unit;

    // Mark the end of a lazy expression evaluation
    u.EndLazy(source, successbb);

    // Restore saved 'failbb' and value map
    u.failbb = savedfailbb;
}


void ExpressionReduction::NewForm ()
// ----------------------------------------------------------------------------
//    Indicate that we are testing a new form for evaluating the expression
// ----------------------------------------------------------------------------
{
    CompiledUnit &u = unit;

    // Save previous basic blocks in the compiled unit
    savedbb = u.code->GetInsertBlock();
    assert(savedbb || !"NewForm called after unconditional success");

    // Create entry / exit basic blocks for this expression
    entrybb = BasicBlock::Create(*llvm, "subexpr", u.function);
    u.failbb = NULL;

    // Set the insertion point to the new invokation code
    u.code->SetInsertPoint(entrybb);

}


void ExpressionReduction::Succeeded(void)
// ----------------------------------------------------------------------------
//   We successfully compiled a reduction for that expression
// ----------------------------------------------------------------------------
//   In that case, we connect the basic blocks to evaluate the expression
{
    CompiledUnit &u = unit;

    // Branch from current point (end of expression) to exit of evaluation
    u.code->CreateBr(successbb);

    // Branch from initial basic block position to this subcase
    u.code->SetInsertPoint(savedbb);
    u.code->CreateBr(entrybb);

    // If there were tests, we keep testing from that 'else' spot
    if (u.failbb)
    {
        u.code->SetInsertPoint(u.failbb);
    }
    else
    {
        // Create a fake basic block in case someone decides to add code
        BasicBlock *empty = BasicBlock::Create(*llvm, "empty", u.function);
        u.code->SetInsertPoint(empty);
    }
    u.failbb = NULL;
}


void ExpressionReduction::Failed()
// ----------------------------------------------------------------------------
//    We figured out statically that the current form doesn't apply
// ----------------------------------------------------------------------------
{
    CompiledUnit &u = unit;

    u.CallFormError(source);
    u.code->CreateBr(successbb);
    if (u.failbb)
    {
        IRBuilder<> failTail(u.failbb);
        u.code->SetInsertPoint(u.failbb);
        u.CallFormError(source);
        failTail.CreateBr(successbb);
        u.failbb = NULL;
    }

    u.code->SetInsertPoint(savedbb);
}


XL_END
