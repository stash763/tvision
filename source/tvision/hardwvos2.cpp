/*------------------------------------------------------------*/
/* filename -       hardwvos2.cpp                             */
/*                                                            */
/* THardwareInfo for OS/2 32-bit: VT escapes out, keys in.    */
/*------------------------------------------------------------*/
/*
 * hardwrvr.cpp holds the driver bodies for Borland's Windows console and leaves
 * every other compiler the class with no implementation: the first program that
 * constructs a TApplication asks the link for 28 THardwareInfo members and finds
 * none of them.  This file answers that for OS/2, which is the platform whose
 * compiler (Open Watcom) builds the classic classes and cannot build the modern
 * terminal layer in source/platform.
 *
 * It draws with VT escapes on stdout and reads keystrokes from stdin.  The other
 * driver this could have been -- VIO, the console the box owns -- was tried first
 * and measured: every Vio* entry point resolves in os2386.lib and then fails in a
 * process the telnet server started, with ERROR_VIO_INVALID_HANDLE (436), and
 * KbdPeek fails with 445.  A screen that works only while somebody sits at the
 * machine is of no use to a program whose job is to be reached over a network, so
 * escapes are the driver and VIO stays port work.  The price of that is honest
 * and worth stating: OS/2's own full-screen session is not a VT terminal, so
 * nothing here renders on the box's own display.
 *
 * Three measurements in TESTING.md shaped the input loop, and each is the kind
 * that reads as a bug in the driver if it is not written down beside the code:
 *
 *   * select() does not work on the console handle; it returns -1.  Readiness is
 *     polled with kbhit(), which does see every byte.
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
 *   * Shift and control state for ordinary keys.  A character stream carries the
 *     character, not the keys that made it, so the modifier bits stay empty except
 *     where the byte itself proves one (a control byte is Ctrl plus a letter) or
 *     where an escape sequence carries one.  Mouse reports do carry modifiers.
 *   * Home and End over a session that eats the introducer.  Measured: Home
 *     arrives as '[' then 'H', End as '[' then 'F', the Escape having been lost on
 *     the way.  Treating a bare bracket as half a key would misread a pasted "[H",
 *     so a bracket stays a bracket and those two keys are the session's to fix.
 *   * The clipboard.  The Presentation Manager clipboard belongs to a session this
 *     process does not have; the terminal's own clipboard, asked for with OSC 52,
 *     is the way to do it here.
 *   * Noticing that the window was resized.  Nothing tells a stream driver, so the
 *     size is taken once, from ATUI_COLS and ATUI_ROWS if they are set and 80x25
 *     otherwise, and stays until something calls setScreenMode.
 */

#if defined( __FLAT__ ) && defined( __OS2__ ) && !defined( __BORLANDC__ )

/* tvision's compat/windows/windows.h asks for <os2.h> on this platform, so the
   feature macros have to be set before tv.h is read rather than with the include
   below -- otherwise DosSleep is not in the header by the time this file wants
   it, and the first read of <os2.h> is the one that decides. */
#define INCL_DOSPROCESS

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

// What the terminal is set to, so that a run of cells emits one attribute
// change rather than one per cell.  -1 means the terminal's state is not known
// to us, which is how it starts and how it is left by anything that resets it.
static int gAttr = -1;

static ushort gCols = kDefCols;
static ushort gRows = kDefRows;

// The caret as the application left it.  Emitting it is not enough on a stream:
// every write moves the terminal's cursor along, so this is what the position is
// restored to after a pass over the screen.
static ushort gCaretX = 0;
static ushort gCaretY = 0;

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

static Boolean anyInput()
{
    return gKeyPending || gMousePending || kbhit() != 0;
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

//
// Construction
//

THardwareInfo::THardwareInfo() noexcept
{
    // Not plDPMI32, which is what the member is initialised to and which the
    // classic code reads as "a DOS machine I may poke".
    platform = plOS2;

    // The size is asked for before anything is drawn, and a stream driver cannot
    // know it: the terminal is at the other end of a pipe as far as this process
    // is concerned.  So it is stated rather than discovered.
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
    outStr( size != 0 ? "\x1b[?25h" : "\x1b[?25l" );
}

void THardwareInfo::setCaretPosition( ushort x, ushort y ) noexcept
{
    gCaretX = x;
    gCaretY = y;
    sbInfo.dwCursorPosition.X = (SHORT) x;
    sbInfo.dwCursorPosition.Y = (SHORT) y;
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
    flushScreen();
}

void THardwareInfo::clearScreen( ushort, ushort ) noexcept
{
    // A VT clear is the whole screen, so the extent asked for is not used.
    outStr( "\x1b[0m\x1b[2J\x1b[H" );
    gAttr = -1;
}

void THardwareInfo::flushScreen() noexcept
{
    // The writes have moved the terminal's cursor along from wherever the
    // application put it, and it stays there; putting it back is what makes the
    // difference between a caret and a stray block.
    char seq[24];
    sprintf( seq, "\x1b[%u;%uH", (unsigned) gCaretY + 1, (unsigned) gCaretX + 1 );
    outStr( seq );
    fflush( stdout );
}

/* One row of cells, at the position the classic writer computed for them.  Note
   that the run always stays inside one row, so a sequence per call is enough.
   Writing the last column of the last row leaves an emulated terminal in its
   pending-wrap state, which the next escape clears -- flushScreen() emits one, so
   a full-screen redraw does not scroll the screen away underneath the program. */
void THardwareInfo::screenWrite( ushort x, ushort y, CHAR_INFO *buf, DWORD len ) noexcept
{
    if( len == 0 )
        return;

    moveTo( x, y );

    DWORD i = 0;
    while( i < len )
        {
        int attr = buf[i].Attributes & 0xFF;
        selectAttr( attr );

        // One write per run of the same attribute rather than per cell: a line of
        // a window is usually one attribute long, and every byte here travels to
        // the other end of a telnet connection.
        char run[128];
        size_t n = 0;
        while( i < len && (buf[i].Attributes & 0xFF) == attr && n < sizeof( run ) - 1 )
            run[n++] = printableCell( (unsigned char) buf[i++].Char.AsciiChar );
        run[n] = '\0';
        outStr( run );
        }
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
    // Ask for the alternate screen.  A terminal without it ignores the escape, so
    // asking costs nothing and a terminal that has it gives the user back whatever
    // was on the screen before the program ran.
    outStr( "\x1b[?1049h\x1b[0m\x1b[2J\x1b[H" );
    gAttr = -1;
    gCaretX = 0;
    gCaretY = 0;
    fflush( stdout );
}

void THardwareInfo::restoreConsole() noexcept
{
    outStr( "\x1b[?1000l\x1b[?1002l\x1b[?1006l" );   // mouse reports off
    outStr( "\x1b[0m\x1b[?25h" );                    // the terminal's own cursor back
    outStr( "\x1b[H\x1b[J" );                        // and a clean screen under it
    outStr( "\x1b[?1049l" );
    gAttr = -1;
    fflush( stdout );
}

//
// Mouse functions.
//

DWORD THardwareInfo::getButtonCount() noexcept
{
    /* There is no way to ask a stream, and the answer only decides whether
       tvision bothers to enable mouse reporting -- which a terminal without a
       mouse will ignore just as it ignores the rest.  Three is the safe claim. */
    return 3;
}

void THardwareInfo::cursorOn() noexcept
{
    // Press and release, drag with a button held, and SGR coordinates, which are
    // the only report that survives a window past 223 columns in either axis.
    outStr( "\x1b[?1000h\x1b[?1002h\x1b[?1006h" );
}

void THardwareInfo::cursorOff() noexcept
{
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

#endif // __FLAT__ && __OS2__ && !__BORLANDC__
