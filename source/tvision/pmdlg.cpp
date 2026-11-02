/* tvision/source/tvision/pmdlg.cpp -- the native dialogs of the PM front.

   Why this file lives in the driver's tree and not in the application: the
   dialog code has to know it is talking PM, and the only translation unit
   that is ever told (ATUI_PM, set by `build.cmd pmdrv`) is one compiled
   beside hardwspm.cpp.  The application is compiled once for every front,
   so it owns one pointer -- gPmDialogs in src/tui/pmdialogs.h -- that this
   unit fills at window birth; a console front reads null forever and draws
   its tvision dialogs, which stay the reference implementations.  pmdlg.cpp
   carries UI and nothing else: every fact a dialog shows or changes comes
   from the application through the little C interface in that header.

   The dialog templates are resources (tools/os2/atuipm.rc), written header-
   less with bare numbers, which the box's RC/2 was measured to accept
   (2026-10-31: DLGTEMPLATE 900.4 written from a probe of that spelling).
   The numbers are PM's, not the Windows ones the reflexes expect: BS_ is
   bitfield-composed (BS_PUSHBUTTON is 0, BS_AUTOCHECKBOX 2, BS_AUTORADIO-
   BUTTON 4), WS_VISIBLE is 0x80000000, and the coordinates grow upward from
   the bottom edge.  Each value here and in the rc was read from the box's
   own headers (Watcom's pmwin.h; WS_VSCROLL/WS_HSCROLL from the toolkit's
   os2win.h, Watcom's copy not carrying them -- another 1.9 vocabulary gap
   like WinQuerySysInfo and WinSetTimer), not guessed.

   The opening mechanism is eFTE's, measured path: WinDlgBox(HWND_DESKTOP,
   frame, proc, 0, templateId, NULL) -- the templates ride in the image, and
   hmod 0 finds them there (g_pm.cpp:1539).  WinDlgBox runs its own modal
   loop, which is what suspends the tvision event loop for the duration;
   streamed replies keep arriving in the worker's queue meanwhile, exactly
   as behind a tvision modal dialog. */

#if defined( __FLAT__ ) && defined( __OS2__ ) && !defined( __BORLANDC__ ) \
    && defined( ATUI_PM )

/* The include ritual is hardwspm.cpp's, for hardwspm.cpp's reasons: the
   INCL_ group first, tv.h next -- which is the one that pulls <os2.h> in
   through compat's windows header -- and never INCL_WININPUT, whose VK_*
   definitions and tvision's compat header cannot share a compilation.  */
#define INCL_DOSPROCESS
#define INCL_DOSERRORS
#define INCL_GPI
#define INCL_WIN        // ...and Watcom's pmwin.h takes INCL_WIN to mean
                        // all fifty-odd window groups, dialogs (WinDlgBox,
                        // DID_*, MB_*) and listboxes (LM_*) included --
                        // measured from its own #ifdef ladder, lines 26..80
#define INCL_VIO
#define INCL_AVIO

#define Uses_TKeys
#define Uses_TEvent
#define Uses_TScreen
#define Uses_THardwareInfo
#include <tvision/tv.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>           // atol, for the settings fields

#include "tui/pmdialogs.h"

/* hardwspm.cpp owns the window; the dialogs want it as their owner so they
   sit above it and the frame disables behind them (the modal loop's doing,
   not ours). */
extern HWND pmFrameWindow( void );
extern void pmNoteDialog( int delta );    // hardwspm.cpp: the driver
                                          // defers resizes while the
                                          // count is up

/* LM_QUERYSELECTION's "nothing selected" answer is the -1 MPARAM that later
   SDKs spell MP_DELNEG; neither header set this build reads carries the
   name (measured), so the number gets its own. */
const unsigned short kNoListSelection = 0xffff;

/* Ids shared with the rc, the same two-files-one-contract arrangement as
   the menu ids and pmTakeCommand. */
enum
{
    kDlgTheme       = 901,
    kDlgSwitchModel = 902,
    kDlgSettings    = 903,
    kDlgProviders   = 904,
    kDlgUndo        = 905,

    kCtlThemeClassic = 401,
    kCtlThemeDark    = 402,
    kCtlModelList    = 403,

    /* settings (903): the fields carry the code's interest; the labels
       and range notes are statics the init fills but never queries. */
    kCtlSetWheel      = 431,
    kCtlSetTimeout    = 432,
    kCtlSetModeAsk    = 433,
    kCtlSetModeExact  = 434,
    kCtlSetModeAgent  = 435,
    kCtlSetJudgeModel = 436,
    kCtlSetJudgeSec   = 437,
    kCtlSetCmdTimeout = 438,
    kCtlSetNoteWheel  = 441,
    kCtlSetNoteReq    = 442,
    kCtlSetNoteJudge  = 446,
    kCtlSetNoteCmd    = 448,

    /* providers (904) */
    kCtlProvList      = 451,
    kCtlProvNew       = 452,
    kCtlProvDel       = 453,
    kCtlProvActive    = 454,
    kCtlProvName      = 455,
    kCtlProvUrl       = 456,
    kCtlProvKey       = 457,
    kCtlProvModel     = 458,
    kCtlProvTemp      = 459,
    kCtlProvResend    = 460,
    kCtlProvRefresh   = 467,
    kCtlProvModelList = 468,
    kCtlProvStatus    = 469,

    /* undo (905) */
    kCtlUndoHeader    = 480,
    kCtlUndoList      = 481,
    kCtlUndoRestore   = 482,
    kCtlUndoDiscard   = 483,
    kCtlUndoStatus    = 484
};

/* LM_QUERYITEMTEXT answers through the toolkit's STRREC (os2wrh.h: a
   length over a buffer).  Neither header set this build reads carries
   the name (measured: no STRREC anywhere in Watcom's h/os2 or the
   toolkit's h), and the records are one contract with the control, so
   it is copied with its provenance the way MLEOVERFLOW was. */
typedef struct _ATUISTRREC
    {
    USHORT usStrLength;
    PSZ    pszBuffer;
    } ATUISTRREC;

/* "This one was shown": WM_INITDLG only runs on a dialog that made it to
   the screen, so it is the proof the box itself cannot give -- a WinDlgBox
   that fails to load its template and a dialog the user cancelled both
   come back as zero.  Cancel is an answer, handled; failure to appear is
   not, and only that is worth the tvision fallback.  One flag serves
   both dialogs: they are never up together -- each runs its own modal
   loop, and the count in the driver says so. */
static int gDlgShown = 0;

/* ------------------------------------------------------------------ theme */

MRESULT EXPENTRY pmThemeDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch( msg )
    {
    case WM_INITDLG:
        // The radios are BS_AUTORADIOBUTTON: the dialog manager does the
        // either/or from a click.  Only the current theme has to be shown
        // checked when the dialog opens.
        if( pmThemeCurrent() == 0 )
            WinSendDlgItemMsg( hwnd, kCtlThemeClassic, BM_SETCHECK,
                               MPVOID, MPVOID );
        gDlgShown = 1;          // on screen: the wrapper's proof
        return 0;

    case WM_COMMAND:
        switch( SHORT1FROMMP( mp1 ) )
        {
        case DID_OK:
            pmThemeApply( WinSendDlgItemMsg( hwnd, kCtlThemeClassic,
                                             BM_QUERYCHECK,
                                             MPVOID, MPVOID ) ? 0 : 1 );
            WinDismissDlg( hwnd, TRUE );
            return 0;
        case DID_CANCEL:
            WinDismissDlg( hwnd, FALSE );
            return 0;
        }
        break;

    case WM_CLOSE:
        WinDismissDlg( hwnd, FALSE );
        return 0;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

static int pmThemeDialog( void )
{
    // The dialog applies the theme itself (pmThemeApply from its own
    // WM_COMMAND); the return value only says whether the PM front had
    // this one, and shown is the answer -- cancel included, which is
    // why == DID_OK was wrong: it called an honest "no thanks" a
    // failure and the caller then drew the tvision dialog after it.
    // pmNoteDialog (hardwspm.cpp) counts the modal loops: the driver
    // defers any resize that arrives while one is running.
    pmNoteDialog( +1 );
    gDlgShown = 0;
    WinDlgBox( HWND_DESKTOP, pmFrameWindow(), pmThemeDlgProc,
               0, kDlgTheme, NULL );
    const int shown = gDlgShown;
    pmNoteDialog( -1 );
    return shown;
}

/* ------------------------------------------------------------ about ------- */

static int pmAboutDialog( void )
{
    // eFTE's simplest dialog (g_pm.cpp:878): a message box is a dialog, and
    // an About box is a message.  MB_INFORMATION buys the icon and the
    // system beep the toolkit prescribes.
    pmNoteDialog( +1 );
    WinMessageBox( HWND_DESKTOP, pmFrameWindow(), pmAboutText(),
                   "atui - About", 0, MB_INFORMATION | MB_OK );
    pmNoteDialog( -1 );
    return 1;
}

/* -------------------------------------------------------- switch model ---- */

MRESULT EXPENTRY pmSwitchModelDlgProc( HWND hwnd, ULONG msg,
                                       MPARAM mp1, MPARAM mp2 )
{
    switch( msg )
    {
    case WM_INITDLG:
        {
        // The list is app knowledge, and this is the one moment it may be
        // read: the dialog is modal, the provider store cannot change
        // underneath it.
        const int count = pmModelCount();
        for( int i = 0; i < count; ++i )
            WinSendDlgItemMsg( hwnd, kCtlModelList, LM_INSERTITEM,
                               MPFROMSHORT( LIT_END ),
                               MPFROMP( (PVOID) pmModelLabel( i ) ) );
        const int active = pmModelActive();
        if( active >= 0 && active < count )
            WinSendDlgItemMsg( hwnd, kCtlModelList, LM_SELECTITEM,
                               MPFROMSHORT( (short) active ),
                               MPFROMSHORT( TRUE ) );
        gDlgShown = 1;          // on screen: the wrapper's proof
        return 0;
        }

    case WM_COMMAND:
        switch( SHORT1FROMMP( mp1 ) )
        {
        case DID_OK:
            {
            const USHORT sel = SHORT1FROMMR( WinSendDlgItemMsg( hwnd,
                kCtlModelList, LM_QUERYSELECTION, MPFROMSHORT( LIT_FIRST ),
                MPVOID ) );
            if( sel != kNoListSelection )   // nothing selected says no, too
                {
                pmModelChosen( (int) sel );
                WinDismissDlg( hwnd, TRUE );
                return 0;
                }
            return 0;
            }
        case DID_CANCEL:
            WinDismissDlg( hwnd, FALSE );
            return 0;
        }
        break;

    case WM_CLOSE:
        WinDismissDlg( hwnd, FALSE );
        return 0;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

static int pmSwitchModelDialog( void )
{
    if( pmModelCount() == 0 )
        return 0;
    pmNoteDialog( +1 );
    gDlgShown = 0;
    WinDlgBox( HWND_DESKTOP, pmFrameWindow(), pmSwitchModelDlgProc,
               0, kDlgSwitchModel, NULL );
    const int shown = gDlgShown;
    pmNoteDialog( -1 );
    return shown;
}

/* ------------------------------------------------------- shared dialog tools */

/* Emptying a listbox before refilling it: LM_DELETEITEM one item at a
   time is the shape the toolkit samples show (eas.c:1063), so that is
   what loops. */
static void listClear_( HWND hwndDlg, ULONG id )
{
    SHORT n = SHORT1FROMMR( WinSendDlgItemMsg( hwndDlg, id,
                                               LM_QUERYITEMCOUNT,
                                               MPVOID, MPVOID ) );
    for( ; n > 0; --n )
        WinSendDlgItemMsg( hwndDlg, id, LM_DELETEITEM,
                           MPFROMSHORT( 0 ), MPVOID );
}

/* A row's text, via the STRREC the control answers with.  The length
   query that precedes it is Watcom-declared and says how much the
   control will write, which is what keeps the buffer's NUL honest. */
static Boolean listRowText_( HWND hwndDlg, ULONG id, SHORT row,
                             char *buf, USHORT bufLen )
{
    buf[0] = '\0';
    if( row < 0 || bufLen == 0 )
        return False;
    const USHORT need = (USHORT) SHORT1FROMMR( WinSendDlgItemMsg( hwndDlg,
                                        id, LM_QUERYITEMTEXTLENGTH,
                                        MPFROMSHORT( row ), MPVOID ) );
    if( need + 1 > bufLen )
        return False;           // the answer cannot fit: no row, no action
    ATUISTRREC rec;
    rec.usStrLength = bufLen - 1;
    rec.pszBuffer = (PSZ) buf;
    WinSendDlgItemMsg( hwndDlg, id, LM_QUERYITEMTEXT,
                       MPFROMSHORT( row ), MPFROMLONG( &rec ) );
    buf[ bufLen - 1 ] = '\0';
    return True;
}

static void dlgText_( HWND hwndDlg, ULONG id, char *buf, ULONG bufLen )
{
    buf[0] = '\0';
    WinQueryDlgItemText( hwndDlg, id, (LONG) bufLen, (PCSZ) buf );
}

/* The tvision numberIn quirk, kept whole: a field with no digit in it
   at all means "leave it alone" -- the keep-value answers -- and what
   does count starts at the first digit.  0 in commandTimeout remains a
   real answer either way. */
static long fieldLong_( HWND hwndDlg, ULONG id, long keep )
{
    char buf[ 32 ];
    dlgText_( hwndDlg, id, buf, sizeof( buf ) );
    int i = 0;
    while( buf[i] != '\0' && ( buf[i] < '0' || buf[i] > '9' ) )
        ++i;
    if( buf[i] == '\0' )
        return keep;
    return (long) atol( buf + i );
}

/* ------------------------------------------------------------- settings ---- */

/* The snapshot is held between init and OK so "unchanged" -- a field
   with no digits -- answers with the value the box was opened on, and
   the radio with nothing newly chosen keeps the mode it had. */
static PmSettings gSet;

static void setHide_( HWND hwndDlg, ULONG id )
{
    const HWND h = WinWindowFromID( hwndDlg, id );
    if( h != 0 )
        WinSetWindowPos( h, HWND_TOP, 0, 0, 0, 0, SWP_HIDE );
}

MRESULT EXPENTRY pmSettingsDlgProc( HWND hwnd, ULONG msg,
                                    MPARAM mp1, MPARAM mp2 )
{
    switch( msg )
    {
    case WM_INITDLG:
        {
        pmSettingsSnapshot( &gSet );
        char t[ 32 ];
        std::snprintf( t, sizeof( t ), "%d", gSet.wheelLines );
        WinSetDlgItemText( hwnd, kCtlSetWheel, (PSZ) t );
        std::snprintf( t, sizeof( t ), "%d", gSet.requestMinutes );
        WinSetDlgItemText( hwnd, kCtlSetTimeout, (PSZ) t );
        std::snprintf( t, sizeof( t ), "%d", gSet.judgeSeconds );
        WinSetDlgItemText( hwnd, kCtlSetJudgeSec, (PSZ) t );
        std::snprintf( t, sizeof( t ), "%ld", gSet.commandTimeout );
        WinSetDlgItemText( hwnd, kCtlSetCmdTimeout, (PSZ) t );
        WinSetDlgItemText( hwnd, kCtlSetJudgeModel,
                           (PSZ) gSet.judgeModel );
        WinSendDlgItemMsg( hwnd, kCtlSetJudgeModel, EM_SETTEXTLIMIT,
                           MPFROM2SHORT( 63, 0 ), MPVOID );

        if( gSet.permissionMode == 0 )
            WinSendDlgItemMsg( hwnd, kCtlSetModeAsk, BM_SETCHECK,
                               MPVOID, MPVOID );
        else if( gSet.permissionMode == 1 )
            WinSendDlgItemMsg( hwnd, kCtlSetModeExact, BM_SETCHECK,
                               MPVOID, MPVOID );
        else
            WinSendDlgItemMsg( hwnd, kCtlSetModeAgent, BM_SETCHECK,
                               MPVOID, MPVOID );

        std::snprintf( t, sizeof( t ), "%d to %d",
                       gSet.wheelMin, gSet.wheelMax );
        WinSetDlgItemText( hwnd, kCtlSetNoteWheel, (PSZ) t );
        std::snprintf( t, sizeof( t ), "%d to %d",
                       gSet.requestMin, gSet.requestMax );
        WinSetDlgItemText( hwnd, kCtlSetNoteReq, (PSZ) t );
        std::snprintf( t, sizeof( t ), "%d to %d",
                       gSet.judgeMin, gSet.judgeMax );
        WinSetDlgItemText( hwnd, kCtlSetNoteJudge, (PSZ) t );
        std::snprintf( t, sizeof( t ), "%ld to %ld, 0 = none",
                       gSet.commandMin, gSet.commandMax );
        WinSetDlgItemText( hwnd, kCtlSetNoteCmd, (PSZ) t );

        // The judging fields and their radio are the agent's, and a
        // build without one shows none of them -- absent fields change
        // nothing on OK, exactly as in tvision.
        if( !gSet.agentAvailable )
            {
            setHide_( hwnd, kCtlSetModeAgent );
            setHide_( hwnd, kCtlSetJudgeModel );
            setHide_( hwnd, kCtlSetJudgeSec );
            setHide_( hwnd, kCtlSetNoteJudge );
            }
        gDlgShown = 1;          // on screen: the wrapper's proof
        return 0;
        }

    case WM_COMMAND:
        switch( SHORT1FROMMP( mp1 ) )
        {
        case DID_OK:
            {
            PmSettings s = gSet;
            s.wheelLines = (int) fieldLong_( hwnd, kCtlSetWheel,
                                             gSet.wheelLines );
            s.requestMinutes = (int) fieldLong_( hwnd, kCtlSetTimeout,
                                                 gSet.requestMinutes );
            s.judgeSeconds = (int) fieldLong_( hwnd, kCtlSetJudgeSec,
                                               gSet.judgeSeconds );
            s.commandTimeout = fieldLong_( hwnd, kCtlSetCmdTimeout,
                                           gSet.commandTimeout );
            dlgText_( hwnd, kCtlSetJudgeModel, s.judgeModel,
                      sizeof( s.judgeModel ) );
            // A radio nobody checked (the hidden agent radio of a
            // no-agent build, say) keeps the mode the box opened with.
            if( WinSendDlgItemMsg( hwnd, kCtlSetModeAsk, BM_QUERYCHECK,
                                   MPVOID, MPVOID ) )
                s.permissionMode = 0;
            else if( WinSendDlgItemMsg( hwnd, kCtlSetModeExact,
                                        BM_QUERYCHECK, MPVOID, MPVOID ) )
                s.permissionMode = 1;
            else if( WinSendDlgItemMsg( hwnd, kCtlSetModeAgent,
                                        BM_QUERYCHECK, MPVOID, MPVOID ) )
                s.permissionMode = 2;
            pmSettingsApply( &s );
            WinDismissDlg( hwnd, TRUE );
            return 0;
            }
        case DID_CANCEL:
            WinDismissDlg( hwnd, FALSE );
            return 0;
        }
        break;

    case WM_CLOSE:
        WinDismissDlg( hwnd, FALSE );
        return 0;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* ---------------------------------------------------------------- undo ----- */

static void undoFill_( HWND hwndDlg )
{
    listClear_( hwndDlg, kCtlUndoList );
    const int n = pmUndoCount();     // rebuilds the app-side snapshot
    for( int i = 0; i < n; ++i )
        WinSendDlgItemMsg( hwndDlg, kCtlUndoList, LM_INSERTITEM,
                           MPFROMSHORT( LIT_END ),
                           MPFROMP( (PVOID) pmUndoLabel( i ) ) );
    WinSetDlgItemText( hwndDlg, kCtlUndoHeader, (PSZ) pmUndoHeader() );
}

/* The chosen row's entry, found the tvision way: the rows sort, so the
   row number is nothing and the ordinal that opens the row text is the
   identity.  -1 when nothing sensible is chosen. */
static int undoWhich_( HWND hwndDlg )
{
    const SHORT sel = SHORT1FROMMR( WinSendDlgItemMsg( hwndDlg,
        kCtlUndoList, LM_QUERYSELECTION, MPFROMSHORT( LIT_FIRST ),
        MPVOID ) );
    if( (unsigned short) sel == kNoListSelection )
        return -1;
    char row[ 200 ];
    if( !listRowText_( hwndDlg, kCtlUndoList, sel, row, sizeof( row ) ) )
        return -1;
    int i = 0;
    while( row[i] >= '0' && row[i] <= '9' )
        ++i;
    if( i == 0 )
        return -1;
    row[i] = '\0';
    const int which = (int) atol( row ) - 1;
    if( which < 0 || which >= pmUndoCount() )
        return -1;
    return which;
}

MRESULT EXPENTRY pmUndoDlgProc( HWND hwnd, ULONG msg,
                                MPARAM mp1, MPARAM mp2 )
{
    switch( msg )
    {
    case WM_INITDLG:
        undoFill_( hwnd );
        WinSetDlgItemText( hwnd, kCtlUndoStatus, (PSZ)
            "Restore puts a saved copy back; Discard throws it away." );
        gDlgShown = 1;
        return 0;

    case WM_COMMAND:
        switch( SHORT1FROMMP( mp1 ) )
        {
        case kCtlUndoRestore:
            {
            const int idx = undoWhich_( hwnd );
            if( idx < 0 )
                {
                WinSetDlgItemText( hwnd, kCtlUndoStatus, (PSZ)
                    "Pick a row first." );
                return 0;
                }
            if( pmUndoNeedsConfirm( idx ) )
                {
                char q[ 1024 ];
                pmUndoQuestion( idx, q, sizeof( q ) );
                // The same question askYesNo_ asked, in PM's own box,
                // No as default -- tvision's answer buttons had No so.
                if( WinMessageBox( HWND_DESKTOP, pmFrameWindow(), q,
                                    "Restore file", 0,
                                    MB_YESNO | MB_DEFBUTTON2 )
                    != MBID_YES )
                    {
                    WinSetDlgItemText( hwnd, kCtlUndoStatus, (PSZ)
                        "Not restored: you said no" );
                    return 0;
                    }
                }
            char msgText[ 512 ];
            pmUndoRestore( idx, msgText, sizeof( msgText ) );
            WinSetDlgItemText( hwnd, kCtlUndoStatus, (PSZ) msgText );
            undoFill_( hwnd );
            return 0;
            }
        case kCtlUndoDiscard:
            {
            const int idx = undoWhich_( hwnd );
            if( idx < 0 )
                {
                WinSetDlgItemText( hwnd, kCtlUndoStatus, (PSZ)
                    "Pick a row first." );
                return 0;
                }
            // Discarding is the one action here that cannot be undone.
            char q[ 1024 ];
            std::snprintf( q, sizeof( q ),
                "Throw away the saved copy of\n\n  %s\n\nfrom %s? "
                "Nothing else holds what was in that file before "
                "atui changed it.",
                pmUndoTarget( idx ), pmUndoStamp( idx ) );
            if( WinMessageBox( HWND_DESKTOP, pmFrameWindow(), q,
                                "Discard saved copy", 0,
                                MB_YESNO | MB_DEFBUTTON2 )
                != MBID_YES )
                return 0;
            char msgText[ 512 ];
            pmUndoDiscard( idx, msgText, sizeof( msgText ) );
            WinSetDlgItemText( hwnd, kCtlUndoStatus, (PSZ) msgText );
            undoFill_( hwnd );
            return 0;
            }
        case DID_CANCEL:
            WinDismissDlg( hwnd, FALSE );
            return 0;
        }
        break;

    case WM_CLOSE:
        WinDismissDlg( hwnd, FALSE );
        return 0;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* ----------------------------------------------------------- providers ----- */

/* Which provider the fields describe, and the listbox selections the
   dialog has already applied.  Modal, single instance, same guarantee
   the switch-model list always relied on: the store cannot change
   under these dialogs, and these variables live and die inside one. */
static int    gProvEditing = 0;
static SHORT  gProvRow = -1;
static SHORT  gProvModelRow = -1;

static void provStatus_( HWND hwndDlg, const char *text )
{
    WinSetDlgItemText( hwndDlg, kCtlProvStatus, (PSZ) text );
}

/* The row text of a provider: the name, " *" for the active one --
   tvision's marker, and providerRowToIndex_'s trimming mirrored here:
   names resolve through the text, never through a row number, because
   the listbox sorts. */
static Boolean provTrimRow_( char *row )
{
    size_t n = strlen( row );
    while( n > 0 && ( row[n-1] == ' ' || row[n-1] == '*' ) )
        row[--n] = '\0';
    return n > 0;
}

static int provIndexOfRow_( HWND hwndDlg, SHORT row )
{
    char buf[ 80 ];
    if( !listRowText_( hwndDlg, kCtlProvList, row, buf, sizeof( buf ) ) )
        return -1;
    if( !provTrimRow_( buf ) )
        return -1;
    for( int i = 0; i < pmProvCount(); ++i )
        if( strcmp( buf, pmProvName( i ) ) == 0 )
            return i;
    return -1;
}

static SHORT provRowForName_( HWND hwndDlg, const char *name )
{
    const SHORT n = SHORT1FROMMR( WinSendDlgItemMsg( hwndDlg,
                                        kCtlProvList, LM_QUERYITEMCOUNT,
                                        MPVOID, MPVOID ) );
    for( SHORT r = 0; r < n; ++r )
        {
        char buf[ 80 ];
        if( listRowText_( hwndDlg, kCtlProvList, r, buf, sizeof( buf ) )
            && provTrimRow_( buf ) && strcmp( buf, name ) == 0 )
            return r;
        }
    return -1;
}

static void provModelList_( HWND hwndDlg, const char *chosen )
{
    listClear_( hwndDlg, kCtlProvModelList );
    const int n = pmProvModelCount( gProvEditing );
    for( int m = 0; m < n; ++m )
        WinSendDlgItemMsg( hwndDlg, kCtlProvModelList, LM_INSERTITEM,
                           MPFROMSHORT( LIT_END ),
                           MPFROMP( (PVOID) pmProvModelName( gProvEditing,
                                                             m ) ) );
    // Highlight the chosen model as a selection, and mark that row
    // already applied so the notice handler does not "pick" it back.
    gProvModelRow = -1;
    if( chosen != 0 && chosen[0] != '\0' )
        {
        const SHORT rows = SHORT1FROMMR( WinSendDlgItemMsg( hwndDlg,
            kCtlProvModelList, LM_QUERYITEMCOUNT, MPVOID, MPVOID ) );
        for( SHORT r = 0; r < rows; ++r )
            {
            char buf[ 160 ];
            if( listRowText_( hwndDlg, kCtlProvModelList, r, buf,
                              sizeof( buf ) )
                && strcmp( buf, chosen ) == 0 )
                {
                WinSendDlgItemMsg( hwndDlg, kCtlProvModelList,
                                   LM_SELECTITEM, MPFROMSHORT( r ),
                                   MPFROMSHORT( TRUE ) );
                gProvModelRow = r;
                break;
                }
            }
        }
}

static void provFields_( HWND hwndDlg )   // store -> fields and lists
{
    PmProvider p;
    pmProvGet( gProvEditing, &p );
    WinSetDlgItemText( hwndDlg, kCtlProvName, (PSZ) p.name );
    WinSetDlgItemText( hwndDlg, kCtlProvUrl, (PSZ) p.baseUrl );
    WinSetDlgItemText( hwndDlg, kCtlProvKey, (PSZ) p.apiKey );
    WinSetDlgItemText( hwndDlg, kCtlProvModel, (PSZ) p.model );
    WinSetDlgItemText( hwndDlg, kCtlProvTemp, (PSZ) p.temperature );
    WinSendDlgItemMsg( hwndDlg, kCtlProvResend, BM_SETCHECK,
                       MPFROMLONG( p.resendThinking ), MPVOID );
    provModelList_( hwndDlg, p.model );
}

static void provList_( HWND hwndDlg )     // store -> the providers list
{
    listClear_( hwndDlg, kCtlProvList );
    const int n = pmProvCount();
    const int act = pmProvActive();
    for( int i = 0; i < n; ++i )
        {
        char row[ 80 ];
        std::snprintf( row, sizeof( row ), "%s%s", pmProvName( i ),
                       i == act ? " *" : "" );
        WinSendDlgItemMsg( hwndDlg, kCtlProvList, LM_INSERTITEM,
                           MPFROMSHORT( LIT_END ),
                           MPFROMP( (PVOID) row ) );
        }
    gProvRow = provRowForName_( hwndDlg, pmProvName( gProvEditing ) );
    if( gProvRow >= 0 )
        {
        WinSendDlgItemMsg( hwndDlg, kCtlProvList, LM_SELECTITEM,
                           MPFROMSHORT( gProvRow ), MPFROMSHORT( TRUE ) );
        WinSendDlgItemMsg( hwndDlg, kCtlProvList, LM_SETTOPINDEX,
                           MPFROMSHORT( gProvRow ), MPVOID );
        }
}

static void provCommit_( HWND hwndDlg )   // fields -> store (memory)
{
    PmProvider p;
    memset( &p, 0, sizeof( p ) );
    dlgText_( hwndDlg, kCtlProvName, p.name, sizeof( p.name ) );
    dlgText_( hwndDlg, kCtlProvUrl, p.baseUrl, sizeof( p.baseUrl ) );
    dlgText_( hwndDlg, kCtlProvKey, p.apiKey, sizeof( p.apiKey ) );
    dlgText_( hwndDlg, kCtlProvModel, p.model, sizeof( p.model ) );
    dlgText_( hwndDlg, kCtlProvTemp, p.temperature,
              sizeof( p.temperature ) );
    p.resendThinking = WinSendDlgItemMsg( hwndDlg, kCtlProvResend,
                                          BM_QUERYCHECK, MPVOID,
                                          MPVOID ) ? 1 : 0;
    pmProvSet( gProvEditing, &p );
}

/* A listbox has been noticed: whichever selection actually moved gets
   its action, and a selection that has not moves nothing -- which is
   what makes it safe to call this on every notification the listboxes
   send, without trusting any particular notification code (the MLE
   taught what this box's notifications are worth; the codes themselves
   are the other thing the mirrors never showed for listboxes). */
static void provNoticed_( HWND hwndDlg )
{
    const SHORT sel = SHORT1FROMMR( WinSendDlgItemMsg( hwndDlg,
        kCtlProvList, LM_QUERYSELECTION, MPFROMSHORT( LIT_FIRST ),
        MPVOID ) );
    if( (unsigned short) sel != kNoListSelection && sel != gProvRow )
        {
        const int i = provIndexOfRow_( hwndDlg, sel );
        if( i >= 0 )
            {
            provCommit_( hwndDlg );   // tvision committed before
            gProvEditing = i;         // switching, field-edits and all
            gProvRow = sel;
            provFields_( hwndDlg );
            char t[ 128 ];
            std::snprintf( t, sizeof( t ), "Editing %s",
                           pmProvName( i ) );
            provStatus_( hwndDlg, t );
            }
        }
    const SHORT msel = SHORT1FROMMR( WinSendDlgItemMsg( hwndDlg,
        kCtlProvModelList, LM_QUERYSELECTION, MPFROMSHORT( LIT_FIRST ),
        MPVOID ) );
    if( (unsigned short) msel != kNoListSelection
        && msel != gProvModelRow )
        {
        gProvModelRow = msel;
        char m[ 140 ];
        if( listRowText_( hwndDlg, kCtlProvModelList, msel, m,
                          sizeof( m ) ) && m[0] != '\0' )
            {
            pmProvPickModel( gProvEditing, m );
            WinSetDlgItemText( hwndDlg, kCtlProvModel, (PSZ) m );
            char t[ 176 ];
            std::snprintf( t, sizeof( t ), "Model: %s", m );
            provStatus_( hwndDlg, t );
            }
        }
}

MRESULT EXPENTRY pmProvidersDlgProc( HWND hwnd, ULONG msg,
                                     MPARAM mp1, MPARAM mp2 )
{
    switch( msg )
    {
    case WM_INITDLG:
        gProvEditing = pmProvActive();
        gProvRow = -1;
        gProvModelRow = -1;
        provList_( hwnd );
        provFields_( hwnd );
        // The fields' PM default limit is 32 -- the ceiling this
        // project already fought once; each box opens to hold what the
        // store can carry.
        WinSendDlgItemMsg( hwnd, kCtlProvName, EM_SETTEXTLIMIT,
                           MPFROM2SHORT( 63, 0 ), MPVOID );
        WinSendDlgItemMsg( hwnd, kCtlProvUrl, EM_SETTEXTLIMIT,
                           MPFROM2SHORT( 127, 0 ), MPVOID );
        WinSendDlgItemMsg( hwnd, kCtlProvKey, EM_SETTEXTLIMIT,
                           MPFROM2SHORT( 63, 0 ), MPVOID );
        WinSendDlgItemMsg( hwnd, kCtlProvModel, EM_SETTEXTLIMIT,
                           MPFROM2SHORT( 127, 0 ), MPVOID );
        WinSendDlgItemMsg( hwnd, kCtlProvTemp, EM_SETTEXTLIMIT,
                           MPFROM2SHORT( 15, 0 ), MPVOID );
        gDlgShown = 1;
        return 0;

    /* A WM_COMMAND with a notification in its high half is a control
       speaking (the listboxes); one with zero is a menu-style delivery,
       and buttons land in the cases below by id. */
    case WM_CONTROL:
        if( SHORT1FROMMP( mp1 ) == kCtlProvList
            || SHORT1FROMMP( mp1 ) == kCtlProvModelList )
            provNoticed_( hwnd );
        return 0;

    case WM_COMMAND:
        switch( SHORT1FROMMP( mp1 ) )
        {
        case DID_OK:
            provCommit_( hwnd );
            pmProvSave();
            WinDismissDlg( hwnd, TRUE );
            return 0;
        case DID_CANCEL:
            WinDismissDlg( hwnd, FALSE );
            return 0;

        case kCtlProvNew:
            provCommit_( hwnd );          // tvision's order: commit,
            {                             // then add, then edit the new
            const int i = pmProvAdd();
            if( i >= 0 )
                {
                gProvEditing = i;
                provList_( hwnd );
                provFields_( hwnd );
                provStatus_( hwnd, "Created: set its URL and key." );
                }
            return 0;
            }
        case kCtlProvDel:
            if( !pmProvDelete( gProvEditing ) )
                {
                provStatus_( hwnd, "Cannot delete the only provider." );
                return 0;
                }
            gProvEditing = pmProvActive();
            provList_( hwnd );
            provFields_( hwnd );
            provStatus_( hwnd, "Provider deleted." );
            return 0;
        case kCtlProvActive:
            provCommit_( hwnd );
            pmProvSetActive( gProvEditing );
            provList_( hwnd );
            {
            char t[ 160 ];
            std::snprintf( t, sizeof( t ), "Active provider: %s",
                           pmProvName( gProvEditing ) );
            provStatus_( hwnd, t );
            }
            return 0;
        case kCtlProvRefresh:
            {
            // The url and key as typed -- uncommitted edits included,
            // exactly the tvision refresh's reading.  It saves nothing.
            char url[ 140 ], key[ 80 ], model[ 140 ], err[ 200 ];
            dlgText_( hwnd, kCtlProvUrl, url, sizeof( url ) );
            dlgText_( hwnd, kCtlProvKey, key, sizeof( key ) );
            dlgText_( hwnd, kCtlProvModel, model, sizeof( model ) );
            const int n = pmProvRefresh( gProvEditing, url, key, model,
                                         err, sizeof( err ) );
            if( n < 0 )
                {
                provStatus_( hwnd, err );
                return 0;
                }
            PmProvider p;
            pmProvGet( gProvEditing, &p );
            WinSetDlgItemText( hwnd, kCtlProvModel, (PSZ) p.model );
            provModelList_( hwnd, p.model );
            char t[ 200 ];
            std::snprintf( t, sizeof( t ), "%d model(s) loaded.", n );
            provStatus_( hwnd, t );
            return 0;
            }
        default:
            if( SHORT2FROMMP( mp1 ) != 0 )
                {
                // A control notification that arrived as WM_COMMAND
                // rather than WM_CONTROL -- both routes are documented
                // company, and the handler is idempotent.
                provNoticed_( hwnd );
                return 0;
                }
            break;
        }
        break;

    case WM_CLOSE:
        WinDismissDlg( hwnd, FALSE );
        return 0;
    }
    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}

/* --------------------------------------------------------------- the kit -- */

static int pmSettingsDialog( void )
{
    pmNoteDialog( +1 );
    gDlgShown = 0;
    WinDlgBox( HWND_DESKTOP, pmFrameWindow(), pmSettingsDlgProc,
               0, kDlgSettings, NULL );
    const int shown = gDlgShown;
    pmNoteDialog( -1 );
    return shown;
}

static int pmUndoDialog( void )
{
    pmNoteDialog( +1 );
    gDlgShown = 0;
    WinDlgBox( HWND_DESKTOP, pmFrameWindow(), pmUndoDlgProc,
               0, kDlgUndo, NULL );
    const int shown = gDlgShown;
    pmNoteDialog( -1 );
    return shown;
}

static int pmProvidersDialog( void )
{
    if( pmProvCount() == 0 )
        return 0;               // nothing to show: let tvision answer
    pmNoteDialog( +1 );
    gDlgShown = 0;
    WinDlgBox( HWND_DESKTOP, pmFrameWindow(), pmProvidersDlgProc,
               0, kDlgProviders, NULL );
    const int shown = gDlgShown;
    pmNoteDialog( -1 );
    return shown;
}

/* Every dialog the front has a native twin for is in the table now; a
   console build never installs the kit at all and draws its tvision
   dialogs, which stay the reference implementations.

   The last two fields are the chrome, defined with the windows they drive
   in hardwspm.cpp -- status hands the native status button its text,
   keysToPage is the keyboard's traffic light for tvision modals. */
extern "C" void pmStatusKit( const char *text );
extern "C" void pmKeysToPageKit( int on );

static const PmDialogs kPmDialogs =
    {
    pmAboutDialog,
    pmThemeDialog,
    pmSwitchModelDialog,
    pmSettingsDialog,
    pmProvidersDialog,
    pmUndoDialog,
    pmStatusKit,
    pmKeysToPageKit
    };

void pmDialogsInstall( void )
{
    gPmDialogs = &kPmDialogs;
}

#endif  // ATUI_PM
