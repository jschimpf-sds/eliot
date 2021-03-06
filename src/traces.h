#ifndef TRACES_H
#define TRACES_H
// ****************************************************************************
//   traces.h                                                    ELIOT project
// ****************************************************************************
//
//   File Description:
//
//     Traces declarations. This file relies on traces.tbl to build a class
//     derived from ELIOT::Traces (traces_base.h).
//
//
//
//
//
//
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
// ****************************************************************************


#include "traces_base.h"


#define TRACE(name)
#include "traces.tbl"
#undef TRACE

#ifdef ELIOT_TRACE_GROUP

#define _ELIOT_MAKE_TRACE_CLASSNAME(x)    x##Traces
#define  ELIOT_MAKE_TRACE_CLASSNAME(x)   _ELIOT_MAKE_TRACE_CLASSNAME(x)
#define  ELIOT_TRACE_CLASSNAME            ELIOT_MAKE_TRACE_CLASSNAME(ELIOT_TRACE_GROUP)

#define _ELIOT_MAKE_TRACE_INSTNAME(x)     x##TracesInstance
#define  ELIOT_MAKE_TRACE_INSTNAME(x)    _ELIOT_MAKE_TRACE_INSTNAME(x)
#define  ELIOT_TRACE_INSTNAME             ELIOT_MAKE_TRACE_INSTNAME(ELIOT_TRACE_GROUP)

class ELIOT_TRACE_CLASSNAME : public ELIOT::Traces
// ----------------------------------------------------------------------------
//   Wrap all the trace flags for the group
// ----------------------------------------------------------------------------
{
public:

    ELIOT_TRACE_CLASSNAME() :
#define TRACE(name)     name(false),
#include "traces.tbl"
#undef TRACE
        unused(false)
    {
#define TRACE(name)        groupAddTrace(#name, &name);
#include "traces.tbl"
#undef  TRACE
#define _STRINGIFY(x) #x
#define STRINGIFY(x)  _STRINGIFY(x)
        addGroup(STRINGIFY(ELIOT_TRACE_GROUP), this);
#undef  STRINGIFY
    }

#define TRACE(name)     bool name;
#include "traces.tbl"
#undef  TRACE
    bool unused;
};

extern ELIOT_TRACE_CLASSNAME *ELIOT_TRACE_INSTNAME;

#define ELIOT_DEFINE_TRACES  ELIOT_TRACE_CLASSNAME *ELIOT_TRACE_INSTNAME = NULL;
#define ELIOT_INIT_TRACES()                                     \
    do {                                                        \
        if (!ELIOT_TRACE_INSTNAME)                              \
            ELIOT_TRACE_INSTNAME = new ELIOT_TRACE_CLASSNAME(); \
    } while(0)

#endif // ELIOT_TRACE_GROUP

#endif
