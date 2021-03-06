/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Xterm.h"
#include "Font.h"
#include "Terminal.h"
#include <syslog.h>
#include <time.h>

#define C_BLACK   0
#define C_RED     1
#define C_GREEN   2
#define C_YELLOW  3
#define C_BLUE    4
#define C_MAGENTA 5
#define C_CYAN    6
#define C_WHITE   7

#define C_BRIGHT  8

// Set to 1 to use Framebuffer::line instead of Unicode lines.
// Fairly buggy unfortunately.
#define USE_FRAMEBUFFER_LINES       1

#include "environment.h"

#include <config/Config.h>
#include <graphics/Graphics.h>

 // RGBA -> BGRA

extern uint32_t g_Colours[];
extern uint32_t g_BrightColours[];

uint8_t g_DefaultFg = 7;
uint8_t g_DefaultBg = 0;

bool g_FontsPrecached = false;

extern Font *g_NormalFont, *g_BoldFont;

extern cairo_t *g_Cairo;
extern cairo_surface_t *g_Surface;

static void getXtermColorFromDb(const char *colorName, uint8_t &color)
{
    // The query string
    std::string sQuery;

    // Create the query string
    sQuery += "select r from 'colour_scheme' where name='";
    sQuery += colorName;
    sQuery += "';";

    // Query the database
    Config::Result *pResult = Config::query(sQuery.c_str());

    // Did the query fail?
    if(!pResult)
    {
        syslog(LOG_ALERT, "TUI: Error looking up '%s' colour.", colorName);
        return;
    }
    if(!pResult->succeeded())
    {
        syslog(LOG_ALERT, "TUI: Error looking up '%s' colour: %s\n", colorName, pResult->errorMessage().c_str());
        delete pResult;
        return;
    }

    // Get the color from the query result
    color = pResult->getNum(0, "r");

    // Dispose of the query result
    delete pResult;
}

Xterm::Xterm(PedigreeGraphics::Framebuffer *pFramebuffer, size_t nWidth, size_t nHeight, size_t offsetLeft, size_t offsetTop, Terminal *pT) :
    m_ActiveBuffer(0), m_Cmd(), m_OsCtl(), m_bChangingState(false), m_bContainedBracket(false),
    m_bContainedParen(false), m_bIsOsControl(false), m_Flags(0), m_Modes(0), m_TabStops(0),
    m_SavedX(0), m_SavedY(0), m_pT(pT), m_bFbMode(false)
{
    size_t width = nWidth / g_NormalFont->getWidth();
    size_t height = nHeight / g_NormalFont->getHeight();

    m_pWindows[0] = new Window(height, width, pFramebuffer, 1000, offsetLeft, offsetTop, nWidth, this);
    m_pWindows[1] = new Window(height, width, pFramebuffer, 1000, offsetLeft, offsetTop, nWidth, this);

    getXtermColorFromDb("xterm-bg", g_DefaultBg);
    getXtermColorFromDb("xterm-fg", g_DefaultFg);

    // Set default tab stops.
    m_TabStops = new char[width];
    memset(m_TabStops, 0, width);
    for(size_t i = 0; i < width; i += 8)
    {
        m_TabStops[i] = '|';
    }

    // Set default modes.
    m_Modes |= AnsiVt52;
}

Xterm::~Xterm()
{
    delete m_pWindows[0];
    delete m_pWindows[1];
}

void Xterm::processKey(uint64_t key)
{
    if (key & Keyboard::Special)
    {
        // Handle specially.
        uint32_t utf32 = key & ~0UL;
        char *str = reinterpret_cast<char*> (&utf32);
        if (!strncmp(str, "left", 4))
        {
            m_pT->addToQueue('\e');
            if((m_Modes & AnsiVt52) == AnsiVt52)
            {
                if(m_Modes & CursorKey)
                    m_pT->addToQueue('O');
                else
                    m_pT->addToQueue('[');
            }
            m_pT->addToQueue('D');
        }
        if (!strncmp(str, "righ", 4))
        {
            m_pT->addToQueue('\e');
            if((m_Modes & AnsiVt52) == AnsiVt52)
            {
                if(m_Modes & CursorKey)
                    m_pT->addToQueue('O');
                else
                    m_pT->addToQueue('[');
            }
            m_pT->addToQueue('C');
        }
        if (!strncmp(str, "up", 2))
        {
            m_pT->addToQueue('\e');
            if((m_Modes & AnsiVt52) == AnsiVt52)
            {
                if(m_Modes & CursorKey)
                    m_pT->addToQueue('O');
                else
                    m_pT->addToQueue('[');
            }
            m_pT->addToQueue('A');
        }
        if (!strncmp(str, "down", 4))
        {
            m_pT->addToQueue('\e');
            if((m_Modes & AnsiVt52) == AnsiVt52)
            {
                if(m_Modes & CursorKey)
                    m_pT->addToQueue('O');
                else
                    m_pT->addToQueue('[');
            }
            m_pT->addToQueue('B');
        }
    }
    else if (key & Keyboard::Alt)
    {
        // Xterm ALT escape = ESC <char>
        m_pT->addToQueue('\e');
        m_pT->addToQueue(key & 0x7F);
    }
    else if(key == '\n')
    {
        m_pT->addToQueue('\r');
        if(m_Modes & LineFeedNewLine)
            m_pT->addToQueue('\n');
    }
    else
    {
        uint32_t utf32 = key&0xFFFFFFFF;

        // Begin Utf-32 -> Utf-8 conversion.
        char buf[4];
        size_t nbuf = 0;
        if (utf32 <= 0x7F)
        {
            buf[0] = utf32&0x7F;
            nbuf = 1;
        }
        else if (utf32 <= 0x7FF)
        {
            buf[0] = 0xC0 | ((utf32>>6) & 0x1F);
            buf[1] = 0x80 | (utf32 & 0x3F);
            nbuf = 2;
        }
        else if (utf32 <= 0xFFFF)
        {
            buf[0] = 0xE0 | ((utf32>>12) & 0x0F);
            buf[1] = 0x80 | ((utf32>>6) & 0x3F);
            buf[2] = 0x80 | (utf32 & 0x3F);
            nbuf = 3;
        }
        else if (utf32 <= 0x10FFFF)
        {
            buf[0] = 0xE0 | ((utf32>>18) & 0x07);
            buf[1] = 0x80 | ((utf32>>12) & 0x3F);
            buf[2] = 0x80 | ((utf32>>6) & 0x3F);
            buf[3] = 0x80 | (utf32 & 0x3F);
            nbuf = 4;
        }

        // End Utf-32 -> Utf-8 conversion.
        for (size_t i = 0; i < nbuf; i++)
            m_pT->addToQueue(buf[i]);
    }

    m_pT->addToQueue(0, true);
}

void Xterm::setFlagsForUtf32(uint32_t utf32)
{
    switch(utf32)
    {
        case '\e':
            m_Flags |= Escape;
            break;

        case '?':
            m_Flags |= Question;
            break;

        case ' ':
            m_Flags |= Space;
            break;

        case '#':
            m_Flags |= Hash;
            break;

        case '%':
            m_Flags |= Percent;
            break;

        case '(':
            m_Flags |= LeftRound;
            break;

        case ')':
            m_Flags |= RightRound;
            break;

        case '[':
            m_Flags |= LeftSquare;
            break;

        case ']':
            m_Flags |= RightSquare;
            break;

        case '_':
            m_Flags |= Underscore;
            break;

        case '/':
            m_Flags |= Slash;
            break;

        case '.':
            m_Flags |= Period;
            break;

        case '-':
            m_Flags |= Minus;
            break;

        case '+':
            m_Flags |= Plus;
            break;

        case '*':
            m_Flags |= Asterisk;
            break;

        case '\\':
            m_Flags |= Backslash;
            break;

        case '!':
            m_Flags |= Bang;
            break;

        case '\'':
            m_Flags |= Apostrophe;
            break;

        case '"':
            m_Flags |= Quote;
            break;

        case '$':
            m_Flags |= Dollar;
            break;

        default:
            break;
    }
}

void Xterm::write(uint32_t utf32, DirtyRectangle &rect)
{
//    char str[64];
//    sprintf(str, "XTerm: Write: %c/%x", utf32, utf32);
//    log(str);

    if(!m_Flags)
    {
        switch(utf32)
        {
            case 0x05:
                {
                    // Reply with answerback.
                    const char *answerback = "\e[1;2c";
                    while(*answerback)
                    {
                        m_pT->addToQueue(*answerback);
                        ++answerback;
                    }
                }
            case 0x08:
                m_pWindows[m_ActiveBuffer]->backspace(rect);
                break;

            case '\n':
            case 0x0B:
            case 0x0C:
                if(m_Modes & LineFeedNewLine)
                    m_pWindows[m_ActiveBuffer]->cursorDownAndLeftToMargin(rect);
                else
                    m_pWindows[m_ActiveBuffer]->cursorDown(1, rect);
                break;

            case '\t':
                m_pWindows[m_ActiveBuffer]->cursorTab(rect);
                break;

            case '\r':
                m_pWindows[m_ActiveBuffer]->cursorLeftToMargin(rect);
                break;

            case 0x0E:
            case 0x0F:
                // SHIFT-IN, SHIFT-OUT (character sets)
                break;

            case '\e':
                m_Flags = Escape;

                m_Cmd.cur_param = 0;
                memset(m_Cmd.params, 0, sizeof(m_Cmd.params[0]) * XTERM_MAX_PARAMS);

                m_OsCtl.cur_param = 0;
                for(size_t i = 0; i < XTERM_MAX_PARAMS; ++i)
                {
                    m_OsCtl.params[i] = "";
                }
                break;

            default:
                // Handle line drawing.
                if(m_pWindows[m_ActiveBuffer]->getLineRenderMode())
                {
                    switch(utf32)
                    {
                        case 'j': utf32 = 0x2518; break; // Lower right corner
                        case 'k': utf32 = 0x2510; break; // Upper right corner
                        case 'l': utf32 = 0x250c; break; // Upper left corner
                        case 'm': utf32 = 0x2514; break; // Lower left corner
                        case 'n': utf32 = 0x253c; break; // Crossing lines.
                        case 'q': utf32 = 0x2500; break; // Horizontal line.
                        case 't': utf32 = 0x251c; break; // Left 'T'
                        case 'u': utf32 = 0x2524; break; // Right 'T'
                        case 'v': utf32 = 0x2534; break; // Bottom 'T'
                        case 'w': utf32 = 0x252c; break; // Top 'T'
                        case 'x': utf32 = 0x2502; break; // Vertical bar
                        default:
                        ;
                    }
                }

                // Printable character?
                if(utf32 >= 32)
                {
                    m_pWindows[m_ActiveBuffer]->addChar(utf32, rect);
                }
        }
    }
    else if((m_Flags & (Escape | LeftSquare | RightSquare | Underscore)) == (Escape | LeftSquare))
    {
        setFlagsForUtf32(utf32);

        switch(utf32)
        {
            case 0x08:
                // Backspace within control sequence.
                m_pWindows[m_ActiveBuffer]->backspace(rect);
                break;

            case '\n':
            case 0x0B:
                if(m_Modes & LineFeedNewLine)
                    m_pWindows[m_ActiveBuffer]->cursorLeftToMargin(rect);
                m_pWindows[m_ActiveBuffer]->cursorDown(1, rect);
                break;

            case '\r':
                m_pWindows[m_ActiveBuffer]->cursorLeftToMargin(rect);
                break;

            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                m_Cmd.params[m_Cmd.cur_param] = m_Cmd.params[m_Cmd.cur_param] * 10 + (utf32-'0');
                break;

            case ';':
            case ',':
                m_Cmd.cur_param++;
                break;

            case '@':
                m_pWindows[m_ActiveBuffer]->insertCharacters(m_Cmd.params[0] ? m_Cmd.params[0] : 1, rect);
                m_Flags = 0;
                break;

            case 'A':
                m_pWindows[m_ActiveBuffer]->cursorUpWithinMargin(1, rect);
                m_Flags = 0;
                break;

            case 'B':
                m_pWindows[m_ActiveBuffer]->cursorDownWithinMargin(1, rect);
                m_Flags = 0;
                break;

            case 'C':
                m_pWindows[m_ActiveBuffer]->cursorRightWithinMargin(1, rect);
                m_Flags = 0;
                break;

            case 'D':
                m_pWindows[m_ActiveBuffer]->cursorLeftWithinMargin(1, rect);
                m_Flags = 0;
                break;

            case 'E':
                m_pWindows[m_ActiveBuffer]->cursorLeftToMargin(rect);
                m_pWindows[m_ActiveBuffer]->cursorDown(m_Cmd.params[0] ? m_Cmd.params[0] : 1, rect);
                m_Flags = 0;
                break;

            case 'F':
                m_pWindows[m_ActiveBuffer]->cursorLeftToMargin(rect);
                m_pWindows[m_ActiveBuffer]->cursorUp(m_Cmd.params[0] ? m_Cmd.params[0] : 1, rect);
                m_Flags = 0;
                break;

            case 'G':
                // Absolute column reference. (XTERM)
                m_pWindows[m_ActiveBuffer]->setCursorX((m_Cmd.params[0]) ? m_Cmd.params[0]-1 : 0, rect);
                m_Flags = 0;
                break;

            case 'H':
            case 'f':
                {
                    size_t x = (m_Cmd.params[1]) ? m_Cmd.params[1]-1 : 0;
                    size_t y = (m_Cmd.params[0]) ? m_Cmd.params[0]-1 : 0;
                    m_pWindows[m_ActiveBuffer]->setCursor(x, y, rect);
                    m_Flags = 0;
                }
                break;

            case 'I':
                if(!m_Cmd.params[0])
                    ++m_Cmd.params[0];

                for(int n = 0; n < m_Cmd.params[0]; ++n)
                {
                    m_pWindows[m_ActiveBuffer]->cursorTab(rect);
                }
                m_Flags = 0;
                break;

            case 'J':
                switch (m_Cmd.params[0])
                {
                    case 0: // Erase down.
                         m_pWindows[m_ActiveBuffer]->eraseDown(rect);
                        break;
                    case 1: // Erase up.
                        m_pWindows[m_ActiveBuffer]->eraseUp(rect);
                        break;
                    case 2: // Erase entire screen and move to home.
                        m_pWindows[m_ActiveBuffer]->eraseScreen(rect);
                        break;
                }
                m_Flags = 0;
                break;

            case 'K':
                switch (m_Cmd.params[0])
                {
                    case 0: // Erase end of line.
                        m_pWindows[m_ActiveBuffer]->eraseEOL(rect);
                        break;
                    case 1: // Erase start of line.
                        m_pWindows[m_ActiveBuffer]->eraseSOL(rect);
                        break;
                    case 2: // Erase entire line.
                        m_pWindows[m_ActiveBuffer]->eraseLine(rect);
                        break;
                }
                m_Flags = 0;
                break;

            case 'L':
                m_pWindows[m_ActiveBuffer]->insertLines(m_Cmd.params[0] ? m_Cmd.params[0] : 1, rect);
                m_Flags = 0;
                break;

            case 'M':
                m_pWindows[m_ActiveBuffer]->deleteLines(m_Cmd.params[0] ? m_Cmd.params[0] : 1, rect);
                m_Flags = 0;
                break;

            case 'P':
                m_pWindows[m_ActiveBuffer]->deleteCharacters((m_Cmd.params[0]) ? m_Cmd.params[0] : 1, rect);
                m_Flags = 0;
                break;

            case 'S':
                {
                    size_t nScrollLines = (m_Cmd.params[0]) ? m_Cmd.params[0] : 1;
                    m_pWindows[m_ActiveBuffer]->scrollDown(nScrollLines, rect);
                    m_Flags = 0;
                }
                break;

            case 'T':
                if(m_Flags & RightAngle)
                {
                    /// \todo Changes how title modes are set...
                }
                else if(m_Cmd.cur_param > 1)
                {
                    syslog(LOG_INFO, "XTERM: highlight mouse tracking is not supported.");
                }
                else
                {
                    size_t nScrollLines = (m_Cmd.params[0]) ? m_Cmd.params[0] : 1;
                    m_pWindows[m_ActiveBuffer]->scrollUp(nScrollLines, rect);
                }
                m_Flags = 0;
                break;

            case 'X': // Erase characters (XTERM)
                 m_pWindows[m_ActiveBuffer]->eraseChars((m_Cmd.params[0]) ? m_Cmd.params[0]-1 : 1, rect);
                m_Flags = 0;
                break;

            case 'Z':
                if(!m_Cmd.params[0])
                    ++m_Cmd.params[0];

                for(int n = 0; n < m_Cmd.params[0]; ++n)
                {
                    m_pWindows[m_ActiveBuffer]->cursorTabBack(rect);
                }
                m_Flags = 0;
                break;

            case 'c':
                if(m_Cmd.params[0])
                {
                    syslog(LOG_INFO, "XTERM: Device Attributes command with non-zero parameter");
                }
                else if(m_Flags & RightAngle)
                {
                    // Secondary Device Attributes
                    const char *attribs = "\e[?85;95;0c";
                    while(*attribs)
                    {
                        m_pT->addToQueue(*attribs);
                        ++attribs;
                    }
                }
                else
                {
                    // Primary Device Attributes
                    const char *attribs = "\e[?1;2c";
                    while(*attribs)
                    {
                        m_pT->addToQueue(*attribs);
                        ++attribs;
                    }
                }
                m_Flags = 0;
                break;

            case 'd':
                // Absolute row reference. (XTERM)
                m_pWindows[m_ActiveBuffer]->setCursorY((m_Cmd.params[0]) ? m_Cmd.params[0]-1 : 0, rect);
                m_Flags = 0;
                break;

            case 'e':
                // Relative row reference. (XTERM)
                m_pWindows[m_ActiveBuffer]->cursorDown((m_Cmd.params[0]) ? m_Cmd.params[0]-1 : 1, rect);
                m_Flags = 0;
                break;

            case 'g':
                if(m_Cmd.params[0] != 3)
                    m_pWindows[m_ActiveBuffer]->clearTabStop();
                else
                    m_pWindows[m_ActiveBuffer]->clearAllTabStops();
                m_Flags = 0;
                break;

            case 'h':
            case 'l':
                {
                size_t modesToChange = 0;

                if(m_Flags & Question)
                {
                    // DEC private mode set.
                    for(int i = 0; i <= m_Cmd.cur_param; ++i)
                    {
                        switch(m_Cmd.params[i])
                        {
                            case 1:
                                modesToChange |= CursorKey;
                                break;
                            case 2:
                                modesToChange |= AnsiVt52;
                                break;
                            case 3:
                                // DECCOLM is ignored.
                                break;
                            case 4:
                                modesToChange |= Scrolling;
                                break;
                            case 5:
                                if((utf32 == 'h') && ((m_Modes & Screen) == 0))
                                    modesToChange |= Screen;
                                else if((utf32 == 'l') && ((m_Modes & Screen) != 0))
                                    modesToChange |= Screen;
                                break;
                            case 6:
                                modesToChange |= Origin;
                                break;
                            case 7:
                                modesToChange |= AutoWrap;
                                break;
                            case 8:
                                modesToChange |= AutoRepeat;
                                break;
                            case 67:
                                modesToChange |= AppKeypad;
                                break;
                            case 69:
                                modesToChange |= Margin;
                                break;
                            case 1048:
                            case 1049:
                                if(utf32 == 'h')
                                {
                                    m_SavedX = m_pWindows[m_ActiveBuffer]->getCursorX();
                                    m_SavedY = m_pWindows[m_ActiveBuffer]->getCursorY();
                                }
                                else
                                {
                                    m_pWindows[m_ActiveBuffer]->setCursor(m_SavedX, m_SavedY, rect);
                                }
                            case 47:
                            case 1047:
                                // 1048 is only save/restore cursor.
                                if(m_Cmd.params[i] != 1048)
                                {
                                    m_ActiveBuffer = (utf32 == 'h') ? 1 : 0;
                                    if((utf32 == 'h') && (m_Cmd.params[i] == 1049))
                                    {
                                        // Clear new buffer.
                                        m_pWindows[m_ActiveBuffer]->eraseScreen(rect);
                                    }
                                    else
                                    {
                                        // Merely switch to the new buffer.
                                        m_pWindows[m_ActiveBuffer]->renderAll(rect, m_pWindows[0]);
                                    }
                                }
                                break;
                            default:
                                syslog(LOG_INFO, "XTERM: unknown DEC Private Mode %d", m_Cmd.params[i]);
                                break;
                        }
                    }
                }
                else
                {
                    for(int i = 0; i <= m_Cmd.cur_param; ++i)
                    {
                        switch(m_Cmd.params[i])
                        {
                            case 20:
                                modesToChange |= LineFeedNewLine;
                                break;
                            default:
                                syslog(LOG_INFO, "XTERM: unknown standard mode %d", m_Cmd.params[i]);
                                break;
                        }
                    }
                }

                if(utf32 == 'h')
                {
                    m_Modes |= modesToChange;
                }
                else
                {
                    m_Modes &= ~(modesToChange);
                }

                // Setting some modes causes things to change.
                if(modesToChange & Origin)
                {
                    // Move to top left corner.
                    m_pWindows[m_ActiveBuffer]->setCursor(0, 0, rect);
                }

                if(modesToChange & Screen)
                {
                    // Invert the entire screen.
                    m_pWindows[m_ActiveBuffer]->invert(rect);
                }

                }
                m_Flags = 0;
                break;

            case 'm':
                {
                    // Character attributes.
                    for (int i = 0; i < m_Cmd.cur_param+1; i++)
                    {
                        switch (m_Cmd.params[i])
                        {
                            case 0:
                            {
                                // Reset all attributes.
                                m_pWindows[m_ActiveBuffer]->setFlags(0);
                                m_pWindows[m_ActiveBuffer]->setForeColour(g_DefaultFg);
                                m_pWindows[m_ActiveBuffer]->setBackColour(g_DefaultBg);
                                break;
                            }
                            case 1:
                            {
                                // Bold
                                uint8_t flags = m_pWindows[m_ActiveBuffer]->getFlags();
                                if(!(flags & XTERM_BOLD))
                                {
                                    flags |= XTERM_BOLD;
                                    m_pWindows[m_ActiveBuffer]->setFlags(flags);
                                }
                                break;
                            }
                            case 7:
                            {
                                // Inverse
                                uint8_t flags = m_pWindows[m_ActiveBuffer]->getFlags();
                                if(flags & XTERM_INVERSE)
                                    flags &= ~XTERM_INVERSE;
                                else
                                    flags |= XTERM_INVERSE;
                                m_pWindows[m_ActiveBuffer]->setFlags(flags);
                                break;
                            }
                            case 30:
                            case 31:
                            case 32:
                            case 33:
                            case 34:
                            case 35:
                            case 36:
                            case 37:
                                // Foreground.
                                m_pWindows[m_ActiveBuffer]->setForeColour(m_Cmd.params[i]-30);
                                break;
                            case 38:
                                // xterm-256 foreground
                                if(m_Cmd.params[i+1] == 5)
                                {
                                    m_pWindows[m_ActiveBuffer]->setForeColour(m_Cmd.params[i+2]);
                                    i += 3;
                                }
                                break;
                            case 39:
                                m_pWindows[m_ActiveBuffer]->setForeColour(g_DefaultFg);
                                break;
                            case 40:
                            case 41:
                            case 42:
                            case 43:
                            case 44:
                            case 45:
                            case 46:
                            case 47:
                                // Background.
                                m_pWindows[m_ActiveBuffer]->setBackColour(m_Cmd.params[i]-40);
                                break;
                            case 48:
                                // xterm-256 background
                                if(m_Cmd.params[i+1] == 5)
                                {
                                    m_pWindows[m_ActiveBuffer]->setBackColour(m_Cmd.params[i+2]);
                                    i += 3;
                                }
                                break;
                            case 49:
                                m_pWindows[m_ActiveBuffer]->setForeColour(g_DefaultBg);
                                break;

                            default:
                                // Do nothing.
                                syslog(LOG_INFO, "XTERM: unknown character attribute %d", m_Cmd.params[i]);
                                break;
                        }
                    }
                    m_Flags = 0;
                }
                break;

            case 'n':
                if((m_Flags & RightAngle) == 0)
                {
                    // Device Status Reports
                    switch(m_Cmd.params[0])
                    {
                        case 5:
                            {
                                // Ready, no malfunction status.
                                const char *status = "\e[0n";
                                while(*status)
                                {
                                    m_pT->addToQueue(*status);
                                    ++status;
                                }
                            }
                            break;

                        case 6:
                            {
                                // Report cursor position.
                                char buf[128];
                                sprintf(buf, "\e[%d;%dR", m_pWindows[m_ActiveBuffer]->getCursorY() + 1, m_pWindows[m_ActiveBuffer]->getCursorX() + 1);
                                const char *p = (const char *) buf;
                                while(*p)
                                {
                                    m_pT->addToQueue(*p);
                                    ++p;
                                }
                            }
                            break;

                        default:
                            syslog(LOG_INFO, "XTERM: unknown device status request %d", m_Cmd.params[0]);
                            break;
                    }
                }
                m_Flags = 0;
                break;

            case 'p':
                // Could be soft terminal reset, request ANSI mode, set resource level, etc...
                m_Flags = 0;
                break;

            case 'q':
                if(m_Flags & Space)
                {
                    // Set cursor style.
                }

                m_Flags = 0;
                break;

            case 'r':
                if((m_Flags & Question) == 0)
                {
                    m_pWindows[m_ActiveBuffer]->setScrollRegion((m_Cmd.params[0]) ? m_Cmd.params[0]-1 : ~0,
                                                                (m_Cmd.params[1]) ? m_Cmd.params[1]-1 : ~0);
                }
                m_Flags = 0;
                break;

            case 's':
                if(m_Cmd.cur_param == 0)
                {
                    if((m_Modes & Margin) == 0)
                    {
                        m_SavedX = m_pWindows[m_ActiveBuffer]->getCursorX();
                        m_SavedY = m_pWindows[m_ActiveBuffer]->getCursorY();
                    }
                }
                else if(m_Modes & Margin)
                {
                    // Set left/right margins.
                    m_pWindows[m_ActiveBuffer]->setMargins((m_Cmd.params[0]) ? m_Cmd.params[0]-1 : ~0,
                                                           (m_Cmd.params[1]) ? m_Cmd.params[1]-1 : ~0);
                }
                m_Flags = 0;
                break;

            case 'u':
                if((m_Flags & Space) == 0)
                {
                    m_pWindows[m_ActiveBuffer]->setCursor(m_SavedX, m_SavedY, rect);
                }
                m_Flags = 0;
                break;

            case 'x':
                if((m_Flags & Asterisk) == 0)
                {
                    if(m_Cmd.params[0] <= 1)
                    {
                        const char *termparams = 0;
                        if(m_Cmd.params[0] == 0)
                            termparams = "\e[3;1;1;120;120;1;0x";
                        else
                            termparams = "\e[2;1;1;120;120;1;0x";

                        while(*termparams)
                        {
                            m_pT->addToQueue(*termparams);
                            ++termparams;
                        }
                    }
                }
                m_Flags = 0;
                break;

            default:
                break;
        }
    }
    else if((m_Flags & (Escape | LeftSquare | RightSquare | Underscore)) == Escape)
    {
        setFlagsForUtf32(utf32);

        switch(utf32)
        {
            case 0x08:
                // Backspace within escape sequence.
                m_pWindows[m_ActiveBuffer]->backspace(rect);
                break;

            case '0':
                if(m_Flags & LeftRound) // \e(0
                {
                    // Set DEC Special Character and Line Drawing Set
                    m_pWindows[m_ActiveBuffer]->setLineRenderMode(true);
                }
                m_Flags = 0;
                break;

            case '7':
                m_SavedX = m_pWindows[m_ActiveBuffer]->getCursorX();
                m_SavedY = m_pWindows[m_ActiveBuffer]->getCursorY();
                m_Flags = 0;
                break;

            case '8':
                if(m_Flags & Hash)
                {
                    // DEC Screen Alignment Test (fill screen with 'E')
                    // ...
                }
                else
                {
                    m_pWindows[m_ActiveBuffer]->setCursor(m_SavedX, m_SavedY, rect);
                }
                m_Flags = 0;
                break;

            case '<':
                m_Modes |= AnsiVt52;
                m_Flags = 0;
                break;

            case '>':
                /// \todo Alternate keypad mode
                m_Flags = 0;
                break;

            case 'A':
                m_pWindows[m_ActiveBuffer]->cursorUpWithinMargin(1, rect);
                m_Flags = 0;
                break;

            case 'B':
                if(m_Flags & LeftRound) // \e(B
                {
                    // Set USASCII character set.
                    m_pWindows[m_ActiveBuffer]->setLineRenderMode(false);
                }
                else
                {
                    m_pWindows[m_ActiveBuffer]->cursorDownWithinMargin(1, rect);
                }
                m_Flags = 0;
                break;

            case 'C':
                m_pWindows[m_ActiveBuffer]->cursorRightWithinMargin(1, rect);
                m_Flags = 0;
                break;

            case 'D':
                if(m_Modes & AnsiVt52)
                {
                    m_pWindows[m_ActiveBuffer]->cursorDown(1, rect);
                }
                else
                {
                    m_pWindows[m_ActiveBuffer]->cursorLeftWithinMargin(1, rect);
                }
                m_Flags = 0;
                break;

            case 'E':
                m_pWindows[m_ActiveBuffer]->cursorDownAndLeftToMargin(rect);
                m_Flags = 0;
                break;

            case 'F':
                /// \note We don't handle "\e F" (7-bit controls)
                // \eF is enabled by the hpLowerleftBugCompat resource

                /// \todo In VT52 mode, we need to go into VT52 graphics mode...
                m_Flags = 0;
                break;

            case 'G':
                /// \todo In VT52 mode, exit VT52 graphics mode...
                m_Flags = 0;
                break;

            case 'H':
                if(m_Modes & AnsiVt52)
                {
                    // Set tab stop.
                    m_TabStops[m_pWindows[m_ActiveBuffer]->getCursorX()] = '|';
                }
                else
                {
                    // Cursor to home.
                    m_pWindows[m_ActiveBuffer]->setCursor(0, 0, rect);
                }
                m_Flags = 0;
                break;

            case 'I':
                if(!(m_Modes & AnsiVt52))
                {
                    // Reverse line feed.
                    m_pWindows[m_ActiveBuffer]->cursorUp(1, rect);
                }
                m_Flags = 0;
                break;

            case 'J':
                if(!(m_Modes & AnsiVt52))
                {
                    m_pWindows[m_ActiveBuffer]->eraseDown(rect);
                }
                m_Flags = 0;
                break;

            case 'K':
                if(!(m_Modes & AnsiVt52))
                {
                    m_pWindows[m_ActiveBuffer]->eraseEOL(rect);
                }
                m_Flags = 0;
                break;

            case 'M':
                if((m_Modes & AnsiVt52) && !(m_Flags & Space))
                {
                    // Reverse Index.
                    m_pWindows[m_ActiveBuffer]->cursorUp(1, rect);
                }
                m_Flags = 0;
                break;

            case 'N':
            case 'O':
                // Single shift select for G2/3 character sets.
                m_Flags = 0;
                break;

            case 'Y':
                if(!(m_Modes & AnsiVt52))
                {
                    // Set cursor position.
                    /// \todo Do this, somehow. Need readahead or something.
                }
                m_Flags = 0;
                break;

            case 'Z':
                // Terminal identification.
                {
                    const char *answerback = 0;
                    if(m_Modes & AnsiVt52)
                        answerback = "\e[1;2c";
                    else
                        answerback = "\e/Z";

                    while(*answerback)
                    {
                        m_pT->addToQueue(*answerback);
                        ++answerback;
                    }
                }
                m_Flags = 0;
                break;
        }
    }
    else if((m_Flags & (Escape | LeftSquare | RightSquare | Underscore)) == (Escape | RightSquare))
    {
        switch(utf32)
        {
            case '\007':
            case 0x9C:
                if(!m_OsCtl.cur_param)
                {
                    syslog(LOG_INFO, "XTERM: not enough parameters for OS control");
                }
                else
                {
                    if(m_OsCtl.params[0] == "0" ||
                        m_OsCtl.params[0] == "1" ||
                        m_OsCtl.params[0] == "2")
                    {
                        g_pEmu->setTitle(m_OsCtl.params[1]);
                    }
                    else
                    {
                        syslog(LOG_INFO, "XTERM: unhandled OS control '%s'", m_OsCtl.params[0].c_str());
                    }
                }
                m_Flags = 0;
                break;

            case ';':
            case ':':
                m_OsCtl.cur_param++;
                break;

            default:
                if(utf32 >= ' ')
                {
                    m_OsCtl.params[m_OsCtl.cur_param] += static_cast<char>(utf32);
                }
                break;
        }
    }
    else if((m_Flags & (Escape | LeftSquare | RightSquare | Underscore)) == (Escape | Underscore))
    {
        // No application program commands in xterm.
        if(utf32 == 0x9C)
            m_Flags = 0;
    }

    m_pT->addToQueue(0, true);
}

void Xterm::renderAll(DirtyRectangle &rect)
{
    m_pWindows[m_ActiveBuffer]->renderAll(rect, 0);
}

Xterm::Window::Window(size_t nRows, size_t nCols, PedigreeGraphics::Framebuffer *pFb, size_t nMaxScrollback, size_t offsetLeft, size_t offsetTop, size_t fbWidth, Xterm *parent) :
    m_pBuffer(0), m_BufferLength(nRows*nCols), m_pFramebuffer(pFb), m_FbWidth(fbWidth), m_Width(nCols), m_Height(nRows), m_OffsetLeft(offsetLeft), m_OffsetTop(offsetTop), m_nMaxScrollback(nMaxScrollback), m_CursorX(0), m_CursorY(0), m_ScrollStart(0), m_ScrollEnd(nRows-1),
    m_pInsert(0), m_pView(0), m_Fg(g_DefaultFg), m_Bg(g_DefaultBg), m_Flags(0), m_bCursorFilled(true), m_bLineRender(false), m_pParentXterm(parent)
{
    // Using malloc() instead of new[] so we can use realloc()
    m_pBuffer = reinterpret_cast<TermChar*>(malloc(m_Width*m_Height*sizeof(TermChar)));

    TermChar blank;
    blank.fore = m_Fg;
    blank.back = m_Bg;
    blank.utf32 = ' ';
    blank.flags = 0;
    for (size_t i = 0; i < m_Width*m_Height; i++)
        m_pBuffer[i] = blank;

    if(g_Cairo)
    {
        cairo_save(g_Cairo);
        cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

        cairo_set_source_rgba(
                g_Cairo,
                ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
                ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
                ((g_Colours[m_Bg]) & 0xFF) / 256.0,
                0.8);

        cairo_rectangle(g_Cairo, m_OffsetLeft, m_OffsetTop, nCols * g_NormalFont->getWidth(), nRows * g_NormalFont->getHeight());
        cairo_fill(g_Cairo);

        cairo_restore(g_Cairo);
    }

    m_pInsert = m_pView = m_pBuffer;

    m_LeftMargin = 0;
    m_RightMargin = m_Width;
}

Xterm::Window::~Window()
{
    free(m_pBuffer);
}

void Xterm::Window::showCursor(DirtyRectangle &rect)
{
    render(rect, m_bCursorFilled ? XTERM_INVERSE : XTERM_BORDER);
}

void Xterm::Window::hideCursor(DirtyRectangle &rect)
{
    render(rect);
}

void Xterm::Window::resize(size_t nWidth, size_t nHeight, bool bActive)
{
    m_pFramebuffer = 0;

    size_t cols = nWidth / g_NormalFont->getWidth();
    size_t rows = nHeight / g_NormalFont->getHeight();

    g_NormalFont->setWidth(nWidth);
    g_BoldFont->setWidth(nWidth);

    if(bActive && m_Bg && g_Cairo)
    {
        cairo_save(g_Cairo);
        cairo_set_source_rgba(
                g_Cairo,
                ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
                ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
                ((g_Colours[m_Bg]) & 0xFF) / 256.0,
                1.0);

        cairo_rectangle(g_Cairo, m_OffsetLeft, m_OffsetTop, nWidth, nHeight);
        cairo_fill(g_Cairo);

        cairo_restore(g_Cairo);
    }

    TermChar *newBuf = reinterpret_cast<TermChar*>(malloc(cols*rows*sizeof(TermChar)));

    TermChar blank;
    blank.fore = m_Fg;
    blank.back = m_Bg;
    blank.utf32 = ' ';
    blank.flags = 0;
    for (size_t i = 0; i < cols*rows; i++)
        newBuf[i] = blank;

    for (size_t r = 0; r < m_Height; r++)
    {
        if (r >= rows) break;
        for (size_t c = 0; c < m_Width; c++)
        {
            if (c >= cols) break;

            newBuf[r*cols + c] = m_pInsert[r*m_Width + c];
        }
    }

    free(m_pBuffer);

    m_pInsert = m_pView = m_pBuffer = newBuf;
    m_BufferLength = rows*cols;

    m_FbWidth = nWidth;
    m_Width = cols;
    m_Height = rows;
    m_ScrollStart = 0;
    m_ScrollEnd = rows-1;
    if (m_CursorX >= m_Width)
        m_CursorX = 0;
    if (m_CursorY >= m_Height)
        m_CursorY = 0;

    if(m_RightMargin > cols)
        m_RightMargin = cols;
    if(m_LeftMargin > cols)
        m_LeftMargin = cols;

    // Set default tab stops.
    delete [] m_pParentXterm->m_TabStops;
    m_pParentXterm->m_TabStops = new char[cols];
    memset(m_pParentXterm->m_TabStops, 0, cols);
    for(size_t i = 0; i < cols; i += 8)
    {
        m_pParentXterm->m_TabStops[i] = '|';
    }
}

void Xterm::Window::setScrollRegion(int start, int end)
{
    if (start == -1)
        start = 0;
    if (end == -1)
        end = m_Height-1;
    m_ScrollStart = start;
    m_ScrollEnd = end;
}

void Xterm::Window::setForeColour(uint8_t fgColour)
{
    m_Fg = fgColour;
}

void Xterm::Window::setBackColour(uint8_t bgColour)
{
    m_Bg = bgColour;
}

void Xterm::Window::setFlags(uint8_t flags)
{
    m_Flags = flags;
}

uint8_t Xterm::Window::getFlags()
{
    return m_Flags;
}

void Xterm::Window::renderAll(DirtyRectangle &rect, Xterm::Window *pPrevious)
{
    TermChar *pOld = (pPrevious) ? pPrevious->m_pView : 0;
    TermChar *pNew = m_pView;

    // "Cleverer" full redraw - only redraw those glyphs that are different from the previous window.
    for (size_t y = 0; y < m_Height; y++)
    {
        for (size_t x = 0; x < m_Width; x++)
        {
//            if (!pOld || (pOld[y*m_Width+x] != pNew[y*m_Width+x]))
                render(rect, 0, x, y);
        }
    }

}

void Xterm::Window::setChar(uint32_t utf32, size_t x, size_t y)
{
    if(x > m_Width)
        return;
    if(y > m_Height)
        return;

    TermChar *c = &m_pInsert[y*m_Width+x];

    c->fore = m_Fg;
    c->back = m_Bg;

    c->flags = m_Flags;
    c->utf32 = utf32;
}

Xterm::Window::TermChar Xterm::Window::getChar(size_t x, size_t y)
{
    if (x == ~0UL) x = m_CursorX;
    if (y == ~0UL) y = m_CursorY;
    return m_pInsert[y*m_Width+x];
}

void Xterm::Window::cursorDown(size_t n, DirtyRectangle &rect)
{
    m_CursorY += n;
    if (m_CursorY > m_ScrollEnd)
    {
        size_t amount = m_CursorY-m_ScrollEnd;
        if (m_ScrollStart == 0 && m_ScrollEnd == m_Height-1)
        {
            scrollScreenUp(amount, rect);
        }
        else
        {
            scrollRegionUp(amount, rect);
        }
        m_CursorY = m_ScrollEnd;
    }
}

void Xterm::Window::cursorUp(size_t n, DirtyRectangle &rect)
{
    size_t amount = 0;
    if (n > m_CursorY)
    {
        amount = n - m_CursorY;
        m_CursorY = 0;
    }
    else
        m_CursorY -= n;

    if (amount)
    {
        if (m_ScrollStart == 0 && m_ScrollEnd == m_Height-1)
        {
            scrollRegionDown(amount, rect);
        }
        else
        {
            scrollRegionDown(amount, rect);
        }
    }
}

void Xterm::Window::cursorUpWithinMargin(size_t n, DirtyRectangle &rect)
{
    if (n > m_CursorY)
        n = m_CursorY;

    m_CursorY -= n;

    if (m_CursorY < m_ScrollStart)
        m_CursorY = m_ScrollStart;
}

void Xterm::Window::cursorLeftWithinMargin(size_t n, DirtyRectangle &)
{
    if (n > m_CursorX)
        n = m_CursorX;

    m_CursorX -= n;

    if (m_CursorX < m_LeftMargin)
        m_CursorX = m_LeftMargin;
}

void Xterm::Window::cursorRightWithinMargin(size_t n, DirtyRectangle &)
{
    m_CursorX += n;

    if (m_CursorX >= m_RightMargin)
        m_CursorY = m_RightMargin - 1;
}

void Xterm::Window::cursorDownWithinMargin(size_t n, DirtyRectangle &)
{
    m_CursorY += n;

    if (m_CursorY > m_ScrollEnd)
        m_CursorY = m_ScrollEnd;
}


void Xterm::Window::backspace(DirtyRectangle &rect)
{
    if(m_CursorX == m_RightMargin)
        --m_CursorX;

    if(m_CursorX > m_LeftMargin)
        --m_CursorX;
}

void Xterm::Window::render(DirtyRectangle &rect, size_t flags, size_t x, size_t y)
{
    if (x == ~0UL) x = m_CursorX;
    if (y == ~0UL) y = m_CursorY;

    TermChar c = m_pView[y*m_Width+x];

    // If both flags and c.flags have inverse, invert the inverse by
    // unsetting it in both sets of flags.
    if ((flags & XTERM_INVERSE) && (c.flags & XTERM_INVERSE))
    {
        c.flags &= ~XTERM_INVERSE;
        flags &= ~XTERM_INVERSE;
    }

    flags = c.flags | flags;

    uint32_t fg = g_Colours[c.fore];
    if (flags & XTERM_BRIGHTFG)
        fg = g_BrightColours[c.fore];
    uint32_t bg = g_Colours[c.back];
    if (flags & XTERM_BRIGHTBG)
        bg = g_BrightColours[c.back];

    if (flags & XTERM_INVERSE)
    {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    uint32_t utf32 = c.utf32;
    // char str[64];
    // sprintf(str, "Render(%d,%d,%d,%d)", m_OffsetLeft, m_OffsetTop, x, y);
    // log(str);
    // Ensure the painted area is marked dirty.
    rect.point(x*g_NormalFont->getWidth()+m_OffsetLeft, y*g_NormalFont->getHeight()+m_OffsetTop);
    rect.point((x+1)*g_NormalFont->getWidth()+m_OffsetLeft+1, (y+1)*g_NormalFont->getHeight()+m_OffsetTop);

    Font *pFont = g_NormalFont;
    if(flags & XTERM_BOLD)
    {
        pFont = g_BoldFont;
    }

    pFont->render(m_pFramebuffer, utf32,
                  x * pFont->getWidth() + m_OffsetLeft,
                  y * pFont->getHeight() + m_OffsetTop,
                  fg, bg);

    if(flags & XTERM_BORDER)
    {
        // Border around the cell.
        cairo_save(g_Cairo);
        cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

        cairo_set_line_width(g_Cairo, 1.0);
        cairo_set_line_join(g_Cairo, CAIRO_LINE_JOIN_MITER);
        cairo_set_antialias(g_Cairo, CAIRO_ANTIALIAS_NONE);

        cairo_set_source_rgba(
                g_Cairo,
                ((fg >> 16) & 0xFF) / 256.0,
                ((fg >> 8) & 0xFF) / 256.0,
                (fg & 0xFF) / 256.0,
                0.8);

        cairo_rectangle(
                g_Cairo,
                (x * pFont->getWidth()) + m_OffsetLeft + 1,
                (y * pFont->getHeight()) + m_OffsetTop + 1,
                pFont->getWidth() - 2,
                pFont->getHeight() - 2);
        cairo_stroke(g_Cairo);

        cairo_restore(g_Cairo);
    }
}

void Xterm::Window::scrollRegionUp(size_t n, DirtyRectangle &rect)
{

    /*
    +----------+
    |          |
    |----------| Top2       ^
    |----------| Top1       |
    |          |
    |----------| Bottom2    ^
    |----------| Bottom1    |
    |          |
    +----------+

    */

    size_t top1_y   = (m_ScrollStart+n) * g_NormalFont->getHeight();
    size_t top2_y   = m_ScrollStart * g_NormalFont->getHeight();

    // ScrollStart and ScrollEnd are *inclusive*, so add one to make the end exclusive.
    size_t bottom1_y   = (m_ScrollEnd+1) * g_NormalFont->getHeight();
    size_t bottom2_y   = (m_ScrollEnd+1-n) * g_NormalFont->getHeight();

    rect.point(m_OffsetLeft, top2_y + m_OffsetTop);
    rect.point(m_OffsetLeft + m_FbWidth, bottom1_y + m_OffsetTop);

    // If we're bitblitting, we need to commit all changes before now.
    doRedraw(rect);
    rect.reset();

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(g_Cairo, g_Surface, 0, -((double) top1_y));

    cairo_rectangle(g_Cairo, m_OffsetLeft, top2_y, m_FbWidth, bottom2_y - top2_y);
    cairo_fill(g_Cairo);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, m_OffsetLeft, bottom2_y, m_FbWidth, bottom1_y - bottom2_y);
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // m_pFramebuffer->copy(0, top1_y, m_OffsetLeft, top2_y, m_FbWidth, bottom2_y - top2_y);
    // m_pFramebuffer->rect(0, bottom2_y, m_FbWidth, bottom1_y - bottom2_y, g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, top2_y + m_OffsetTop);
    rect.point(m_OffsetLeft + m_FbWidth, bottom1_y + m_OffsetTop);

    memmove(&m_pBuffer[m_ScrollStart*m_Width], &m_pBuffer[(m_ScrollStart+n)*m_Width], (m_ScrollEnd+1-n-m_ScrollStart)*m_Width*sizeof(TermChar));

    TermChar blank;
    blank.fore = m_Fg;
    blank.back = m_Bg;
    blank.utf32 = ' ';
    blank.flags = 0;
    for (size_t i = m_Width*(m_ScrollEnd+1-n); i < m_Width*(m_ScrollEnd+1); i++)
        m_pBuffer[i] = blank;
}

void Xterm::Window::scrollRegionDown(size_t n, DirtyRectangle &rect)
{
    /*
    +----------+
    |          |
    |----------| Top1       |
    |----------| Top2       v
    |          |
    |----------| Bottom1    |
    |----------| Bottom2    v
    |          |
    +----------+

    */

    size_t top1_y   = m_ScrollStart * g_NormalFont->getHeight();
    size_t top2_y   = (m_ScrollStart+n) * g_NormalFont->getHeight();

    // ScrollStart and ScrollEnd are *inclusive*, so add one to make the end exclusive.
    size_t bottom1_y   = (m_ScrollEnd+1-n) * g_NormalFont->getHeight();
    size_t bottom2_y   = (m_ScrollEnd+1) * g_NormalFont->getHeight();

    rect.point(m_OffsetLeft, top1_y + m_OffsetTop);
    rect.point(m_OffsetLeft + m_FbWidth, bottom2_y + m_OffsetTop);

    // If we're bitblitting, we need to commit all changes before now.
    doRedraw(rect);
    rect.reset();

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_rectangle(g_Cairo, m_OffsetLeft, top2_y, m_FbWidth, bottom2_y - top2_y);

    cairo_push_group(g_Cairo);
    cairo_set_source_surface(g_Cairo, g_Surface, 0, top2_y);
    cairo_fill_preserve(g_Cairo);
    cairo_pop_group_to_source(g_Cairo);

    cairo_fill(g_Cairo);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, m_OffsetLeft, top1_y, m_FbWidth, top2_y - top1_y);
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // m_pFramebuffer->copy(0, top1_y, 0, top2_y, m_FbWidth, bottom2_y - top2_y);
    // m_pFramebuffer->rect(0, top1_y, m_FbWidth, top2_y - top1_y, g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, top1_y);
    rect.point(m_OffsetLeft + m_FbWidth, bottom2_y);

    memmove(&m_pBuffer[(m_ScrollStart+n)*m_Width], &m_pBuffer[((m_ScrollStart))*m_Width], ((m_ScrollEnd+1)-n-m_ScrollStart)*m_Width*sizeof(TermChar));

    TermChar blank;
    blank.fore = m_Fg;
    blank.back = m_Bg;
    blank.utf32 = ' ';
    blank.flags = 0;
    for (size_t i = m_Width*m_ScrollStart; i < m_Width*(m_ScrollStart+n); i++)
        m_pBuffer[i] = blank;
}

void Xterm::Window::scrollScreenUp(size_t n, DirtyRectangle &rect)
{
    size_t top_px   = 0;
    size_t top2_px   = n * g_NormalFont->getHeight();

    size_t bottom2_px = top_px + (m_Height-n)*g_NormalFont->getHeight();

    // If we're bitblitting, we need to commit all changes before now.
    cairo_surface_flush(g_Surface);

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(g_Cairo, g_Surface, 0, -((double) top2_px));

    cairo_rectangle(g_Cairo, 0, top_px, m_FbWidth, (m_Height - n) * g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, 0, bottom2_px, m_FbWidth, n * g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // m_pFramebuffer->copy(0, top2_px, 0, top_px, m_FbWidth, (m_Height - n) * g_NormalFont->getHeight());
    // m_pFramebuffer->rect(0, bottom2_px, m_FbWidth, n * g_NormalFont->getHeight(), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, top_px + m_OffsetTop);
    rect.point(m_OffsetLeft + m_FbWidth, bottom2_px + (n * g_NormalFont->getHeight()) + m_OffsetTop);

    bool bViewEqual = (m_pView == m_pInsert);
    memmove(m_pInsert, &m_pInsert[n*m_Width], ((m_Width * m_Height) - (n * m_Width)) * sizeof(TermChar));
    if(bViewEqual)
        m_pView = m_pInsert;

    // Blank out the remainder after moving the buffer
    for(size_t i = ((m_Width * m_Height) - (n * m_Width)); i < (m_Width * m_Height); i++)
    {
        TermChar blank;
        blank.fore = m_Fg;
        blank.back = m_Bg;
        blank.utf32 = ' ';
        blank.flags = 0;
        m_pView[i] = blank;
    }
    //m_pInsert += n*m_Width;

    // Have we gone off the end of the scroll area?
    if (m_pInsert + m_Height*m_Width >= (m_pBuffer+m_BufferLength))
    {
        // Either expand the buffer (if the length is less than the scrollback size) or memmove data up a bit.
        if (m_BufferLength < m_nMaxScrollback*m_Width)
        {
            // Expand.
            size_t amount = m_Width*m_Height;
            if (amount+m_BufferLength > m_nMaxScrollback*m_Width)
                amount = (m_nMaxScrollback*m_Width)-m_BufferLength;

            uintptr_t pInsOffset  = reinterpret_cast<uintptr_t>(m_pInsert) - reinterpret_cast<uintptr_t>(m_pBuffer);
            uintptr_t pViewOffset = reinterpret_cast<uintptr_t>(m_pView) - reinterpret_cast<uintptr_t>(m_pBuffer);

            m_pBuffer = reinterpret_cast<TermChar*>(realloc(m_pBuffer, (amount+m_BufferLength)*sizeof(TermChar)));

            m_pInsert = reinterpret_cast<TermChar*>(reinterpret_cast<uintptr_t>(m_pBuffer) + pInsOffset);
            m_pView   = reinterpret_cast<TermChar*>( reinterpret_cast<uintptr_t>(m_pBuffer) + pViewOffset);

            TermChar blank;
            blank.fore = m_Fg;
            blank.back = m_Bg;
            blank.utf32 = ' ';
            blank.flags = 0;
            for (size_t i = m_BufferLength; i < m_BufferLength+amount; i++)
                m_pBuffer[i] = blank;

            m_BufferLength += amount;
        }
        else
        {
            memmove(&m_pBuffer[m_Width*m_Height],
                    &m_pBuffer[0],
                    m_Width*m_Height*sizeof(TermChar));
            TermChar blank;
            blank.fore = m_Fg;
            blank.back = m_Bg;
            blank.utf32 = ' ';
            blank.flags = 0;
            for (size_t i = m_BufferLength-m_Width*m_Height; i < m_BufferLength; i++)
                m_pBuffer[i] = blank;

            m_BufferLength += m_Width*m_Height;
        }
    }
}

void Xterm::Window::setCursor(size_t x, size_t y, DirtyRectangle &rect)
{
    m_CursorX = x;
    m_CursorY = y;
}

void Xterm::Window::setCursorX(size_t x, DirtyRectangle &rect)
{
    setCursor(x, m_CursorY, rect);
}
void Xterm::Window::setCursorY(size_t y, DirtyRectangle &rect)
{
    setCursor(m_CursorX, y, rect);
}

size_t Xterm::Window::getCursorX()
{
    return m_CursorX;
}
size_t Xterm::Window::getCursorY()
{
    return m_CursorY;
}

void Xterm::Window::cursorLeft(DirtyRectangle &rect)
{
    if (m_CursorX == 0) return;
    m_CursorX --;
}

void Xterm::Window::cursorLeftNum(size_t n, DirtyRectangle &rect)
{
    if (m_CursorX == 0) return;

    if(n > m_CursorX) n = m_CursorX;

    m_CursorX -= n;
}

void Xterm::Window::cursorDownAndLeftToMargin(DirtyRectangle &rect)
{
    cursorDown(1, rect);
    m_CursorX = m_LeftMargin;
}

void Xterm::Window::cursorLeftToMargin(DirtyRectangle &rect)
{
    m_CursorX = m_LeftMargin;
}

void Xterm::Window::cursorTab(DirtyRectangle &rect)
{
    bool tabStopFound = false;
    for(size_t x = (m_CursorX + 1); x < m_RightMargin; ++x)
    {
        if(m_pParentXterm->m_TabStops[x] != 0)
        {
            m_CursorX = x;
            tabStopFound = true;
            break;
        }
    }

    if(!tabStopFound)
    {
        m_CursorX = m_RightMargin - 1;
    }
    else if(m_CursorX >= m_RightMargin)
        m_CursorX = m_RightMargin - 1;
}

void Xterm::Window::cursorTabBack(DirtyRectangle &rect)
{
    bool tabStopFound = false;
    for(ssize_t x = (m_CursorX - 1); x >= 0; --x)
    {
        if(m_pParentXterm->m_TabStops[x] != 0)
        {
            m_CursorX = x;
            tabStopFound = true;
            break;
        }
    }

    if(!tabStopFound)
    {
        m_CursorX = m_LeftMargin;
    }
    else if(m_CursorX < m_LeftMargin)
        m_CursorX = m_LeftMargin;
}

void Xterm::Window::addChar(uint32_t utf32, DirtyRectangle &rect)
{
    TermChar tc = getChar();
    setChar(utf32, m_CursorX, m_CursorY);
    if (getChar() != tc)
        render(rect);

    m_CursorX++;
    if ((m_CursorX % m_Width) == 0)
    {
        m_CursorX = 0;
        cursorDown(1, rect);
    }
}

void Xterm::Window::scrollUp(size_t n, DirtyRectangle &rect)
{
    scrollRegionDown(n, rect);
}

void Xterm::Window::scrollDown(size_t n, DirtyRectangle &rect)
{
    scrollRegionUp(n, rect);
}

void Xterm::Window::eraseScreen(DirtyRectangle &rect)
{
    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, 0, 0, m_FbWidth, m_Height * g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // One good fillRect should do the job nicely.
    // m_pFramebuffer->rect(0, 0, m_FbWidth, m_Height * g_NormalFont->getHeight(), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, m_OffsetTop);
    rect.point(m_OffsetLeft + m_FbWidth, m_OffsetTop + (m_Height * g_NormalFont->getHeight()));

    for(size_t row = 0; row < m_Height; row++)
    {
        for(size_t col = 0; col < m_Width; col++)
        {
            setChar(' ', col, row);
        }
    }
}

void Xterm::Window::eraseEOL(DirtyRectangle &rect)
{
    size_t l = (m_CursorX * g_NormalFont->getWidth());

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, l, m_CursorY * g_NormalFont->getHeight(), m_FbWidth - l, g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // Again, one fillRect should do it.
    //m_pFramebuffer->rect(l, m_CursorY * g_NormalFont->getHeight(), m_FbWidth - l, g_NormalFont->getHeight(), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, m_OffsetTop + (m_CursorY * g_NormalFont->getHeight()));
    rect.point(m_OffsetLeft + m_FbWidth, m_OffsetTop + ((m_CursorY + 1) * g_NormalFont->getHeight()));

    size_t row = m_CursorY;
    for(size_t col = m_CursorX; col < m_Width; col++)
    {
        setChar(' ', col, row);
    }
}

void Xterm::Window::eraseSOL(DirtyRectangle &rect)
{
    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, 0, m_CursorY * g_NormalFont->getHeight(), m_CursorY * g_NormalFont->getWidth(), g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // Again, one fillRect should do it.
    // m_pFramebuffer->rect(0, m_CursorY * g_NormalFont->getHeight(), m_CursorX * g_NormalFont->getWidth(), g_NormalFont->getHeight(), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, m_OffsetTop + (m_CursorY * g_NormalFont->getHeight()));
    rect.point(m_OffsetLeft + (m_CursorX * g_NormalFont->getWidth()), m_OffsetTop + ((m_CursorY + 1) * g_NormalFont->getHeight()));

    size_t row = m_CursorY;
    for(size_t col = 0; col < m_CursorX; col++)
    {
        setChar(' ', col, row);
    }
}

void Xterm::Window::eraseLine(DirtyRectangle &rect)
{
    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, 0, m_CursorY * g_NormalFont->getHeight(), m_FbWidth - m_OffsetLeft, g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // Again, one fillRect should do it.
    //m_pFramebuffer->rect(0, m_CursorY * g_NormalFont->getHeight(), m_FbWidth - m_OffsetLeft, g_NormalFont->getHeight(), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, m_OffsetTop + (m_CursorY * g_NormalFont->getHeight()));
    rect.point(m_OffsetLeft + m_FbWidth, m_OffsetTop + ((m_CursorY + 1) * g_NormalFont->getHeight()));

    size_t row = m_CursorY;
    for(size_t col = 0; col < m_Width; col++)
    {
        setChar(' ', col, row);
    }
}

void Xterm::Window::eraseChars(size_t n, DirtyRectangle &rect)
{
    // Again, one fillRect should do it.
    size_t left = (m_CursorX * g_NormalFont->getWidth());
    if((m_CursorX + n) > m_Width)
        n = m_Width - m_CursorX;
    size_t width = n * g_NormalFont->getWidth();

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, left, m_CursorY * g_NormalFont->getHeight(), width, g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // m_pFramebuffer->rect(left, m_CursorY * g_NormalFont->getHeight(), width, g_NormalFont->getHeight(), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, m_OffsetTop + (m_CursorY * g_NormalFont->getHeight()));
    rect.point(m_OffsetLeft + width, m_OffsetTop + ((m_CursorY + 1) * g_NormalFont->getHeight()));

    size_t row = m_CursorY;
    for(size_t col = m_CursorX; col < (m_CursorX + n); col++)
    {
        setChar(' ', col, row);
    }
}

void Xterm::Window::eraseUp(DirtyRectangle &rect)
{
    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, 0, 0, m_FbWidth, g_NormalFont->getHeight() * m_CursorY);
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // m_pFramebuffer->rect(0, 0, m_FbWidth, g_NormalFont->getHeight() * m_CursorY, g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, m_OffsetTop);
    rect.point(m_OffsetLeft + m_FbWidth, m_OffsetTop + ((m_CursorY + 1) * g_NormalFont->getHeight()));

    for(size_t row = 0; row < m_CursorY; row++)
    {
        for(size_t col = 0; col < m_Width; col++)
        {
            setChar(' ', col, row);
        }
    }
}

void Xterm::Window::eraseDown(DirtyRectangle &rect)
{
    size_t top = m_CursorY * g_NormalFont->getHeight();

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, 0, top, m_FbWidth, g_NormalFont->getHeight() * (m_Height - m_CursorY));
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // m_pFramebuffer->rect(0, top, m_FbWidth, g_NormalFont->getHeight() * (m_Height - m_CursorY), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft, top + m_OffsetTop);
    rect.point(m_OffsetLeft + m_FbWidth, top + m_OffsetTop + ((m_Height - m_CursorY) * g_NormalFont->getHeight()));

    for(size_t row = m_CursorY; row < m_Height; row++)
    {
        for(size_t col = 0; col < m_Width; col++)
        {
            setChar(' ', col, row);
        }
    }
}

void Xterm::Window::deleteCharacters(size_t n, DirtyRectangle &rect)
{
    // Start of the delete region
    size_t deleteStart = m_CursorX;

    // End of the delete region
    size_t deleteEnd = (deleteStart + n);

    // Number of characters to shift
    size_t numChars = m_Width - deleteEnd;

    // Shift all the characters across from the end of the delete area to the start.
    memmove(&m_pBuffer[(m_CursorY * m_Width) + deleteStart], &m_pBuffer[(m_CursorY * m_Width) + deleteEnd], numChars * sizeof(TermChar));

    // Now that the characters have been shifted, clear the space after the region we copied
    syslog(LOG_INFO, "w=%d h=%d", m_Width, m_Height);
    size_t left = (m_Width - n) * g_NormalFont->getWidth();
    size_t top = m_CursorY * g_NormalFont->getHeight();

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, left, top, n * g_NormalFont->getWidth(), g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // m_pFramebuffer->rect(left, top, n * g_NormalFont->getWidth(), g_NormalFont->getHeight(), g_Colours[m_Bg], PedigreeGraphics::Bits24_Rgb);

    // Update the moved section
    size_t row = m_CursorY, col = 0;
    for(col = deleteStart; col < (m_Width - n); col++)
    {
        render(rect, 0, col, row);
    }

    // And then update the cleared section
    for(col = (m_Width - n); col < m_Width; col++)
    {
        setChar(' ', col, row);
    }
}

void Xterm::Window::insertCharacters(size_t n, DirtyRectangle &rect)
{
    // Start of the insertion region
    size_t insertStart = m_CursorX;

    // End of the insertion region
    size_t insertEnd = insertStart + n;

    // Number of characters to shift.
    size_t numChars = m_Width - insertEnd;

    // Shift characters.
    memmove(&m_pBuffer[(m_CursorY * m_Width) + insertEnd], &m_pBuffer[(m_CursorY * m_Width) + insertStart], numChars);

    // Now that the characters have been shifted, clear the space inside the region we inserted
    size_t left = insertStart * g_NormalFont->getWidth();
    size_t top = m_CursorY * g_NormalFont->getHeight();

    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((g_Colours[m_Bg] >> 16) & 0xFF) / 256.0,
            ((g_Colours[m_Bg] >> 8) & 0xFF) / 256.0,
            ((g_Colours[m_Bg]) & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, left, top, n * g_NormalFont->getWidth(), g_NormalFont->getHeight());
    cairo_fill(g_Cairo);

    cairo_restore(g_Cairo);

    // Update the inserted section
    size_t row = m_CursorY, col = 0;
    for(col = insertStart; col < insertEnd; col++)
    {
        setChar(' ', col, row);
    }

    // Update the moved section
    for(col = insertStart; col < m_Width; col++)
    {
        render(rect, 0, col, row);
    }
}

void Xterm::Window::insertLines(size_t n, DirtyRectangle &rect)
{
    // If we'll go past the end of the screen in doing this, we can just erase.
    if((m_CursorY + 1 + n) >= m_ScrollEnd)
    {
        ++m_CursorY;
        eraseDown(rect);
        --m_CursorY;
    }
    else
    {
        // Otherwise, we need to scroll and then do an erase.
        size_t oldStart = m_ScrollStart;
        m_ScrollStart = m_CursorY + 1;

        scrollRegionDown(n, rect);

        size_t savedY = m_CursorY;
        for(size_t newY = savedY + 1; newY < (savedY + 1 + n); ++newY)
        {
            m_CursorY = newY;
            eraseLine(rect);
        }
        m_CursorY = savedY;

        m_ScrollStart = oldStart;
    }
}

void Xterm::Window::deleteLines(size_t n, DirtyRectangle &rect)
{
    /// \todo I have no idea if this will actually work.

    // If we'll go past the end of the screen in doing this, we can just erase.
    if((m_CursorY + 1 + n) >= m_ScrollEnd)
    {
        ++m_CursorY;
        eraseDown(rect);
        --m_CursorY;
    }
    else
    {
        // Otherwise, we need to scroll and then do an erase.
        size_t oldStart = m_ScrollStart;
        m_ScrollStart = m_CursorY + 1;

        scrollScreenUp(n, rect);

        size_t savedY = m_CursorY;
        for(size_t newY = savedY + n + 1; newY < (savedY + 1 + n); ++newY)
        {
            m_CursorY = newY;
            eraseLine(rect);
        }
        m_CursorY = savedY;

        m_ScrollStart = oldStart;
    }
}

void Xterm::Window::lineRender(uint32_t utf32, DirtyRectangle &rect)
{
    syslog(LOG_NOTICE, "line render: %c", utf32);

    /*
    if(m_CursorX > m_Width)
        return;
    if(m_CursorY > m_Height)
        return;
    */

    size_t left = m_OffsetLeft + (m_CursorX * g_NormalFont->getWidth());
    size_t top = m_OffsetTop + (m_CursorY * g_NormalFont->getHeight());

    m_CursorX++;
    if(m_CursorX > m_Width)
    {
        m_CursorX = 0;
        cursorDown(1, rect);
    }

    size_t fullWidth = g_NormalFont->getWidth();
    size_t fullHeight = g_NormalFont->getHeight();

    size_t halfWidth = (fullWidth / 2) + 1;
    size_t halfHeight = (fullHeight / 2) + 1;

    uint32_t bgColourInt = g_Colours[m_Bg];
    uint32_t fgColourInt = g_Colours[m_Fg];


    cairo_save(g_Cairo);
    cairo_set_operator(g_Cairo, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(
            g_Cairo,
            ((bgColourInt >> 16) & 0xFF) / 256.0,
            ((bgColourInt >> 8) & 0xFF) / 256.0,
            (bgColourInt & 0xFF) / 256.0,
            0.8);

    cairo_rectangle(g_Cairo, left, top, fullWidth, fullHeight);
    cairo_fill(g_Cairo);

    cairo_set_source_rgba(
            g_Cairo,
            ((fgColourInt >> 16) & 0xFF) / 256.0,
            ((fgColourInt >> 8) & 0xFF) / 256.0,
            (fgColourInt & 0xFF) / 256.0,
            1.0);

    // m_pFramebuffer->rect(left, top, fullWidth, fullHeight, bgColourInt, PedigreeGraphics::Bits24_Rgb);

    rect.point(m_OffsetLeft + left, m_OffsetTop + top);
    rect.point(m_OffsetLeft + left + fullWidth, m_OffsetTop + top + fullHeight);

    switch(utf32 & 0xFF)
    {
        case 'j':
            // Bottom right corner

            // Middle left to Center
            cairo_move_to(g_Cairo, left, top + halfHeight);
            cairo_line_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left, top + halfHeight, left + halfWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Center to Middle top
            cairo_move_to(g_Cairo, left + halfWidth, top);
            cairo_line_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top, left + halfWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'k':
            // Upper right corner

            // Middle left to Center
            cairo_move_to(g_Cairo, left, top + halfHeight);
            cairo_line_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left, top + halfHeight, left + halfWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Center to Middle bottom
            cairo_move_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_line_to(g_Cairo, left + halfWidth, top + fullHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top + halfHeight, left + halfWidth, top + fullHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'l':
            // Upper left corner

            // Center to Middle right
            cairo_move_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_line_to(g_Cairo, left + fullWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top + halfHeight, left + fullWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Center to Middle bottom
            cairo_move_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_line_to(g_Cairo, left + halfWidth, top + fullHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top + halfHeight, left + halfWidth, top + fullHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'm':
            // Lower left corner

            // Center to Middle top
            cairo_move_to(g_Cairo, left + halfWidth, top);
            cairo_line_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top, left + halfWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Center to Middle right
            cairo_move_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_line_to(g_Cairo, left + fullWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top + halfHeight, left + fullWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'n':
            // Crossing lines

            // Middle left to Middle right
            cairo_move_to(g_Cairo, left, top + halfHeight);
            cairo_line_to(g_Cairo, left + fullWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left, top + halfHeight, left + fullWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Middle top to Middle bottom
            cairo_move_to(g_Cairo, left + halfWidth, top);
            cairo_line_to(g_Cairo, left + halfWidth, top + fullHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top, left + halfWidth, top + fullHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'q':
            // Horizontal line

            // Middle left to Middle right
            cairo_move_to(g_Cairo, left, top + halfHeight);
            cairo_line_to(g_Cairo, left + fullWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left, top + halfHeight, left + fullWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 't':
            // Left 'T'
            cairo_move_to(g_Cairo, left + halfWidth, top);
            cairo_line_to(g_Cairo, left + halfWidth, top + fullHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top, left + halfWidth, top + fullHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            cairo_move_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_line_to(g_Cairo, left + fullWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top + halfHeight, left + fullWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'u':
            // Right 'T'

            // Middle top to Middle bottom
            cairo_move_to(g_Cairo, left + halfWidth, top);
            cairo_line_to(g_Cairo, left + halfWidth, top + fullHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top, left + halfWidth, top + fullHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Middle left to Center
            cairo_move_to(g_Cairo, left, top + halfHeight);
            cairo_line_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left, top + halfHeight, left + halfWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'v':
            // Bottom 'T'

            // Middle left to Middle right
            cairo_move_to(g_Cairo, left, top + halfHeight);
            cairo_line_to(g_Cairo, left + fullWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left, top + halfHeight, left + fullWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Middle top to Center
            cairo_move_to(g_Cairo, left + halfWidth, top);
            cairo_line_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top, left + halfWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'w':
            // Top 'T'

            // Middle left to Middle right
            cairo_move_to(g_Cairo, left, top + halfHeight);
            cairo_line_to(g_Cairo, left + fullWidth, top + halfHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left, top + halfHeight, left + fullWidth, top + halfHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);

            // Middle bottom to Center
            cairo_move_to(g_Cairo, left + halfWidth, top + halfHeight);
            cairo_line_to(g_Cairo, left + halfWidth, top + fullHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top + halfHeight, left + halfWidth, top + fullHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        case 'x':
            // Vertical line

            // Middle top to Middle bottom
            cairo_move_to(g_Cairo, left + halfWidth, top);
            cairo_line_to(g_Cairo, left + halfWidth, top + fullHeight);
            cairo_stroke(g_Cairo);

            //m_pFramebuffer->line(left + halfWidth, top, left + halfWidth, top + fullHeight, fgColourInt, PedigreeGraphics::Bits24_Rgb);
            break;
        default:
            break;
    }

    cairo_restore(g_Cairo);
}

void Xterm::Window::invert(DirtyRectangle &rect)
{
    // Invert the entire screen, if using default colours.
    TermChar *pNew = m_pView;
    for (size_t y = 0; y < m_Height; y++)
    {
        for (size_t x = 0; x < m_Width; x++)
        {
            TermChar *pChar = &m_pView[(y * m_Width) + x];
            if((pChar->fore == g_DefaultFg) && (pChar->back == g_DefaultBg))
            {
                uint8_t fore = pChar->fore;
                pChar->fore = pChar->back;
                pChar->back = fore;
            }
        }
    }

    // Redraw.
    renderAll(rect, this);
}

void Xterm::Window::setTabStop()
{
    m_pParentXterm->m_TabStops[m_CursorX] = '|';
}

void Xterm::Window::clearTabStop()
{
    m_pParentXterm->m_TabStops[m_CursorX] = 0;
}

void Xterm::Window::clearAllTabStops()
{
    memset(m_pParentXterm->m_TabStops, 0, m_Width);
}
