// ****************************************************************************
//  types.cpp                                                       XLR project
// ****************************************************************************
//
//   File Description:
//
//     The type system in XL
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

#include "types.h"
#include "tree.h"
#include "runtime.h"
#include "errors.h"
#include "options.h"
#include "save.h"
#include "compiler-arg.h"
#include "renderer.h"
#include <iostream>

XL_BEGIN

// ============================================================================
//
//    Type allocation and unification algorithms (hacked Damas-Hilney-Milner)
//
// ============================================================================

ulong TypeInference::id = 0;

TypeInference::TypeInference(Context *context)
// ----------------------------------------------------------------------------
//   Constructor for top-level type inferences
// ----------------------------------------------------------------------------
    : context(context),
      types(),
      unifications(),
      rcalls(),
      prototyping(false)
{}


TypeInference::TypeInference(Context *context, TypeInference *parent)
// ----------------------------------------------------------------------------
//   Constructor for "child" type inferences, i.e. done within a parent
// ----------------------------------------------------------------------------
    : context(context),
      types(parent->types),
      unifications(parent->unifications),
      rcalls(),
      prototyping(false)
{}


TypeInference::~TypeInference()
// ----------------------------------------------------------------------------
//    Destructor - Nothing to explicitly delete, but useful for debugging
// ----------------------------------------------------------------------------
{}


bool TypeInference::TypeCheck(Tree *program)
// ----------------------------------------------------------------------------
//   Perform all the steps of type inference on the given program
// ----------------------------------------------------------------------------
{
    // First process all the declarations of the program in current context
    context->ProcessDeclarations(program);

    // Once this is done, record all type information for the program
    bool result = program->Do(this);

    // Dump debug information if approriate
    IFTRACE(types)
    {
        std::cout << "TYPE CHECK FOR " << program << "\n";
        std::cout << "TYPES:\n"; debugt(this);
        std::cout << "UNIFICATIONS:\n"; debugu(this);
        std::cout << "CALLS:\n"; debugr(this);
    }

    return result;
}


Tree *TypeInference::Type(Tree *expr)
// ----------------------------------------------------------------------------
//   Return the base type associated with a given expression
// ----------------------------------------------------------------------------
{
    Tree *type = types[expr];
    if (!type)
    {
        AssignType(expr);
        type = types[expr];
    }
    return Base(type);
}



bool TypeInference::DoInteger(Integer *what)
// ----------------------------------------------------------------------------
//   Annotate an integer tree with its value
// ----------------------------------------------------------------------------
{
    return DoConstant(what);
}


bool TypeInference::DoReal(Real *what)
// ----------------------------------------------------------------------------
//   Annotate a real tree with its value
// ----------------------------------------------------------------------------
{
    return DoConstant(what);
}


bool TypeInference::DoText(Text *what)
// ----------------------------------------------------------------------------
//   Annotate a text tree with its own value
// ----------------------------------------------------------------------------
{
    return DoConstant(what);
}


bool TypeInference::DoConstant(Tree *what)
// ----------------------------------------------------------------------------
//   All constants have themselves as type, and don't normally evaluate
// ----------------------------------------------------------------------------
{
    bool result = AssignType(what, what);

    // Only try to evaluate constants if the context has constant rewrites
    if (result && context->hasConstants)
        result = Evaluate(what);

    return result;
}


bool TypeInference::DoName(Name *what)
// ----------------------------------------------------------------------------
//   Assign an unknown type to a name
// ----------------------------------------------------------------------------
{
    if (!AssignType(what))
        return false;
    return Evaluate(what);
}


bool TypeInference::DoPrefix(Prefix *what)
// ----------------------------------------------------------------------------
//   Assign an unknown type to a prefix and then to its children
// ----------------------------------------------------------------------------
{
    if (!AssignType(what))
        return false;
    
    // Compute sub-expression, which may fail if Evaluate() fails, it's OK
    if (Name *name = what->left->AsName())
    {
        // Make sure we don't care if 'bar' is not defined in 'bar 3'
        XL::Save<bool> savePrototyping(prototyping, true);
        name->Do(this);
    }
    else
    {
        what->left->Do(this);
    }

    what->right->Do(this);
    
    // What really matters is if we can evaluate the top-level expression
    return Evaluate(what);
}


bool TypeInference::DoPostfix(Postfix *what)
// ----------------------------------------------------------------------------
//   Assign an unknown type to a postfix and then to its children
// ----------------------------------------------------------------------------
{
    if (!AssignType(what))
        return false;
    
    // Compute sub-expression, which may fail if Evaluate() fails, it's OK
    if (Name *name = what->right->AsName())
    {
        // Make sure we don't care if 'bar' is not defined in 'bar 3'
        XL::Save<bool> savePrototyping(prototyping, true);
        name->Do(this);
    }
    else
    {
        what->right->Do(this);
    }

    what->left->Do(this);
   
    // What really matters is if we can evaluate the top-level expression
    return Evaluate(what);
}


bool TypeInference::DoInfix(Infix *what)
// ----------------------------------------------------------------------------
//   Special treatment for the special infix forms
// ----------------------------------------------------------------------------
{
    // Case of 'X : T' : Set type of X to T and unify X:T with X
    if (what->name == ":")
        return (AssignType(what->left, what->right) &&
                what->left->Do(this) &&
                AssignType(what) &&
                Unify(what, what->left));

    // Case of 'X -> Y': Analyze type of X and Y, unify them, set type of result
    if (what->name == "->")
        return Rewrite(what);
 
    // For other cases, we assign types to left and right
    if (!AssignType(what))
        return false;

    // For a sequence, both sub-expressions must succeed individually.
    // The type of the sequence is the type of the last statement
    if (what->name == "\n" || what->name == ";")
    {
        if (!what->left->Do(this))
            return false;
        if (!what->right->Do(this))
            return false;
        return Unify(what, what->right);
    }

    // For other infix expressions, evaluate terms, which may individually fail
    what->left->Do(this);
    what->right->Do(this);

    // Success depends on successful evaluation of the complete form
    return Evaluate(what);
}


bool TypeInference::DoBlock(Block *what)
// ----------------------------------------------------------------------------
//   A block has the same type as its children, except if child alone fails
// ----------------------------------------------------------------------------
{
    // Assign a type to the block
    if (!AssignType(what))
        return false;

    // If child succeeds, the block and its child have the same type
    if (what->child->Do(this))
        return Unify(what, what->child);

    // Otherwise, try to find a matching form
    return Evaluate(what);
}


bool TypeInference::AssignType(Tree *expr, Tree *type)
// ----------------------------------------------------------------------------
//   Assign a type to a given tree
// ----------------------------------------------------------------------------
{
    // Check if we already have a type
    if (Tree *existing = types[expr])
    {
        // If no type given, that's it
        if (!type || existing == type)
            return true;

        // We have two types specified for that entity, need to unify
        return UnifyTypes(existing, type);
    }

    // Generate a unique type name if nothing is given
    if (type == NULL)
        type = NewTypeName(expr->Position());

    // Record the type for that tree
    types[expr] = type;

    // Success
    return true;
}


bool TypeInference::Rewrite(Infix *what)
// ----------------------------------------------------------------------------
//   Assign a type to a rewrite
// ----------------------------------------------------------------------------
{
    // Create a context for the rewrite parameters
    Context *childContext = new Context(context, context);
    XL::Save<Context_p> saveContext(context, childContext);

    // Assign types on the left of the rewrite
    XL::Save<bool> proto(prototyping, true);
    if (!what->left->Do(this))
    {
        Ooops("Malformed rewrite pattern $1", what->left);
        return false;
    }

    // Create a new function type for the rewrite tree
    Tree *formType = Type(what->left);
    Tree *valueType = Type(what->right);
    Infix *fntype = new Infix("=>", formType, valueType, what->Position());
    if (!AssignType(what, fntype))
        return false;

    // We need to be able to unify pattern and definition types
    if (!UnifyTypes(formType, valueType))
        return false;

    // The type of the definition is a pattern type, perform unification
    if (what->left->AsInfix())
    {
        Tree *patternType = new Prefix(new Name("type"), what->left,
                                       what->left->Position());
        if (!UnifyTypes(formType, patternType))
            return false;
    }

    // Well done, success!
    return true;
}


bool TypeInference::Evaluate(Tree *what)
// ----------------------------------------------------------------------------
//   Find candidates for the given expression and infer types from that
// ----------------------------------------------------------------------------
{
    // We don't evaluate expressions while prototyping a pattern
    if (prototyping)
        return true;

    // Evaluating constants is always successful
    if (what->IsConstant() && !context->hasConstants)
        return AssignType(what, what);

    // For a name, check if bound and evaluate bound value
    if (Name *name = what->AsName())
    {
        if (Tree *existing = context->Bound(name))
        {
            if (existing == name)
                return true;    // Example: 'true' or 'integer'
            if (!Evaluate(existing))
                return false;
            Tree *etype = Type(existing);
            Tree *ntype = Type(name);
            return UnifyTypes(ntype, etype);
        }
    }

    // Identify all candidate rewrites in the current context
    RewriteCalls_p rc = new RewriteCalls(this);
    ulong key = context->Hash(what);
    context->Evaluate(what, *rc, key, Context::NORMAL_LOOKUP);

    // If we have no candidate, this is a failure
    uint count = rc->candidates.size();
    if (count == 0)
    {
        Ooops("No form matches $1", what);
        return false;
    }

    // The resulting type is the union of all candidates
    Tree *type = rc->candidates[0].type;
    for (uint i = 1; i < count; i++)
        type = UnionType(context, type, rc->candidates[i].type);

    // Memorize the rewrite calls associated to this tree
    rcalls[what] = rc;

    // Perform type unification
    Tree *wtype = Type(what);
    return UnifyTypes(wtype, type);
}


bool TypeInference::Unify(Tree *expr1, Tree *expr2)
// ----------------------------------------------------------------------------
//   Indicates that the two trees must have identical types
// ----------------------------------------------------------------------------
{
    Tree *t1 = Type(expr1);
    Tree *t2 = Type(expr2);

    // If already unified, we are done
    if (t1 == t2)
        return true;

    return UnifyTypes(t1, t2);
}


bool TypeInference::UnifyTypes(Tree *t1, Tree *t2)
// ----------------------------------------------------------------------------
//   Unify two type forms
// ----------------------------------------------------------------------------
//  A type form in XL can be:
//   - A type name              integer
//   - A generic type name      #ABC
//   - A litteral value         0       1.5             "Hello"
//   - A range of values        0..4    1.3..8.9        "A".."Z"
//   - A union of types         0,3,5   integer|real
//   - A block for precedence   (real)
//   - A rewrite specifier      integer => real
//   - The type of a pattern    type (X:integer, Y:integer)
//
// Unification happens almost as "usual" for Algorithm W, except for how
// we deal with XL "shape-based" type constructors, e.g. type(P)
{
    // Make sure we have the canonical form
    t1 = Base(t1);
    t2 = Base(t2);
    if (t1 == t2)
        return true;            // Already unified

    // Strip out blocks in type specification
    if (Block *b1 = t1->AsBlock())
        if (UnifyTypes(b1->child, t2))
            return JoinTypes(b1, t2);
    if (Block *b2 = t2->AsBlock())
        if (UnifyTypes(t1, b2->child))
            return JoinTypes(t1, b2);

    // Lookup type names, replace them with their value
    t1 = LookupTypeName(t1);
    t2 = LookupTypeName(t2);
    if (t1 == t2)
        return true;            // This may have been enough for unifiation

    // If either is a generic, unify with the other
    if (IsGeneric(t1))
        return JoinTypes(t1, t2);
    if (IsGeneric(t2))
        return JoinTypes(t1, t2);

    // If we have a type name at this stage, this is a failure
    if (IsTypeName(t1))
    {
        Ooops("Cannot unify named type $1", t1);
        Ooops("with structured type $1", t2);
        return false;
    }
    if (IsTypeName(t2))
    {
        Ooops("Cannot unify named type $1", t2);
        Ooops("with structured type $1", t1);
        return false;
    }

    // Check if t1 is one of the infix constructor types
    if (Infix *i1 = t1->AsInfix())
    {
        // Function types: Unifies only with a function type
        if (i1->name == "=>")
        {
            if (Infix *i2 = t2->AsInfix())
                if (i2->name == "=>")
                    return
                        UnifyTypes(i1->left, i2->left) &&
                        UnifyTypes(i1->right, i2->right);

            Ooops("Cannot unify function type $1", i1);
            Ooops("with non-function $1", t2);
        }

        // Union types: Unify with either side
        if (i1->name == "|" || i1->name == ",")
            return UnifyTypes(i1->left, t2) || UnifyTypes(i1->right, t2);

        Ooops("Malformed type definition $1", i1);
        return false;
    }
    if (Infix *i2 = t2->AsInfix())
    {
        // Union types: Unify with either side
        if (i2->name == "|" || i2->name == ",")
            return UnifyTypes(t1, i2->left) || UnifyTypes(t1, i2->right);

        Ooops("Malformed type definition $1", i2);
        return false;
    }

    // Check prefix constructor types
    if (Prefix *p1 = t1->AsPrefix())
        if (Name *pn1 = p1->left->AsName())
            if (pn1->value == "type")
                if (Prefix *p2 = t2->AsPrefix())
                    if (Name *pn2 = p2->right->AsName())
                        if (pn2->value == "type")
                            if (UnifyPatterns(p1->right, p2->right))
                                return JoinTypes(t1, t2);

    // None of the above: fail
    Ooops ("Unable to unify $1", t1);
    Ooops ("with $1", t2);
    return false;
}


Tree *TypeInference::Base(Tree *type)
// ----------------------------------------------------------------------------
//   Return the base type for a given type, i.e. after all substitutions
// ----------------------------------------------------------------------------
{
    Tree *chain = type;

    // If we had some unification, find the reference type
    tree_map::iterator found = unifications.find(type);
    while (found != unifications.end())
    {
        type = (*found).second;
        found = unifications.find(type);
        assert(type != chain || !"Circularity in unification chain");
    }

    // Make all elements in chain point to correct type for performance
    while (chain != type)
    {
        Tree_p &u = unifications[chain];
        chain = u;
        u = type;
    }
    
    return type;
}


bool TypeInference::JoinTypes(Tree *base, Tree *other, bool knownGood)
// ----------------------------------------------------------------------------
//   Use 'base' as the prototype for the other type
// ----------------------------------------------------------------------------
{
    if (!knownGood)
    {
        // If we have a type name, prefer that to a more complex form
        // in order to keep error messages more readable
        if (IsTypeName(other) && !IsTypeName(base))
            std::swap(other, base);

        // If what we want to use as a base is a generic and other isn't, swap
        // (otherwise we could later unify through that variable)
        else if (IsGeneric(base))
            std::swap(other, base);
    }

    // Connext the base type classes
    base = Base(base);
    other = Base(other);
    if (other != base)
        unifications[other] = base;
    return true;
}


bool TypeInference::UnifyPatterns(Tree *t1, Tree *t2)
// ----------------------------------------------------------------------------
//   Check if two patterns describe the same tree shape
// ----------------------------------------------------------------------------
{
    if (t1 == t2)
        return true;

    switch(t1->Kind())
    {
    case INTEGER:
        if (Integer *x1 = t1->AsInteger())
            if (Integer *x2 = t2->AsInteger())
                return x1->value == x2->value;
        return false;
    case REAL:
        if (Real *x1 = t1->AsReal())
            if (Real *x2 = t2->AsReal())
                return x1->value == x2->value;
        return false;

    case TEXT:
        if (Text *x1 = t1->AsText())
            if (Text *x2 = t2->AsText())
                return x1->value == x2->value;
        return false;

    case NAME:
        // We don't attempt to allow renames. Names must match, it's simpler.
        if (Name *x1 = t1->AsName())
            if (Name *x2 = t2->AsName())
                return x1->value == x2->value;
        return false;

    case INFIX:
        if (Infix *x1 = t1->AsInfix())
            if (Infix *x2 = t2->AsInfix())
                return
                    x1->name == x2->name &&
                    UnifyPatterns(x1->left, x2->left) &&
                    UnifyPatterns(x1->right, x2->right);

        return false;

    case PREFIX:
        if (Prefix *x1 = t1->AsPrefix())
            if (Prefix *x2 = t2->AsPrefix())
                return
                    UnifyPatterns(x1->left, x2->left) &&
                    UnifyPatterns(x1->right, x2->right);

        return false;

    case POSTFIX:
        if (Postfix *x1 = t1->AsPostfix())
            if (Postfix *x2 = t2->AsPostfix())
                return
                    UnifyPatterns(x1->left, x2->left) &&
                    UnifyPatterns(x1->right, x2->right);

        return false;

    case BLOCK:
        if (Block *x1 = t1->AsBlock())
            if (Block *x2 = t2->AsBlock())
                return
                    x1->opening == x2->opening &&
                    x1->closing == x2->closing &&
                    UnifyPatterns(x1->child, x2->child);

        return false;

    }

    return false;
}


Name * TypeInference::NewTypeName(TreePosition pos)
// ----------------------------------------------------------------------------
//   Automatically generate new type names
// ----------------------------------------------------------------------------
{
    ulong v = id++;
    text  name = "";
    do
    {
        name = char('A' + v % 26) + name;
        v /= 26;
    } while (v);
    return new Name("#" + name, pos);
}


Tree *TypeInference::LookupTypeName(Tree *type)
// ----------------------------------------------------------------------------
//   If we have a type name, lookup its definition
// ----------------------------------------------------------------------------
{
    if (Name *name = type->AsName())
    {
        // Don't lookup type variables (generic names such as #A)
        if (IsGeneric(name->value))
            return name;

        // Check if we have a type definition. If so, use it
        Tree *definition = context->Bound(name);
        if (definition && definition != name)
        {
            JoinTypes(definition, name);
            return Base(definition);
        }
    }

    // Temporary hack: promote constant types to their base type
    kind k = type->Kind();
    switch(k)
    {
    case INTEGER:
        JoinTypes(integer_type, type, true);
        return integer_type;
    case REAL:
        JoinTypes(real_type, type, true);
        return real_type;
    case TEXT:
        JoinTypes(text_type, type, true);
        return text_type;
    default:
        break;
    }

    // Otherwise, simply return input type
    return type;
}



// ============================================================================
//
//   High-level type functions
//
// ============================================================================

Tree *ValueMatchesType(Context *ctx, Tree *type, Tree *value, bool convert)
// ----------------------------------------------------------------------------
//   Checks if a value matches a type, return value or NULL if no match
// ----------------------------------------------------------------------------
{
    // Check if we match some of the built-in leaf types
    if (type == integer_type)
        if (Integer *iv = value->AsInteger())
            return iv;
    if (type == real_type)
    {
        if (Real *rv = value->AsReal())
            return rv;
        if (convert)
        {
            if (Integer *iv = value->AsInteger())
            {
                Tree *result = new Real(iv->value);
                if (ctx->keepSource)
                    xl_set_source(result, value);
                return result;
            }
        }
    }
    if (type == text_type)
        if (Text *tv = value->AsText())
            if (tv->opening == "\"" && tv->closing == "\"")
                return tv;
    if (type == character_type)
        if (Text *cv = value->AsText())
            if (cv->opening == "'" && cv->closing == "'")
                return cv;
    if (type == boolean_type)
        if (Name *nv = value->AsName())
            if (nv->value == "true" || nv->value == "false")
                return nv;
    if (type == tree_type)
        return value;
    if (type == symbol_type)
        if (Name *nv = value->AsName())
            return nv;
    if (type == name_type)
        if (Name *nv = value->AsName())
            if (nv->value.length() && isalpha(nv->value[0]))
                return nv;
    if (type == operator_type)
        if (Name *nv = value->AsName())
            if (nv->value.length() && !isalpha(nv->value[0]))
                return nv;
    if (type == infix_type)
        if (Infix *iv = value->AsInfix())
            return iv;
    if (type == prefix_type)
        if (Prefix *pv = value->AsPrefix())
            return pv;
    if (type == postfix_type)
        if (Postfix *pv = value->AsPostfix())
            return pv;
    if (type == block_type)
        if (Block *bv = value->AsBlock())
            return bv;

    // Check if we match constant values
    if (Integer *it = type->AsInteger())
        if (Integer *iv = value->AsInteger())
            if (iv->value == it->value)
                return iv;
    if (Real *rt = type->AsReal())
        if (Real *rv = value->AsReal())
            if (rv->value == rt->value)
                return rv;
    if (Text *tt = type->AsText())
        if (Text *tv = value->AsText())
            if (tv->value == tt->value &&
                tv->opening == tt->opening &&
                tv->closing == tt->closing)
                return tv;
    if (Name *nt = type->AsName())
        if (value == nt)
            return value;

    // Check if we match one of the constructed types
    if (Block *bt = type->AsBlock())
        return ValueMatchesType(ctx, bt->child, value, convert);
    if (Infix *it = type->AsInfix())
    {
        if (it->name == "|")
        {
            if (Tree *lfOK = ValueMatchesType(ctx, it->left, value, convert))
                return lfOK;
            if (Tree *rtOK = ValueMatchesType(ctx, it->right, value, convert))
                return rtOK;
        }
        else if (it->name == "->")
        {
            if (Infix *iv = value->AsInfix())
                if (iv->name == "->")
                {
                    // REVISIT: Compare function signatures
                    Ooops("Unimplemented: "
                          "signature comparison of $1 and $2",
                          value, type);
                    return iv;
                }
        }
    }
    if (Prefix *pt = type->AsPrefix())
    {
        if (Name *typeKeyword = pt->left->AsName())
        {
            if (typeKeyword->value == "type")
            {
                if (Block *block = pt->right->AsBlock())
                {
                    if (block->child)
                    {
                        // REVISIT: Match value with pattern
                        Ooops("Unimplemented: "
                              "testing $1 against pattern-based type $2",
                              value, type);
                        return value;
                    }
                }
            }
        }
    }

    // Failed to match type
    return NULL;
}


Tree *TypeCoversType(Context *ctx, Tree *type, Tree *test, bool convert)
// ----------------------------------------------------------------------------
//   Check if type 'test' is covered by 'type'
// ----------------------------------------------------------------------------
{
    // Quick exit when types are the same or the tree type is used
    if (type == test)
        return test;
    if (type == tree_type)
        return test;
    if (convert)
    {
        // REVISIT: Could we deduce this without knowing xl_real_cast?
        if (type == real_type && test == integer_type)
            return test;
    }

    // Check if test is constructed
    if (Infix *itst = test->AsInfix())
    {
        if (itst->name == "|")
        {
            // Does 'integer' cover 0 | 1 ? Yes if it covers both
            if (TypeCoversType(ctx, type, itst->left, convert) &&
                TypeCoversType(ctx, type, itst->right, convert))
                return test;
        }
        else if (itst->name == "=>")
        {
            if (Infix *it = type->AsInfix())
            {
                if (it->name == "=>")
                {
                    // REVISIT: Coverage of function types
                    Ooops("Unimplemented: "
                          "Coverage of function $1 by $2",
                          test, type);
                    return test;
                }
            }
        }
    }
    if (Block *btst = test->AsBlock())
        return TypeCoversType(ctx, type, btst->child, convert);

    // General case where the tested type is a value of the type
    if (test->IsConstant())
        if (ValueMatchesType(ctx, type, test, convert))
            return test;

    // Check if we match one of the constructed types
    if (Block *bt = type->AsBlock())
        return TypeCoversType(ctx, bt->child, test, convert);
    if (Infix *it = type->AsInfix())
    {
        if (it->name == "|")
        {
            if (Tree *lfOK = TypeCoversType(ctx, it->left, test, convert))
                return lfOK;
            if (Tree *rtOK = TypeCoversType(ctx, it->right, test, convert))
                return rtOK;
        }
        else if (it->name == "->")
        {
            if (Infix *iv = test->AsInfix())
                if (iv->name == "->")
                {
                    // REVISIT: Compare function signatures
                    Ooops("Unimplemented: "
                          "Signature comparison of $1 against $2",
                          test, type);
                    return iv;
                }
        }
    }
    if (Prefix *pt = type->AsPrefix())
    {
        if (Name *typeKeyword = pt->left->AsName())
        {
            if (typeKeyword->value == "type")
            {
                if (Block *block = pt->right->AsBlock())
                {
                    if (block->child)
                    {
                        // REVISIT: Match test with pattern
                        Ooops("Unimplemented: "
                              "Pattern type comparison of $1 against $2",
                              test, type);
                        return test;
                    }
                }
            }
        }
    }

    // Failed to match type
    return NULL;
}


Tree *TypeIntersectsType(Context *ctx, Tree *type, Tree *test, bool convert)
// ----------------------------------------------------------------------------
//   Check if type 'test' intersects 'type'
// ----------------------------------------------------------------------------
{
    // Quick exit when types are the same or the tree type is used
    if (type == test)
        return test;
    if (type == tree_type || test == tree_type)
        return test;
    if (convert)
    {
        if (type == real_type && test == integer_type)
            return test;
        if (test == real_type && type == integer_type)
            return test;
    }

    // Check if test is constructed
    if (Infix *itst = test->AsInfix())
    {
        if (itst->name == "|")
        {
            // Does 'integer' intersect 0 | 1 ? Yes if it intersects either
            if (TypeIntersectsType(ctx, type, itst->left, convert) ||
                TypeIntersectsType(ctx, type, itst->right, convert))
                return test;
        }
        else if (itst->name == "->")
        {
            if (Infix *it = type->AsInfix())
            {
                if (it->name == "->")
                {
                    // REVISIT: Coverage of function types
                    Ooops("Unimplemented: "
                          "Coverage of function $1 by $2",
                          test, type);
                    return test;
                }
            }
        }
    }
    if (Block *btst = test->AsBlock())
        return TypeIntersectsType(ctx, type, btst->child, convert);

    // General case where the tested type is a value of the type
    if (test->IsConstant())
        if (ValueMatchesType(ctx, type, test, convert))
            return test;

    // Check if we match one of the constructed types
    if (Block *bt = type->AsBlock())
        return TypeIntersectsType(ctx, bt->child, test, convert);
    if (Infix *it = type->AsInfix())
    {
        if (it->name == "|")
        {
            if (Tree *lfOK = TypeIntersectsType(ctx, it->left, test, convert))
                return lfOK;
            if (Tree *rtOK = TypeIntersectsType(ctx, it->right,test, convert))
                return rtOK;
        }
        else if (it->name == "->")
        {
            if (Infix *iv = test->AsInfix())
                if (iv->name == "->")
                {
                    // REVISIT: Compare function signatures
                    Ooops("Unimplemented: "
                          "Signature comparison of $1 against $2",
                          test, type);
                    return iv;
                }
        }
    }
    if (Prefix *pt = type->AsPrefix())
    {
        if (Name *typeKeyword = pt->left->AsName())
        {
            if (typeKeyword->value == "type")
            {
                if (Block *block = pt->right->AsBlock())
                {
                    if (block->child)
                    {
                        // REVISIT: Match test with pattern
                        Ooops("Unimplemented: "
                              "Pattern type comparison of $1 against $2",
                              test, type);
                        return test;
                    }
                }
            }
        }
    }

    // Failed to match type
    return NULL;
}


Tree *UnionType(Context *ctx, Tree *t1, Tree *t2)
// ----------------------------------------------------------------------------
//    Create the union of two types
// ----------------------------------------------------------------------------
{
    if (t1 == NULL)
        return t2;
    if (t2 == NULL)
        return t1;
    if (TypeCoversType(ctx, t1, t2, false))
        return t1;
    if (TypeCoversType(ctx, t2, t1, false))
        return t2;
    return new Infix("|", t1, t2);
}


Tree *CanonicalType(Context *ctx, Tree *value)
// ----------------------------------------------------------------------------
//   Return the canonical type for the given value
// ----------------------------------------------------------------------------
{
    Tree *type = tree_type;
    (void) ctx;
    switch (value->Kind())
    {
    case INTEGER:
    case REAL:
    case TEXT:          type = value; break;
    case NAME:          type = symbol_type; break;
    case INFIX:         type = infix_type; break;
    case PREFIX:        type = prefix_type; break;
    case POSTFIX:       type = postfix_type; break;
    case BLOCK:         type = block_type; break;
    }
    return type;
}


Tree *StructuredType(Context *ctx, Tree *value)
// ----------------------------------------------------------------------------
//   Return the type of a structured value
// ----------------------------------------------------------------------------
{
    // First check if we already figured out the type for this
    Tree *type = value->Get<TypeInfo>();
    if (type)
        return type;

    // If there is no type, we need to be pessimistic
    type = tree_type;

    switch(value->Kind())
    {
    case INTEGER:
    case REAL:
    case TEXT:
        // Constants have themselves as type
        type = value;
        break;

    case NAME:
        // For names, we may be lucky and have a name in the value
        if (Tree *ref = ctx->Bound((Name *) value))
            if (ref != value)
                type = StructuredType(ctx, ref);
        break;

    case INFIX:
        if (Infix *infix = (Infix *) value)
        {
            Tree *lt = StructuredType(ctx, infix->left);
            Tree *rt = StructuredType(ctx, infix->right);
            type = new Infix(infix->name, lt, rt, infix->Position());
        }
        break;

    case PREFIX:
        type = prefix_type;
        break;

    case POSTFIX:
        type = postfix_type;
        break;

    case BLOCK:
        if (Block *block = (Block *) value)
            type = StructuredType(ctx, block->child);
        break;
    }

    // Memorize the type for next time
    if (type && type != tree_type)
    {
        IFTRACE(types)
            std::cerr << "Caching type " << type << " for " << value << '\n';
        value->Set<TypeInfo>(type);
    }

    return type;
}

XL_END


void debugt(XL::TypeInference *ti)
// ----------------------------------------------------------------------------
//   Dump a type inference
// ----------------------------------------------------------------------------
{
    using namespace XL;
    uint i = 0;

    tree_map &map = ti->types;
    for (tree_map::iterator t = map.begin(); t != map.end(); t++)
    {
        Tree *value = (*t).first;
        Tree *type = (*t).second;
        Tree *base = ti->Base(type);
        std::cout << "#" << ++i << "\t" << value << "\t: " << type;
        if (base != type)
            std::cout << "\t= " << base;
        std::cout << "\n";
    }
}


void debugu(XL::TypeInference *ti)
// ----------------------------------------------------------------------------
//   Dump type unifications in a given inference system
// ----------------------------------------------------------------------------
{
    using namespace XL;
    uint i = 0;

    tree_map &map = ti->unifications;
    for (tree_map::iterator t = map.begin(); t != map.end(); t++)
    {
        Tree *value = (*t).first;
        Tree *type = (*t).second;
        std::cout << "#" << ++i << "\t" << value << "\t= "
                  << type << "\t= " << ti->Base(type) << "\n";
    }
}



void debugr(XL::TypeInference *ti)
// ----------------------------------------------------------------------------
//   Dump rewrite calls associated with each tree in this type inference system
// ----------------------------------------------------------------------------
{
    using namespace XL;
    uint i = 0;

    rcall_map &map = ti->rcalls;
    for (rcall_map::iterator t = map.begin(); t != map.end(); t++)
    {
        Tree *expr = (*t).first;
        std::cout << "#" << ++i << "\t" << expr << "\n";

        uint j = 0;
        RewriteCalls *calls = (*t).second;
        RewriteCandidates &rc = calls->candidates;
        for (RewriteCandidates::iterator r = rc.begin(); r != rc.end(); r++)
        {
            std::cout << "\t#" << j
                      << "\t" << (*r).rewrite->from
                      << "\t: " << (*r).type << "\n";

            RewriteBindings &rb = (*r).bindings;
            for (RewriteBindings::iterator b = rb.begin(); b != rb.end(); b++)
                std::cout << "\t\t" << (*b).name
                          << "\t= " << (*b).value << "\n";
        }
    }
}
