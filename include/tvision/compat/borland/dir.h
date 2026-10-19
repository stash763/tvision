/*  dir.h

    Defines structures, macros, and functions for dealing with
    directories and pathnames.

*/

/*
 *      C/C++ Run Time Library - Version 6.0
 *
 *      Copyright (c) 1987, 1993 by Borland International
 *      All Rights Reserved.
 *
 */

#ifdef __BORLANDC__
#include <dir.h>
#else

#ifndef TVISION_COMPAT_DIR_H
#define TVISION_COMPAT_DIR_H

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#if defined( __WATCOMC__ )
/* chdir -- the one thing in here that <unistd.h> does not provide on OS/2.
   Watcom declares it in <direct.h>; measured by compiling a call to chdir against
   each candidate header: direct.h yes, unistd.h no, io.h no.  TChDirDialog calls
   it, and fails with "symbol 'chdir' has not been declared" until this is here. */
#include <direct.h>
#endif
#endif

#include "_defs.h"

#include <stdint.h>

#define WILDCARDS 0x01
#define EXTENSION 0x02
#define FILENAME  0x04
#define DIRECTORY 0x08
#define DRIVE     0x10

#define MAXDRIVE  3

#ifndef _FFBLK_DEF
#define _FFBLK_DEF
struct  ffblk   {
    int32_t         ff_reserved;
    int32_t         ff_fsize;
    uint32_t        ff_attrib;
    unsigned short  ff_ftime;
    unsigned short  ff_fdate;
    char            ff_name[256];
};
#endif

/* The file-attribute bits, which findfirst takes as its 'attrib' argument and
   which the classic layer tests against ff_attrib (five files use FA_DIREC).
   Borland's <dir.h> defines these next to struct ffblk, so an emulation of
   <dir.h> that leaves them out is not a complete one; they were being picked up
   from <dos.h> instead, which works until a compiler ships a dos.h of its own
   that cannot be shadowed.  Open Watcom does: its dos.h is the 16-bit DOS one,
   it defines no FA_ names, and D:\WATCOM\h is searched before any include path.
   Measured by five files failing with "symbol 'FA_DIREC' has not been declared".
   Same values as compat/borland/dos.h carries. */

#ifndef FA_NORMAL
#define FA_NORMAL   0x00        /* Normal file, no attributes */
#define FA_RDONLY   0x01        /* Read only attribute */
#define FA_HIDDEN   0x02        /* Hidden file */
#define FA_SYSTEM   0x04        /* System file */
#define FA_LABEL    0x08        /* Volume label */
#define FA_DIREC    0x10        /* Directory */
#define FA_ARCH     0x20        /* Archive */
#endif

/* The date and time a directory entry carries, packed into bit fields the way
   Borland packs them: struct ffblk's ff_ftime and ff_fdate hold this, so does
   TSearchRec's 'time' member, and stddlg.cpp unpacks it to print when a file was
   last written.  compat/borland/io.h defines it, and that reaches every compiler
   except one that ships an io.h of its own -- Open Watcom does, and nothing can
   shadow it.  Measured: stddlg.cpp's 'struct ftime *' failed as 'attempt to use
   pointer to undefined class'.  A type wanted by two headers, only one of which
   is reachable everywhere, has to be guarded rather than copied, so the same
   guard is in compat/borland/io.h and whichever header comes first defines it. */

#ifndef TVISION_COMPAT_FTIME
#define TVISION_COMPAT_FTIME
struct  ftime   {
  unsigned    ft_tsec  : 5;   /* Two second interval */
  unsigned    ft_min   : 6;   /* Minutes */
  unsigned    ft_hour  : 5;   /* Hours */
  unsigned    ft_day   : 5;   /* Days */
  unsigned    ft_month : 4;   /* Months */
  unsigned    ft_year  : 7;   /* Year */
};
#endif

#define MAXPATH   260
#define MAXDIR    256
#define MAXFILE   256
#define MAXEXT    256

int findfirst( const char *__path, struct ffblk *__ffblk, int __attrib ) noexcept;
int findnext( struct ffblk *__ffblk ) noexcept;
void fnmerge( char *__path,
              const char *__drive,
              const char *__dir,
              const char *__name,
              const char *__ext ) noexcept;
int fnsplit( const char *__path,
             char *__drive,
             char *__dir,
             char *__name,
             char *__ext ) noexcept;
int getcurdir( int __drive, char *__directory ) noexcept;
int getdisk( void ) noexcept;
int setdisk( int __drive ) noexcept;

#endif // TVISION_COMPAT_DIR_H

#endif // __BORLANDC__
