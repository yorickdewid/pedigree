/*
 * Copyright (c) 2008 James Molloy, Jörg Pfähler, Matthew Iselin, Eduard Burtescu
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
#ifndef UHCI_H
#define UHCI_H

#include <machine/Device.h>
#include <machine/IrqHandler.h>
#include <process/Thread.h>
#include <processor/IoBase.h>
#include <processor/IoPort.h>
#include <processor/MemoryRegion.h>
#include <processor/PhysicalMemoryManager.h>
#include <processor/types.h>
#include <process/Semaphore.h>
#include <usb/UsbConstants.h>
#include <usb/UsbController.h>

/** Device driver for the Uhci class */
class Uhci : public UsbController, public IrqHandler
{
    public:
        Uhci(Device* pDev);
        ~Uhci();

        typedef struct TD
        {
            uint32_t next_invalid : 1;
            uint32_t next_qh : 1;
            uint32_t next_depth : 1;
            uint32_t res0 : 1;
            uint32_t next : 28;
            uint32_t actlen : 11;
            uint32_t res1 : 5;
            uint32_t status : 8;
            uint32_t ioc : 1;
            uint32_t iso : 1;
            uint32_t speed : 1;
            uint32_t cerr : 2;
            uint32_t spd : 1;
            uint32_t res2 : 2;
            uint32_t nPid : 8;
            uint32_t nAddress : 7;
            uint32_t nEndpoint : 4;
            uint32_t data_toggle : 1;
            uint32_t res3 : 1;
            uint32_t maxlen : 11;
            uint32_t buff;

            //uint32_t periodic : 1;
            uint32_t phys : 28;
            uint32_t res4 : 4;
            uint32_t pCallback;
            uint32_t param;
            uint32_t buffer;
        } PACKED TD;

        typedef struct QH
        {
            uint32_t next_invalid : 1;
            uint32_t next_qh : 1;
            uint32_t res0 : 2;
            uint32_t next : 28;
            uint32_t elem_invalid : 1;
            uint32_t elem_qh : 1;
            uint32_t res1 : 2;
            uint32_t elem : 28;
            TD *pCurrent;
            uint8_t res2[24-sizeof(uintptr_t)];
        } PACKED QH;

        virtual void getName(String &str)
        {
            str = "UHCI";
        }

        virtual void doAsync(uint8_t nAddress, uint8_t nEndpoint, uint8_t nPid, uintptr_t pBuffer, uint16_t nBytes, void (*pCallback)(uintptr_t, ssize_t)=0, uintptr_t pParam=0);
        virtual void addInterruptInHandler(uint8_t nAddress, uint8_t nEndpoint, uintptr_t pBuffer, uint16_t nBytes, void (*pCallback)(uintptr_t, ssize_t), uintptr_t pParam=0);

        // IRQ handler callback.
        virtual bool irq(irq_id_t number, InterruptState &state);
        IoBase *m_pBase;

    private:

        enum UhciConstants {
            UHCI_CMD = 0x00,            // Command register
            UHCI_STS = 0x02,            // Status register
            UHCI_INTR = 0x04,           // Intrerrupt Enable register
            UHCI_FRMN = 0x06,           // Frame Number register
            UHCI_FRLP = 0x08,           // Frame List Pointer register
            UHCI_PORTSC = 0x10,         // Port Status/Control registers

            UHCI_CMD_GRES = 0x04,       // Global Reset bit
            UHCI_CMD_HCRES = 0x02,      // Host Controller Reset bit
            UHCI_CMD_RUN = 0x01,        // Run bit

            UHCI_STS_INT = 0x01,        // On Completition Interrupt bit

            UHCI_PORTSC_PRES = 0x200,   // Port Reset bit
            UHCI_PORTSC_EDCH = 0x8,     // Port Enable/Disable Change bit
            UHCI_PORTSC_ENABLE = 0x4,   // Port Enable bit
            UHCI_PORTSC_CSCH = 0x2,     // Port Connect Status Change bit
            UHCI_PORTSC_CONN = 0x1,     // Port Connected bit
        };

        uint8_t m_nPorts;
        uint16_t m_nFrames;

        Mutex m_Mutex;

        TD *m_pTDList;
        uintptr_t m_pTDListPhys;
        QH *m_pAsyncQH;
        uintptr_t m_pAsyncQHPhys;
        QH *m_pPeriodicQH;
        uintptr_t m_pPeriodicQHPhys;
        uint32_t *m_pFrameList;
        uintptr_t m_pFrameListPhys;
        uint8_t *m_pTransferPages;
        uintptr_t m_pTransferPagesPhys;
        MemoryRegion m_UhciMR;

        Uhci(const Uhci&);
        void operator =(const Uhci&);
};

#endif