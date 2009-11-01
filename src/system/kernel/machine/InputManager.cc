/*
 * Copyright (c) 2008 James Molloy, J�rg Pf�hler, Matthew Iselin
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

#include <machine/InputManager.h>
#include <LockGuard.h>
#include <Log.h>

#include <process/Event.h>

class InputEvent : public Event
{
    public:
        InputEvent(InputManager::CallbackType type, uint64_t key, uintptr_t handlerAddress) :
                    Event(handlerAddress, true),
                    m_Type(type),
                    m_Key(key)
        {};
        virtual ~InputEvent()
        {};

        virtual size_t serialize(uint8_t *pBuffer)
        {
            pBuffer[0] = static_cast<uint8_t>(m_Type);
            *(reinterpret_cast<uint64_t*>(&pBuffer[1])) = m_Key;
            return sizeof(uint8_t) + sizeof(uint64_t);
        }

        static bool unserialize(uint8_t *pBuffer, Event &event)
        {
            /// \todo How to do this?
            /*
            InputEvent *ev = static_cast<InputEvent*>(&event);
            ev->m_Type = static_cast<InputManager::CallbackType>(pBuffer[0]);
            ev->m_Key = *(reinterpret_cast<uint64_t*>(&pBuffer[1]));
            */
            return true;
        }

        virtual inline size_t getNumber()
        {
            return EventNumbers::InputEvent;
        }

        inline InputManager::CallbackType getType()
        {
            return m_Type;
        }

        inline uint64_t getKey()
        {
            return m_Key;
        }
    private:
        InputManager::CallbackType m_Type;
        uint64_t m_Key;
};

InputManager InputManager::m_Instance;

InputManager::InputManager() :
    m_KeyQueue(), m_KeyQueueSize(0), m_QueueLock(), m_KeyCallbacks(), m_pThread(0)
{
}

InputManager::~InputManager()
{
}

void InputManager::initialise()
{
    // Start the worker thread.
#ifdef THREADS
    m_pThread = new Thread(Processor::information().getCurrentThread()->getParent(),
                           reinterpret_cast<Thread::ThreadStartFunc> (&trampoline),
                           reinterpret_cast<void*> (this));
#endif
}

void InputManager::keyPressed(uint64_t key)
{
    LockGuard<Spinlock> guard(m_QueueLock);
    m_KeyQueue.pushBack(key);
    m_KeyQueueSize.release();
}

void InputManager::installCallback(CallbackType type, callback_t callback, Thread *pThread)
{
    LockGuard<Spinlock> guard(m_QueueLock);
    if(type == Key)
    {
        CallbackItem *item = new CallbackItem;
        item->func = callback;
        item->pThread = pThread;
        m_KeyCallbacks.pushBack(item);
    }
}

int InputManager::trampoline(void *ptr)
{
    InputManager *p = reinterpret_cast<InputManager *>(ptr);
    p->mainThread();
    return 0;
}

void InputManager::mainThread()
{
    while(true)
    {
        m_KeyQueueSize.acquire();
        if(!m_KeyQueue.count())
            continue; /// \todo Handle exit condition

        m_QueueLock.acquire();
        uint64_t key = m_KeyQueue.popFront();
        m_QueueLock.release();

        for(List<CallbackItem*>::Iterator it = m_KeyCallbacks.begin();
            it != m_KeyCallbacks.end();
            it++)
        {
            if(*it)
            {
                Thread *pThread = (*it)->pThread;
                if(!pThread)
                    pThread = Processor::information().getCurrentThread();
                callback_t func = (*it)->func;
                
                InputEvent *pEvent = new InputEvent(Key, key, reinterpret_cast<uintptr_t>(func));
                pThread->sendEvent(pEvent);
            }
        }
    }
}