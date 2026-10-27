/*------------------------------------------------------------*/
/* filename -       hardwvos2.cpp                             */
/*                                                            */
/* THardwareInfo for OS/2 32-bit: VIO when there is a screen, */
/* VT escapes when there is not, keys from the session.       */
/*------------------------------------------------------------*/
/*
 * hardwrvr.cpp holds the driver bodies for Borland's Windows console and leaves
 * every other compiler the class with no implementation: the first program that
 * constructs a TApplication asks the link for 28 THardwareInfo members and finds
 * none of them.  This file answers that for OS/2, which is the platform whose
 * compiler (Open Watcom) builds the classic classes and cannot build the modern
 * terminal layer in source/platform.
 *
 * Two displays, chosen once at construction by asking for one:
 *
 *   VIO    the screen the session owns.  Tried first, ruled out by a probe that
 *          passed 1 as the session handle and took the resulting
 *          ERROR_VIO_INVALID_HANDLE (436) for a policy of the system; the handle
 *          is 0, and with it the whole of it works -- geometry, cells, attributes,
 *          cursor, keyboard -- in a session the telnet server started as well as
 *          at the machine's own screen.  It is the display, and the only way to
 *          learn the size of the screen instead of being told it.
 *
 *   VT     escapes on stdout, for a session with no VIO screen and for a client
 *          that cannot render one.  ATUI_OS2_STREAM=1 asks for it deliberately.
 *          Its one cost is that OS/2's own full-screen session is not a VT
 *          terminal, so nothing drawn this way appears on the box's display.
 *
 * Which of the two is in use is decided in the constructor and asked by every
 * output member, so nothing above this file knows there is a choice.
 *
 * Three measurements in TESTING.md shaped the input loop, and each is the kind
 * that reads as a bug in the driver if it is not written down beside the code:
 *
 *   * select() does not work on the console handle; it returns -1.  Readiness is
 *     polled with kbhit(), which does see every byte.  (kbhit() is itself
 *     KbdPeek in Open Watcom's run time, which is why the same input path serves
 *     either display: the RTL and this driver ask the keyboard the same question.)
 *   * The session translates incoming VT sequences into DOS extended codes, so an
 *     arrow key arrives as 0xE0 and then the BIOS scan code, and F1 as 0x00 0x3B.
 *     Reading a key therefore means taking a prefix and then its tail.
 *   * The two halves of one key have been seen 390 ms apart, so the tail is waited
 *     for rather than assumed lost.  A reader that stops at the first gap splits
 *     every translated key in two, and half a key reaches the view as a key nobody
 *     pressed.
 *
 * What this driver does not do, and why:
 *
 *   * Shift and control state on the stream path.  A character stream carries the
 *     character, not the keys that made it, so the modifier bits stay empty except
 *     where the byte itself proves one (a control byte is Ctrl plus a letter) or
 *     where an escape sequence carries one.  Mouse reports do carry modifiers.
 *     Under VIO this is not a guess at all: the keyboard record carries the
 *     modifier state beside the key, which is what makes the menu bar's Alt
 *     hotkeys work at a real keyboard.
 *   * Home and End over a session that eats the introducer.  Measured: Home
 *     arrives as '[' then 'H', End as '[' then 'F', the Escape having been lost on
 *     the way.  Treating a bare bracket as half a key would misread a pasted "[H",
 *     so a bracket stays a bracket and those two keys are the session's to fix.
 *     Under VIO they are not: the keyboard reports Home as 0xE0/0x47 and End as
 *     0xE0/0x4F whatever the client in front of the session does with escapes.
 *   * The clipboard.  The Presentation Manager clipboard belongs to a session this
 *     process does not have; the terminal's own clipboard, asked for with OSC 52,
 *     is the way to do it here.
 *   * Mouse wheel.  There is no wheel in the OS/2 mouse interface of this
 *     generation -- three buttons and a position -- so a wheel event cannot come
 *     from the machine's own mouse.  Over a session, where the terminal reports
 *     one as an escape, it does.
 *   * Noticing that the window was resized.  With VIO the size is asked once, and
 *     asked correctly: an OS/2 session's screen does not resize under a running
 *     program, and the telnet server's does not resize at all -- measured 80x25
 *     both for a client whose window was 100x40 at login and for an 80x25 session
 *     resized afterwards.  A stream driver cannot know either way, so there the
 *     size is taken from ATUI_COLS and ATUI_ROWS if they are set and 80x25
 *     otherwise, and stays until something calls setScreenMode.
 */

#if defined( __FLAT__ ) && defined( __OS2__ ) && !defined( __BORLANDC__ )

/* tvision's compat/windows/windows.h asks for <os2.h> on this platform, so the
   feature macros have to be set before tv.h is read rather than with the include
   below -- otherwise DosSleep is not in the header by the time this file wants
   it, and the first read of <os2.h> is the one that decides.  INCL_VIO for the
   screen; INCL_DOSERRORS for NO_ERROR, which every VIO call is answered with. */
#define INCL_DOSPROCESS
#define INCL_DOSERRORS
#define INCL_VIO
#define INCL_KBD
#define INCL_MOU

#define Uses_TKeys
#define Uses_TEvent
#define Uses_TScreen        // TDisplay, whose video mode constants getScreenMode answers with
#define Uses_THardwareInfo
#include <tvision/tv.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <conio.h>
#include <sys/timeb.h>

enum
    {
    kGapWaitMs  = 450,  // How long the rest of a split key is waited for.  Has
                        // to exceed the 390 ms the measurement produced.
    kPollMs     = 10,   // How often readiness is asked for while waiting.
    kSeqMax     = 24,   // Escape sequence parameters, digits and separators.
    kDefCols    = 80,
    kDefRows    = 25,
    kDefaultAttr = 0x07 // Light grey on black: leave it to the terminal.
    };

#ifdef TV_DEBUG_LOOP
/* Compiled out unless the build passes -DTV_DEBUG_LOOP, and it is there for one
   measurement: atui's own log showed the UI thread dispatching no timer event for 317
   seconds while a worker thread drove a turn (TESTING.md, "A whole turn on a thread, in
   the app").  Two places can swallow a timer and they look identical from the
   application -- this wait not returning, or the loop above it not collecting timers --
   and the only thing that tells them apart is a number from inside the wait.  Aggregated
   about once a second rather than one line per call, because the call rate is itself
   what is being measured, and one line immediately when a wait runs long, because a
   wait that never returns is the interesting case and it still gets said. */
static uint64_t gWaitLastReportMs = 0;
static int gWaitCalls = 0;
static long gWaitMs = 0;
static long gWaitLongestMs = 0;
static int gWaitLongestAsked = 0;
static int gWaitLastAsked = 0;
static const char *gWaitLastHow = "";

static void noteWait( int askedMs, uint64_t tookMs, const char *how, Boolean report )
{
    const uint64_t nowMs = THardwareInfo::getTickCountMs();

    ++gWaitCalls;
    gWaitMs += tookMs;
    gWaitLastAsked = askedMs;
    gWaitLastHow = how;
    if( tookMs > gWaitLongestMs )
    {
        gWaitLongestMs = tookMs;
        gWaitLongestAsked = askedMs;
    }

    if( tookMs >= 1000 )
    {
        fprintf( stderr, "[tvwait] LONG wait: asked %d ms, took %lu ms (%s)\n",
                 askedMs, (unsigned long) tookMs, how );
        fflush( stderr );
    }
    if( report && nowMs - gWaitLastReportMs >= 1000 )
    {
        fprintf( stderr, "[tvwait] %d calls, %lu ms total, longest %lu ms (asked %d), "
                 "last asked %d (%s)\n",
                 gWaitCalls, (unsigned long) gWaitMs, (unsigned long) gWaitLongestMs,
                 gWaitLongestAsked, gWaitLastAsked, gWaitLastHow );
        fflush( stderr );
        gWaitLastReportMs = nowMs;
        gWaitCalls = 0;
        gWaitMs = 0;
        gWaitLongestMs = 0;
    }
}
#endif // TV_DEBUG_LOOP

// What the terminal is set to, so that a run of cells emits one attribute
// change rather than one per cell.  -1 means the terminal's state is not known
// to us, which is how it starts and how it is left by anything that resets it.
static int gAttr = -1;

static ushort gCols = kDefCols;
static ushort gRows = kDefRows;

/* Which of the two displays this is: cells or escapes.  Set once in the
   constructor and asked by every member that draws or reads a key.  It is
   declared here rather than with the VIO code below because the input helpers
   are above that, and reading a key means knowing which of the two ways the
   keyboard is being read. */
static Boolean gVio = False;

// The caret as the application left it.  Emitting it is not enough on a stream:
// every write moves the terminal's cursor along, so this is what the position is
// restored to after a pass over the screen.
static ushort gCaretX = 0;
static ushort gCaretY = 0;

// Whether the terminal was last told to show the caret.  -1 is "not yet told", so
// the first cursorOn() or cursorOff() is never swallowed.
static int gCaretShown = -1;

// Where the terminal's own cursor was last addressed, and whether that is still
// where it is: writing text moves it along by itself, which is what the flag below
// records.  Seven bytes per caret placement, twenty times a second, is the second
// most expensive thing this driver used to do.
static ushort gTermX = 0;
static ushort gTermY = 0;
static Boolean gTermPosKnown = False;

/* The shadow of the terminal: what we believe the screen at the other end of the
   pipe is holding, one character and one attribute per cell.

   The classic writer does not diff.  Every cell a view draws reaches
   THardwareInfo::screenWrite() (tvwrite.cpp calls it for the run whether or not the
   cell changed), so a driver that forwards whatever it is handed turns a repaint
   into output.  Measured on eComStation with atui sitting at an idle prompt: 6.2 KB
   per second and 25 repaints per second of the two rows of the input panel, because
   the application redraws them on a 50 ms timer and each of those redraws was 160
   characters of output.  On a 486 over telnet that is not invisible: the separator
   visibly flickers, and the loop doing it is the same loop that reads the keyboard.

   Remembering what the terminal holds makes a repaint cost what changed instead of
   what was drawn, which is what every modern driver in this library does on the
   platforms that have one.

   An attribute of 0xFF with a character of 0 means "nothing that matches this has
   ever been written here", and no real cell can collide with it because
   printableCell() never sends a NUL.  An unknown cell is therefore always sent rather
   than always suppressed -- the one direction this can be wrong in that costs bytes
   rather than pixels.  It is also what a screen the driver did not clear itself
   starts as, which is the honest answer to a terminal this process cannot see. */
static uchar *gShadowCh = 0;
static uchar *gShadowAttr = 0;
static ushort gShadowCols = 0;
static ushort gShadowRows = 0;

static size_t shadowCells()
{
    return (size_t) gShadowCols * gShadowRows;
}

/* Cells that have been drawn and are not on the screen yet -- one byte each, and a
   count so that the flush of a screen with nothing to send is one integer test.

   Why this exists is that the classic writer draws a view by filling it with
   background and writing the text over it: TView::drawView() -> the view's draw()
   -> writeChar(' ', ..., whole row) -> writeStr().  Sent as they are made, those
   are two states on the screen for every repaint, and the one in between is blank.
   Streaming an answer therefore flashes the whole response area once per
   character, which is what it did on the box's own screen (reported as flicker)
   and, at 2 KB of wire per row on top of it, over telnet too.

   Holding the writes until the application goes looking for an event collapses
   fill-and-text back into one state: what reaches the screen is the difference
   between the last frame and this one, which is also all it ever costs.
   `source/platform/hardware.cpp` does exactly this on the platforms with a modern
   driver -- "flush the screen once for every time all events have been processed,
   only when blocking for events" -- and this driver is doing the same for the
   same reason. */
static uchar *gDirty = 0;
static size_t gDirtyCount = 0;

/* What the screen held at the end of the last flush, and whether that is a
   statement anyone can make -- only VIO fills it in, and only the flush does.
   It exists to recognise one frame shape: the last frame shifted up by a few
   rows, which is what every scrolled transcript is, and which the flush can
   hand to the hardware as one VioScrollUp instead of one VioWrtCellStr per
   row.  Measured (local.trc / local2.trc, TESTING.md): while an answer
   follows, the session spends more time inside the VIO write path than the
   application spends in all of its own code -- at 25 rows that is ~25 calls
   per frame, eight frames a second, and on the box each call costs the
   Presentation Manager machinery that mirrors it, not the bytes carried. */
static uchar *gPrevCh = 0;
static uchar *gPrevAttr = 0;
static Boolean gPrevValid = False;

/* Counting the expensive calls for the application's once-a-second log: how
   many writes went out, and how many frames a scroll replaced them with. */
static unsigned long gVioCalls = 0;
static unsigned long gVioScrolls = 0;

/* What the last frame cost to send, in milliseconds -- the number that tells
   the application's repaint interval how much a frame is worth on this
   machine.  A P4 spends a few on a scrolled frame; a 486 spends tens, and an
   interval that is right for one should not be asked of the other.  ftime()
   resolution is the same clock the driver's input waits already use. */
static unsigned long gVioFlushMs = 0;

/* Nothing outstanding: the screen holds what the shadow says, either because it
   always did or because whatever it held has just been painted over. */
static void forgetPending()
{
    if( gDirty != 0 )
        memset( gDirty, 0, shadowCells() );
    gDirtyCount = 0;
}

/* Drop the shadow, so that the next write of any cell goes out whatever it holds. */
static void forgetScreen()
{
    if( gShadowCh == 0 )
        return;
    memset( gShadowCh, 0, shadowCells() );
    memset( gShadowAttr, 0xFF, shadowCells() );
    gPrevValid = False;             // what the screen holds is now nobody's guess
    forgetPending();
}

/* A VT clear really does leave the terminal holding spaces in its default colours,
   and our model of the default attribute is kDefaultAttr, so this is not a guess:
   it is exactly what the escape we have just sent did. */
static void rememberScreenCleared()
{
    if( gShadowCh == 0 )
        return;
    memset( gShadowCh, ' ', shadowCells() );
    memset( gShadowAttr, kDefaultAttr, shadowCells() );
    if( gPrevCh != 0 )
        {
        // The screen holds exactly what the shadow now says -- so the shadow
        // of this moment is also an honest previous frame.
        memcpy( gPrevCh, gShadowCh, shadowCells() );
        memcpy( gPrevAttr, gShadowAttr, shadowCells() );
        gPrevValid = True;
        }
    forgetPending();                // the clear covers whatever was waiting
}

/* The shadow has to be as big as the screen, and the screen is not known until the
   constructor has read the environment.  Called before every write rather than at
   startup because the size can also change under setScreenMode. */
static Boolean ensureShadow()
{
    if( gShadowCh != 0 && gShadowCols == gCols && gShadowRows == gRows )
        return True;

    delete[] gShadowCh;
    delete[] gShadowAttr;
    delete[] gDirty;
    delete[] gPrevCh;
    delete[] gPrevAttr;
    gShadowCh = new uchar[(size_t) gCols * gRows];
    gShadowAttr = new uchar[(size_t) gCols * gRows];
    gDirty = new uchar[(size_t) gCols * gRows];
    gPrevCh = new uchar[(size_t) gCols * gRows];
    gPrevAttr = new uchar[(size_t) gCols * gRows];
    gShadowCols = gCols;
    gShadowRows = gRows;
    gPrevValid = False;
    if( gShadowCh == 0 || gShadowAttr == 0 || gDirty == 0
        || gPrevCh == 0 || gPrevAttr == 0 )
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

// One decoded event that the other half of the polling pair has not collected.
// getKeyEvent and getMouseEvent share one byte stream, so whichever of them gets
// a byte belonging to the other has to hold on to it: the queue polls both and
// expects each to find what it is looking for.
static Boolean gKeyPending = False;
static TEvent gPendingKey;
static Boolean gMousePending = False;
static MouseEventType gPendingMouse;

static Boolean gInterruptWait = False;

//
// Output
//

static void outStr( const char *s )
{
    fputs( s, stdout );
}

static void moveTo( ushort x, ushort y )
{
    char seq[24];
    sprintf( seq, "\x1b[%u;%uH", (unsigned) y + 1, (unsigned) x + 1 );
    outStr( seq );
    gTermX = x;
    gTermY = y;
    gTermPosKnown = True;
}

/* The legacy colour model is one BIOS attribute byte, which is eight foreground
   colours and a bright bit, eight background colours and a blink bit -- and that
   is exactly what an ANSI terminal offers, 30-37 and 90-97 over 40-47 and
   100-107.  Nothing has to be approximated, which is the reason the 16-colour
   themes exist for this target.  Bit 7 goes to the bright background rather than
   to blinking, as it does on the hardware and as the themes assume; a blinking
   transcript is not wanted in any case.  The default attribute is passed on as a
   plain reset so that the terminal shows its own colours rather than our guess
   at them. */
static void selectAttr( int attr )
{
    if( attr == gAttr )
        return;

    if( attr == kDefaultAttr )
        outStr( "\x1b[0m" );
    else
        {
        char seq[24];
        int fg = attr & 0x07;
        int bg = (attr >> 4) & 0x07;
        if( attr & 0x80 )
            bg += 8;
        sprintf( seq, "\x1b[0;%d;%dm",
                 (attr & 0x08) ? 90 + fg : 30 + fg,
                 bg >= 8 ? 100 + (bg - 8) : 40 + bg );
        outStr( seq );
        }
    gAttr = attr;
}

/* A cell is written as itself unless it is a control byte, which is never meant
   for the screen: a NUL from an untouched buffer would be a hole in the line and
   a stray 0x1B would be an escape sequence the application never drew.  High-bit
   bytes go out as they are, since what they mean is the terminal's business and
   not the driver's. */
static char printableCell( unsigned char c )
{
    if( c < 0x20 || c == 0x7F )
        return ' ';
    return (char) c;
}

//
// Input
//

/* BIOS scan codes for the characters this session hands over as bytes, on the
   US layout.  The codes worth checking are not the letters but the ones every
   view compares in full: Backspace 0x0E08, Tab 0x0F09, Enter 0x1C0D, Escape
   0x011B, and the Alt+letter pair a dialog matches its hotkeys against.  Those
   are (scan << 8 | character), so a driver that had only the character would
   leave every one of those comparisons false, and the symptom would be an input
   line that shows what was typed but does not accept Enter.  The table is
   therefore the BIOS data area's replacement, and it agrees with tkeys.h: 'm' is
   0x32 in both, Enter's scan code is 0x1C in both, and a shifted character keeps
   the scan code of the key under the shift -- '!' is 0x02 because it is 1. */
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

/* The next byte of input, waiting up to waitMs for one that has not turned up.
   kbhit() rather than select(), which does not work on this handle, and getch()
   rather than read(), because kbhit() is the RTL's own idea of what is available
   and the two have to agree about when a byte exists. */
static int nextByte( int waitMs )
{
    int waited = 0;
    while( !kbhit() )
        {
        if( waited >= waitMs )
            return -1;
        DosSleep( kPollMs );
        waited += kPollMs;
        }
    return getch();
}

static Boolean vioKeyWaiting();       // with the VIO section below
static Boolean vioMouseWaiting();

static Boolean anyInput()
{
    if( gKeyPending || gMousePending )
        return True;
    /* Two ways to ask whether anything is waiting, and they are not
       interchangeable: conio looks at a byte stream, VIO looks at the keyboard
       and mouse record queues.  Asking the wrong one is not an error -- it is a
       key that never arrives. */
    if( gVio )
        return vioKeyWaiting() || vioMouseWaiting();
    return kbhit() != 0;
}

static void fillKey( TEvent &event, uchar ch, uchar scan, ushort modifiers )
{
    event.what = evKeyDown;
    event.keyDown.charScan.charCode = ch;
    event.keyDown.charScan.scanCode = scan;
    event.keyDown.controlKeyState = modifiers;
    /* A view takes typed text from getText(), which is text and textLength
       together, so a printable character has to go there as well as into the
       key code: the Windows console path is fed that field by the modern layer,
       and this driver is the only thing between a keystroke and a classic view. */
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

// What decodeEvent() managed to make of the bytes it took.
enum
    {
    kDecodedNothing,  // The bytes ran out or meant something not understood.
    kDecodedKey,
    kDecodedMouse
    };

/* SGR mouse reporting (xterm's 1006), which is what an escape-capable terminal
   sends when cursorOn() asks for it: ESC [ < button ; column ; row M for a press,
   m for a release.  The button byte carries the motion and wheel cases and the
   modifier keys, which is the only place in this driver where a modifier is known
   rather than guessed. */
static int decodeSgrMouse( const char *params, char final, MouseEventType &mouse )
{
    int button = 3, col = 1, row = 1;
    if( sscanf( params, "%d ; %d ; %d", &button, &col, &row ) < 3 )
        return kDecodedNothing;

    ushort modifiers = 0;
    int extra = (button >> 2) & 7;
    if( extra & 1 )
        modifiers |= kbShift;
    if( extra & 2 )
        modifiers |= kbAltShift;
    if( extra & 4 )
        modifiers |= kbCtrlShift;

    uchar buttons = 0;
    switch( button & 3 )
        {
        case 0: buttons = mbLeftButton; break;
        case 1: buttons = mbMiddleButton; break;
        case 2: buttons = mbRightButton; break;
        default: buttons = 0; break;   // 3 means no button is held
        }

    int wheel = 0;
    ushort eventFlags = 0;
    if( button & 64 )
        {
        wheel = (button & 1) ? mwDown : mwUp;
        eventFlags = MOUSE_WHEELED;
        buttons = 0;
        }
    else if( (button & 32) != 0 )
        eventFlags = MOUSE_MOVED;
    else if( final == 'm' )
        buttons = 0;                  // a release: the button byte is the one pressed

    fillMouse( mouse, col - 1, row - 1, buttons, eventFlags, (uchar) wheel, modifiers );
    return kDecodedMouse;
}

/* A CSI or SS3 sequence, the introducer already taken.  Parameters are collected
   up to the final byte, each continuation waited for; what is left is matched
   against the keys a terminal sends and translated to BIOS scan codes, because
   scan codes are what tkeys.h and every view in the library are written against. */
static int decodeCsi( int introducer, TEvent &event, MouseEventType &mouse )
{
    char params[kSeqMax];
    int count = 0;
    int final = -1;

    for( ;; )
        {
        int b = nextByte( kGapWaitMs );
        if( b < 0 )
            break;
        if( b >= 0x40 && b <= 0x7E )
            {
            final = b;
            break;
            }
        // Parameter and intermediate bytes: digits, ';', '?', '<'.
        if( count < kSeqMax - 1 && b >= 0x20 && b <= 0x3F )
            params[count++] = (char) b;
        }
    params[count] = '\0';

    if( introducer == 'O' )
        {
        switch( final )
            {
            case 'A': fillKey( event, 0, 0x48, 0 ); return kDecodedKey;
            case 'B': fillKey( event, 0, 0x50, 0 ); return kDecodedKey;
            case 'C': fillKey( event, 0, 0x4D, 0 ); return kDecodedKey;
            case 'D': fillKey( event, 0, 0x4B, 0 ); return kDecodedKey;
            case 'P': fillKey( event, 0, 0x3B, 0 ); return kDecodedKey;   // F1
            case 'Q': fillKey( event, 0, 0x3C, 0 ); return kDecodedKey;   // F2
            case 'R': fillKey( event, 0, 0x3D, 0 ); return kDecodedKey;   // F3
            case 'S': fillKey( event, 0, 0x3E, 0 ); return kDecodedKey;   // F4
            default: return kDecodedNothing;
            }
        }

    if( final == 'M' || final == 'm' )
        {
        if( params[0] == '<' )
            return decodeSgrMouse( params + 1, (char) final, mouse );
        // The X10 form, ESC [ Cb Cx Cy, carries the three numbers as bytes offset
        // by 32, so they have already been collected as "parameters" -- provided
        // they stayed below 0x40, which means this reading only reaches column and
        // row 31.  It is kept for a terminal that ignores the request for SGR
        // coordinates; one that answers it never comes here.
        if( count >= 3 )
            {
            int button = (unsigned char) params[0] - 32;
            int col = (unsigned char) params[1] - 32;
            int row = (unsigned char) params[2] - 32;
            char sgr[24];
            sprintf( sgr, "%d ; %d ; %d", button, col, row );
            return decodeSgrMouse( sgr, (char) final, mouse );
            }
        return kDecodedNothing;
        }

    // A parameter after the separator is a modifier: "1" for none, then shift,
    // alt and ctrl added on, which is the only place a key press from a terminal
    // says what was held.
    ushort modifiers = 0;
    const char *semi = strchr( params, ';' );
    if( semi != NULL )
        {
        int held = atoi( semi + 1 ) - 1;
        if( held > 0 )
            {
            if( held & 1 )
                modifiers |= kbShift;
            if( held & 2 )
                modifiers |= kbAltShift;
            if( held & 4 )
                modifiers |= kbCtrlShift;
            }
        }

    uchar scan = 0;
    switch( final )
        {
        case 'A': scan = 0x48; break;   // up
        case 'B': scan = 0x50; break;   // down
        case 'C': scan = 0x4D; break;   // right
        case 'D': scan = 0x4B; break;   // left
        case 'H': scan = 0x47; break;   // home
        case 'F': scan = 0x4F; break;   // end
        case 'Z':                        // shift-tab, which tkeys.h writes as
            fillKey( event, 0, 0x0F, kbShift );   // 0x0F00: the scan code, no
            return kDecodedKey;                   // character, and the shift flag
        case '~':
            switch( atoi( params ) )
                {
                case 1: case 7:  scan = 0x47; break;     // home
                case 2:          scan = 0x52; break;     // insert
                case 3:          scan = 0x53; break;     // delete
                case 4: case 8:  scan = 0x4F; break;     // end
                case 5:          scan = 0x49; break;     // prior
                case 6:          scan = 0x51; break;     // next
                // F1 to F10, then F11 and F12, whose BIOS scan codes are the two
                // entries the conversion tables in hardwrvr.cpp still carry.
                case 11: scan = 0x3B; break;
                case 12: scan = 0x3C; break;
                case 13: scan = 0x3D; break;
                case 14: scan = 0x3E; break;
                case 15: scan = 0x3F; break;
                case 17: scan = 0x40; break;
                case 18: scan = 0x41; break;
                case 19: scan = 0x42; break;
                case 20: scan = 0x43; break;
                case 21: scan = 0x44; break;
                case 23: scan = 0x85; break;
                case 24: scan = 0x86; break;
                default: return kDecodedNothing;
                }
            break;
        default:
            return kDecodedNothing;
        }

    fillKey( event, 0, scan, modifiers );
    return kDecodedKey;
}

/* One keystroke, from the byte that starts it: a DOS extended code and its scan
   code, an escape sequence, or a character.  A prefix that has no tail is not
   reported as anything, and neither is a sequence that means nothing here: a
   keystroke the driver does not understand is better dropped than delivered as
   something the view will act on. */
static int decodeEvent( TEvent &event, MouseEventType &mouse )
{
    int c = nextByte( 0 );
    if( c < 0 )
        return kDecodedNothing;

    if( c == 0x00 || c == 0xE0 )
        {
        int scan = nextByte( kGapWaitMs );
        if( scan < 0 )
            return kDecodedNothing;
        fillKey( event, 0, (uchar) scan, 0 );
        return kDecodedKey;
        }

    if( c == 0x1B )
        {
        int next = nextByte( kGapWaitMs );
        if( next < 0 )
            {
            fillKey( event, 0x1B, 0x01, 0 );        // Escape on its own
            return kDecodedKey;
            }
        if( next == '[' || next == 'O' )
            return decodeCsi( next, event, mouse );
        if( next == 0x1B )
            return kDecodedNothing;                 // Escape Escape
        // Meta: Alt held while a key was pressed arrives as the key after an
        // escape.  The BIOS reports those with no character, which is how
        // kbAltA and the rest of them are written in tkeys.h.
        fillKey( event, 0, scanForChar( next ), kbAltShift );
        return kDecodedKey;
        }

    ushort modifiers = 0;
    if( c == 0x7F )
        c = 0x08;                                   // terminals send DEL for backspace
    if( c == '\n' )
        c = '\r';                                   // and some send LF for Enter
    if( c >= 0x01 && c <= 0x1A && c != '\t' && c != '\r' )
        modifiers = kbCtrlShift;                    // Ctrl and a letter
    fillKey( event, (uchar) c, scanForChar( c ), modifiers );
    return kDecodedKey;
}

/*
 * The VIO back end: the screen this session owns.
 *
 * Session handle 0.  Not 1: that is a word from the 16-bit guarded-segment era
 * rather than a value to pass, and every Vio* call answers a bad handle with 436
 * (ERROR_VIO_INVALID_HANDLE), which is what this driver was written away from
 * once.  KbdOpen(0) and MouOpen(0) succeeding in the same probe run as those 436s
 * is the shape of the mistake: a process with no session does not get a keyboard.
 *
 * These entry points never left the 16-bit interface -- APIRET16 APIENTRY16 --
 * and it shows in two ways worth knowing before writing against them: the sizes
 * are USHORT where a 32-bit API would use PULONG (VioGetBuf, VioReadCellStr,
 * VioGetAnsi), and the write call is VioWrtCellStr, keeping the name OS/2 1.x gave
 * it.  VioReadCellStr's count goes in as the bytes asked for and comes back as the
 * bytes read; VioWrtCellStr's is the bytes of the string, a cell being a character
 * and an attribute byte -- the same byte the rest of tvision carries, which is why
 * no colour mapping is needed here.
 *
 * And the geometry: VIOMODEINFO.row and .col are the count of rows and columns,
 * not the index of the last one.  Asking VioReadCellStr for row 25 of a 25-row
 * screen answers 358 (ERROR_VIO_ROW), which is the error this driver used to
 * produce before it had read the struct properly.
 */

static const ushort kVioSession = 0;

// gVio itself is declared above with the screen state: the input helpers are
// higher up in this file than the VIO code, and they have to ask it too.
static uchar *gVioRow = 0;                // one row of cells, interleaved
static ushort gVioRowCols = 0;

// The screen as it was found, to put back on the way out.  A stream driver asks
// the terminal for an alternate screen; VIO has no such thing and needs none,
// because the screen can be read.
static uchar *gVioSaved = 0;
static ushort gVioSavedRows = 0;
static ushort gVioSavedCols = 0;
static ushort gVioSavedCurRow = 0;
static ushort gVioSavedCurCol = 0;
static VIOCURSORINFO gVioSavedCurType;
static Boolean gVioSavedCurTypeOk = False;
static Boolean gVioScreenSaved = False;

/* The staging row, as wide as the screen.  Called before each write rather than
   once at startup because setScreenMode can change the width. */
static Boolean vioEnsureRow()
{
    if( gVioRow != 0 && gVioRowCols == gCols )
        return True;
    delete[] gVioRow;
    gVioRow = new uchar[(size_t) gCols * 2];
    gVioRowCols = 0;
    if( gVioRow == 0 )
        return False;
    gVioRowCols = gCols;
    return True;
}

/* Ask the session how big its screen is.  False means either that there is no
   VIO screen -- which is what a session started without one answers -- or that it
   is too small to lay the application out in, and in both cases the escape driver
   is the better of two bad options. */
static Boolean vioOpen()
{
    VIOMODEINFO mode;

    memset( &mode, 0, sizeof( mode ) );
    mode.cb = sizeof( mode );
    if( VioGetMode( &mode, kVioSession ) != NO_ERROR )
        return False;
    if( mode.row < 2 || mode.col < 20 )
        return False;

    gRows = mode.row;                 // counts, not last index: see above
    gCols = mode.col;
    return vioEnsureRow();
}

static void vioClearScreen()
{
    /* VioWrtNCell would be the obvious call, and its count is documented in
       cells where VioWrtCellStr's is in bytes; a staged row of blanks costs a
       loop this one cannot be misread in. */
    if( !vioEnsureRow() )
        return;

    for( ushort i = 0; i < gCols; ++i )
        {
        gVioRow[ i * 2 ]     = ' ';
        gVioRow[ i * 2 + 1 ] = kDefaultAttr;
        }
    for( ushort y = 0; y < gRows; ++y )
        VioWrtCellStr( (PCH) gVioRow, (USHORT) ( gCols * 2 ), y, 0, kVioSession );
}

/* Take the screen in, once.  Called from setUpConsole, which is called more than
   once if the application ever suspends itself, and only the first call counts:
   a second would save what the program had drawn. */
static void vioSaveScreen()
{
    if( gVioScreenSaved || gVioSaved != 0 )
        return;

    gVioSaved = new uchar[(size_t) gRows * gCols * 2];
    if( gVioSaved == 0 )
        return;

    for( ushort y = 0; y < gRows; ++y )
        {
        USHORT count = (USHORT) ( gCols * 2 );
        if( VioReadCellStr( (PCH) ( gVioSaved + (size_t) y * gCols * 2 ), &count,
                            y, 0, kVioSession ) != NO_ERROR )
            {
            delete[] gVioSaved;
            gVioSaved = 0;
            return;                 // no saved screen: exit leaves the session as
                                    // the program left it, which is worse than
                                    // restoring and the only honest fallback
            }
        }

    if( VioGetCurPos( &gVioSavedCurRow, &gVioSavedCurCol, kVioSession )
        != NO_ERROR )
        {
        gVioSavedCurRow = 0;
        gVioSavedCurCol = 0;
        }
    memset( &gVioSavedCurType, 0, sizeof( gVioSavedCurType ) );
    gVioSavedCurTypeOk =
        ( VioGetCurType( &gVioSavedCurType, kVioSession ) == NO_ERROR );

    gVioSavedRows = gRows;
    gVioSavedCols = gCols;
    gVioScreenSaved = True;
}

static void vioRestoreScreen()
{
    if( gVioSaved == 0 )
        return;

    for( ushort y = 0; y < gVioSavedRows; ++y )
        VioWrtCellStr( (PCH) ( gVioSaved + (size_t) y * gVioSavedCols * 2 ),
                       (USHORT) ( gVioSavedCols * 2 ), y, 0, kVioSession );
    VioSetCurPos( gVioSavedCurRow, gVioSavedCurCol, kVioSession );
    if( gVioSavedCurTypeOk )
        VioSetCurType( &gVioSavedCurType, kVioSession );

    delete[] gVioSaved;
    gVioSaved = 0;
    gVioSavedRows = 0;
    gVioSavedCols = 0;
    gVioScreenSaved = False;
}

/* The cursor's shape as the session had it, shown or hidden.  This is the 16-bit
   cursor struct, which has no visibility flag: an attr of 0xFFFF is an invisible
   cursor, as it is in Open Watcom's own OS/2 code, and any other value both shows
   the cursor and says what it is drawn with. */
static void vioShowCaret( Boolean shown )
{
    VIOCURSORINFO shape;

    memset( &shape, 0, sizeof( shape ) );
    if( gVioSavedCurTypeOk )
        shape = gVioSavedCurType;
    else
        {
        shape.yStart = 14;            // the shape this session was measured with:
        shape.cEnd = 15;              // an underscore one scanline high
        shape.cx = 1;
        shape.attr = 1;
        }
    if( shown )
        {
        if( shape.attr == 0xFFFF )
            shape.attr = 1;
        }
    else
        shape.attr = 0xFFFF;
    VioSetCurType( &shape, kVioSession );
}

//
// Input under VIO: one record per key, with the modifiers in it.
//

/*
 * Asked directly, the keyboard answers with the character, the scan code and the
 * state of the modifier keys as one record.  conio answers with a byte, which is
 * why the stream path carries a table of the scan code a character probably came
 * from, waits kGapWaitMs for the tail of a key whose two halves have been seen
 * 390 ms apart, and cannot tell Alt-F from a control character -- there is no
 * byte that says Alt.  Both matter most in the menu bar, which is written in
 * exactly those two things: kbAltF to open a menu, kbDownArrow to move through
 * it.  A menu is also where a lost or duplicated key is most visible, because
 * there is a highlighted item on the screen showing which one the application
 * thinks is selected.
 */

/* Is a key waiting?  KbdPeek answers NO_ERROR whether or not one is, so the
   answer is in fbStatus's top three bits -- which is the test Open Watcom's own
   OS/2 port uses (bld/vi/os2/biosos2.c), and the one worth trusting over a
   reading of the documentation of the day. */
static Boolean vioKeyWaiting()
{
    KBDKEYINFO probe;

    memset( &probe, 0, sizeof( probe ) );
    if( KbdPeek( &probe, kVioSession ) != NO_ERROR )
        return False;
    return ( probe.fbStatus & 0xE0 ) != 0;
}

/* The next key, which the caller has just seen waiting, so this does not block.
   The second argument is fWait and OS/2's 16-bit API spells true 0. */
static BOOL vioTakeKey( TEvent &event )
{
    KBDKEYINFO k;

    memset( &k, 0, sizeof( k ) );
    if( KbdCharIn( &k, 0, kVioSession ) != NO_ERROR )
        return False;

    ushort modifiers = 0;
    if( k.fsState & KBDSTF_ALT )
        modifiers |= kbAltShift;
    if( k.fsState & KBDSTF_CONTROL )
        modifiers |= kbCtrlShift;
    if( k.fsState & ( KBDSTF_LEFTSHIFT | KBDSTF_RIGHTSHIFT ) )
        /* Mapped bit by bit rather than assigned: OS/2 spells the modifier state
           with KBDSTF_* (shift 0x0001/0x0002, ctrl 0x0004, alt 0x0008) while
           tvision's controlKeyState uses the Windows console values (ctrl 0x0008,
           alt 0x0002, shift 0x0010), and copying one into the other turns Alt
           into Shift. */
        modifiers |= kbShift;

    /* An extended key -- arrows, Home, Insert, the cursor pad -- arrives with
       chChar holding 0xE0 as a marker and the real scan code beside it.  What
       every view compares against is the BIOS pair with no character in it
       (Up is 0x4800, not 0x48E0), so the marker is dropped rather than passed on
       as a character. */
    uchar ch = k.chChar;
    if( ch == 0xE0 )
        ch = 0;
    else if( ch == 0x7F )
        {
        /* A client in front of the session sends DEL for Backspace and the
           session can hand that straight through.  A keyboard at this machine
           answers 0x08/0x0E, so this is only reached over a cable, and it is the
           same correction the stream path makes: what the user meant is the key
           with the left-pointing arrow on it. */
        ch = 0x08;
        if( k.chScan == 0 )
            fillKey( event, ch, 0x0E, modifiers );
        else
            fillKey( event, ch, k.chScan, modifiers );
        return True;
        }

    fillKey( event, ch, k.chScan, modifiers );
    return True;
}

/*
 * The machine's own mouse.
 *
 * The queue says what the mouse did, and one thing is missing from what it says:
 * releasing a button is not an event.  A release arrives as a plain motion with
 * no button bit set, which is why Open Watcom's own OS/2 UI reads fs & 0x06 as
 * "button 1 is down" and never looks for the up (bld/ui/os2/c/uimous.c).  That
 * costs nothing here: the layer above this driver turns a stream of (position,
 * buttons held) into press, release and drag by comparing each event with the
 * last one (TEventQueue does it, and it is what makes a click out of the two),
 * so the driver reports what the mouse is doing now and lets that do the rest.
 */
static HMOU gMou = 0;
static Boolean gMouUsable = False;
static ushort gMouButtons = 0;            // MouGetNumButtons, for the BN2 question

/* Open the mouse and ask which events to queue.  Silence is the right answer to
   a failure: a box with no mouse driver in CONFIG.SYS is an ordinary place to run
   this program, and there it should work with a keyboard -- which is what it
   will do, because gMouUsable is what every other mouse entry point checks. */
static void vioOpenMouse()
{
    USHORT mask = MOUSE_MOTION | MOUSE_MOTION_WITH_BN1_DOWN | MOUSE_BN1_DOWN
                | MOUSE_MOTION_WITH_BN2_DOWN | MOUSE_BN2_DOWN
                | MOUSE_MOTION_WITH_BN3_DOWN | MOUSE_BN3_DOWN;

    if( gMouUsable )
        return;             // getButtonCount and setUpConsole both ask
    if( MouOpen( (PSZ) 0, &gMou ) != NO_ERROR )
        return;
    if( MouSetEventMask( &mask, gMou ) != NO_ERROR )
        {
        MouClose( gMou );
        gMou = 0;
        return;
        }
    USHORT buttons = 0;
    if( MouGetNumButtons( &buttons, gMou ) == NO_ERROR )
        gMouButtons = buttons;
    gMouUsable = True;
}

static void vioCloseMouse()
{
    if( gMouUsable )
        MouClose( gMou );
    gMou = 0;
    gMouUsable = False;
    gMouButtons = 0;
}

static Boolean vioMouseWaiting()
{
    MOUQUEINFO q;

    if( !gMouUsable )
        return False;
    if( MouGetNumQueEl( &q, gMou ) != NO_ERROR )
        return False;
    return q.cEvents != 0;
}

/* One event out of the queue.  MOU_NOWAIT because the caller has just been told
   one is there, and a driver that blocks inside a poll is a frozen interface. */
static BOOL vioTakeMouse( MouseEventType &mouse )
{
    MOUEVENTINFO ev;
    USHORT wait = MOU_NOWAIT;

    if( MouReadEventQue( &ev, &wait, gMou ) != NO_ERROR )
        {
        /* The caller asked just now whether one was there.  If the queue says yes
           and the read says no, the two calls disagree and every poll will ask
           again -- which is a busy loop with the interface switched off, the
           worst of both.  Retire the mouse rather than spin on it. */
        gMouUsable = False;
        return False;
        }

    uchar buttons = 0;
    if( ev.fs & ( MOUSE_BN1_DOWN | MOUSE_MOTION_WITH_BN1_DOWN ) )
        buttons |= mbLeftButton;
    /* What BN2 means depends on the mouse: a two-button mouse has no middle, so
       its BN2 is the button the application calls right, and a three-button one
       calls BN2 middle and BN3 right.  Open Watcom's UI makes the same
       distinction for the same reason. */
    if( ev.fs & ( MOUSE_BN2_DOWN | MOUSE_MOTION_WITH_BN2_DOWN ) )
        buttons |= ( gMouButtons == 2 ? mbRightButton : mbMiddleButton );
    if( ev.fs & ( MOUSE_BN3_DOWN | MOUSE_MOTION_WITH_BN3_DOWN ) )
        buttons |= ( gMouButtons == 2 ? mbMiddleButton : mbRightButton );

    ushort flags = 0;
    if( ev.fs & ( MOUSE_MOTION | MOUSE_MOTION_WITH_BN1_DOWN
                  | MOUSE_MOTION_WITH_BN2_DOWN | MOUSE_MOTION_WITH_BN3_DOWN ) )
        flags = MOUSE_MOVED;

    /* Row and column are the session's, zero-based, which is what tvision's
       mouse events are too -- no scaling and no clamping, because both are
       counting the same cells. */
    fillMouse( mouse, (int) ev.col, (int) ev.row, buttons, flags, 0, 0 );
    return True;
}

//
// Construction
//

THardwareInfo::THardwareInfo() noexcept
{
    // Not plDPMI32, which is what the member is initialised to and which the
    // classic code reads as "a DOS machine I may poke".
    platform = plOS2;

    /* Ask for the screen.  If the session has one this is the display, and it is
       also the only source of the size: VioGetMode says how many rows and columns
       there are, which is what the application then lays out in.  ATUI_OS2_STREAM
       declines it -- for a client that cannot render VIO, since a session renders
       its screen to a client that has said ANSI and not to one that has said
       pcansi or nothing at all. */
    if( getenv( "ATUI_OS2_STREAM" ) == NULL )
        gVio = vioOpen();

    /* Without a VIO screen the size has to be stated, because a stream cannot be
     * asked: the terminal is at the other end of a pipe as far as this process is
     * concerned.  With one, these two variables would only make the application
     * lie about the screen it is being given.
     *
     * They are still read when VIO is not in use, which is what keeps an escape
     * session in a window of any size usable. */
    if( !gVio )
        {
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
        }

    sbInfo.dwSize.X = gCols;
    sbInfo.dwSize.Y = gRows;
    crInfo.dwSize = 1;
    crInfo.bVisible = True;

    consoleHandle[cnInput] = (HANDLE) 0;
    consoleHandle[cnOutput] = (HANDLE) 1;
}

THardwareInfo::~THardwareInfo()
{
    restoreConsole();
    delete[] gVioRow;
    gVioRow = 0;
    gVioRowCols = 0;
}

//
// Caret functions.
//

ushort THardwareInfo::getCaretSize() noexcept
{
    return (ushort) crInfo.dwSize;
}

/* The header inlines this one for Borland only, so an old-compiler build needs
   the answer given somewhere: it is the same remembered state, because nothing
   else in a stream session could know what the terminal's cursor is doing. */
BOOL THardwareInfo::isCaretVisible() noexcept
{
    return crInfo.bVisible;
}

void THardwareInfo::setCaretSize( ushort size ) noexcept
{
    crInfo.dwSize = size;
    crInfo.bVisible = (size != 0) ? True : False;
    // A size of 0 is the classic driver's way of saying "hide it", matching the
    // Windows console's own treatment of a zero-height cursor.
    //
    // Only sent when it changes: tvision places the caret on every pass over the
    // screen, and this is called each time, so an unconditional escape is three bytes
    // per pass for a terminal that is already in the state being asked for.
    const int shown = (size != 0) ? 1 : 0;
    if( gCaretShown == shown )
        return;
    gCaretShown = shown;
    if( gVio )
        {
        vioShowCaret( shown ? True : False );
        return;
        }
    outStr( shown ? "\x1b[?25h" : "\x1b[?25l" );
}

void THardwareInfo::setCaretPosition( ushort x, ushort y ) noexcept
{
    gCaretX = x;
    gCaretY = y;
    sbInfo.dwCursorPosition.X = (SHORT) x;
    sbInfo.dwCursorPosition.Y = (SHORT) y;
    if( gVio )
        {
        /* The session's cursor, moved to where the application says its caret
           goes.  Note that this one is not worth skipping: writing cells does not
           move the cursor along, as it does on a stream, so there is no state in
           which it is already right and the call costs a trap to the kernel
           instead of six bytes on a wire. */
        if( gTermX != x || gTermY != y || !gTermPosKnown )
            {
            VioSetCurPos( y, x, kVioSession );
            gTermX = x;
            gTermY = y;
            gTermPosKnown = True;
            }
        return;
        }
    // The same argument as above, in cursor addresses: a caret that has not moved
    // does not need to be told where it is, and after any write to the screen the
    // terminal's cursor is wherever that write left it (so this has to say so).
    if( !gTermPosKnown || gTermX != x || gTermY != y )
        moveTo( x, y );
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

void THardwareInfo::setScreenMode( ushort mode ) noexcept
{
    if( gVio )
        {
        /* The session's mode is not ours to change: VioSetMode would put the
           machine's display into another video mode, which is not what a
           TDisplay::setMode() call means to do to a user, and a session cannot be
           resized under a running program anyway.  So the mode is asked for again
           -- it can differ from what was read at construction if a shell changed
           it before starting us -- and whatever it is stays. */
        ushort wasRows = gRows;
        ushort wasCols = gCols;
        if( !vioOpen() )
            {
            gRows = wasRows;            // a failed re-ask must not shrink the
            gCols = wasCols;            // application's idea of its own screen
            return;
            }
        sbInfo.dwSize.X = gCols;
        sbInfo.dwSize.Y = gRows;
        if( gRows != wasRows || gCols != wasCols )
            {
            ensureShadow();
            forgetScreen();
            }
        return;
        }

    gRows = (mode & TDisplay::smFont8x8) ? 50 : 25;
    sbInfo.dwSize.X = gCols;
    sbInfo.dwSize.Y = gRows;

    // xterm's "resize window" request.  A terminal that does not implement it
    // ignores the escape, which is the same outcome as not asking; the size we
    // hold is what the application gets either way.
    char seq[24];
    sprintf( seq, "\x1b[8;%u;%ut", (unsigned) gRows, (unsigned) gCols );
    outStr( seq );

    gAttr = -1;
    // A resized terminal is a screen whose contents nobody can predict -- what is
    // kept, what is cleared and what scrolls is the emulator's choice -- so nothing
    // may be skipped on the strength of what was there before.
    ensureShadow();
    forgetScreen();
    flushScreen();
}

void THardwareInfo::clearScreen( ushort, ushort ) noexcept
{
    if( gVio )
        {
        vioClearScreen();
        rememberScreenCleared();
        return;
        }
    // A VT clear is the whole screen, so the extent asked for is not used.
    outStr( "\x1b[0m\x1b[2J\x1b[H" );
    gAttr = -1;
    gTermX = 0;
    gTermY = 0;
    gTermPosKnown = True;         // the \x1b[H above is what makes that true
    rememberScreenCleared();
}

/* One row of a frame against one row of another: characters and colours. */
static Boolean framesRowMatch( const uchar *chA, const uchar *atA, size_t rowA,
                               const uchar *chB, const uchar *atB, size_t rowB )
{
    const size_t a = rowA * gShadowCols, b = rowB * gShadowCols;
    return memcmp( chA + a, chB + b, gShadowCols ) == 0
        && memcmp( atA + a, atB + b, gShadowCols ) == 0;
}

static void clearRowDirty( ushort y )
{
    size_t at = (size_t) y * gShadowCols, end = at + gShadowCols;
    for( ; at != end; ++at )
        if( gDirty[at] ) { gDirty[at] = 0; --gDirtyCount; }
}

static void markRowDirty( ushort y )
{
    size_t at = (size_t) y * gShadowCols, end = at + gShadowCols;
    for( ; at != end; ++at )
        if( !gDirty[at] ) { gDirty[at] = 1; ++gDirtyCount; }
}

/* The frame shape every scrolled transcript makes: the screen as it was,
   shifted up by k rows, with the new text at the bottom of the scrolled
   region.  Written run by run that is one VioWrtCellStr per row of the
   region; asked of the hardware it is one VioScrollUp and the k new rows --
   and on the local screen the call itself, not the bytes, is what costs: the
   session mirrors every write (local2.trc: the kernel display path was 43% of
   the UI thread while an answer scrolled, with the application itself under
   2%; see TESTING.md).

   The rule is deliberately narrow.  From the first dirty row down, every row
   of the chosen window must equal the previous frame's row k below it -- that
   is what the scroll will deliver -- and the k rows the scroll leaves exposed
   are marked dirty in full, so the run loop below rewrites every one of them
   from the shadow.  Changes that are not a shift sit outside the window or
   break the match, and the frame goes out the ordinary way.  A shift that
   matches on blank rows and costs a few blank writes is wrong about nothing:
   the screen ends with the shadow either way. */
static Boolean rowIsDirty( ushort y )
{
    const size_t at = (size_t) y * gShadowCols;
    for( ushort x = 0; x < gShadowCols; ++x )
        if( gDirty[ at + x ] )
            return True;
    return False;
}

/* Can this frame be delivered as one scroll up from `first`?  The window ends
   where the matching does, so whatever else the frame changed below it sits
   outside and goes out as written rows.  The few blank rows a greedy match
   folds in at the bottom cost a scroll and k blank writes and are wrong about
   nothing: the screen ends holding the shadow either way. */
static Boolean vioScrollFrom( ushort first )
{
    for( ushort k = 1; k <= 8; ++k )
        {
        /* Rows [first .. first+len-1] shift; rows [first+len .. first+len+k-1]
           are what the scroll exposes.  Stopping the shift at `limit` keeps
           every shifted row inside the previous frame. */
        const ushort limit = (ushort) ( gShadowRows - k );
        ushort len = 0;
        while( first + len < limit
               && framesRowMatch( gShadowCh, gShadowAttr, first + len,
                                  gPrevCh, gPrevAttr, first + len + k ) )
            ++len;
        if( len < 3 )
            continue;

        const ushort bottom = (ushort) ( first + len + k - 1 );
        //  bsesub.h: VioScrollUp( top, left, bottom, right, lines, pAttrFill,
        //          session ) -- the exposed rows take no fill attribute, the
        //          flush writes every cell of them from the shadow right after.
        if( VioScrollUp( first, 0, bottom, (ushort) ( gShadowCols - 1 ),
                         k, 0, kVioSession ) != NO_ERROR )
            return False;           // no scroll, and so no writes either way
        ++gVioScrolls;
        for( ushort y = first; y < first + len; ++y )
            clearRowDirty( y );     // delivered by the scroll
        for( ushort y = (ushort) ( first + len ); y <= bottom; ++y )
            markRowDirty( y );      // exposed: every cell goes out again
        return True;
        }
    return False;
}

static void vioTryScroll()
{
    if( !gPrevValid || gPrevCh == 0 || gDirtyCount < 3u * gShadowCols )
        return;

    /* Candidate window tops, the first few dirty rows: a frame that redraws
       one changed line above the scrolled region -- a heading, a moved clock
       -- makes its own row the first dirty one, and the shift starts below it.
       Fewer than half a dozen starts covers the shapes that occur, and every
       start the search gives up on costs nothing but the comparisons. */
    for( ushort y = 0, tried = 0; y < gShadowRows && tried < 4; ++y )
        {
        if( !rowIsDirty( y ) )
            continue;
        ++tried;
        if( vioScrollFrom( y ) )
            return;
        }
}

/* Send what has been drawn and not yet sent, as few times as the screen will take.
   This is the only place either back end writes cells, which is what makes a
   repaint cost what changed rather than what was drawn -- and what makes a repaint
   that changed nothing cost nothing at all, the idle screen sitting at 0 bytes a
   second with the application redrawing itself twenty times a second.

   The runs are cut out of the shadow, not out of anything the caller handed over:
   the shadow holds the *last* word for each cell, and a cell written twice in one
   frame is sent once, with the value it ended on.  That is the whole of the
   anti-flicker argument -- the fill with background that every view's draw() starts
   with never reaches the screen unless it is still there when the frame ends. */
static void flushCells()
{
    if( gDirty == 0 || gDirtyCount == 0 )
        return;

    if( gShadowCols != gCols || gShadowRows != gRows )
        {
        // A screen that changed size under a frame that was drawn for the old one.
        // setScreenMode drops the shadow for the same reason: nobody knows what the
        // new screen holds, so nothing may be written on the strength of that.
        forgetPending();
        gPrevValid = False;         // and the frame beside it is a stranger now
        return;
        }

    /* Shift-shaped frames go to the hardware as one scroll before the run
       loop gets them (see vioTryScroll; VIO only -- a stream has no scroll
       to ask for, and the wire is the cheap one). */
    struct timeb t0;                // what this frame costs, for the interval
    t0.time = 0;
    t0.millitm = 0;
    if( gVio )
        {
        ftime( &t0 );
        vioTryScroll();             // shift-shaped frames: one scroll, few rows
        }

    for( ushort y = 0; y < gShadowRows; ++y )
        {
        const size_t row = (size_t) y * gShadowCols;
        ushort x = 0;
        while( x < gShadowCols )
            {
            if( !gDirty[ row + x ] )
                {
                ++x;
                continue;
                }
            const ushort first = x;
            while( x < gShadowCols && gDirty[ row + x ] )
                ++x;
            const ushort len = (ushort) ( x - first );

            if( gVio )
                {
                if( vioEnsureRow() )
                    {
                    for( ushort v = 0; v < len; ++v )
                        {
                        gVioRow[ v * 2 ]     = gShadowCh[ row + first + v ];
                        gVioRow[ v * 2 + 1 ] = gShadowAttr[ row + first + v ];
                        }
                    VioWrtCellStr( (PCH) gVioRow, (USHORT) ( len * 2 ), y, first,
                                   kVioSession );
                    ++gVioCalls;
                    }
                continue;
                }

            /* A stream: one address for the run, then one escape per change of
               colour.  A run across a row is normally one colour throughout, so the
               inner loop is normally entered once. */
            moveTo( first, y );
            ushort v = first;
            while( v < x )
                {
                const uchar attr = gShadowAttr[ row + v ];
                selectAttr( attr );

                char text[ 128 ];
                size_t n = 0;
                while( v < x && gShadowAttr[ row + v ] == attr
                       && n < sizeof( text ) - 1 )
                    text[ n++ ] = printableCell( gShadowCh[ row + v ] );
                text[ n ] = '\0';
                outStr( text );
                v += (ushort) n;
                }
            // The terminal's cursor is after what was just written, not where it was
            // addressed to, and only the next moveTo can claim to know where it is.
            gTermPosKnown = False;
            }
        }

    forgetPending();

    // The screen now holds the shadow, so the shadow is the frame the next
    // flush will compare against for the scroll shape (vioTryScroll).
    if( gPrevCh != 0 )
        {
        memcpy( gPrevCh, gShadowCh, shadowCells() );
        memcpy( gPrevAttr, gShadowAttr, shadowCells() );
        gPrevValid = True;
        }

    if( gVio )
        {
        struct timeb t1;
        ftime( &t1 );
        gVioFlushMs = (unsigned long)
            ( ( (long) t1.time - (long) t0.time ) * 1000L
              + (long) t1.millitm - (long) t0.millitm );
        }
}

void THardwareInfo::flushScreen() noexcept
{
    if( gVio )
        {
        /* Cell writes go to the screen in runs at flush time and leave the cursor
           where it was, so there is nothing else to put back -- the pending-wrap
           problem this call exists for on a stream is a thing a screen buffer does
           not have.  stdout is flushed because the application may have written a
           line of its own to it, and that does need moving. */
        flushCells();
        fflush( stdout );
        return;
        }
    flushCells();
    // The writes have moved the terminal's cursor along from wherever the application
    // put it, and it stays there; putting it back is what makes the difference between a
    // caret and a stray block.  Which is also why it can be skipped: nothing has been
    // written since the last time the caret was addressed, the flag says, then the
    // terminal is already showing it and 8 bytes on the wire buy nothing.
    if( !gTermPosKnown || gTermX != gCaretX || gTermY != gCaretY )
        moveTo( gCaretX, gCaretY );
    fflush( stdout );
}

/* Write the whole screen again.
 *
   Everything else in this file sends what changed; this sends what the application
   drew whether or not it changed.  It is for the moments where the two are allowed
   to have come apart -- a box drawn over the desktop and taken away again, a
   session that redrew itself behind us, a program that used the screen between two
   of our writes.  The shadow is the application's own picture, so dirtying every
   cell and flushing puts that picture back; it is a repair, not a clear, and the
   difference matters because a clear shows a blank screen where the application
   has already drawn.
 *
   One full screen of cells goes out -- about 2 KB of cells on an 80x25 session,
   more on a big one -- which is why this is called at a dialog's two transitions
   and not from the frame loop. */
void THardwareInfo::redrawScreen() noexcept
{
    if( gDirty == 0 || gShadowCols != gCols || gShadowRows != gRows )
        {
        // Either there is no shadow yet or it is not the screen's size, in which
        // case its cells are not anyone's picture of anything and writing them
        // would put garbage on the screen.  flushScreen() refuses a mismatched
        // shadow for the same reason.
        return;
        }
    memset( gDirty, 1, shadowCells() );
    gDirtyCount = shadowCells();
    flushCells();
    fflush( stdout );
}

/* One row of cells, at the position the classic writer computed for them.  The run
   always stays inside one row, so one dirty range is enough.

   Nothing is sent from here.  The cells are copied into the shadow, which is what
   marks them as owing a write, and flushScreen() puts them on the screen -- once per
   turn of the event loop, just before that loop blocks for input.  A view draws
   itself by filling its rows with background and writing the text over the fill, and
   a screen told both shows the blank in between: that was the response area flashing
   once for every character of an answer.  See the dirty map above, and
   source/platform/hardware.cpp, which defers for the same reason. */
void THardwareInfo::screenWrite( ushort x, ushort y, CHAR_INFO *buf, DWORD len ) noexcept
{
    if( len == 0 )
        return;
    if( !ensureShadow() )
        return;                     // no memory for the shadow: draw nothing rather
                                    // than draw the wrong thing and lose the diff
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
    // The classic writer indexes the topmost buffer as CHAR_INFO even though the
    // cell type of a legacy build is half the size, so this allocation is the
    // place where that has to be honoured.
    return (TScreenCell *) new CHAR_INFO[(size_t) gCols * gRows];
}

void THardwareInfo::freeScreenBuffer( TScreenCell *buffer ) noexcept
{
    delete[] (CHAR_INFO *) buffer;
}

void THardwareInfo::setUpConsole() noexcept
{
    if( gVio )
        {
        /* Read the session's screen in, then blank it.  There is no alternate
           screen to ask for and none is needed: what is put back on the way out is
           what was read here, including the shell's scrollback, because a screen
           buffer can be read as well as written. */
        vioSaveScreen();
        ensureShadow();
        vioClearScreen();
        rememberScreenCleared();
        vioOpenMouse();
        // The session's cursor is where the shell left it, which is the one thing
        // this driver no longer knows until something says otherwise.
        gTermPosKnown = False;
        gCaretShown = -1;
        return;
        }

    // Ask for the alternate screen.  A terminal without it ignores the escape, so
    // asking costs nothing and a terminal that has it gives the user back whatever
    // was on the screen before the program ran.
    outStr( "\x1b[?1049h\x1b[0m\x1b[2J\x1b[H" );
    gAttr = -1;
    gCaretX = 0;
    gCaretY = 0;
    gTermX = 0;
    gTermY = 0;
    gTermPosKnown = True;
    gCaretShown = -1;             // the alternate screen's caret is not known to us
    // The shadow starts here rather than in the constructor because this is where the
    // screen is known: the size has been read from the environment by now, and the
    // alternate screen we have just asked for is genuinely cleared, so holding spaces
    // in the default attribute is a statement about the escape above and not a hope.
    ensureShadow();
    rememberScreenCleared();
    fflush( stdout );
}

void THardwareInfo::restoreConsole() noexcept
{
    if( gVio )
        {
        vioCloseMouse();            // stop the mouse driver touching a screen
        vioRestoreScreen();         // that is about to belong to the shell again
        }
    else
        {
        outStr( "\x1b[?1000l\x1b[?1002l\x1b[?1006l" );   // mouse reports off
        outStr( "\x1b[0m\x1b[?25h" );                    // the terminal's own cursor back
        outStr( "\x1b[H\x1b[J" );                        // and a clean screen under it
        outStr( "\x1b[?1049l" );
        gAttr = -1;
        gCaretShown = 1;          // the \x1b[?25h above is what the terminal now has
        }

    delete[] gShadowCh;
    delete[] gShadowAttr;
    delete[] gDirty;                // with the shadow it described: nothing is
    gShadowCh = 0;                  // pending on a screen being handed back
    gShadowAttr = 0;
    gDirty = 0;
    gDirtyCount = 0;
    gShadowCols = 0;
    gShadowRows = 0;
    fflush( stdout );
}

//
// Mouse functions.
//

DWORD THardwareInfo::getButtonCount() noexcept
{
    if( gVio )
        {
        /* The answer decides whether tvision bothers with the mouse at all, so
           this is where the mouse is opened: waiting for setUpConsole would ask
           the question before there was a mouse to answer with. */
        if( !gMouUsable )
            vioOpenMouse();
        if( !gMouUsable )
            return 0;               // no mouse driver in this session
        return gMouButtons != 0 ? (DWORD) gMouButtons : 3;
        }
    /* There is no way to ask a stream, and the answer only decides whether
       tvision bothers to enable mouse reporting -- which a terminal without a
       mouse will ignore just as it ignores the rest.  Three is the safe claim. */
    return 3;
}

void THardwareInfo::cursorOn() noexcept
{
    if( gVio )
        return;       // asking for mouse reports with escapes on a screen that is
                      // not a terminal would put the request on the screen
    // Press and release, drag with a button held, and SGR coordinates, which are
    // the only report that survives a window past 223 columns in either axis.
    outStr( "\x1b[?1000h\x1b[?1002h\x1b[?1006h" );
}

void THardwareInfo::cursorOff() noexcept
{
    if( gVio )
        return;
    outStr( "\x1b[?1000l\x1b[?1002l\x1b[?1006l" );
}

//
// Event functions.
//

BOOL THardwareInfo::getMouseEvent( MouseEventType& event ) noexcept
{
    if( gMousePending )
        {
        event = gPendingMouse;
        gMousePending = False;
        return True;
        }
    if( gVio )
        {
        if( vioMouseWaiting() )
            return vioTakeMouse( event );
        return False;
        }
    if( gKeyPending || !kbhit() )
        return False;

    TEvent key;
    int kind = decodeEvent( key, gPendingMouse );
    if( kind == kDecodedKey )
        {
        gPendingKey = key;
        gKeyPending = True;
        return False;
        }
    if( kind == kDecodedMouse )
        {
        gMousePending = True;
        return False;       // Left for the queue's next poll, which asks for it.
        }
    return False;
}

BOOL THardwareInfo::getKeyEvent( TEvent& event ) noexcept
{
    if( gKeyPending )
        {
        event = gPendingKey;
        gKeyPending = False;
        }
    else if( gVio )
        {
        if( gMousePending || !vioKeyWaiting() )
            return False;
        /* One record, one key: no gap to wait for and no half of a key to lose,
           which is the difference between a menu that follows the arrow keys and
           one that highlights the item below the item that was pressed. */
        if( !vioTakeKey( event ) )
            return False;
        if( event.keyDown.keyCode == kbNoKey )
            return False;   // a key with no name, dropped as below
        }
    else
        {
        if( gMousePending || !kbhit() )
            return False;   // Never wait for a first byte: the queue calls this
                            // several times over to look for a paste.
        for( ;; )
            {
            int kind = decodeEvent( event, gPendingMouse );
            // A key with no code is one this driver cannot name, and passing it on
            // would have a view act on a keystroke it cannot see.  The classic
            // driver drops those the same way.
            if( kind == kDecodedKey && event.keyDown.keyCode != kbNoKey )
                break;
            if( kind == kDecodedMouse )
                {
                gMousePending = True;
                return False;
                }
            if( !kbhit() )
                return False;
            }
        }

    // Insert is a state the application reports through the event, and this is
    // the only place that sees every keystroke.
    if( event.keyDown.keyCode == kbIns )
        insertState = !insertState;
    if( insertState )
        event.keyDown.controlKeyState |= kbInsState;

    return True;
}

void THardwareInfo::waitForEvents( int timeoutMs ) noexcept
{
    /* The frame ends here.  Everything drawn since the last time through the event
       loop goes to the screen now, in one pass, which is what keeps a view's
       fill-then-write from being seen as two states -- and it is free to be here
       rather than in screenWrite because nothing can look at the screen while the
       application is standing in this function.  `source/platform/hardware.cpp`
       does the same, with the same reason: flush once for every time all the events
       have been processed, only when blocking for events. */
    flushScreen();

#ifdef TV_DEBUG_LOOP
    const uint64_t t0 = THardwareInfo::getTickCountMs();
    if( anyInput() )
        {
        noteWait( timeoutMs, 0, "input already ready", True );
        return;
        }

    int waited = 0;
    const char *how = "timeout";
    while( !anyInput() )
        {
        if( gInterruptWait )
            {
            how = "interrupted";
            break;
            }
        if( timeoutMs >= 0 && waited >= timeoutMs )
            break;
        DosSleep( kPollMs );
        waited += kPollMs;
        }
    gInterruptWait = False;
    noteWait( timeoutMs, THardwareInfo::getTickCountMs() - t0, how, True );
#else
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
        }
    gInterruptWait = False;
#endif
}

void THardwareInfo::interruptEventWait() noexcept
{
    gInterruptWait = True;
}

BOOL THardwareInfo::setClipboardText( TStringView text ) noexcept
{
    (void) text;
    return False;   // See the header of this file.
}

BOOL THardwareInfo::requestClipboardText( void (&accept)( TStringView ) ) noexcept
{
    (void) accept;
    return False;   // See the header of this file.
}

//
// Clock.
//

uint64_t THardwareInfo::getTickCountMs() noexcept
{
    /* ftime() rather than an OS/2 API: it is milliseconds, it is monotonic enough
       for timeouts and double-clicks, and it asks nothing of a session that may
       not have the timer devices an OS/2 program would otherwise open. */
    struct timeb now;
    ftime( &now );
    return (uint64_t) (long) now.time * 1000 + (uint64_t) now.millitm;
}

/* The write-call counters the application logs once a second (app.cpp, the
   `vio` line): totals, because the app differences them itself, and plain C
   because that is the cheapest thing this side of a header to share. */
extern "C" {

unsigned long tvVioStatCalls()   { return gVioCalls; }
unsigned long tvVioStatScrolls() { return gVioScrolls; }
unsigned long tvVioStatFlushMs() { return gVioFlushMs; }

} // extern "C"

#endif // __FLAT__ && __OS2__ && !__BORLANDC__
