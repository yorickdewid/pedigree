/*
 * Copyright (c) 2010 Eduard Burtescu
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
#ifndef USBHUB_H
#define USBHUB_H

#include <process/Mutex.h>
#include <processor/types.h>
#include <machine/Controller.h>
#include <usb/UsbDevice.h>
#include <utilities/ExtensibleBitmap.h>

class UsbHub : public virtual Device
{
    public:

        enum Speed
        {
            Low = 0,
            Full,
            High,
            Super
        };

        inline UsbHub() : m_SyncSemaphore(0) {}
        inline virtual ~UsbHub() {}

        virtual void doAsync(uint8_t nAddress, uint8_t nEndpoint, uint8_t nPid, uintptr_t pBuffer, uint16_t nBytes, void (*pCallback)(uintptr_t, ssize_t)=0, uintptr_t pParam=0) =0;
        virtual void addInterruptInHandler(uint8_t nAddress, uint8_t nEndpoint, uintptr_t pBuffer, uint16_t nBytes, void (*pCallback)(uintptr_t, ssize_t), uintptr_t pParam=0) =0;

        void deviceConnected(uint8_t nPort);
        void deviceDisconnected(uint8_t nPort);

        void getUsedAddresses(ExtensibleBitmap *pBitmap);

        ssize_t doSync(uint8_t nAddress, uint8_t nEndpoint, uint8_t nPid, uintptr_t pBuffer, size_t nBytes, uint32_t timeout=5000);

        virtual Speed getSpeed()
        {
            return Low;
        }

    private:

        Mutex m_SyncMutex;
        Semaphore m_SyncSemaphore;
        ssize_t m_SyncRet;

        static void syncCallback(uintptr_t pParam, ssize_t ret);
};

#endif