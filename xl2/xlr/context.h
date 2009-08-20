#ifndef CONTEXT_H
#define CONTEXT_H
// ****************************************************************************
//  context.h                       (C) 1992-2003 Christophe de Dinechin (ddd) 
//                                                                 XL2 project 
// ****************************************************************************
// 
//   File Description:
// 
//     The execution environment for XL
// 
// 
// 
// 
// 
// 
// 
// 
// ****************************************************************************
// This program is released under the GNU General Public License.
// See http://www.gnu.org/copyleft/gpl.html for details
// ****************************************************************************
// * File       : $RCSFile$
// * Revision   : $Revision$
// * Date       : $Date$
// ****************************************************************************

#include <map>
#include <set>
#include <vector>
#include "base.h"

XL_BEGIN

struct Tree;
struct Context;
struct Errors;

typedef std::map<text, Tree *>  symbol_table;
typedef std::set<Tree *>        active_set;


struct Namespace
// ----------------------------------------------------------------------------
//   Holds the symbols in a given context
// ----------------------------------------------------------------------------
{
    Namespace(Namespace *p): parent(p) {}

    Namespace *         Parent()                { return parent; }

    // Symbol management
    Tree *              Name (text name, bool deep = true);
    Tree *              Infix (text name, bool deep = true);
    Tree *              Block (text opening, bool deep = true);

    // Entering symbols in the symbol table
    void                EnterName (text name, Tree *value);
    void                EnterInfix (text name, Tree *value);
    void                EnterBlock (text name, Tree *value);

protected:
    Namespace *         parent;
    symbol_table        name_symbols;
    symbol_table        infix_symbols;
    symbol_table        block_symbols;
};


struct Context : Namespace
// ----------------------------------------------------------------------------
//   This is the execution context in which we evaluate trees
// ----------------------------------------------------------------------------
{
    // Constructors and destructors
    Context(Errors &err):
        Namespace(NULL), types(NULL),
        errors(err), gc_threshold(200), error_handler(NULL) {}
    Context(Context *p):
        Namespace(p), types(p->types),
        errors(p->errors),gc_threshold(200),error_handler(NULL) {}

    // Context properties
    Context *           Parent()                 { return (Context *) parent;}
    Tree *              ErrorHandler();
    void                SetErrorHandler(Tree *e) { error_handler = e; }
    Namespace &         Types()                 { return types; }
    
    // Garbage collection
    void                Root(Tree *t)           { roots.insert(t); }
    void                Mark(Tree *t)           { active.insert(t); }
    void                CollectGarbage();

    // Evaluation of trees
    Tree *              Error (text message, Tree *args = NULL);

public:
    static ulong        gc_increment;
    static ulong        gc_growth_percent;
    static Context *    context;

private:
    Namespace           types;
    Errors &            errors;
    active_set          active;
    active_set          roots;
    ulong               gc_threshold;
    Tree *              error_handler;
};

XL_END

#endif // CONTEXT_H
