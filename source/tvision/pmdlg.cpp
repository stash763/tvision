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

    kCtlThemeClassic = 401,
    kCtlThemeDark    = 402,
    kCtlModelList    = 403
};

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
    // WinDismissDlg's TRUE is what WinDlgBox hands back; DID_OK is 1.
    // pmNoteDialog (hardwspm.cpp) counts the modal loops: the driver
    // defers any resize that arrives while one is running.
    pmNoteDialog( +1 );
    const int shown = WinDlgBox( HWND_DESKTOP, pmFrameWindow(), pmThemeDlgProc,
                                 0, kDlgTheme, NULL ) == DID_OK;
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
    const int shown = WinDlgBox( HWND_DESKTOP, pmFrameWindow(), pmSwitchModelDlgProc,
                                 0, kDlgSwitchModel, NULL ) == DID_OK;
    pmNoteDialog( -1 );
    return shown;
}

/* --------------------------------------------------------------- the kit -- */

/* settings, providers, undo: the fields exist so the kit's shape is final;
   null means the tvision dialog still wears that one.  They arrive as their
   own measured steps, the way everything else here did. */
static const PmDialogs kPmDialogs =
    {
    pmAboutDialog,
    pmThemeDialog,
    pmSwitchModelDialog,
    0,
    0,
    0
    };

void pmDialogsInstall( void )
{
    gPmDialogs = &kPmDialogs;
}

#endif  // ATUI_PM
