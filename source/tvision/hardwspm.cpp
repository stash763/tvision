/*------------------------------------------------------------*/
/* filename -       hardwspm.cpp                              */
/*                                                            */
/* THardwareInfo for OS/2 Presentation Manager: a window      */
/* whose grid of cells is the screen, keys from WM_CHAR and   */
/* mouse from the button and motion messages.                 */
/*------------------------------------------------------------*/
/*
 * The same 28 members hardwvos2.cpp answers for the VIO session, answered
 * for a PM window instead: the application above this file does not know
 * there is a difference, which is the whole value of the THardwareInfo
 * seam.  It is selected by ATUI_PM (only this file's guard tests it), the
 * object is spliced into a second tvision library, and a PM image links
 * that one -- two drivers in one link would be a duel of duplicated
 * symbols the linker resolves in whichever order it likes.
 *
 * What it borrows from where, because every one of these was the answer to
 * a question this toolchain asked the hard way (TESTING.md, "PM probe:
 * green"):
 *
 *   - the birth: WinInitialize / WinCreateMsgQueue / WinRegisterClass /
 *     WinCreateStdWindow(FCF_STANDARD & ~FCF_SHELLPOSITION) with
 *     idResources = 256, then WinSetWindowPos to appear -- the stdwnd
 *     sample's sequence.  A frameless window never receives focus, so
 *     keys would never arrive; a Watcom image has no resource segment
 *     until rc appends one, so the frame's resources would not be found.
 *   - the keyboard: WM_CHAR carries char/SC_/flags/scan as
 *     SHORT1FROMMP(mp2)/SHORT2FROMMP(mp2)/SHORT1FROMMP(mp1)/CHAR4FROMMP(mp1)
 *     -- eFTE's ConvertKey decodes exactly that (g_pm.cpp:2616).  The key
 *     numbers are transcribed from Watcom's header rather than included,
 *     for reasons the input-section comment tells.  The BIOS scan codes
 *     they map to are the table hardwvos2.cpp carries, because
 *     the views above compare the (scan, char) pairs of tkeys.h.
 *   - the mouse: PM's (position, buttons held) is the same contract OS/2's
 *     VIO mouse queue fills, and TEventQueue turns it into press, release
 *     and drag, so the driver reports state, never edges.
 *   - the grid itself -- cells, glyphs, colours, clearing --: IBM's AVIO
 *     page, not a hand-drawn one.  GpiCreatePS(PU_PELS | GPIF_DEFAULT
 *     | GPIT_MICRO | GPIA_ASSOC) on the window's DC, VioCreatePS for a
 *     page of rows and columns, VioAssociate to hang the page on the
 *     window: eFTE's construction verbatim (g_pm.cpp:2015..2030).  A page
 *     is a BIOS text screen -- attribute-first cell strings, CP850 glyphs,
 *     the attribute byte's own palette, scroll-up clearing -- so the VioWrt
 *     family and the VioScroll family that hardwvos2.cpp already speaks
 *     write here too, the page handle taking the slot the session console's
 *     handle takes.  The alternative, this driver drawing every cell itself with
 *     WinFillRect and GpiCharStringAt, was builds two through seven, and
 *     the box found each answer wrong: the post-mortem is the screenshot
 *     log (transparent holes where unpainted rows let the desktop show
 *     through, colours asked of GpiSetColor in a colour long it reads as
 *     a palette index).  eFTE has shipped this page engine for years on
 *     kernels exactly this one; that beats one more guess.
 *   - the wait: WinGetMsg as the engine and nothing else -- eFTE's
 *     ConGetEvent (g_pm.cpp:2385) and every TOOLKIT sample fetch their
 *     whole life through it.  This driver first polled with DosSleep and
 *     WinPeekMsg, invented because the VIO driver it mirrors has no
 *     message loop; the box answered that design with errors and a deaf
 *     window (atuipm.run.txt builds two and three).  Timeouts ride on
 *     posted messages -- Watcom 1.9's PM headers carry no WinSetTimer at
 *     all (measured), and eFTE wakes its blocked fetches with
 *     WinPostMsg, which is the mechanism copied here.
 *
 * The shadow/dirty machinery is the VIO driver's, copied with the reasons
 * in its comments: a repaint must cost what changed, and the screen must
 * see one state per frame, never the fill-then-text pair the classic
 * writer produces.  Here the "screen" is the page: a flush pushes each
 * dirty run as one cell string (VioWrtCellStr), the page repaints the
 * window holding it, and the dirty bits clear at the flush -- the moment
 * the truth leaves the shadow.  WM_PAINT no longer draws the screen at
 * all; it re-shows the page whole (VioShowBuf), which is what occlusion
 * and un-minimize need and the page answers from memory.
 *
 * Known v1 limits, each one a decision and not an oversight:
 *
 *   - 80x25 fixed.  The window resizes; the grid does not follow it yet.
 *     VIO could not resize either and the application copes; making the
 *     grid follow needs setScreenMode to drive a tvision resize, and that
 *     is its own step.
 *   - closing the window asks the application to quit the way Alt-X does
 *     -- an injected key, the only exit key this application answers --
 *     and after five seconds of not having exited, it stops asking.
 *   - no clipboard (the PM one is reachable from here, unlike the VIO
 *     driver's excuse, and wiring it is still undone), no wheel.
 *   - the caret is PM's own -- WinCreateCursor, created on focus and
 *     recreated to move, the eFTE pattern (g_pm.cpp:2105..2130) -- and
 *     PM's caret speaks one shape, a flashing solid block; tvision's
 *     sizes all become that.
 */

#if defined( __FLAT__ ) && defined( __OS2__ ) && !defined( __BORLANDC__ ) \
    && defined( ATUI_PM )

/* Before tv.h, which drags <os2.h> in through compat's windows header: the
   first include is the one that decides what the rest of the headers
   contain (measured in hardwvos2.cpp), and PM needs the window groups.
   INCL_WININPUT is deliberately not defined: its whole section -- the KC_*
   flags and the mouse messages included -- also defines a set of VK_*
   names, and tvision's compat windows.h then redefines those to the
   Windows values in the same compilation (measured: forty W086 warnings).
   Whichever header won the name, the other's numbers would be the wrong
   ones to compare keys against, so the input vocabulary is transcribed
   below instead and the name conflict never opens. */
#define INCL_DOSPROCESS
#define INCL_DOSERRORS
#define INCL_GPI
#define INCL_WIN
#define INCL_VIO        // bsesub.h:365 keeps the whole VIO section -- the
                        // HVIO handle, the VioWrt/VioScroll write side and
                        // their VIO16 import aliases -- behind this one
                        // word; eFTE's g_pm.cpp:25 asks with it too
#define INCL_AVIO       // pm.h:35 reads this one and adds pmavio.h: the
                        // page engine's VioCreatePS, VioAssociate and
                        // friends, the page half of the same VIO16 family

#define Uses_TKeys
#define Uses_TEvent
#define Uses_TScreen        // TDisplay, whose video mode constants getScreenMode answers with
#define Uses_THardwareInfo
#include <tvision/tv.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>        // _beginthread, for the wake thread

#include "tui/pmdialogs.h"  // the kit: pmDialogsInstall(), gPmDialogs

/* W869 ("_beginthread requires the build target to be multi-threaded")
   speaks of a runtime this program is not: core/thread_os2.cpp measured
   both runtimes on the box and found the Watcom OS/2 32-bit one upgrades
   itself in place on the first _beginthread -- which is why atui.exe
   ships threads without -bm, and this alarm thread is the same call from
   the same kind of thread. */
#pragma disable_message( 869 )
#include <sys/timeb.h>

enum
    {
    kDefCols    = 80,
    kDefRows    = 25,
    kDefaultAttr = 0x07,    // light grey on black, the VGA default cell

    kCellW      = 9,        // the fallback grid: VGA's classic cell,
    kCellH      = 16,       // right when the font asks for nothing else
    kCellWReq   = 8,        // the cell to ask the engine for: the classic
    kCellHReq   = 16,       // VGA text cell, 80x25 of it fitting any
                            // screen this tool has ever met
    kCellWMin   = 4,        // measurements outside these are not believed
    kCellWMax   = 32,
    kCellHMin   = 6,
    kCellHMax   = 32,

    /* WinPeekMsg's only flag, eFTE's spelling (PM_NOREMOVE): the driver
       asks "anything in the queue" and lets WinGetMsg do the taking.  The
       PMQ_NOSENDUSERPAINT bit an earlier design added is nowhere in the
       references, and every WinPeekMsg carrying it failed 0x81019 on the
       box (measured, atuipm.run.txt build three) -- correlation the fix
       takes, causation the box may yet refine. */
    kPmqNoRemove        = 0x0000,

    /* The alarm eFTE owns a blocked WinGetMsg with: a posted message.
       WM_USER + 1 is eFTE's own numbering for exactly this purpose
       (g_pm.cpp:59, UWM_NOTIFY). */
    kWmWake             = WM_USER + 1,
    kIdleWakeMs         = 250,    // the pace of a waiting-with-nothing-to-do
    kWakeMsMin          = 10,     // never post faster than this
    kWakeStack          = 64UL * 1024UL,

    /* The window class and its resources, under the one id the frame is
       built with (the stdwnd sample's ID_WINDOW = 256, same numbering so
       atuipm.rc and pmhello.rc match). */
    kIdWindow   = 256,

    kMouseQueue = 16,       // mouse events held between pump and drain

    /* WM_KILLFOCUS is absent from both header trees (measured), WM_SETFOCUS
       is there at pmwin.h:718 with IBM's value; IBM's own numbering puts
       the death notice next door at 0x0010.  Only ever used for the log
       line that says which of the two the window saw. */
    kWmKillFocus = 0x0010,

    kQuitForceMs = 5000,    // how long the closed window waits for the
                            // application to obey the injected Alt-X
    };

/* The WM_CHAR and mouse vocabulary, transcribed from the box's pmwin.h
   (Watcom 1.9, 1579..1622) rather than included -- the reasons are in the
   comment above the INCL_* defines, and the values are IBM's SC_* numbers
   under Watcom's VK_* spellings.  What WM_CHAR delivers in mp2's high
   short; KC_* are the mp1 flag bits. */
enum
    {
    kKcChar       = 0x0001,
    kKcShift      = 0x0008,
    kKcCtrl       = 0x0010,
    kKcAlt        = 0x0020,
    kKcKeyUp      = 0x0040,

    kVirtBackspace = 0x05,
    kVirtTab       = 0x06,
    kVirtBacktab   = 0x07,
    kVirtNewline   = 0x08,      // the main Enter; kVirtEnter is numeric
    kVirtEsc       = 0x0f,
    kVirtPageup    = 0x11,
    kVirtPagedown  = 0x12,
    kVirtEnd       = 0x13,
    kVirtHome      = 0x14,
    kVirtLeft      = 0x15,
    kVirtUp        = 0x16,
    kVirtRight     = 0x17,
    kVirtDown      = 0x18,
    kVirtInsert    = 0x1a,
    kVirtDelete    = 0x1b,
    kVirtEnter     = 0x1e,
    kVirtF1        = 0x20,
    kVirtF2        = 0x21,
    kVirtF3        = 0x22,
    kVirtF4        = 0x23,
    kVirtF5        = 0x24,
    kVirtF6        = 0x25,
    kVirtF7        = 0x26,
    kVirtF8        = 0x27,
    kVirtF9        = 0x28,
    kVirtF10       = 0x29,
    kVirtF11       = 0x2a,
    kVirtF12       = 0x2b,
    };

/* The mouse messages (pmwin.h 1614..1622, WM_MOUSEFIRST through
   WM_BUTTON3UP): same reason for transcribing as the keys above. */
enum
    {
    kWmMouseMove    = 0x0070,
    kWmButton1Down  = 0x0071,
    kWmButton1Up    = 0x0072,
    kWmButton2Down  = 0x0074,
    kWmButton2Up    = 0x0075,
    kWmButton3Down  = 0x0077,
    kWmButton3Up    = 0x0078,
    };

//
// PM state
//

static HAB    gHab      = 0;
static HMQ    gHmq      = 0;
static HWND   gFrame    = 0;
static HWND   gClient   = 0;
static HPS    gHpsScreen = 0;   // the PS built on the window's DC and
                                // associated with the page (GPIA_ASSOC);
                                // WinBeginPaint is handed it, eFTE's way
static HVPS   gHvps      = 0;   // the AVIO page: a BIOS text screen that
                                // remembers its cells and shows them in
                                // the window it is associated to
static Boolean gAvioOk   = False;
static Boolean gPmUp    = False; // window + queue exist; drawing is legal
static Boolean gClosed  = False; // the user closed the window
static Boolean gQuitKeySent = False;
static uint64_t gClosedMs = 0;
static ulong  gClientW  = 0;    // client area in pixels, kept from WM_SIZE
static ulong  gClientH  = 0;

static ushort gCols = kDefCols;
static ushort gRows = kDefRows;
static ushort gCellPxW = kCellW;
static ushort gCellPxH = kCellH;

/* The caret is PM's own (WinCreateCursor), which exists only while the
   window has focus -- so the driver remembers what the application asked
   for and re-creates it when focus returns.  gFocused is that memory. */
static Boolean gFocused = False;
static ushort gCaretX = 0;
static ushort gCaretY = 0;
static Boolean gCaretShown = False;
static int gCaretWasShown = -1; // "not yet told", so the first call sticks

/* The shadow of the window: what it is believed to hold, one character and
   one attribute per cell, plus the dirty map of cells drawn since the last
   time the window was told.  The comments in hardwvos2.cpp carry the
   measurements behind this design; the short of it: the classic writer
   repaints whether or not anything changed, and only the difference
   between the last frame and this one is worth drawing. */
static uchar *gShadowCh = 0;
static uchar *gShadowAttr = 0;
static uchar *gDirty = 0;
static size_t gDirtyCount = 0;
static ushort gShadowCols = 0;
static ushort gShadowRows = 0;

static size_t shadowCells()
{
    return (size_t) gShadowCols * gShadowRows;
}

static Boolean gKeyPending = False;
static TEvent gPendingKey;

/* Mouse events between the pump that receives them and the polls that read
   them.  One slot is what the VIO driver needs because its queue holds
   state until asked; PM messages arrive during the pump inside
   waitForEvents and are converted on the spot, and a drag can fill more
   than one slot before the application comes back around. */
static MouseEventType gMouseQueue[kMouseQueue];
static int gMouseHead = 0;
static int gMouseCount = 0;
static uchar gMouseButtons = 0;         // what is held right now

static Boolean gInterruptWait = False;

/* The alarm clock: a thread that sleeps and posts kWmWake, because Watcom
   1.9 carries no PM timers (measured: no WinSetTimer anywhere in its
   headers) and eFTE -- built with this very compiler -- wakes its blocked
   WinGetMsg with posted messages (g_pm.cpp:59 and its WinPostMsg calls).
   The pace follows the application's last stated timeout: a streaming
   wait of 20 ms is answered in about 20 ms, an idle wait of nothing-in-
   particular posts four times a second, which is nothing to measure. */
static unsigned long gWakeMs = kIdleWakeMs;
static Boolean gWakeUp = False;
static unsigned gWakeTid = 0;

/* Counting what the screen costs for the application's once-a-second log:
   cell strings pushed to the page (one per dirty run), whole re-shows the
   window asked for, and what the last one took. */
static unsigned long gPmRuns = 0;     // cell strings pushed to the page
static unsigned long gPmPaints = 0;   // WM_PAINTs that re-showed it
static unsigned long gPmPaintMs = 0;

/* atuipm.run.txt: the same lifeline the probe carried, because a PM image
   started at the box's own display has no console to complain to, and the
   question "did the window fail to appear or did it appear and paint
   nothing" has very different answers.  Written next to the other probes'
   logs and fetched over FTP.  The first key and the first paint log once
   each: those are the events whose absence means the window exists but is
   deaf or blind, which is otherwise invisible. */
static FILE *gLog = 0;
static Boolean gKeyLogged = False;
static Boolean gFocusInLogged = False;    // which window answers focus, and
static Boolean gFocusOutLogged = False;   // whether the pointer is heard
static Boolean gMouseLogged = False;      // at all, each logged once

static void pmLog( const char *what )
{
    if( gLog != 0 )
        fprintf( gLog, "%s\n", what ), fflush( gLog );
}

/* Log the PM error code with its human text.  WinGetErrorInfo returns one
   block whose strings live at offsets inside it -- the unpack the toolkit
   documents and the probe proved (TESTING.md, "PM probe: green"). */
static void pmLogErr( const char *what )
{
    if( gLog == 0 )
        return;
    fprintf( gLog, "%s: error 0x%lx\n", what, WinGetLastError( gHab ) );
    PERRINFO ei = WinGetErrorInfo( gHab );
    if( ei != 0 )
        {
        PSZ pszOffSet = (PSZ) ei + ei->offaoffszMsg;
        PSZ pszMsg    = (PSZ) ei + *(PUSHORT) pszOffSet;
        fprintf( gLog, "  info: %s\n", pszMsg );
        WinFreeErrorInfo( ei );
        }
    fflush( gLog );
}

/* No palette table here: the attribute byte is drawn by the page engine
 * with its own colours, which are the BIOS ones -- the same reading the
 * VIO session gives the same bytes.  The table this file used to carry
 * (kRgb, sixteen longs) fed colour longs to GpiSetColor, whose argument
 * is a palette *index*: every colour request was a garbage index, which
 * is post-mortem half of "the colour scheme is not working at all"
 * (screenshot3).  The engine knows better than this file ever will.
 */

//
// The shadow
//

static void forgetPending()
{
    if( gDirty != 0 )
        memset( gDirty, 0, shadowCells() );
    gDirtyCount = 0;
}

static void forgetScreen()
{
    if( gShadowCh == 0 )
        return;
    memset( gShadowCh, 0, shadowCells() );
    memset( gShadowAttr, 0xFF, shadowCells() );
    forgetPending();
}

static Boolean ensureShadow()
{
    if( gShadowCh != 0 && gShadowCols == gCols && gShadowRows == gRows )
        return True;

    delete[] gShadowCh;
    delete[] gShadowAttr;
    delete[] gDirty;
    gShadowCh = new uchar[(size_t) gCols * gRows];
    gShadowAttr = new uchar[(size_t) gCols * gRows];
    gDirty = new uchar[(size_t) gCols * gRows];
    gShadowCols = gCols;
    gShadowRows = gRows;
    if( gShadowCh == 0 || gShadowAttr == 0 || gDirty == 0 )
        return False;
    forgetScreen();
    return True;
}

static Boolean cellIsCurrent( ushort x, ushort y, const CHAR_INFO &cell )
{
    const size_t at = (size_t) y * gShadowCols + x;
    return gShadowCh[at] == (uchar) cell.Char.AsciiChar
        && gShadowAttr[at] == (uchar) ( cell.Attributes & 0xFF );
}

static void rememberCell( ushort x, ushort y, const CHAR_INFO &cell )
{
    const size_t at = (size_t) y * gShadowCols + x;
    if( !gDirty[ at ] )
        {
        gDirty[ at ] = 1;
        ++gDirtyCount;
        }
    gShadowCh[at] = (uchar) cell.Char.AsciiChar;
    gShadowAttr[at] = (uchar) ( cell.Attributes & 0xFF );
}

/* Ask the window for a whole repaint of the page: for the moments the
   window may hold something the page no longer shows (after a suspend,
   after a clear) and for redrawScreen.  Ordinary frames never come here:
   a cell string written to the page repaints what it covers by itself. */
static void pmInvalidateWhole()
{
    if( !gPmUp )
        return;
    WinInvalidateRect( gClient, 0, False );
}

//
// Input helpers, shared in spirit with hardwvos2.cpp
//

/* BIOS scan codes for the characters WM_CHAR hands over with KC_CHAR, from
   hardwvos2.cpp -- the same table, because the same tkeys.h constants are
   what the views compare, and a shifted character keeps the scan code of
   the key under the shift.  PM usually carries the real scan code in
   CHAR4FROMMP(mp1); this table is what happens when it says zero. */
static const uchar kScanByChar[128] =
    {
    0x00, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22,   // 00 ^@ ctrl-A B C D E F G
    0x0E, 0x0F, 0x1C, 0x00, 0x00, 0x1C, 0x00, 0x00,   // 08 bs tab lf  .. enter ..
    0x00, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11,   // 10 .. ^Q ^R ^S ^T ^U ^V ^W
    0x2D, 0x15, 0x2C, 0x01, 0x00, 0x00, 0x00, 0x00,   // 18 ^X ^Y ^Z esc ..
    0x39, 0x02, 0x28, 0x04, 0x05, 0x06, 0x08, 0x28,   // 20 sp ! " # $ % & '
    0x0A, 0x0B, 0x0C, 0x0D, 0x33, 0x0C, 0x34, 0x35,   // 28 ( ) * + , - . /
    0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,   // 30 0 1 2 3 4 5 6 7
    0x09, 0x0A, 0x27, 0x27, 0x33, 0x0D, 0x34, 0x35,   // 38 8 9 : ; < = > ?
    0x03, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22,   // 40 @ A B C D E F G
    0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,   // 48 H I J K L M N O
    0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11,   // 50 P Q R S T U V W
    0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x07, 0x0C,   // 58 X Y Z [ \ ] ^ _
    0x29, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22,   // 60 ` a b c d e f g
    0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,   //    h i j k l m n o
    0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11,   // 70 p q r s t u v w
    0x2D, 0x15, 0x2C, 0x1A, 0x2B, 0x1B, 0x29, 0x0E    // 78 x y z { | } ~ del
    };

static uchar scanForChar( int c )
{
    if( c < 0 || c > 127 )
        return 0;
    return kScanByChar[c];
}

static void fillKey( TEvent &event, uchar ch, uchar scan, ushort modifiers )
{
    event.what = evKeyDown;
    event.keyDown.charScan.charCode = ch;
    event.keyDown.charScan.scanCode = scan;
    event.keyDown.controlKeyState = modifiers;
    // Typed text goes through getText(), which reads text and textLength;
    // a printable character not entered with a modifier belongs in both.
    if( ch >= 0x20 && ch != 0x7F && !(modifiers & (kbAltShift | kbCtrlShift)) )
        {
        event.keyDown.text[0] = (char) ch;
        event.keyDown.textLength = 1;
        }
    else
        event.keyDown.textLength = 0;
}

static void fillMouse( MouseEventType &mouse, int x, int y, uchar buttons,
                       ushort eventFlags, uchar wheel, ushort modifiers )
{
    mouse.where.x = x;
    mouse.where.y = y;
    mouse.buttons = buttons;
    mouse.eventFlags = eventFlags;
    mouse.wheel = wheel;
    mouse.controlKeyState = modifiers;
}

static void putMouse( const MouseEventType &mouse )
{
    /* Motion replaces motion: a queue of places the pointer passed is a
       queue of drags the screen never saw, and the newest position is the
       truth the application should get.  Only motion with motion and with
       the buttons held the same way -- a press or release is a fact, not a
       position update, and every fact deserves its own slot. */
    if( gMouseCount > 0 && ( mouse.eventFlags & MOUSE_MOVED ) != 0 )
        {
        const int last = (gMouseHead + gMouseCount - 1) % kMouseQueue;
        if( ( gMouseQueue[ last ].eventFlags & MOUSE_MOVED ) != 0
            && gMouseQueue[ last ].buttons == mouse.buttons )
            {
            gMouseQueue[ last ] = mouse;
            return;
            }
        }

    const int tail = (gMouseHead + gMouseCount) % kMouseQueue;
    if( gMouseCount < kMouseQueue )
        ++gMouseCount;
    else
        gMouseHead = (gMouseHead + 1) % kMouseQueue;   // drop the oldest
    gMouseQueue[ tail ] = mouse;
}

/*
 * The keyboard.
 *
 * WM_CHAR says three things at once: what character the key made
 * (SHORT1FROMMP(mp2)), which key it was (SHORT2FROMMP(mp2), the VK_* of
 * Watcom's header -- IBM's SC_*), and what was held and where the key sat
 * (SHORT1FROMMP(mp1) flags, CHAR4FROMMP(mp1) scan).  eFTE decodes exactly
 * this and so does every PM program worth the name.
 *
 * The answers are written as BIOS pairs because that is what the classic
 * library speaks: arrows are (0, 0x48..0x50), the menu's Alt hotkeys are
 * Alt held with the key's own pair.  A driver that wrote neither leaves a
 * UI that shows keys but moves for none of them.
 */
static Boolean pmVirtualKey( ushort virt, uchar &ch, uchar &scan,
                             ushort &modifiers )
{
    switch( virt )
        {
    case kVirtLeft:       scan = 0x4B; return True;
    case kVirtUp:         scan = 0x48; return True;
    case kVirtRight:      scan = 0x4D; return True;
    case kVirtDown:       scan = 0x50; return True;
    case kVirtHome:       scan = 0x47; return True;
    case kVirtEnd:        scan = 0x4F; return True;
    case kVirtPageup:     scan = 0x49; return True;
    case kVirtPagedown:   scan = 0x51; return True;
    case kVirtInsert:     scan = 0x52; return True;
    case kVirtDelete:     ch = 0x00; scan = 0x53; return True;
    case kVirtEsc:        ch = 0x1B; scan = 0x01; return True;
    case kVirtNewline:    ch = 0x0D; scan = 0x1C; return True;   // main Enter
    case kVirtEnter:      ch = 0x0D; scan = 0x1C; return True;   // numeric
    case kVirtBackspace:  ch = 0x08; scan = 0x0E; return True;
    case kVirtTab:        ch = 0x09; scan = 0x0F; return True;
    case kVirtBacktab:    ch = 0x00; scan = 0x0F;
                           modifiers |= kbShift;
                           return True;
    case kVirtF1:         scan = 0x3B; return True;
    case kVirtF2:         scan = 0x3C; return True;
    case kVirtF3:         scan = 0x3D; return True;
    case kVirtF4:         scan = 0x3E; return True;
    case kVirtF5:         scan = 0x3F; return True;
    case kVirtF6:         scan = 0x40; return True;
    case kVirtF7:         scan = 0x41; return True;
    case kVirtF8:         scan = 0x42; return True;
    case kVirtF9:         scan = 0x43; return True;
    case kVirtF10:        scan = 0x44; return True;
    case kVirtF11:        scan = 0x85; return True;
    case kVirtF12:        scan = 0x86; return True;
    default:
        return False;
    }
}

static void pmTakeChar( ULONG flags, short ch, ushort virt, uchar scan )
{
    if( gKeyPending )
        return;     // the application has one key unclaimed; it will ask again

    ushort modifiers = 0;
    if( flags & kKcShift )
        modifiers |= kbShift;
    if( flags & kKcCtrl )
        modifiers |= kbCtrlShift;
    if( flags & kKcAlt )
        modifiers |= kbAltShift;

    uchar chB = (uchar) ch;
    uchar scanB = scan;

    // The named keys carry no character of their own; the table says which
    // pair tkeys.h names them by.  A key that is both named and printable
    // (Enter arrives so) still prefers the table: the tables scan is the
    // one kbEnter compares equal to.
    uchar chTry = chB;
    uchar scanTry = scanB;
    if( pmVirtualKey( virt, chTry, scanTry, modifiers ) )
        {
        chB = chTry;
        scanB = scanTry;
        }
    else if( !(flags & kKcChar) )
        return;     // a bare modifier press, a dead key, nothing to report
    else if( scanB == 0 )
        scanB = scanForChar( chB );

    TEvent event;
    fillKey( event, chB, scanB, modifiers );
    if( event.keyDown.keyCode == kbNoKey )
        return;     // a pair no name resolves -- drop it, do not deliver it
    if( !gKeyLogged )
        {
        gKeyLogged = True;    // the probe never confirmed keyboard delivery;
        if( gLog != 0 )       // this line is that confirmation, once
            fprintf( gLog, "key char=0x%02X virt=0x%04X scan=0x%02X mods=0x%04X\n",
                     (unsigned) chB, (unsigned) virt, (unsigned) scanB,
                     (unsigned) modifiers ), fflush( gLog );
        }
    gPendingKey = event;
    gKeyPending = True;
}

/* A PM menu command becomes the very key the application already answers.
   The menu item and the hot key are two doors to the same room: Help,
   Undo, Providers, Settings, Theme, Exit and About each carry an Alt
   combination that app.cpp's own menu bar answers (its initMenuBar), and
   a menu click injects a byte-identical key event -- so identical that
   the application cannot tell a mouse from a keyboard, which is exactly
   the point of the THardwareInfo seam.  The scans are BIOS's for h, u,
   p, i, t, x, a, and Alt-X of close-the-window is the same arithmetic.
   The ids are atuipm.rc's; the rc is the top of tools/os2, and this
   table lives two screens below its last line, which is how keeping
   them in step stays possible.  Switch model is absent on purpose: its
   submenu's contents are app-side knowledge, and it arrives with the
   PM-side menu building that the native-dialog phase brings. */
/* pmdlg.cpp hangs its dialogs off the frame; the frame is this file's, and
   that one line is the whole of what it needs to know about it. */
HWND pmFrameWindow( void )
{
    return gFrame;
}

static void pmTakeCommand( int id )
{
    struct CommandKey
        {
        int id;
        uchar scan;
        const char *name;
        };
    static const CommandKey kCommands[] =
        {
        { 1101, 0x23, "Alt-H help"      },
        { 1102, 0x16, "Alt-U undo"      },
        { 1103, 0x19, "Alt-P providers" },
        { 1104, 0x17, "Alt-I settings"  },
        { 1105, 0x14, "Alt-T theme"     },
        { 1106, 0x1F, "Alt-S switch model" },
        { 1110, 0x2D, "Alt-X quit"      },
        { 1201, 0x1E, "Alt-A about"     },
        };
    for( unsigned i = 0; i < sizeof( kCommands ) / sizeof( kCommands[ 0 ] ); ++i )
        if( kCommands[ i ].id == id )
            {
            if( gLog != 0 )
                {
                fprintf( gLog, "command %d -> %s\n", id, kCommands[ i ].name );
                fflush( gLog );
                }
            fillKey( gPendingKey, 0, kCommands[ i ].scan, kbAltShift );
            gKeyPending = True;   // the click wins the slot: it is the newer
            return;               // intention, and whatever it replaces was
            }                     // one keystroke nobody double-clicked
    if( gLog != 0 )
        fprintf( gLog, "command %d unmapped\n", id ), fflush( gLog );
}

//
// Window procedure
//

/* PM's caret: a flashing solid block, created when the focus arrives and
   re-created to move -- eFTE's idiom (g_pm.cpp:2105..2130).  Its rectangle
   is the cell's, in PM's bottom-up client coordinates.  There is no
   redraw bookkeeping here because the caret is not drawn: the window
   manager owns its maintenance, and that is one less thing this file can
   get wrong on the box. */
static void pmUpdateCaret()
{
    if( !gPmUp || !gFocused )
        return;
    if( !gCaretShown || gCaretX >= gCols || gCaretY >= gRows )
        {
        WinDestroyCursor( gClient );
        return;
        }
    /* Destroyed before it is re-made, eFTE's PMSetCursorPos (g_pm.cpp:
       3210..3218): what must never survive the move is the old inversion.
       Without this pair of calls a typed character sat under a stale
       block until the next paint said otherwise. */
    WinDestroyCursor( gClient );
    WinCreateCursor( gClient,
                     (LONG) gCaretX * (LONG) gCellPxW,
                     (LONG) ( (long) gRows - (long) gCaretY - 1L )
                           * (LONG) gCellPxH,
                     (LONG) gCellPxW, (LONG) gCellPxH,
                     CURSOR_FLASH | CURSOR_SOLID, (PRECTL) 0 );
    WinShowCursor( gClient, TRUE );
}

static MRESULT EXPENTRY pmWndProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch( msg )
        {
    case WM_PAINT:
        {
            struct timeb t0;
            ftime( &t0 );
            RECTL rcl;
            HPS hps = WinBeginPaint( hwnd, gHpsScreen, &rcl );
            RECTL rect;
            WinQueryWindowRect( hwnd, &rect );
            gClientW = (ulong) ( rect.xRight - rect.xLeft );
            gClientH = (ulong) ( rect.yTop - rect.yBottom );
            if( gHvps != 0 )
                {
                // Not a draw: a re-show.  The page remembers every cell
                // it has ever been handed and paints them from memory,
                // which is why occlusion, resize and un-minimize need no
                // paint logic of this driver's own -- eFTE's WM_PAINT is
                // these same three calls (g_pm.cpp:2200).
                VioShowBuf( 0, (USHORT) ( 2u * (unsigned long) gCols
                                            * (unsigned long) gRows ),
                            (HVIO) gHvps );
                }
            WinEndPaint( hps );
            struct timeb t1;
            ftime( &t1 );
            gPmPaintMs = (unsigned long)
                ( ( (long) t1.time - (long) t0.time ) * 1000L
                  + (long) t1.millitm - (long) t0.millitm );
            if( gPmPaints == 0 && gHvps != 0 )
                pmLog( "first paint" );
            if( gHvps != 0 )
                ++gPmPaints;
        }
        return 0;

    case WM_ERASEBACKGROUND:
        // The page fills every cell it shows, background included; the
        // default erase pass would paint the client white first -- the
        // white this window has been known for (eFTE returns FALSE for
        // the same reason, g_pm.cpp:2048).
        return (MRESULT) FALSE;

    case WM_SIZE:
        // The page engine hears of the new client first -- eFTE forwards
        // WM_SIZE to WinDefAVioWindowProc (g_pm.cpp:2065) to keep the
        // associated viewport honest.  The grid is fixed, so all this
        // file keeps afterwards is the pixel size the mouse maps against.
        WinDefAVioWindowProc( hwnd, msg, (ULONG) mp1, (ULONG) mp2 );
        {
            RECTL rect;
            WinQueryWindowRect( hwnd, &rect );
            gClientW = (ulong) ( rect.xRight - rect.xLeft );
            gClientH = (ulong) ( rect.yTop - rect.yBottom );
        }
        return 0;

    case WM_SETFOCUS:
        gFocused = True;
        if( !gFocusInLogged )
            {
            gFocusInLogged = True;
            pmLog( "focus in" );   // keys can only arrive after this line;
                                   // their absence without it is explained
            }
        pmUpdateCaret();
        return WinDefWindowProc( hwnd, msg, mp1, mp2 );

    case kWmKillFocus:
        gFocused = False;
        WinDestroyCursor( gClient );  // PM's caret lives only while the
                                      // focus does
        if( !gFocusOutLogged )
            {
            gFocusOutLogged = True;
            pmLog( "focus out" );   // once: the fact is the news, every
            }                       // theft after it is gossip
        return WinDefWindowProc( hwnd, msg, mp1, mp2 );

    case WM_CHAR:
        if( SHORT1FROMMP( mp1 ) & kKcKeyUp )
            return 0;
        pmTakeChar( SHORT1FROMMP( mp1 ), (short) SHORT1FROMMP( mp2 ),
                    (ushort) SHORT2FROMMP( mp2 ), (uchar) CHAR4FROMMP( mp1 ) );
        return 0;

    case kWmMouseMove:
    case kWmButton1Down:
    case kWmButton1Up:
    case kWmButton2Down:
    case kWmButton2Up:
    case kWmButton3Down:
    case kWmButton3Up:
        {
            // The held-buttons state, which is all tvision's mouse contract
            // needs and the same thing the VIO mouse queue reports: press
            // and release and drag are made by the queue from it.
            if( msg == kWmButton1Down )       gMouseButtons |= mbLeftButton;
            else if( msg == kWmButton1Up )    gMouseButtons &= (uchar) ~mbLeftButton;
            else if( msg == kWmButton2Down )  gMouseButtons |= mbRightButton;
            else if( msg == kWmButton2Up )    gMouseButtons &= (uchar) ~mbRightButton;
            else if( msg == kWmButton3Down )  gMouseButtons |= mbMiddleButton;
            else if( msg == kWmButton3Up )    gMouseButtons &= (uchar) ~mbMiddleButton;

            /* Mouse positions are signed, and during a drag with capture
               they go negative outside the window: SHORT1FROMMP returns a
               USHORT, so the sign has to be put back by way of short.
               Clamping to the grid keeps a pointer dragged past the edge
               reporting the edge's cell, which is what the VIO mouse queue
               delivers and what a release outside still reaches. */
            LONG px = (long) (short) SHORT1FROMMP( mp1 );
            LONG py = (long) (short) SHORT2FROMMP( mp1 );
            if( px < 0 )
                px = 0;
            if( py < 0 )
                py = 0;
            if( py >= (long) gClientH )
                py = (long) gClientH - 1;
            MouseEventType mouse;
            fillMouse( mouse,
                       (int) (px / (long) gCellPxW),
                       (int) ((long) gClientH - 1 - py) / (int) gCellPxH,
                       gMouseButtons,
                       MOUSE_MOVED,  // motion is PM's only continuous truth
                       0,
                       (SHORT1FROMMP( mp2 ) & kKcCtrl) ? kbCtrlShift : 0 );
            putMouse( mouse );
            if( !gMouseLogged )
                {
                gMouseLogged = True;   // the pump answers the pointer: this
                pmLog( "mouse first" );// is the proof, and its absence the
                }                      // answer about clicks at a deaf window
        }
        return 0;

    // No WM_CAPTURECHANGED safety net here, though PM does offer the
    // capture-changed notice: the message and its constant are not in
    // Watcom's headers (measured), and the capture mechanism itself is what
    // delivers button-up to the window that took the button-down, even for
    // releases outside it -- eFTE ships on exactly that and never drops a
    // held button, so the state above stays honest without asking.

    case WM_COMMAND:
        // Menu picks from the frame's PM menu.  PM delivers them to the
        // client window -- the frame's default procedure forwards them --
        // and each id becomes the hot key the application already answers
        // (see pmTakeCommand).
        pmTakeCommand( (int) SHORT1FROMMP( mp1 ) );
        return 0;

    case WM_CLOSE:
        // The way out the user expects from a window: the application is
        // asked the way its own status bar says (Alt-X), as a key event --
        // the one exit this application answers from inside the UI.  If it
        // has not gone by kQuitForceMs, it is not going: a window whose
        // application stopped answering is not a window, it is debris.
        gClosed = True;
        gClosedMs = THardwareInfo::getTickCountMs();
        pmLog( "close -> quit" );
        return 0;

    case WM_DESTROY:
        WinPostMsg( HWND_DESKTOP, WM_QUIT, 0, 0 );
        return 0;
    }
    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}

//
// The message pump.  waitForEvents owns it: PM messages become driver
// events during the pump, and the pump is what answers the window manager
// while the application thinks.
//

static Boolean anyInput()
{
    return gKeyPending || gMouseCount > 0;
}

/* One message out of the queue, answered.  False is the order to stop
   waiting -- WM_QUIT, which the close posts -- and the application gets
   the same closed flag the VIO driver raises for a Ctrl-C: quit is quit,
   however it was dressed.  The wake message is consumed here: its addressee
   is the waiting loop, not the window. */
static Boolean deliverMessage( QMSG &q )
{
    if( q.msg == WM_QUIT )
        {
        gClosed = True;
        if( gClosedMs == 0 )
            gClosedMs = THardwareInfo::getTickCountMs();
        return False;
        }
    if( q.msg == kWmWake )
        return True;
    WinDispatchMsg( gHab, &q );
    return True;
}

/* Drain what the queue already holds, without blocking: the exact pair
   eFTE's ConGetEvent uses (g_pm.cpp:2406..2412) -- a NOREMOVE peek asks
   whether there is anything, and the WinGetMsg that follows takes it and
   returns at once, because what it would wait for is already there.  Both
   calls NULLHANDLE-filtered and flag-zero, eFTE's spelling throughout.

   The first failure logs the PM error's human text: build three's pump
   failed every call with 0x81019 and nothing in the header trees names
   that number, so the box's own error table is the only witness. */
static long gPumpCalls = 0;
static long gPumpLogs = 0;
static Boolean gPumpErrLogged = False;

static void dispatchPending()
{
    for( int guard = 0; guard < 1024; ++guard )
        {
        QMSG probe;
        const BOOL seen = WinPeekMsg( gHab, &probe, NULLHANDLE, 0, 0,
                                      kPmqNoRemove );
        ++gPumpCalls;
        if( gPumpLogs < 10 && gLog != 0 )
            {
            ++gPumpLogs;
            fprintf( gLog, "peek %ld seen=%d msg=0x%lx err=0x%lx\n",
                     gPumpCalls, (int) seen,
                     seen ? (unsigned long) probe.msg : 0UL,
                     seen ? 0UL : (unsigned long) WinGetLastError( gHab ) );
            fflush( gLog );
            }
        if( !seen )
            {
            if( !gPumpErrLogged )
                {
                gPumpErrLogged = True;
                pmLogErr( "WinPeekMsg" );
                }
            return;
            }
        QMSG q;
        if( !WinGetMsg( gHab, &q, NULLHANDLE, 0, 0 ) )
            {
            gClosed = True;   // FALSE means the quit arrived (eFTE: return -1)
            if( gClosedMs == 0 )
                gClosedMs = THardwareInfo::getTickCountMs();
            return;
            }
        if( !deliverMessage( q ) )
            return;
        if( anyInput() )
            return;       // what the application came for is in hand; the
        }                 // rest of the queue can wait for the next turn
}

/* The alarm thread, which is eFTE's PipeThread pattern from head to foot
   (g_pm.cpp:4341..4426, built with this compiler and running on boxes like
   this one): WinInitialize(0) for the connection, post the notice into the
   window's queue to wake a blocked WinGetMsg, WinTerminate on the way out
   -- which is why the flag goes False before the window dies, and
   pmCloseWindow waits out a cycle: WinTerminate here lands the thread's
   own connection, whenever that happens to be. */
static void pmWakeLoop( void * )
{
    const HAB hab = WinInitialize( 0 );
    while( gWakeUp )
        {
        DosSleep( gWakeMs );
        if( gWakeUp && gClient != 0 )
            WinPostMsg( gClient, kWmWake, 0, 0 );
        }
    if( hab != 0 )
        WinTerminate( hab );
}

//
// Construction: the window is born here, the way the stdwnd sample draws
// its birth certificate -- class, frame with resources, then show.
//

/* The grid's engine: IBM's AVIO page, assembled the way eFTE's PM front
   assembles it (g_pm.cpp:2015..2030).  A page is a BIOS text screen --
   cells of attribute over CP850 character, the attribute byte's own
   palette, its own scroll and clear -- and the association makes the
   window show it.  Which font, which glyphs, which colours the attribute
   bytes mean: those are the kernel's answers now, and they have shipped
   longer than this file has existed.  Everything this driver used to do
   by hand to get the same job (builds two through seven: the font recipe,
   the palette table, the fill-and-ink paint) is gone, and each of its
   measured failures with it. */
static Boolean pmInitAvio()
{
    /* The DC and a PS built on it with GPIA_ASSOC -- the association flag
       is what lets WinBeginPaint hand the same PS to each paint, which
       eFTE passes to it (g_pm.cpp:2016, 2201). */
    const HDC hdc = WinOpenWindowDC( gClient );
    if( hdc == 0 )
        {
        pmLogErr( "avio: WinOpenWindowDC" );
        return False;
        }
    SIZEL sizl;
    sizl.cx = 0;
    sizl.cy = 0;
    gHpsScreen = GpiCreatePS( gHab, hdc, &sizl,
                              PU_PELS | GPIF_DEFAULT | GPIT_MICRO
                              | GPIA_ASSOC );
    if( gHpsScreen == 0 )
        {
        pmLogErr( "avio: GpiCreatePS" );
        return False;
        }

    /* rows and columns, format 0, attribute format 1, reserved 0 --
       eFTE's exact arguments (g_pm.cpp:2018), its page merely sized to
       its own maximum where this one is sized to the grid the application
       was started with. */
    const USHORT rcPs = VioCreatePS( &gHvps, (SHORT) gRows, (SHORT) gCols,
                                     0, 1, 0 );
    if( gLog != 0 )
        fprintf( gLog, "avio ps rc=%u hvps=%u\n",
                 (unsigned) rcPs, (unsigned) gHvps ), fflush( gLog );
    if( rcPs != 0 )
        return False;

    const USHORT rcAs = VioAssociate( hdc, gHvps );
    if( gLog != 0 )
        fprintf( gLog, "avio associate rc=%u\n", (unsigned) rcAs ),
            fflush( gLog );
    if( rcAs != 0 )
        return False;

    /* Ask for the cell before reading one: this box's engine default came
       back 12x30 (measured, run eight), two steps scaled for a display
       with the pixels doubled, and an 80x25 grid of it stood 968x805 --
       taller than the whole 1024x768 screen, title bar off the top.  The
       request is eFTE's WindowFont mechanism, its height-width spelling
       (g_pm.cpp:2021..2026); 8x16 is the classic cell every theme was
       designed on and the size the grid arithmetic below trusts.  What
       the engine answers is still what gets believed. */
    const USHORT rcSet = VioSetDeviceCellSize( kCellHReq, kCellWReq, gHvps );
    if( gLog != 0 )
        fprintf( gLog, "avio cell set rc=%u\n", (unsigned) rcSet ),
            fflush( gLog );

    /* The cell the engine actually has, believed on sight this time --
       build five's diagonal drift was an assumed cell over a real font,
       and this number is not an assumption, it is the engine's.  It
       decides the frame size, the mouse mapping and the caret. */
    SHORT cyChar = 0;
    SHORT cxChar = 0;
    const USHORT rcCell = VioGetDeviceCellSize( &cyChar, &cxChar, gHvps );
    if( gLog != 0 )
        fprintf( gLog, "avio cell rc=%u %ux%u\n", (unsigned) rcCell,
                 (unsigned) cxChar, (unsigned) cyChar ), fflush( gLog );
    if( cxChar >= kCellWMin && cxChar <= kCellWMax
        && cyChar >= kCellHMin && cyChar <= kCellHMax )
        {
        gCellPxW = (ushort) cxChar;
        gCellPxH = (ushort) cyChar;
        }

    // The page's own clear: the whole page, one scroll-up of blank
    // attribute (0x07, light grey on black), exactly eFTE's WM_CREATE
    // gesture (g_pm.cpp:2029) -- the BIOS idiom the page understands.
    uchar blankAttr = kDefaultAttr;
    const USHORT rcClr = VioScrollUp( 0, 0, (USHORT) -1, (USHORT) -1,
                                      (USHORT) -1, &blankAttr,
                                      (HVIO) gHvps );
    if( rcClr != 0 && gLog != 0 )
        fprintf( gLog, "avio clear rc=%u\n", (unsigned) rcClr ),
            fflush( gLog );

    gAvioOk = True;
    return True;
}

static void pmOpenWindow()
{
    if( gPmUp )
        return;

    if( gLog == 0 )
        gLog = fopen( "atuipm.run.txt", "w" );
    pmLog( "birth" );

    gHab = WinInitialize( 0 );
    if( gHab == 0 )
        {
        pmLogErr( "WinInitialize" );
        return;
        }
    gHmq = WinCreateMsgQueue( gHab, 0 );
    if( gHmq == 0 )
        {
        pmLogErr( "WinCreateMsgQueue" );
        WinTerminate( gHab );
        gHab = 0;
        return;
        }
    if( !WinRegisterClass( gHab, "AtuiApp", (PFNWP) pmWndProc,
                           CS_SIZEREDRAW | CS_CLIPCHILDREN, 0 ) )
        {
        pmLogErr( "WinRegisterClass" );
        WinDestroyMsgQueue( gHmq );
        WinTerminate( gHab );
        gHmq = 0;
        gHab = 0;
        return;
        }

    // The frame, asked for explicitly rather than as "standard minus a
    // couple of things".  FCF_STANDARD (0xCC3F) requests MENU *and*
    // accelerator resources by id, and their absence answers
    // WinCreateStdWindow with PMERR_RESOURCE_NOT_FOUND, the window never
    // being created at all (measured: atuipm's first run died exactly
    // there).  Today the menu is wanted -- a real PM menu bar is the
    // interface idiom, its items in atuipm.rc, their picks injected as
    // keys (pmTakeCommand).  The accelerator table is not: every hot key
    // the application has arrives as a plain WM_CHAR already, and a frame
    // accelerator would take those combinations over the driver's head
    // and double-answer them.  The maximize button stays gone for the
    // same reason as always: the grid is fixed, and a button that does
    // nothing is a promise not kept.
    ULONG flFrame = (FCF_STANDARD & ~FCF_SHELLPOSITION & ~FCF_MAXBUTTON
                     & ~FCF_ACCELTABLE);
    gFrame = WinCreateStdWindow( HWND_DESKTOP, 0, &flFrame,
                                 "AtuiApp", "atui",
                                 0, (HMODULE) 0, kIdWindow, &gClient );
    if( gFrame == 0 )
        {
        pmLogErr( "WinCreateStdWindow" );
        WinDestroyMsgQueue( gHmq );
        WinTerminate( gHab );
        gHmq = 0;
        gHab = 0;
        return;
        }
    pmLog( "window ok" );

    // The window makes the native dialogs possible: hand them to the
    // application now (src/tui/pmdialogs.h), before the first paint and
    // before any event that could open one.
    pmDialogsInstall();

    if( !pmInitAvio() )
        pmLog( "avio unavailable: window stays blank" );
    if( gLog != 0 )
        fprintf( gLog, "page %s cell %ux%u\n",
                 gAvioOk ? "ok" : "FAILED",
                 gCellPxW, gCellPxH ), fflush( gLog );

    // Size the frame so the client is the grid exactly.  Watcom's
    // WinCalcFrameRect has the third argument (pmwin.h:1415, a BOOL); it
    // converts the given client rect in place, so the grid rect goes in
    // and the frame rect comes out.
    RECTL rcl;
    rcl.xLeft = 0;
    rcl.yBottom = 0;
    rcl.xRight = (LONG) gCols * (LONG) gCellPxW;
    rcl.yTop = (LONG) gRows * (LONG) gCellPxH;
    WinCalcFrameRect( gFrame, &rcl, 0 );
    if( gLog != 0 )
        fprintf( gLog, "grid %ux%u cell %ux%u frame %ldx%ld\n",
                 gCols, gRows, gCellPxW, gCellPxH,
                 (long) ( rcl.xRight - rcl.xLeft ), (long) ( rcl.yTop - rcl.yBottom ) ),
            fflush( gLog );
    gClientW = (ulong) ((LONG) gCols * (LONG) gCellPxW);
    gClientH = (ulong) ((LONG) gRows * (LONG) gCellPxH);
    /* Centre the frame on the screen rather than dropping it at a fixed
       point: the box's display is 1024x768 with an icon bar, and a window
       placed by its bottom-left corner at (40,40) with the engine's
       generous cell ran its title bar off the top -- out of reach until
       the user played title-bar hockey with hot keys (reported, and
       screenshot4 shows why: 805 pixels of frame on a 768 pixel screen).
       Centring from the real screen size keeps the whole frame visible
       whenever it fits at all, and the clamps keep its corner on screen
       when it does not.  The desktop window's rect is the screen: PM's
       1.x idiom, and needed here anyway -- WinQuerySysInfo is absent from
       Watcom's headers and import library both (measured), the same 1.x
       vocabulary gap that cost the timers. */
    RECTL scr;
    WinQueryWindowRect( HWND_DESKTOP, &scr );
    const LONG frameW = rcl.xRight - rcl.xLeft;
    const LONG frameH = rcl.yTop - rcl.yBottom;
    LONG x = (LONG) 40;
    LONG y = (LONG) 40;
    if( scr.xRight > frameW )
        x = ( scr.xRight - frameW ) / 2;
    if( scr.yTop > frameH )
        y = ( scr.yTop - frameH ) / 2;
    if( x < 2 )
        x = 2;
    if( y < 2 )
        y = 2;
    WinSetWindowPos( gFrame, HWND_TOP, x, y, frameW, frameH,
                     SWP_SIZE | SWP_MOVE | SWP_ACTIVATE | SWP_SHOW );
    WinSetFocus( HWND_DESKTOP, gClient );
    gPmUp = True;
    pmLog( "shown" );

    // The alarm thread: born last, because it posts to the window and must
    // never meet one that is not there yet.  It is the timeout machinery of
    // the wait, and nothing else; losing it (a thread the runtime could not
    // start) leaves a window whose waits end on messages only -- late, but
    // not deaf, and the log says so.
    gWakeUp = True;
    const int wakeRc = _beginthread( &pmWakeLoop, 0, (unsigned) kWakeStack, 0 );
    if( wakeRc == -1 )
        {
        gWakeUp = False;
        pmLogErr( "wake thread" );
        }
    else
        gWakeTid = (unsigned) wakeRc;    // the runtime's handle; kept only to
}                                        // say "there is a thread to wait out"

static void pmCloseWindow()
{
    // The alarm stops first, and is waited out: gClient is about to go, and
    // a post into a destroyed window is PM's error to shrug at -- better
    // the thread simply never makes one.  The sleep is one pace plus a
    // little; the thread checks gWakeUp after every sleep.
    gWakeUp = False;
    if( gWakeTid != 0 )
        DosSleep( kIdleWakeMs + 100 );
    if( gHvps != 0 )
        {
        // eFTE's order (g_pm.cpp:2032..2034): come off the DC, destroy
        // the page, destroy the PS it was shown through.
        VioAssociate( (HDC) 0, gHvps );
        VioDestroyPS( gHvps );
        gHvps = 0;
        }
    if( gHpsScreen != 0 )
        {
        GpiDestroyPS( gHpsScreen );
        gHpsScreen = 0;
        }
    if( gFrame != 0 )
        {
        WinDestroyWindow( gFrame );
        gFrame = 0;
        gClient = 0;
        }
    if( gHmq != 0 )
        {
        WinDestroyMsgQueue( gHmq );
        gHmq = 0;
        }
    if( gHab != 0 )
        {
        WinTerminate( gHab );
        gHab = 0;
        }
    gPmUp = False;
}

THardwareInfo::THardwareInfo() noexcept
{
    platform = plOS2;

    // Grid first, window second: the window is sized from the grid, so an
    // ATUI_COLS or ATUI_ROWS that arrived after the sizing would set a grid
    // the window does not have.
    const char *env = getenv( "ATUI_COLS" );
    if( env != NULL )
        {
        int n = atoi( env );
        if( n >= 20 && n <= 255 )
            gCols = (ushort) n;
        }
    env = getenv( "ATUI_ROWS" );
    if( env != NULL )
        {
        int n = atoi( env );
        if( n >= 2 && n <= 99 )
            gRows = (ushort) n;
        }

    pmOpenWindow();

    sbInfo.dwSize.X = gCols;
    sbInfo.dwSize.Y = gRows;
    crInfo.dwSize = 1;
    crInfo.bVisible = True;

    consoleHandle[cnInput] = (HANDLE) 0;
    consoleHandle[cnOutput] = (HANDLE) 1;
}

THardwareInfo::~THardwareInfo()
{
    pmLog( "dtor" );
    restoreConsole();
    pmCloseWindow();
    if( gLog != 0 )
        {
        fclose( gLog );
        gLog = 0;
        }
}

//
// Caret functions.  The caret is PM's own (pmUpdateCaret); these keep the
// state and call for it to be re-made in the new state.
//

ushort THardwareInfo::getCaretSize() noexcept
{
    return (ushort) crInfo.dwSize;
}

BOOL THardwareInfo::isCaretVisible() noexcept
{
    return crInfo.bVisible;
}

void THardwareInfo::setCaretSize( ushort size ) noexcept
{
    crInfo.dwSize = size;
    crInfo.bVisible = (size != 0) ? True : False;
    const int shown = (size != 0) ? 1 : 0;
    if( gCaretWasShown == shown )
        return;
    gCaretWasShown = shown;
    gCaretShown = (shown == 1);
    pmUpdateCaret();
}

void THardwareInfo::setCaretPosition( ushort x, ushort y ) noexcept
{
    if( gCaretX == x && gCaretY == y )
        return;
    gCaretX = x;
    gCaretY = y;
    sbInfo.dwCursorPosition.X = (SHORT) x;
    sbInfo.dwCursorPosition.Y = (SHORT) y;
    pmUpdateCaret();
}

//
// Screen functions.
//

ushort THardwareInfo::getScreenRows() noexcept
{
    return gRows;
}

ushort THardwareInfo::getScreenCols() noexcept
{
    return gCols;
}

/* The one windowed front there is: atui sits in a PM window, and the menu
   bar, the frame and the caret around it are the desktop's.  See
   hardware.h -- and see initMenuBar_ in the application, which stops drawing
   a second menu bar once this answers True. */
Boolean THardwareInfo::windowed() noexcept
{
    return True;
}

ushort THardwareInfo::getScreenMode() noexcept
{
    ushort mode = TDisplay::smCO80;
    if( gRows > 25 )
        mode |= TDisplay::smFont8x8;
    return mode;
}

void THardwareInfo::setScreenMode( ushort ) noexcept
{
    // v1: the grid is what the window was sized to; a resize means a new
    // window size and a tvision layout pass, which is the next step of
    // this port.  Asking is answered by what already is.
}

void THardwareInfo::clearScreen( ushort, ushort ) noexcept
{
    if( !ensureShadow() )
        return;
    memset( gShadowCh, ' ', shadowCells() );
    memset( gShadowAttr, kDefaultAttr, shadowCells() );
    memset( gDirty, 1, shadowCells() );
    gDirtyCount = shadowCells();
    if( gPmUp )
        pmInvalidateWhole();
}

/* One row's dirty runs, pushed to the page as cell strings.  The pair is
   the video-memory order -- character first, attribute second -- which is
   what the box measured (screenshot4: sent the other way round, the blank
   spaces read as attribute 0x20, green-on-black, so the screen was a sea
   of green, and every attribute byte came back as a CP850 control glyph
   -- dotted confetti where the text should have been).  It is also eFTE's
   in-memory order: its TCell is one unsigned short, and the little-endian
   parts put the character at the low byte.  The page repaints what it
   covers in the window it is associated with, which is why the dirty map
   clears at the flush, the moment the truth leaves the shadow, and not
   at the end of some later paint: after this call the page knows. */
static void pmPushRow( ushort row )
{
    uchar cells[ 2 * 256 ];       // a span cannot exceed the widest grid
    const size_t line = (size_t) row * gShadowCols;
    ushort x = 0;
    while( x < gShadowCols )
        {
        if( !gDirty[ line + x ] )
            {
            ++x;
            continue;
            }
        const ushort first = x;
        uchar *p = cells;
        while( x < gShadowCols && gDirty[ line + x ]
               && (size_t) ( p - cells ) + 2 <= sizeof( cells ) )
            {
            *p++ = gShadowCh[ line + x ];
            *p++ = gShadowAttr[ line + x ];
            ++x;
            }
        VioWrtCellStr( (PCH) cells, (USHORT) ( p - cells ),
                       (USHORT) row, (USHORT) first, (HVIO) gHvps );
        ++gPmRuns;
        }
}

void THardwareInfo::flushScreen() noexcept
{
    /* The frame ends here (the rule the VIO driver documents and the
       platform hardware.cpp follows): everything drawn since the last time
       around reaches the screen in one batch of cell strings, never as the
       fill-then-text pair the classic writer makes of every repaint. */
    if( gDirtyCount == 0 )
        return;
    if( gHvps == 0 || gShadowCols != gCols || gShadowRows != gRows )
        return;                 // nowhere to push; the shadow keeps the news
    /* The caret goes away for the duration of the push and comes back
       after it.  PM's caret is an inversion laid over the screen, and a
       write that lands under it without hiding leaves the inverted pixels
       when its own ink does not cover them -- a blank erases nothing, so
       backspacing dragged the caret's white block along behind it
       (screenshot5: a residue at every cell the caret rested on and only
       blanks arrived at afterwards).  eFTE wraps every VioWrtCellStr that
       can touch the caret's row in exactly this pair of calls
       (g_pm.cpp:2871..2877 with 3196..3208); one pair around the whole
       flush serves the same end here, the flush being this driver's one
       writing moment. */
    const Boolean caretWasShown = (Boolean) ( gCaretShown && gFocused );
    if( caretWasShown )
        WinShowCursor( gClient, FALSE );
    for( ushort row = 0; row < gShadowRows; ++row )
        pmPushRow( row );
    forgetPending();
    if( caretWasShown )
        WinShowCursor( gClient, TRUE );
}

void THardwareInfo::redrawScreen() noexcept
{
    if( gDirty == 0 || gShadowCols != gCols || gShadowRows != gRows )
        return;     // a mismatched shadow is nobody's picture
    pmInvalidateWhole();
}

void THardwareInfo::screenWrite( ushort x, ushort y, CHAR_INFO *buf, DWORD len ) noexcept
{
    if( len == 0 )
        return;
    if( !ensureShadow() )
        return;
    if( x >= gCols || y >= gRows )
        return;
    if( (DWORD) ( gCols - x ) < len )
        len = (DWORD) ( gCols - x );

    for( DWORD i = 0; i < len; ++i )
        if( !cellIsCurrent( (ushort) ( x + i ), y, buf[i] ) )
            rememberCell( (ushort) ( x + i ), y, buf[i] );
}

TScreenCell *THardwareInfo::allocateScreenBuffer() noexcept
{
    // The classic writer indexes the topmost buffer as CHAR_INFO even
    // though the cell type of a legacy build is half the size.
    return (TScreenCell *) new CHAR_INFO[(size_t) gCols * gRows];
}

void THardwareInfo::freeScreenBuffer( TScreenCell *buffer ) noexcept
{
    delete[] (CHAR_INFO *) buffer;
}

void THardwareInfo::setUpConsole() noexcept
{
    if( !gPmUp )
        pmOpenWindow();             // a resume from a suspension that
                                    // closed the window gets it back
    ensureShadow();
    if( gPmUp )
        {
        pmInvalidateWhole();        // the window repaints from the shadow:
                                    // it holds nothing the shadow does not
        }
}

void THardwareInfo::restoreConsole() noexcept
{
    delete[] gShadowCh;
    delete[] gShadowAttr;
    delete[] gDirty;
    gShadowCh = 0;
    gShadowAttr = 0;
    gDirty = 0;
    gDirtyCount = 0;
    gShadowCols = 0;
    gShadowRows = 0;
}

//
// Mouse functions.
//

DWORD THardwareInfo::getButtonCount() noexcept
{
    return 3;       // PM's mouse has three; asking never went without
}

void THardwareInfo::cursorOn() noexcept
{
}

void THardwareInfo::cursorOff() noexcept
{
}

//
// Event functions.
//

BOOL THardwareInfo::getMouseEvent( MouseEventType& event ) noexcept
{
    if( gMouseCount == 0 )
        return False;
    event = gMouseQueue[ gMouseHead ];
    gMouseHead = (gMouseHead + 1) % kMouseQueue;
    --gMouseCount;
    return True;
}

BOOL THardwareInfo::getKeyEvent( TEvent& event ) noexcept
{
    if( !gKeyPending )
        return False;
    event = gPendingKey;
    gKeyPending = False;

    // Insert is a state the application reports through the event, and
    // this is the only place that sees every keystroke.
    if( event.keyDown.keyCode == kbIns )
        insertState = !insertState;
    if( insertState )
        event.keyDown.controlKeyState |= kbInsState;

    return True;
}

void THardwareInfo::waitForEvents( int timeoutMs ) noexcept
    /* The first six entries and every 512th log: whether the application
       keeps coming back to wait (alive, deaf queue) or left the loop and
       is stuck somewhere else entirely -- PM build two logged the first
       entry and then nothing, which could mean either. */
{
    static long gWaitCalls = 0;
    ++gWaitCalls;
    if( ( gWaitCalls <= 6 || gWaitCalls % 512 == 0 ) && gLog != 0 )
        {
        fprintf( gLog, "wait #%ld t=%d\n", gWaitCalls, timeoutMs );
        fflush( gLog );
        }
    flushScreen();

    /* The closed window asks the application to end, once; five seconds
       later the asking stops and the process does. */
    if( gClosed && !gQuitKeySent )
        {
        gQuitKeySent = True;
        if( !gKeyPending )
            {
            // The Alt-X of tkeys.h: keyCode 0x2d00 is the key's own scan
            // (0x2d) over character zero with Alt held -- exactly what the
            // VIO driver delivers for Alt and a letter, and so what this
            // application's hotkey table already answers.
            fillKey( gPendingKey, 0, 0x2D, kbAltShift );
            gKeyPending = True;
            }
        }
    if( gClosed
        && THardwareInfo::getTickCountMs() - gClosedMs >= (uint64_t) kQuitForceMs )
        {
        pmLog( "force exit" );
        DosExit( EXIT_PROCESS, 0 );
        }

    /* The wait proper is PM's own fetch -- the call every TOOLKIT sample
       and eFTE spend their lives in.  DosSleep loops and peek-based polls
       are what this driver invented instead, because the VIO driver it
       mirrors has no window to serve; the box answered that design with a
       window that never painted (atuipm.run.txt builds two and three).
       WinGetMsg blocks on the queue, keeps the window manager answered
       while blocked, generates the WM_PAINT an invalidated window is
       asking for, and returns FALSE on WM_QUIT.  What it has no notion of
       is a timeout -- PM timers are not in Watcom 1.9's vocabulary at all
       -- and so the alarm thread posts the deadline into the queue, in
       exactly the manner eFTE wakes its own blocked fetches. */

    dispatchPending();
    if( anyInput() )
        return;
    if( timeoutMs == 0 )
        return;         // a pure poll: the queue said no, and no more

    if( gWakeUp )       // the alarm keeps the cadence of the asking
        gWakeMs = ( timeoutMs > 0 && timeoutMs < kIdleWakeMs )
                    ? ( timeoutMs < kWakeMsMin
                        ? (unsigned long) kWakeMsMin
                        : (unsigned long) timeoutMs )
                    : (unsigned long) kIdleWakeMs;

    const uint64_t deadlineMs =
        ( timeoutMs > 0 )
            ? THardwareInfo::getTickCountMs() + (uint64_t) timeoutMs
            : (uint64_t) 0;

    while( !anyInput() && !gInterruptWait )
        {
        QMSG q;
        if( !WinGetMsg( gHab, &q, NULLHANDLE, 0, 0 ) )
            {
            gClosed = True;   // FALSE is WM_QUIT (eFTE: return -1)
            if( gClosedMs == 0 )
                gClosedMs = THardwareInfo::getTickCountMs();
            break;
            }
        if( q.msg == kWmWake )
            {
            // The alarm: it owns the deadline, and the closed window's
            // five seconds -- a windowful of messages can be what never
            // arrives, and an application deaf to the injected Alt-X must
            // not leave a dead window and a blocked thread forever.
            if( deadlineMs != 0
                && THardwareInfo::getTickCountMs() >= deadlineMs )
                break;
            if( gClosed
                && THardwareInfo::getTickCountMs() - gClosedMs
                       >= (uint64_t) kQuitForceMs )
                {
                pmLog( "force exit" );
                DosExit( EXIT_PROCESS, 0 );
                }
            continue;
            }
        if( !deliverMessage( q ) )
            break;
        }
    gInterruptWait = False;
}

void THardwareInfo::interruptEventWait() noexcept
{
    /* Flag only, and the flag is answered by the next alarm tick: ten-odd
       milliseconds when the application was waiting short because work is
       streaming -- which is the only moment this call matters -- and a
       quarter second when it was waiting on nothing in particular, which
       is exactly when nobody needed waking.

       eFTE's PipeThread proves that a worker can post the wake directly:
       it WinInitialize's on entry and WinTerminate's on exit
       (g_pm.cpp:4359..4426).  atui's workers have no such handshake --
       they are runtime threads that know sockets, not PM -- and bolting a
       per-thread WinInitialize onto this call with thread-local state is
       a machine this driver does not need while the alarm is already
       running.  If a future wait ever has to be answered in less than a
       tick, that is where it belongs. */
    gInterruptWait = True;
}

BOOL THardwareInfo::setClipboardText( TStringView text ) noexcept
{
    (void) text;
    return False;   // The PM clipboard is reachable from here; wiring it
                    // is the undone, not the impossible.
}

BOOL THardwareInfo::requestClipboardText( void (&accept)( TStringView ) ) noexcept
{
    (void) accept;
    return False;
}

//
// Clock.
//

uint64_t THardwareInfo::getTickCountMs() noexcept
{
    struct timeb now;
    ftime( &now );
    return (uint64_t) (long) now.time * 1000 + (uint64_t) now.millitm;
}

/* The same three numbers the application logs for the VIO driver: the
   cell strings pushed to the page, the whole re-shows the window asked
   for, and what the last one cost -- the number the repaint interval is
   shaped from. */
extern "C" {

unsigned long tvVioStatCalls()   { return gPmRuns; }
unsigned long tvVioStatScrolls() { return gPmPaints; }
unsigned long tvVioStatFlushMs() { return gPmPaintMs; }

} // extern "C"

#endif // __FLAT__ && __OS2__ && !__BORLANDC__ && ATUI_PM
