/*
 * Copyright (c) 2008 James Molloy, Jörg Pfähler, Matthew Iselin
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

#include "NetworkStack.h"
#include "Ethernet.h"
#include <Module.h>
#include <Log.h>

#include "Dns.h"

NetworkStack NetworkStack::stack;

NetworkStack::NetworkStack() :
  m_pLoopback(0), m_Children()
{
  //
}

NetworkStack::~NetworkStack()
{
  //
}

void NetworkStack::receive(size_t nBytes, uintptr_t packet, Network* pCard, uint32_t offset)
{
  if(!packet || !nBytes)
      return;

  // pass onto the ethernet layer
  /// \todo We should accept a parameter here that specifies the type of packet
  ///       so we can pass it on to the correct handler, rather than assuming
  ///       Ethernet.
  Ethernet::instance().receive(nBytes, packet, pCard, offset);
}

void NetworkStack::registerDevice(Network *pDevice)
{
  m_Children.pushBack(pDevice);
}

Network *NetworkStack::getDevice(size_t n)
{
  return m_Children[n];
}

size_t NetworkStack::getNumDevices()
{
  return m_Children.count();
}

void NetworkStack::deRegisterDevice(Network *pDevice)
{
  int i = 0;
  for(Vector<Network*>::Iterator it = m_Children.begin();
      it != m_Children.end();
      it++, i++)
  if (*it == pDevice)
  {
    m_Children.erase(it);
    break;
  }
}

void entry()
{
    // Initialise the DNS implementation
    Dns::instance().initialise();
}

void exit()
{
}


MODULE_NAME("network-stack");
MODULE_ENTRY(&entry);
MODULE_EXIT(&exit);
MODULE_DEPENDS(0);