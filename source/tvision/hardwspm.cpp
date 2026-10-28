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
 *   - the font: FATTRS + GpiCreateLogFont(hps, 0, FCID, &fattrs) +
 *     GpiSetCharSet -- the recipe of the toolkit's palette sample -- with
 *     GpiAssociate carrying it into every WinBeginPaint PS.
 *
 * The shadow/dirty machinery is the VIO driver's, copied with the reasons
 * in its comments: a repaint must cost what changed, and the screen must
 * see one state per frame, never the fill-then-text pair the classic
 * writer produces.  Here the "screen" is the window and "sending" is
 * WinInvalidateRect -- the paint that follows draws the shadow, so the
 * dirty bits clear at the end of WM_PAINT rather than at flush.
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

#define Uses_TKeys
#define Uses_TEvent
#define Uses_TScreen        // TDisplay, whose video mode constants getScreenMode answers with
#define Uses_THardwareInfo
#include <tvision/tv.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timeb.h>

enum
    {
    kPollMs     = 10,       // as in the VIO driver: the wait's slice
    kDefCols    = 80,
    kDefRows    = 25,
    kDefaultAttr = 0x07,    // light grey on black, the VGA default cell

    kCellW      = 9,        // the fallback grid: VGA's classic cell,
    kCellH      = 16,       // right when the font asks for nothing else
    kCellWMin   = 4,        // measurements outside these are not believed
    kCellWMax   = 32,
    kCellHMin   = 6,
    kCellHMax   = 32,

    /* GpiCreateLogFont's third argument is the id the set is installed
       under -- 1..255 is the application's range, and FCID 1 is what the
       toolkit's palette sample picks. */
    kFontFcid   = 1,

    /* WinPeekMsg's flags, from the toolkit docs: PMQ_* are not in
       Watcom's headers (measured), and 0x1000 keeps PM from sending
       WM_USERPAINT to the focus window while peeking. */
    kPmqRemove          = 0x0001,
    kPmqNoSendUserPaint = 0x1000,

    /* The window class and its resources, under the one id the frame is
       built with (the stdwnd sample's ID_WINDOW = 256, same numbering so
       atuipm.rc and pmhello.rc match). */
    kIdWindow   = 256,

    kMouseQueue = 16,       // mouse events held between pump and drain

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
static HPS    gHpsFont  = 0;    // the PS the screen font is installed on;
                                // paint PSes inherit it by GpiAssociate
static Boolean gFontOk  = False;
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

// The caret as the application left it; the window has no caret of its own,
// so WM_PAINT draws it as the cell's colours swapped.
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

/* Counting what the screen costs for the application's once-a-second log:
   runs drawn (the cell writes of this driver -- one WinFillRect or
   GpiCharStringAt each), whole paints done, and what the last one took. */
static unsigned long gPmRuns = 0;
static unsigned long gPmPaints = 0;
static unsigned long gPmPaintMs = 0;
static Boolean gInvalidatePending = False;

/* atuipm.run.txt: the same lifeline the probe carried, because a PM image
   started at the box's own display has no console to complain to, and the
   question "did the window fail to appear or did it appear and paint
   nothing" has very different answers.  Written next to the other probes'
   logs and fetched over FTP.  The first key and the first paint log once
   each: those are the events whose absence means the window exists but is
   deaf or blind, which is otherwise invisible. */
static FILE *gLog = 0;
static Boolean gKeyLogged = False;

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

/*
 * The BIOS colour byte made visible.  The themes were designed on the VGA
 * palette, so the palette is written out rather than asked: the system
 * palette of a desktop can be restyled by its user, and a theme should not
 * change because someone liked other colours.  Longs are PM's RGB shape --
 * red in the low byte (the RGB macro's order), and the GPI colour
 * primitives take the same long.
 */
static const long kRgb[16] =
    {
    0x00000000L,      0x00AA0000L,      //   0 black      1 blue
    0x0000AA00L,      0x00AAAA00L,      //   2 green      3 cyan
    0x000000AAL,      0x00AA00AAL,      //   4 red        5 magenta
    0x000055AAL,      0x00AAAAAAL,      //   6 brown      7 light grey
    0x00555555L,      0x00FF5555L,      //   8 grey       9 bright blue
    0x0055FF55L,      0x00FFFF55L,      //  10 br green  11 br cyan
    0x005555FFL,      0x00FF55FFL,      //  12 br red    13 br magenta
    0x0055FFFFL,      0x00FFFFFFL,      //  14 yellow    15 white
    };

static long colorOfFg( int attr )
{
    // Bits 0-3 are the foreground with the bright bit already in place,
    // exactly the index into the VGA palette.
    return kRgb[ attr & 0x0F ];
}

static long colorOfBg( int attr )
{
    // Bits 4-6 are the background and bit 7 brightens it -- the placement
    // every BIOS attribute table uses and the one the VIO driver keeps.
    return kRgb[ ((attr >> 4) & 0x07) | ((attr & 0x80) ? 8 : 0) ];
}

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

/* Cells owed a paint: ask the window for it.  A dirty-cell rectangle
   would clip the paint to what changed, which is a saving worth having
   once the paint itself is measured; the whole window asks for one paint
   of everything, correct first and fast later. */
static void pmInvalidateWhole()
{
    if( !gPmUp )
        return;
    WinInvalidateRect( gClient, 0, False );
    gInvalidatePending = True;
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

//
// Window procedure
//

static void pmPaint( HPS hpsPaint )
{
    if( gShadowCh == 0 || gShadowCols != gCols || gShadowRows != gRows )
        return;     // no picture of anything yet; drawing guesses is not drawing

    if( gFontOk )
        {
        GpiAssociate( gHpsFont, hpsPaint );
        GpiSetCharSet( hpsPaint, kFontFcid );
        }
    GpiSetBackMix( hpsPaint, BM_LEAVEALONE );  // text leaves its background:
                                               // the fill below is the background

    // Text reference point: GPI draws from the bottom left of the cell
    // box upward, which is how the toolkit's palette sample centres its
    // text (cyClient minus the box height, then GpiCharString at the
    // result).  So the cell's bottom edge is the draw point and the box
    // measured in pmInitFont -- the same measurement, mode notwithstanding
    // -- is exactly what the cell must be to hold it, descender included.
    const ushort cw = gCellPxW;
    const ushort ch = gCellPxH;

    for( ushort row = 0; row < gShadowRows; ++row )
        {
        // PM counts from the bottom; the shadow counts from the top.
        const long yBottom = (long) gClientH - (long) ( row + 1 ) * (long) ch;
        if( yBottom >= (long) gClientH )
            continue;                       // scrolled out above the window
        const size_t line = (size_t) row * gShadowCols;

        ushort x = 0;
        while( x < gShadowCols )
            {
            const long bg = colorOfBg( gShadowAttr[ line + x ] );
            const ushort first = x;
            long fg = colorOfFg( gShadowAttr[ line + x ] );
            int attr = gShadowAttr[ line + x ];
            ++x;
            while( x < gShadowCols && gShadowAttr[ line + x ] == attr )
                ++x;

            RECTL rcl;
            rcl.xLeft   = (long) first * (long) cw;
            rcl.xRight  = (long) x * (long) cw;
            rcl.yBottom = yBottom;
            rcl.yTop    = yBottom + (long) ch;
            WinFillRect( hpsPaint, &rcl, bg );
            ++gPmRuns;

            // Text run: blanks carry no ink and cost an ink call for
            // nothing, so skip the ones at either end of the run.
            ushort t = first;
            while( t < x && gShadowCh[ line + t ] == ' ' )
                ++t;
            while( t < x )
                {
                const ushort textStart = t;
                char text[ 256 ];
                size_t n = 0;
                while( t < x && n < sizeof( text ) - 1 )
                    {
                    const uchar c = gShadowCh[ line + t ];
                    text[ n++ ] = (c < 0x20 || c == 0x7F) ? ' ' : (char) c;
                    ++t;
                    if( t < x && gShadowCh[ line + t ] == ' ' )
                        {   // finish at the next blank run's end
                        ushort p = t;
                        while( p < x && gShadowCh[ line + p ] == ' ' )
                            ++p;
                        if( p >= x )
                            break;          // trailing blanks: nothing to draw
                        ++t;                // a single interior blank is text
                        }
                    }
                if( n > 0 )
                    {
                    text[ n ] = '\0';
                    GpiSetColor( hpsPaint, fg );
                    POINTL ptl;
                    ptl.x = (long) textStart * (long) cw;
                    ptl.y = yBottom;              // cell bottom; box grows up
                    GpiCharStringAt( hpsPaint, &ptl, (LONG) n, text );
                    ++gPmRuns;
                    }
                while( t < x && gShadowCh[ line + t ] == ' ' )
                    ++t;
                }
            }
        }

    // The caret last: the cell with its own colours swapped, which is what
    // a hardware caret over a cell looks like.
    if( gCaretShown
        && gCaretY < gShadowRows && gCaretX < gShadowCols )
        {
        const size_t at = (size_t) gCaretY * gShadowCols + gCaretX;
        const int attr = gShadowAttr[ at ];
        RECTL rcl;
        rcl.xLeft   = (long) gCaretX * (long) cw;
        rcl.xRight  = rcl.xLeft + (long) cw;
        rcl.yBottom = (long) gClientH - (long) ( gCaretY + 1 ) * (long) ch;
        rcl.yTop    = rcl.yBottom + (long) ch;
        WinFillRect( hpsPaint, &rcl, colorOfFg( attr ) );
        const uchar c = gShadowCh[ at ];
        if( c != ' ' && c >= 0x20 )
            {
            GpiSetColor( hpsPaint, colorOfBg( attr ) );
            POINTL ptl;
            ptl.x = rcl.xLeft;
            ptl.y = rcl.yBottom;
            char text[2] = { (char) c, '\0' };
            GpiCharStringAt( hpsPaint, &ptl, 1, text );
            }
        ++gPmRuns;
    }
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
            HPS hps = WinBeginPaint( hwnd, 0, &rcl );
            RECTL rect;
            WinQueryWindowRect( hwnd, &rect );
            gClientW = (ulong) ( rect.xRight - rect.xLeft );
            gClientH = (ulong) ( rect.yTop - rect.yBottom );
            pmPaint( hps );
            // The window holds the shadow now, wherever the paint reached;
            // every cell is painted each time, so the frame ends even.
            forgetPending();
            gInvalidatePending = False;
            WinEndPaint( hps );
            struct timeb t1;
            ftime( &t1 );
            gPmPaintMs = (unsigned long)
                ( ( (long) t1.time - (long) t0.time ) * 1000L
                  + (long) t1.millitm - (long) t0.millitm );
            if( gPmPaints == 0 )
                pmLog( "first paint" );
            ++gPmPaints;
        }
        return 0;

    case WM_SIZE:
        {
            RECTL rect;
            WinQueryWindowRect( hwnd, &rect );
            gClientW = (ulong) ( rect.xRight - rect.xLeft );
            gClientH = (ulong) ( rect.yTop - rect.yBottom );
        }
        return 0;

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
        }
        return 0;

    // No WM_CAPTURECHANGED safety net here, though PM does offer the
    // capture-changed notice: the message and its constant are not in
    // Watcom's headers (measured), and the capture mechanism itself is what
    // delivers button-up to the window that took the button-down, even for
    // releases outside it -- eFTE ships on exactly that and never drops a
    // held button, so the state above stays honest without asking.

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

static void pumpMessages()
{
    /* NULLHANDLE, not HWND_DESKTOP: the peek must see the messages sent to
       every window of this thread -- WM_PAINT belongs to the client window,
       and a desktop-only filter would leave the window asking for paint
       forever with nobody dispatching it.  It is also what both references
       do: eFTE's loop (g_pm.cpp:2408) and the probe's.  WinPeekMsg, unlike
       WinGetMsg, does not end the pump on WM_QUIT -- so the quit message,
       once removed from the queue, is turned into the closed flag here. */
    QMSG q;
    while( WinPeekMsg( gHab, &q, NULLHANDLE, 0, 0,
                       kPmqRemove | kPmqNoSendUserPaint ) )
        {
        if( q.msg == WM_QUIT )
            {
            gClosed = True;
            if( gClosedMs == 0 )
                gClosedMs = THardwareInfo::getTickCountMs();
            break;
            }
        WinDispatchMsg( gHab, &q );
    }
}

static Boolean anyInput()
{
    return gKeyPending || gMouseCount > 0;
}

//
// Construction: the window is born here, the way the stdwnd sample draws
// its birth certificate -- class, frame with resources, then show.
//

static void pmInitFont()
{
    /* The header calls this WinOpenWindowDC and says it returns an HDC --
       the 1.x signature.  On 2.x it hands back a presentation-space handle
       (that is why WinReleasePS frees it); the cast records that the
       disagreement is the header's, not the code's. */
    gHpsFont = (HPS) WinOpenWindowDC( gClient );
    if( gHpsFont == 0 )
        return;
    // No WinSetCp: the Watcom prototype is the 1.x (HMQ, ULONG) form, the
    // toolkit samples draw without ever calling it, and neither eFTE nor
    // the samples have needed it.  If the theme's box drawing arrives in
    // the wrong glyphs, the codepage is the first knob to reach for --
    // with the screen in front of whoever fixes it.
    FATTRS fa;
    memset( &fa, 0, sizeof( fa ) );
    fa.usRecordLength = sizeof( fa );
    fa.lMatch = 0;
    strcpy( fa.szFacename, "WarpSans Fixed" ); // the system's fixed face
    fa.idRegistry = 0;
    fa.usCodePage = 0;                 // whatever the PS says
    fa.lMaxBaselineExt = 423L;         // 16 pixels at 96 dpi (0.01 mm)
    fa.lAveCharWidth = 238L;           // 9 pixels
    fa.fsType = 0;
    fa.fsFontUse = 0;                  // any face that matches

    if( !GpiCreateLogFont( gHpsFont, (PSTR8) 0, kFontFcid, &fa ) )
        return;
    if( !GpiSetCharSet( gHpsFont, kFontFcid ) )
        return;

    // Measure what actually came.  GpiQueryTextBox is the five argument
    // form in the Watcom header (pmgpi.h:923): the corner mode and a
    // POINTL array are explicit.  TXTBOX_TOPLEFT anchors the box's top
    // left corner at the draw point, so the cell is the box -- width the
    // advance of M, this being a fixed face, height the full cell with
    // the descender inside.  The corners are folded by min and max rather
    // than by position: which corner comes back first is a spec detail
    // read rather than seen, and code that does not care cannot be wrong
    // about it.
    POINTL ptlBox[ 4 ];
    const LONG got = GpiQueryTextBox( gHpsFont, 1, "M", TXTBOX_TOPLEFT, ptlBox );
    if( gLog != 0 )
        fprintf( gLog, "textbox M rc=%ld p0=(%ld,%ld) p1=(%ld,%ld) p2=(%ld,%ld) p3=(%ld,%ld)\n",
                 got, (long) ptlBox[0].x, (long) ptlBox[0].y,
                 (long) ptlBox[1].x, (long) ptlBox[1].y,
                 (long) ptlBox[2].x, (long) ptlBox[2].y,
                 (long) ptlBox[3].x, (long) ptlBox[3].y ), fflush( gLog );
    if( got )
        {
        LONG xlo = ptlBox[0].x, xhi = ptlBox[0].x;
        LONG ylo = ptlBox[0].y, yhi = ptlBox[0].y;
        for( int i = 1; i < 4; ++i )
            {
            if( ptlBox[i].x < xlo ) xlo = ptlBox[i].x;
            if( ptlBox[i].x > xhi ) xhi = ptlBox[i].x;
            if( ptlBox[i].y < ylo ) ylo = ptlBox[i].y;
            if( ptlBox[i].y > yhi ) yhi = ptlBox[i].y;
            }
        const long w = xhi - xlo;
        const long h = yhi - ylo;
        if( w >= kCellWMin && w <= kCellWMax && h >= kCellHMin && h <= kCellHMax )
            {
            gCellPxW = (ushort) w;
            gCellPxH = (ushort) h;
            gFontOk = True;
            return;
            }
        }
    // A face matched but did not measure; keep drawing with it and the
    // classic cell -- better than no font at all.
    gFontOk = True;
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

    // FCF_STANDARD without the shell's positioning (the window says where
    // it goes) and without the maximize button (the grid is fixed, and a
    // button that does nothing is a promise not kept).  The resources --
    // icon above all, which the frame asks for by name and which a Watcom
    // image does not have until rc appended them -- are id 256.
    ULONG flFrame = (FCF_STANDARD & ~FCF_SHELLPOSITION & ~FCF_MAXBUTTON);
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

    pmInitFont();
    if( gLog != 0 )
        fprintf( gLog, "font %s cell %ux%u\n",
                 gFontOk ? "ok" : "fallback",
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
    gClientW = (ulong) ((LONG) gCols * (LONG) gCellPxW);
    gClientH = (ulong) ((LONG) gRows * (LONG) gCellPxH);
    WinSetWindowPos( gFrame, HWND_TOP, 40, 40,
                     rcl.xRight - rcl.xLeft, rcl.yTop - rcl.yBottom,
                     SWP_SIZE | SWP_MOVE | SWP_ACTIVATE | SWP_SHOW );
    WinSetFocus( HWND_DESKTOP, gClient );
    gPmUp = True;
    pmLog( "shown" );
}

static void pmCloseWindow()
{
    if( gHpsFont != 0 )
        {
        WinReleasePS( gHpsFont );
        gHpsFont = 0;
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
// Caret functions.  There is no caret in a window -- the cell's colours
// swapped is the caret, so these are state plus one invalidate.
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
    if( gPmUp )
        pmInvalidateWhole();      // v1: whole-window; caret-rect clips later
}

void THardwareInfo::setCaretPosition( ushort x, ushort y ) noexcept
{
    if( gCaretX == x && gCaretY == y )
        return;
    gCaretX = x;
    gCaretY = y;
    sbInfo.dwCursorPosition.X = (SHORT) x;
    sbInfo.dwCursorPosition.Y = (SHORT) y;
    if( gCaretShown && gPmUp )
        pmInvalidateWhole();
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

void THardwareInfo::flushScreen() noexcept
{
    /* The frame ends here (the rule the VIO driver documents and the
       platform hardware.cpp follows): everything drawn since the last time
       around reaches the screen in one paint, never as the fill-then-text
       pair the classic writer makes of every repaint. */
    if( gDirtyCount == 0 || gInvalidatePending )
        return;
    pmInvalidateWhole();
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
{
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

    pumpMessages();
    if( anyInput() )
        return;

    int waited = 0;
    while( !anyInput() )
        {
        if( gInterruptWait )
            break;
        if( timeoutMs >= 0 && waited >= timeoutMs )
            break;
        DosSleep( kPollMs );
        waited += kPollMs;
        pumpMessages();
        // A window closed during the wait has already been answered with
        // the injected Alt-X; its deadline is watched from here too, in
        // the sleeping loop and not by breaking out of it -- an
        // application deaf to the injection and idle at that would
        // otherwise spin the CPU at full tilt next to a dead window.
        if( gClosed
            && THardwareInfo::getTickCountMs() - gClosedMs >= (uint64_t) kQuitForceMs )
            {
            pmLog( "force exit" );
            DosExit( EXIT_PROCESS, 0 );
            }
        }
    gInterruptWait = False;
}

void THardwareInfo::interruptEventWait() noexcept
{
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
   writes that went out (runs of cells painted -- fill and ink each count,
   as the kernel counts), the frames that replaced a scroll (PM has no
   scroll shortcut: a painted frame is the scroll), and what the last
   paint cost, which is the number the repaint interval is shaped from. */
extern "C" {

unsigned long tvVioStatCalls()   { return gPmRuns; }
unsigned long tvVioStatScrolls() { return gPmPaints; }
unsigned long tvVioStatFlushMs() { return gPmPaintMs; }

} // extern "C"

#endif // __FLAT__ && __OS2__ && !__BORLANDC__ && ATUI_PM
