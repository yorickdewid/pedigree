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

#define _USE_MATH_DEFINES
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <string.h>
#include <sched.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <sys/fb.h>

#include <map>
#include <list>
#include <set>
#include <queue>

#include <graphics/Graphics.h>
#include <input/Input.h>

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>

#include <protocol.h>

#include "winman.h"
#include "Png.h"

#define DEBUG_REDRAWS          0

RootContainer *g_pRootContainer = 0;

Window *g_pFocusWindow = 0;

uint8_t *g_pBackbuffer = 0;

std::map<uint64_t, Window*> *g_Windows;

std::set<WObject*> g_PendingWindows;

/// UNIX socket (or UDP before UNIX sockets are implemented)
int g_iSocket = 0;

/// Control pipe for us to wake the main loop when state changes.
int g_iControlPipe[2] = {0};

std::string g_StatusField = "";

std::queue<Input::InputNotification> g_InputQueue;

void queueInputCallback(Input::InputNotification &note);

ssize_t g_nWidth = 0;
ssize_t g_nHeight = 0;

ssize_t g_CursorX = 0, g_LastCursorX = 0;
ssize_t g_CursorY = 0, g_LastCursorY = 0;
bool g_bCursorUpdate = false;

/// \todo Make configurable.
#define CLIENT_DEFAULT "/applications/TUI"
// #define CLIENT_DEFAULT "/applications/gears"

#define TEXTONLY_DEFAULT "/applications/ttyterm"

void startClient()
{
    pid_t pid = fork();
    if(pid == -1)
    {
        syslog(LOG_CRIT, "winman: fork failed: %s\n", strerror(errno));
    }
    else if(pid == 0)
    {
        // Forked. Close all of our existing handles, we don't really want the
        // client to inherit them.
        /// \todo Is it worth making these handles FD_CLOEXEC for this reason?
        close(g_iControlPipe[0]);
        close(g_iControlPipe[1]);
        close(g_iSocket);

        execl(CLIENT_DEFAULT, CLIENT_DEFAULT, 0);
        exit(1);
    }
}

void fps()
{
    static unsigned int frames = 0;
    static unsigned int start_time = 0;

    struct timeval now;
    gettimeofday(&now, NULL);
    frames++;
    if (!start_time)
    {
        start_time = now.tv_sec;
    }
    else if (now.tv_sec - start_time >= 5)
    {
        float seconds = now.tv_sec - start_time;
        float fps = frames / seconds;
        syslog(LOG_INFO, "%d frames in %3.1f seconds = %6.3f FPS", frames, seconds, fps);
        start_time = now.tv_sec;
        frames = 0;
    }
}

void handleMessage(char *messageData, struct sockaddr *src, socklen_t slen)
{
    bool bResult = false;
    LibUiProtocol::WindowManagerMessage *pWinMan =
        reinterpret_cast<LibUiProtocol::WindowManagerMessage*>(messageData);

    size_t totalSize = sizeof(LibUiProtocol::WindowManagerMessage);

    if(pWinMan->messageCode == LibUiProtocol::Create)
    {
        LibUiProtocol::CreateMessage *pCreate =
        reinterpret_cast<LibUiProtocol::CreateMessage*>(messageData + sizeof(LibUiProtocol::WindowManagerMessage));
        char *responseData = new char[totalSize];

        Container *pParent = g_pRootContainer;
        if(g_pFocusWindow)
        {
            pParent = g_pFocusWindow->getParent();
        }

        /// \todo Error handling.
        int iSocket = socket(AF_UNIX, SOCK_DGRAM, 0);
        connect(iSocket, src, slen);

        Window *pWindow = new Window(pWinMan->widgetHandle, iSocket, pParent);
        std::string newTitle(pCreate->title);
        pWindow->setTitle(newTitle);
        g_PendingWindows.insert(pWindow);

        if(g_pFocusWindow)
        {
            g_pFocusWindow->nofocus();
        }
        g_pFocusWindow = pWindow;
        pWindow->focus();

        LibUiProtocol::WindowManagerMessage *pHeader =
            reinterpret_cast<LibUiProtocol::WindowManagerMessage*>(responseData);
        pHeader->messageCode = LibUiProtocol::Create;
        pHeader->widgetHandle = pWinMan->widgetHandle;
        pHeader->messageSize = sizeof(LibUiProtocol::CreateMessageResponse);
        pHeader->isResponse = true;

        LibUiProtocol::CreateMessageResponse *pCreateResp =
            reinterpret_cast<LibUiProtocol::CreateMessageResponse*>(responseData + sizeof(LibUiProtocol::WindowManagerMessage));

        g_Windows->insert(std::make_pair(pWinMan->widgetHandle, pWindow));

        send(iSocket, responseData, totalSize, 0);

        delete [] responseData;
    }
    else if(pWinMan->messageCode == LibUiProtocol::Sync)
    {
        std::map<uint64_t, Window*>::iterator it = g_Windows->find(pWinMan->widgetHandle);
        if(it != g_Windows->end())
        {
            Window *pWindow = it->second;
            char *responseData = new char[totalSize];

            LibUiProtocol::WindowManagerMessage *pHeader =
                reinterpret_cast<LibUiProtocol::WindowManagerMessage*>(responseData);
            pHeader->messageCode = LibUiProtocol::Sync;
            pHeader->widgetHandle = pWinMan->widgetHandle;
            pHeader->messageSize = sizeof(LibUiProtocol::SyncMessageResponse);
            pHeader->isResponse = true;

            LibUiProtocol::SyncMessageResponse *pSyncResp =
                reinterpret_cast<LibUiProtocol::SyncMessageResponse*>(responseData + sizeof(LibUiProtocol::WindowManagerMessage));

            send(pWindow->getSocket(), responseData, totalSize, 0);

            delete [] responseData;
        }
    }
    else if(pWinMan->messageCode == LibUiProtocol::RequestRedraw)
    {
        LibUiProtocol::RequestRedrawMessage *pRedrawMsg =
            reinterpret_cast<LibUiProtocol::RequestRedrawMessage*>(messageData + sizeof(LibUiProtocol::WindowManagerMessage));
        PedigreeGraphics::Rect dirty(pRedrawMsg->x, pRedrawMsg->y, pRedrawMsg->width, pRedrawMsg->height);

        std::map<uint64_t, Window*>::iterator it = g_Windows->find(pWinMan->widgetHandle);
        if(it != g_Windows->end())
        {
            Window *pWindow = it->second;
            pWindow->setDirty(dirty);
            g_PendingWindows.insert(pWindow);
        }
    }
    else if(pWinMan->messageCode == LibUiProtocol::Destroy)
    {
        std::map<uint64_t, Window*>::iterator it = g_Windows->find(pWinMan->widgetHandle);
        if(it != g_Windows->end())
        {
            Window *pWindow = it->second;

            Container *myParent = pWindow->getParent();
            Window *newFocus = 0;
            WObject *sibling = 0;

            // Find any siblings if we can.
            WObject *siblingObject = myParent->getLeftSibling(pWindow);
            if(!siblingObject)
            {
                siblingObject = myParent->getRightSibling(pWindow);
            }

            // Will there be any children left?
            if(myParent->getChildCount() > 1)
            {
                // Yes. Focus adjustment is mostly trivial.
            }
            else
            {
                // No. Focus adjustment is less trivial.
                siblingObject = myParent->getLeft(pWindow);
                if(!siblingObject)
                {
                    siblingObject = myParent->getRight(pWindow);
                }
                if(!siblingObject)
                {
                    siblingObject = myParent->getUp(pWindow);
                }
                if(!siblingObject)
                {
                    siblingObject = myParent->getDown(pWindow);
                }
            }

            // Remove from the parent container.
            myParent->removeChild(pWindow);
            if(myParent->getChildCount() > 0)
            {
                myParent->retile();
            }
            else if(myParent->getParent())
            {
                Container *pContainer = static_cast<Container*>(myParent->getParent());
                pContainer->removeChild(myParent);
                pContainer->retile();
                myParent = pContainer;
            }

            // Assign new focus.
            if(siblingObject)
            {
                while(siblingObject->getType() == WObject::Container)
                {
                    Container *pContainer = static_cast<Container*>(siblingObject);
                    siblingObject = pContainer->getFocusWindow();
                }

                if(siblingObject->getType() == WObject::Window)
                {
                    newFocus = static_cast<Window*>(siblingObject);
                }
            }

            // All children are now pending a redraw.
            g_PendingWindows.insert(myParent);

            // Switch focus, if needed.
            if(g_pFocusWindow == pWindow)
            {
                g_pFocusWindow = newFocus;
                newFocus->focus();

                g_PendingWindows.insert(newFocus);
            }

            char *responseData = new char[totalSize];
            LibUiProtocol::WindowManagerMessage *pHeader =
                reinterpret_cast<LibUiProtocol::WindowManagerMessage*>(responseData);
            pHeader->messageCode = LibUiProtocol::Destroy;
            pHeader->widgetHandle = pWinMan->widgetHandle;
            pHeader->messageSize = 0;
            pHeader->isResponse = true;

            send(pWindow->getSocket(), responseData, totalSize, 0);

            delete [] responseData;

            // Clean up!
            delete pWindow;
            g_Windows->erase(it);
        }
    }
    else if(pWinMan->messageCode == LibUiProtocol::Nothing)
    {
        // We inject this to wake up checkForMessages and move on to rendering.
    }
    else if(pWinMan->messageCode == LibUiProtocol::SetTitle)
    {
        LibUiProtocol::SetTitleMessage *pTitleMsg =
            reinterpret_cast<LibUiProtocol::SetTitleMessage*>(messageData + sizeof(LibUiProtocol::WindowManagerMessage));

        std::map<uint64_t, Window*>::iterator it = g_Windows->find(pWinMan->widgetHandle);
        if(it != g_Windows->end())
        {
            Window *pWindow = it->second;
            pWindow->setTitle(std::string(pTitleMsg->newTitle));
            g_PendingWindows.insert(pWindow);
        }
    }
    else
    {
        syslog(LOG_INFO, "winman: unhandled message type");
    }
}

void checkForMessages()
{
    fd_set fds;
    FD_ZERO(&fds);

    // Prepare the set for select()ing on.
    FD_SET(g_iSocket, &fds);
    FD_SET(g_iControlPipe[0], &fds);
    int nMax = std::max(g_iSocket, g_iControlPipe[0]);

    // Do the deed - no timeout.
    int ret = select(nMax + 1, &fds, 0, 0, 0);
    if(ret > 0)
    {
        if(FD_ISSET(g_iSocket, &fds))
        {
            // Socket has a datagram ready to handle.
            // We use recvfrom so we can create a socket back to the client easily.
            char msg[4096];
            struct sockaddr_un saddr;
            socklen_t slen = 0;
            ssize_t sz = recvfrom(g_iSocket, msg, 4096, 0, (struct sockaddr *) &saddr, &slen);
            if(sz > 0)
            {
                // Handle!
                handleMessage(msg, (struct sockaddr *) &saddr, slen);
            }
        }

        if(FD_ISSET(g_iControlPipe[0], &fds))
        {
            // Read a single byte (which will mean multiple writes to the pipe
            // will wake select() up multiple times - good for the input queue).
            char buf[2];
            read(g_iControlPipe[0], buf, 1);

            // Handle any pending input in the input queue.
            if(!g_InputQueue.empty())
            {
                Input::InputNotification n = g_InputQueue.front();
                g_InputQueue.pop();

                queueInputCallback(n);
            }
        }
    }
}

#define ALT_KEY (1ULL << 60)
#define SHIFT_KEY (1ULL << 61)
#define SPECIAL_KEY (1ULL << 63)

enum ActualKey
{
    None,
    Left,
    Right,
    Up,
    Down,
};

/**
 * Handles input, called on notifications stored on a queue.
 *
 * This is done so we handle the kernel event very quickly (by pushing onto a
 * queue), and helps avoid race conditions where we modify global state (such
 * as the "pending windows" list).
 */
void queueInputCallback(Input::InputNotification &note)
{
    syslog(LOG_INFO, "winman: system input (type=%d)", note.type);
    static bool bResize = false;

    bool bHandled = false;
    bool bWakeup = false;
    if(note.type & Input::Key)
    {
        uint64_t c = note.data.key.key;

        ActualKey realKey = None;

        if(c & SPECIAL_KEY)
        {
            uint32_t k = c & 0xFFFFFFFFULL;
            char str[5];
            memcpy(str, reinterpret_cast<char*>(&k), 4);
            str[4] = 0;

            if(!strcmp(str, "left"))
            {
                realKey = Left;
            }
            else if(!strcmp(str, "righ"))
            {
                realKey = Right;
            }
            else if(!strcmp(str, "up"))
            {
                realKey = Up;
            }
            else if(!strcmp(str, "down"))
            {
                realKey = Down;
            }
        }

        /// \todo make configurable
        if((c & ALT_KEY) && (g_pFocusWindow != 0))
        {
            bool bShift = (c & SHIFT_KEY);

            syslog(LOG_INFO, "ALT-%d [%x%x] %c", (uint32_t) c, (uint32_t) (c >> 32ULL), (uint32_t) c, (char) c);

            c &= 0xFFFFFFFFULL;
            Container *focusParent = g_pFocusWindow->getParent();
            Window *newFocus = 0;
            WObject *sibling = 0;
            if(bShift)
            {
                if(c == 'R')
                {
                    g_pRootContainer->retile();
                    bHandled = true;
                }
                else if(c == 'Q')
                {
                    // Are we the only child of the root container?
                    bool bSafe = true;
                    if(focusParent == g_pRootContainer)
                    {
                        if(focusParent->getChildCount() == 1)
                        {
                            syslog(LOG_INFO, "winman: can't (yet) terminate only child of root container!");
                            bSafe = false;
                        }
                    }

                    if(bSafe)
                    {
                        size_t totalSize = sizeof(LibUiProtocol::WindowManagerMessage);
                        char *buffer = new char[totalSize];

                        LibUiProtocol::WindowManagerMessage *pHeader =
                            reinterpret_cast<LibUiProtocol::WindowManagerMessage*>(buffer);
                        pHeader->widgetHandle = g_pFocusWindow->getHandle();
                        pHeader->messageSize = 0;
                        pHeader->messageCode = LibUiProtocol::Destroy;
                        pHeader->isResponse = false;

                        send(g_pFocusWindow->getSocket(), buffer, totalSize, 0);

                        delete [] buffer;

                        bHandled = true;
                    }
                }
            }
            else if(c == '\n')
            {
                // Add window to active container.
                startClient();
                bHandled = true;
            }
            else if((c == 'v') || (c == 'h'))
            {
                ::Container::Layout layout;
                if(c == 'v')
                {
                    layout = Container::Stacked;
                }
                else if(c == 'h')
                {
                    layout = Container::SideBySide;
                }
                else
                {
                    layout = Container::SideBySide;
                }

                // Replace focus window with container (Container::replace) in
                // the relevant layout mode.
                if(focusParent->getChildCount() == 1)
                {
                    focusParent->setLayout(layout);
                }
                else if(focusParent->getLayout() != layout)
                {
                    Container *pNewContainer = new Container(focusParent);
                    pNewContainer->addChild(g_pFocusWindow, true);
                    g_pFocusWindow->setParent(pNewContainer);

                    focusParent->replaceChild(g_pFocusWindow, pNewContainer);
                    focusParent->retile();
                    pNewContainer->setLayout(layout);
                }

                bHandled = true;
            }
            else if(c == 'r')
            {
                // State machine: enter 'resize mode', which allows us to resize
                // the container in which the window with focus is in.
                bResize = true;
                bHandled = true;
                bWakeup = true;
                g_StatusField = "<resize mode>";

                // Don't transmit resizes to the client(s) yet.
                g_pRootContainer->norefresh();
            }
            else if(realKey != None)
            {
                if(realKey == Left)
                {
                    sibling = focusParent->getLeft(g_pFocusWindow);
                }
                else if(realKey == Up)
                {
                    sibling = focusParent->getUp(g_pFocusWindow);
                }
                else if(realKey == Down)
                {
                    sibling = focusParent->getDown(g_pFocusWindow);
                }
                else if(realKey == Right)
                {
                    sibling = focusParent->getRight(g_pFocusWindow);
                }

                // Not edge of screen?
                if(sibling)
                {
                    while(sibling->getType() == WObject::Container)
                    {
                        Container *pContainer = static_cast<Container*>(sibling);
                        sibling = pContainer->getFocusWindow();
                    }

                    if(sibling->getType() == WObject::Window)
                    {
                        newFocus = static_cast<Window*>(sibling);
                    }
                }

                bHandled = true;
            }

            if(newFocus)
            {
                if(g_pFocusWindow)
                {
                    g_PendingWindows.insert(g_pFocusWindow);
                    g_pFocusWindow->nofocus();
                }

                g_PendingWindows.insert(newFocus);

                g_pFocusWindow = newFocus;
                g_pFocusWindow->focus();
                bWakeup = true;
            }
        }

        if((!bHandled) && bResize && (g_pFocusWindow))
        {
            bWakeup = true;
            bHandled = true;

            Container *focusParent = g_pFocusWindow->getParent();
            WObject *sibling = 0;
            if(c == '\e')
            {
                g_StatusField = " ";
                bResize = false;

                // Okay, now the client(s) can get a refreshed context.
                g_pRootContainer->yesrefresh();
            }
            else if(realKey == Left)
            {
                sibling = focusParent->getLeft(g_pFocusWindow);
                if(sibling)
                {
                    sibling->resize(-10, 0);
                }
            }
            else if(realKey == Right)
            {
                sibling = focusParent->getRight(g_pFocusWindow);
                if(sibling)
                {
                    g_pFocusWindow->resize(10, 0);
                }
            }
            else if(realKey == Up)
            {
                sibling = focusParent->getUp(g_pFocusWindow);
                if(sibling)
                {
                    sibling->resize(0, -10);
                }
            }
            else if(realKey == Down)
            {
                sibling = focusParent->getDown(g_pFocusWindow);
                if(sibling)
                {
                    g_pFocusWindow->resize(0, 10);
                }
            }
            else
            {
                bWakeup = false;
            }

            if(bWakeup)
            {
                g_PendingWindows.insert(sibling);
                g_PendingWindows.insert(g_pFocusWindow);
            }
        }
    }

    if(note.type & Input::Mouse)
    {
        // Update our cursor position.
        /// \todo Need a divisor or something, in case the relative updates
        ///       are too big for the user's taste.
        g_CursorX += note.data.pointy.relx;
        g_CursorY -= note.data.pointy.rely;

        // Constrain.
        if(g_CursorX < 0)
            g_CursorX = 0;
        if(g_CursorY < 0)
            g_CursorY = 0;
        if(g_CursorX >= g_nWidth)
            g_CursorX = g_nWidth - 1;
        if(g_CursorY >= g_nHeight)
            g_CursorY = g_nHeight - 1;

        syslog(LOG_INFO, "Cursor update %d, %d [rel %d %d]", g_CursorX, g_CursorY, note.data.pointy.relx, note.data.pointy.rely);

        // Trigger a render.
        g_bCursorUpdate = true;
    }

    if((!bHandled) && (g_pFocusWindow))
    {
        // Forward event to focus window.
        size_t totalSize = sizeof(LibUiProtocol::WindowManagerMessage);
        if(note.type & Input::Key)
            totalSize += sizeof(LibUiProtocol::KeyEventMessage);
        else if(note.type & Input::RawKey)
            totalSize += sizeof(LibUiProtocol::RawKeyEventMessage);

        char *buffer = new char[totalSize];

        LibUiProtocol::WindowManagerMessage *pHeader =
            reinterpret_cast<LibUiProtocol::WindowManagerMessage*>(buffer);
        pHeader->widgetHandle = g_pFocusWindow->getHandle();
        pHeader->messageSize = totalSize - sizeof(*pHeader);
        pHeader->isResponse = false;

        if(note.type & Input::Key)
        {
            pHeader->messageCode = LibUiProtocol::KeyEvent;

            LibUiProtocol::KeyEventMessage *pKeyEvent =
                reinterpret_cast<LibUiProtocol::KeyEventMessage*>(buffer + sizeof(LibUiProtocol::WindowManagerMessage));
            pKeyEvent->state = LibUiProtocol::Up; /// \todo 'keydown' messages.
            pKeyEvent->key = note.data.key.key;

            send(g_pFocusWindow->getSocket(), buffer, totalSize, 0);
        }
        else if(note.type & Input::RawKey)
        {
            pHeader->messageCode = LibUiProtocol::RawKeyEvent;

            LibUiProtocol::RawKeyEventMessage *pKeyEvent =
                reinterpret_cast<LibUiProtocol::RawKeyEventMessage*>(buffer + sizeof(LibUiProtocol::WindowManagerMessage));
            pKeyEvent->state = note.data.rawkey.keyUp ? LibUiProtocol::Up : LibUiProtocol::Down;
            pKeyEvent->scancode = note.data.rawkey.scancode;

            send(g_pFocusWindow->getSocket(), buffer, totalSize, 0);
        }

        delete [] buffer;
    }

}

/// System input callback.
void systemInputCallback(Input::InputNotification &note)
{
    Input::InputNotification n = note;
    g_InputQueue.push(n);

    // Wake up any pending select() - input pushed to queue.
    write(g_iControlPipe[1], "w", 1);

}

void infoPanel(cairo_t *cr)
{
    // Create a nice little bar at the bottom of the screen.
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

    cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.8);
    cairo_rectangle(cr, 0, g_nHeight - 24, g_nWidth, 24);
    cairo_fill(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_font_size(cr, 13);
    cairo_font_extents_t extents;
    cairo_font_extents(cr, &extents);

    cairo_move_to(cr, 3, (g_nHeight - 24) + 3 + extents.height);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_show_text(cr, "The Pedigree Operating System");

    if(g_StatusField.length())
    {
        cairo_move_to(
                cr,
                g_nWidth - 3 - (extents.max_x_advance * g_StatusField.length()),
                (g_nHeight - 24) + 3 + extents.height);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_show_text(cr, g_StatusField.c_str());
        g_StatusField.clear();
    }

    cairo_restore(cr);
}

int main(int argc, char *argv[])
{
    syslog(LOG_INFO, "winman: starting up...");

    // Create ourselves a lock file so we don't end up getting run twice.
    /// \todo Revisit this when exiting the window manager is possible.
    int fd = open("runtime»/winman.lck", O_WRONLY | O_EXCL | O_CREAT);
    if(fd < 0)
    {
        fprintf(stderr, "winman: lock file exists, terminating.\n");
        return 1;
    }
    close(fd);

    // Grab a framebuffer to use.
    int fb = open("/dev/fb", O_RDWR);
    if(fb < 0)
    {
        syslog(LOG_INFO, "winman: no framebuffer device");
        fprintf(stderr, "winman: couldn't open framebuffer device");
        return 1;
    }

    // Grab the current mode so we can restore it if we die.
    pedigree_fb_mode current_mode;
    int result = ioctl(fb, PEDIGREE_FB_GETMODE, &current_mode);
    if(result < 0)
    {
        fprintf(stderr, "winman: could not get current mode information.\n");
        return EXIT_FAILURE;
    }

    // Kick off a 'window manager' process group, fork to run the modeset shim.
    setpgid(0, 0);
    pid_t child = fork();
    if(child == -1)
    {
        fprintf(stderr, "winman: could not fork: %s\n", strerror(errno));
        return 1;
    }
    else if(child != 0)
    {
        // Wait for the child (ie, real window manager process) to terminate.
        int status = 0;
        waitpid(child, &status, 0);

        // Restore old graphics mode.
        pedigree_fb_modeset old_mode = {current_mode.width, current_mode.height, current_mode.depth};
        ioctl(fb, PEDIGREE_FB_SETMODE, &old_mode);

        close(fb);

        // Termination information
        if(WIFEXITED(status))
        {
            fprintf(stderr, "winman: terminated with status %d\n", WEXITSTATUS(status));
        }
        else if(WIFSIGNALED(status))
        {
            fprintf(stderr, "winman: terminated by signal %d\n", WTERMSIG(status));
        }
        else
        {
            fprintf(stderr, "winman: terminated by unknown means\n");
        }

        // Terminate our process group.
        kill(0, SIGTERM);
        return 0;
    }

    // Can we set the graphics mode we want?
    /// \todo Read from a config file!
    pedigree_fb_modeset mode = {1024, 768, 32};
    result = ioctl(fb, PEDIGREE_FB_SETMODE, &mode);
    if(result < 0)
    {
        // No! Bad!
        /// \note Mode set logic will try and find a mode in a lower colour depth
        ///       if the desired one cannot be set.
        syslog(LOG_INFO, "winman: can't set the desired mode");
        fprintf(stderr, "winman: could not set desired mode (%dx%d) in any colour depth.\n", mode.width, mode.height);
        return 1;
    }

    pedigree_fb_mode set_mode;
    result = ioctl(fb, PEDIGREE_FB_GETMODE, &set_mode);
    if(result < 0)
    {
        syslog(LOG_INFO, "winman: can't get mode info");
        fprintf(stderr, "winman: could not get mode information after setting mode.\n");

        // Back to text.
        memset(&mode, 0, sizeof(mode));
        ioctl(fb, PEDIGREE_FB_SETMODE, &mode);
        return 1;
    }

    g_nWidth = set_mode.width;
    g_nHeight = set_mode.height;

    syslog(LOG_INFO, "Actual mode is %dx%d", g_nWidth, g_nHeight);

    cairo_format_t format = CAIRO_FORMAT_ARGB32;
    if(set_mode.format == PedigreeGraphics::Bits24_Rgb)
    {
        if(set_mode.bytes_per_pixel != 4)
        {
            fprintf(stderr, "winman: error: incompatible framebuffer format (bytes per pixel)\n");
            return 1;
        }
    }
    else if(set_mode.format == PedigreeGraphics::Bits16_Rgb565)
    {
        format = CAIRO_FORMAT_RGB16_565;
    }
    else if(set_mode.format > PedigreeGraphics::Bits32_Rgb)
    {
        fprintf(stderr, "winman: error: incompatible framebuffer format (possibly BGR or similar)\n");
        return 1;
    }

    int stride = cairo_format_stride_for_width(format, g_nWidth);

    // Map the framebuffer in to our address space.
    syslog(LOG_INFO, "Mapping /dev/fb in (sz=%x)...", stride * g_nHeight);
    void *framebufferVirt = mmap(
        0,
        stride * g_nHeight,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fb,
        0);
    syslog(LOG_INFO, "Got %p...", framebufferVirt);

    if(framebufferVirt == MAP_FAILED)
    {
        syslog(LOG_CRIT, "winman: couldn't map framebuffer into address space");
        return -1;
    }
    else
    {
        syslog(LOG_INFO, "winman: mapped framebuffer at %p", framebufferVirt);
    }

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
            (uint8_t *) framebufferVirt,
            format,
            g_nWidth,
            g_nHeight,
            stride);
    cairo_t *cr = cairo_create(surface);

    // Create control pipe.
    pipe(g_iControlPipe);

    // Create listening socket.
    g_iSocket = socket(AF_UNIX, SOCK_DGRAM, 0);

    if(g_iSocket < 0)
    {
        syslog(LOG_CRIT, "error: couldn't create the pedigree-winman IPC endpoint!");
        return 0;
    }

    // Bind.
    struct sockaddr_un bind_addr;
    bind_addr.sun_family = AF_UNIX;
    memset(bind_addr.sun_path, 0, sizeof bind_addr.sun_path);
    strcpy(bind_addr.sun_path, WINMAN_SOCKET_PATH);
    socklen_t socklen = sizeof(bind_addr);
    bind(g_iSocket, (struct sockaddr *) &bind_addr, socklen);

    FT_Library font_library;
    FT_Face ft_face;
    int e = FT_Init_FreeType(&font_library);
    if(e)
    {
        syslog(LOG_CRIT, "error: couldn't initialise Freetype");
        return 0;
    }

    e = FT_New_Face(font_library, "/system/fonts/DejaVuSansMono.ttf", 0, &ft_face);
    if(e)
    {
        syslog(LOG_CRIT, "winman: error: couldn't load required font");
        return 0;
    }

    cairo_user_data_key_t key;
    cairo_font_face_t *font_face;

    font_face = cairo_ft_font_face_create_for_ft_face(ft_face, 0);
    cairo_font_face_set_user_data(font_face, &key,
                                  ft_face, (cairo_destroy_func_t) FT_Done_Face);
    cairo_set_font_face(cr, font_face);

    Png *wallpaper = 0;
    FILE *fp = fopen("/system/wallpaper/fields.png", "rb");
    if(fp)
    {
        fclose(fp);

        // A nice wallpaper.
        wallpaper = new Png("/system/wallpaper/trees.png");
        wallpaper->render(cr, 0, 0, g_nWidth, g_nHeight);
    }
    else
    {
        // No PNG found, fall back to a blue background.
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 1.0, 1.0);
        cairo_rectangle(cr, 0, 0, g_nWidth, g_nHeight);
        cairo_fill(cr);
    }

    g_pRootContainer = new RootContainer(g_nWidth, g_nHeight - 24);

    infoPanel(cr);

#if 0
    Container *pMiddle = new Container(g_pRootContainer);
    pMiddle->setLayout(Container::Stacked);

    Container *another = new Container(pMiddle);
    another->setLayout(Container::SideBySide);

    Window *pLeft = new Window(0, 0, g_pRootContainer);
    g_pRootContainer->addChild(pMiddle);
    Window *pRight = new Window(0, 0, g_pRootContainer);

    // Should be evenly split down the screen after retile()
    Window *pTop = new Window(0, 0, pMiddle);
    Window *pMiddleWindow = new Window(0, 0, pMiddle);
    Window *pBottom = new Window(0, 0, pMiddle);

    pMiddle->addChild(another);

    Window *more = new Window(0, 0, another);
    Window *evenmore = new Window(0, 0, another);

    g_pFocusWindow = pLeft;
    pLeft->focus();
#endif

    syslog(LOG_INFO, "winman: entering main loop pid=%d", getpid());

    g_Windows = new std::map<uint64_t, Window*>();

    // Install our global input callback before we kick off our client.
    Input::installCallback(Input::RawKey | Input::Key | Input::Mouse, systemInputCallback);

    // Render all window decorations and non-client display elements first up.
    g_pRootContainer->render(cr);

    // Kick off the first render before any windows are open.
    cairo_surface_flush(surface);
    ioctl(fb, PEDIGREE_FB_REDRAW, 0);

    // Load first tile.
    startClient();

    // Main loop: logic & message handling goes here!
    DirtyRectangle renderDirty;
    while(true)
    {
        // Check for any messages coming in from windows, asynchronously.
        checkForMessages();

        // Check for any windows that may need rendering.
        if((!g_PendingWindows.empty()) || g_StatusField.length() || g_bCursorUpdate)
        {
            // Render each window, also ensuring the wallpaper is rendered again
            // so that windows with alpha look correct.
            std::set<WObject*>::iterator it = g_PendingWindows.begin();
            size_t nDirty = g_StatusField.length() ? 1 : 0;
            if(g_bCursorUpdate)
                ++nDirty;
            for(; it != g_PendingWindows.end(); ++it)
            {
                Window *pWindow = 0;
                if((*it)->getType() == WObject::Window)
                {
                    pWindow = static_cast<Window*>(*it);
                }

                if(pWindow && !pWindow->isDirty())
                {
                    continue;
                }
                else
                {
                    ++nDirty;
                }

                PedigreeGraphics::Rect rt = (*it)->getCopyDimensions();
                PedigreeGraphics::Rect dirty = rt;
                if(pWindow)
                {
                    dirty = pWindow->getDirty();
                }

                // Render wallpaper.
                if(wallpaper)
                {
                    wallpaper->renderPartial(
                            cr,
                            rt.getX() + dirty.getX(),
                            rt.getY() + dirty.getY(),
                            0, 0,
                            dirty.getW(), dirty.getH(),
                            g_nWidth, g_nHeight);
#if DEBUG_REDRAWS
                    cairo_set_source_rgba(cr, 0, 0, 1.0, 1.0);
                    cairo_rectangle(
                            cr,
                            rt.getX() + dirty.getX(),
                            rt.getY() + dirty.getY(),
                            dirty.getW(),
                            dirty.getH());
                    cairo_stroke(cr);
#endif
                }
                else
                {
                    // Boring background.
                    cairo_set_source_rgba(cr, 0, 0, 1.0, 1.0);
                    cairo_rectangle(
                            cr,
                            rt.getX() + dirty.getX(),
                            rt.getY() + dirty.getY(),
                            dirty.getW(),
                            dirty.getH());
#if DEBUG_REDRAWS
                    cairo_stroke_preserve(cr);
#endif
                    cairo_fill(cr);
                }

                // Render window.
                (*it)->render(cr);

                // Update the dirty rectangle.
                renderDirty.point(
                        rt.getX() + dirty.getX(),
                        rt.getX() + dirty.getY());
                renderDirty.point(
                        rt.getX() + dirty.getX() + dirty.getW(),
                        rt.getX() + dirty.getY() + dirty.getH());
            }

            // Empty out the list in full.
            g_PendingWindows.clear();

            // Only do rendering if we actually did some rendering!
            if(!nDirty)
            {
                renderDirty.reset();
                continue;
            }

            if(g_StatusField.length())
            {
                infoPanel(cr);
            }

#if 0
            // After all else is said and done, render the cursor.
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_rectangle(
                    cr,
                    g_CursorX,
                    g_CursorY,
                    32,
                    32);
            cairo_fill(cr);
            renderDirty.point(g_CursorX, g_CursorY);
            renderDirty.point(g_CursorX + 32, g_CursorY + 32);

            // We'll want to redraw the area we just obstructed shortly.
            g_LastCursorX = g_CursorX;
            g_LastCursorY = g_CursorY;

            // Done drawing cursor.
            g_bCursorUpdate = false;
#endif

            // Flush the cairo surface, which will ensure the most recent data is
            // in the framebuffer ready to send to the device.
            cairo_surface_flush(surface);

            // Submit a redraw to the graphics card.
            pedigree_fb_rect fbdirty = {renderDirty.getX(), renderDirty.getY(), renderDirty.getWidth(), renderDirty.getHeight()};
            ioctl(fb, PEDIGREE_FB_REDRAW, &fbdirty);

            // Wipe out the dirty rectangle - we're all done.
            renderDirty.reset();
        }
    }

    /// \todo Clean up?

    return 0;
}
