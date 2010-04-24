/*
 * Copyright (c) 2010 Matthew Iselin
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
#include <processor/Processor.h>
#include <utilities/MemoryPool.h>
#include <processor/VirtualAddressSpace.h>
#include <Log.h>

MemoryPool::MemoryPool() :
    m_BlockSemaphore(0), m_BufferSize(1024), m_Pool("memory-pool"),
        m_bInitialised(false), m_AllocBitmap()
{
}

MemoryPool::~MemoryPool()
{
    // Free all the buffers
    m_bInitialised = false;
    while(!m_BlockSemaphore.getValue())
        m_BlockSemaphore.release();
}

bool MemoryPool::initialise(size_t poolSize, size_t bufferSize)
{
    if(m_bInitialised)
        return true;

    if(!poolSize || !bufferSize || (bufferSize > poolSize))
        return false;

    // Find the next power of two for bufferSize, if it isn't already one
    if(!(bufferSize & (bufferSize - 1)))
    {
        size_t powerOf2 = 1;
        size_t lg2 = 0;
        while(powerOf2 < bufferSize)
        {
            powerOf2 <<= 1;
            lg2 ++;
        }
        bufferSize = powerOf2;
    }

    m_BufferSize = bufferSize;

    m_bInitialised = PhysicalMemoryManager::instance().allocateRegion(
        m_Pool,
        poolSize,
        0,
        VirtualAddressSpace::Write
    );
    if(!m_bInitialised)
        return false;

    size_t nBuffers = (poolSize * 0x1000) / bufferSize;
    m_BlockSemaphore.release(nBuffers);

    return true;
}

uintptr_t MemoryPool::allocate()
{
    /// \bug Race if another allocate() call occurs between the acquire and the doer
    m_BlockSemaphore.acquire();
    return allocateDoer();
}

uintptr_t MemoryPool::allocateNow()
{
    if(m_BlockSemaphore.tryAcquire())
        return allocateDoer();
    else
        return 0;
}

uintptr_t MemoryPool::allocateDoer()
{
    // Find a free buffer
    size_t poolSize = m_Pool.size();
    size_t nBuffers = poolSize / m_BufferSize;
    uintptr_t poolBase = reinterpret_cast<uintptr_t>(m_Pool.virtualAddress());
    for(size_t n = 0; n < nBuffers; n++)
    {
        if(!m_AllocBitmap.test(n))
        {
            m_AllocBitmap.set(n);
            return poolBase + (n * 0x1000);
        }
    }

    FATAL("allocateDoer failed - known race condition");
    return 0;
}

void MemoryPool::free(uintptr_t buffer)
{
    if(!m_bInitialised)
        return;

    size_t n = (buffer - reinterpret_cast<uintptr_t>(m_Pool.virtualAddress())) / m_BufferSize;
    m_AllocBitmap.clear(n);

    m_BlockSemaphore.release();
}
