#ifndef TVISION_IOSFWD_H
#define TVISION_IOSFWD_H

#ifdef __BORLANDC__

#include <_defs.h>

class _EXPCLASS ostream;
class _EXPCLASS streambuf;
typedef long streampos;
typedef long streamoff;

#else

#if defined( __WATCOMC__ )
/* <ios>, not <iosfwd>.  Watcom's <iosfwd> declares ostream and streambuf but
   stops there: the two position types below are not in it, and this is the only
   header that would provide them to the rest of the library.  Measured against
   five candidates with a probe program (probe_hdr.cpp, kept with the OS/2 port
   of this library) -- of which <streambuf> is not one,
   despite appearances -- <ios> is the lightest that declares all four names, and
   <iostream> and <fstream> are far heavier than anything every translation unit
   of this library reads should be carrying. */
#include <ios>
#else
#include <iosfwd>
#endif

using std::ostream;
using std::streambuf;
using std::streampos;
using std::streamoff;

#endif // __BORLANDC__

#endif // TVISION_IOSFWD_H
