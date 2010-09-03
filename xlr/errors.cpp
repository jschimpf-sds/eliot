// ****************************************************************************
//   Christophe de Dinechin                                       XL2 PROJECT
//   XL COMPILER: errors.cpp
// ****************************************************************************
//
//   File Description:
//
//    Handling the compiler errors
//
//
//
//
//
//
//
//
// ****************************************************************************
// This document is distributed under the GNU General Public License
// See the enclosed COPYING file or http://www.gnu.org for information
// ****************************************************************************
// * File       : $RCSFile$
// * Revision   : $Revision$
// * Date       : $Date$
// ****************************************************************************

#include <sstream>
#include <iostream>
#include <stdio.h>
#include "errors.h"
#include "options.h"
#include "scanner.h" // for Positions
#include "context.h" // For error display
#include "tree.h"
#include "main.h"

XL_BEGIN

// ============================================================================
//
//   Encapsulating an error message
//
// ============================================================================

Error::Error(text m, ulong p)
// ----------------------------------------------------------------------------
//   Error without arguments
// ----------------------------------------------------------------------------
    : message(m), position(p)
{}


Error::Error(text m, Tree *a)
// ----------------------------------------------------------------------------
//   Error with a tree argument
// ----------------------------------------------------------------------------
    : message(m), position(UNKNOWN_POSITION)
{
    Arg(a);
}


Error::Error(text m, Tree *a, Tree *b)
// ----------------------------------------------------------------------------
//   Error with two tree arguments
// ----------------------------------------------------------------------------
    : message(m), position(UNKNOWN_POSITION)
{
    Arg(a); Arg(b);
}


Error::Error(text m, Tree *a, Tree *b, Tree *c)
// ----------------------------------------------------------------------------
//   Error with three tree arguments
// ----------------------------------------------------------------------------
    : message(m), position(UNKNOWN_POSITION)
{
    Arg(a); Arg(b); Arg(c);
}


Error &Error::Arg(text t)
// ----------------------------------------------------------------------------
//   Add an argument to the message, replacing $1, $2, ...
// ----------------------------------------------------------------------------
{
    arguments.push_back(t);
    return *this;
}


Error &Error::Arg(long value)
// ----------------------------------------------------------------------------
//   Add an argument to the message, replacing $1, $2, ...
// ----------------------------------------------------------------------------
{
    std::ostringstream out;
    out << value;
    arguments.push_back(out.str());
    return *this;
}


Error &Error::Arg(Tree *arg)
// ----------------------------------------------------------------------------
//   Add a tree argument, using its position if applicable
// ----------------------------------------------------------------------------
{
    if (arg && position == UNKNOWN_POSITION)
        position = arg->Position();
    arguments.push_back(text(*arg));
    return *this;
}


void Error::Display()
// ----------------------------------------------------------------------------
//   Display an error on the error output
// ----------------------------------------------------------------------------
{
    std::cerr << Position() << ": " << Message() << '\n';
}


text Error::Position()
// ----------------------------------------------------------------------------
//   Emit the position in a human-readable form
// ----------------------------------------------------------------------------
{
    switch (position)
    {
    case UNKNOWN_POSITION:      return "<Unknown position>";
    case COMMAND_LINE:          return "<Command line>";
    }

    text  file, source;
    ulong line, column;
    std::ostringstream out;
    MAIN->positions.GetInfo(position, &file, &line, &column, &source);
    out << file << ":" << line;
    return out.str();
}


text Error::Message()
// ----------------------------------------------------------------------------
//    Return the error message for an error
// ----------------------------------------------------------------------------
{
    text result = message;
    for (uint i = 0; i < arguments.size(); i++)
    {
        char buffer[10];
        sprintf(buffer, "$%d", i+1);
        size_t found = result.find(buffer);
        if (found != result.npos)
            result.replace(found, strlen(buffer), arguments[i]);
    }
    return result;
}


Error::operator Tree *()
// ----------------------------------------------------------------------------
//   Convert an error message to a tree encapsulating the error
// ----------------------------------------------------------------------------
{
    Text *msg = new Text(Message(), "\"", "\"", position);
    Name *name = new Name("error", position);
    Prefix *result = new Prefix(name, msg, position);
    return result;
}



// ============================================================================
//
//   Table of all error messages
//
// ============================================================================

Errors::Errors()
// ----------------------------------------------------------------------------
//   Save errors from the top-level error handler
// ----------------------------------------------------------------------------
    : parent(MAIN->errors), count(0)
{
    MAIN->errors = this;
}


Errors::~Errors()
// ----------------------------------------------------------------------------
//   Display errors to top-levle handler
// ----------------------------------------------------------------------------
{
    assert (MAIN->errors == this);
    MAIN->errors = parent;

    if (ulong sz = errors.size())
    {
        if (parent)
            parent->count += sz;
        Display();
    }
}


void Errors::Clear()
// ----------------------------------------------------------------------------
//   Clear error messages at the current level
// ----------------------------------------------------------------------------
{
    errors.clear();
}


void Errors::Display()
// ----------------------------------------------------------------------------
//   Display pending error messages
// ----------------------------------------------------------------------------
{
    std::vector<Error>::iterator e;
    for (e = errors.begin(); e != errors.end(); e++)
        (*e).Display();
}


Error &Errors::Log(const Error &e)
// ----------------------------------------------------------------------------
//   Log an error
// ----------------------------------------------------------------------------
{
    errors.push_back(e);
    return errors.back();
}



// ============================================================================
//
//   Quick error reporting functions
//
// ============================================================================

Error &Ooops (text m, ulong pos)
// ----------------------------------------------------------------------------
//   Report an error message without arguments
// ----------------------------------------------------------------------------
{
    return MAIN->errors->Log(Error(m, pos));
}


Error &Ooops (text m, Tree *a)
// ----------------------------------------------------------------------------
//   Report an error message with one tree argument
// ----------------------------------------------------------------------------
{
    return MAIN->errors->Log(Error(m, FormatTreeForError(a)));
}


Error &Ooops (text m, Tree *a, Tree *b)
// ----------------------------------------------------------------------------
//   Report an error message with two tree arguments
// ----------------------------------------------------------------------------
{
    return MAIN->errors->Log(Error(m,
                                   FormatTreeForError(a),
                                   FormatTreeForError(b)));
}


Error &Ooops (text m, Tree *a, Tree *b, Tree *c)
// ----------------------------------------------------------------------------
//   Report an error message with three tree arguments
// ----------------------------------------------------------------------------
{
    return MAIN->errors->Log(Error(m,
                                   FormatTreeForError(a),
                                   FormatTreeForError(b),
                                   FormatTreeForError(c)));
}


Text *FormatTreeForError(Tree *tree)
// ----------------------------------------------------------------------------
//   Format a tree for error reporting
// ----------------------------------------------------------------------------
{
    // Max length in error messages
    const size_t max = 30;

    text t = *tree;
    size_t first = t.find("\n");
    if (first != text::npos)
    {
        size_t last = t.find("\n");
        t.erase(first, last - first);
        t.insert(first, "...");
    }
    size_t length = t.length();
    if (length > max + 3)
        t.replace(max/2, length - max, "...");

    Text *result = new Text(t, "'", "'", tree->Position());
    return result;
}

XL_END



// ============================================================================
//
//    Runtime support (in global namespace)
//
// ============================================================================


void xl_assert_failed(kstring msg, kstring file, uint line)
// ----------------------------------------------------------------------------
//   Report an assertion failure
// ----------------------------------------------------------------------------
{
    fprintf(stderr, "%s:%u: Assertion failed: %s\n",
            file, line, msg);
    abort();
}