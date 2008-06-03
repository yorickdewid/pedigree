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

#include <Log.h>
#include <Elf32.h>
#include <utilities/utility.h>
#include <BootstrapInfo.h>

Elf32::Elf32() :
  m_pHeader(0),
  m_pSymbolTable(0),
  m_pStringTable(0),
  m_pShstrtab(0),
  m_pGotTable(0),
  m_pRelTable(0),
  m_pDebugTable(0),
  m_pSectionHeaders(0),
  m_nSectionHeaders(0),
  m_pBuffer(0),
  m_LoadBase(0)
{
}

Elf32::~Elf32()
{
}

bool Elf32::load(uint8_t *pBuffer, unsigned int nBufferLength)
{
  // The main header will be at pBuffer[0].
  m_pHeader = reinterpret_cast<Elf32Header_t *>(pBuffer);
  
  // Check the ident.
  if ( (m_pHeader->ident[1] != 'E') ||
       (m_pHeader->ident[2] != 'L') ||
       (m_pHeader->ident[3] != 'F') ||
       (m_pHeader->ident[0] != 127) )
  {
    m_pHeader = 0;
    ERROR("ELF file: ident check failed!");
    return false;
  }
  
  // Load in the section headers.
  m_pSectionHeaders = reinterpret_cast<Elf32SectionHeader_t *>(&pBuffer[m_pHeader->shoff]);
  
  // Find the string table.
  m_pShstrtab = &m_pSectionHeaders[m_pHeader->shstrndx];

  // Temporarily load the string table.
  const char *pStrtab = reinterpret_cast<const char *>(&pBuffer[m_pShstrtab->offset]);
  
  // Go through each section header, trying to find .symtab.
  for (int i = 0; i < m_pHeader->shnum; i++)
  {
    const char *pStr = pStrtab + m_pSectionHeaders[i].name;
    if (!strcmp(pStr, ".symtab"))
      m_pSymbolTable = &m_pSectionHeaders[i];
    if (!strcmp(pStr, ".strtab"))
      m_pStringTable = &m_pSectionHeaders[i];
  }
  
  if (m_pSymbolTable == 0)
    WARNING("ELF file: symbol table not found!");
  
  m_pBuffer = pBuffer;
  
  // Success.
  return true;
}

bool Elf32::writeSections()
{
  // We need to create a copy of the header.
  Elf32Header_t *pNewHeader = new Elf32Header_t;
  memcpy(reinterpret_cast<uint8_t*> (pNewHeader),
         reinterpret_cast<uint8_t*> (m_pHeader),
         sizeof(Elf32Header_t));
  m_pHeader = pNewHeader;
  
  // We need to create a copy of the section header table.
  Elf32SectionHeader_t *pNewTable = new Elf32SectionHeader_t[m_pHeader->shnum];
  memcpy(reinterpret_cast<uint8_t*> (pNewTable),
         reinterpret_cast<uint8_t*> (m_pSectionHeaders),
         sizeof(Elf32Header_t)*m_pHeader->shnum);
  m_pSectionHeaders = pNewTable;
  
  for (int i = 0; i < m_pHeader->shnum; i++)
  {
    if (m_pSectionHeaders[i].flags & SHF_ALLOC)
    {
      // Add load-base into the equation.
      m_pSectionHeaders[i].addr += m_LoadBase;
      if (m_pSectionHeaders[i].type != SHT_NOBITS)
      {
        // Copy section data from the file.
        memcpy(reinterpret_cast<uint8_t*> (m_pSectionHeaders[i].addr),
                        &m_pBuffer[m_pSectionHeaders[i].offset],
                        m_pSectionHeaders[i].size);
      }
      else
      {
        memset(reinterpret_cast<uint8_t*> (m_pSectionHeaders[i].addr),
                        0,
                        m_pSectionHeaders[i].size);
      }
    }
    else
    {
      // Is this .strtab, .symtab or .debug_frame, we need to load it anyway!
      Elf32SectionHeader_t **ppHeader = 0;
      const char *pStr = reinterpret_cast<const char *>(&m_pBuffer[m_pShstrtab->offset]) +
                           m_pSectionHeaders[i].name;
      if (!strcmp(pStr, ".strtab"))
        m_pStringTable = &m_pSectionHeaders[i];
      else if (!strcmp(pStr, ".symtab"))
        m_pSymbolTable = &m_pSectionHeaders[i];
      else if (!strcmp(pStr, ".debug_frame"))
        m_pDebugTable = &m_pSectionHeaders[i];
      else continue;

      // We need to allocate space for this section.
      // For now, we allocate on the kernel heap.
      /// \todo Change this to find some available VA space to mmap into. Will need to know
      ///       whether it's a userspace app or kernelspace module.
      uint8_t *pSection = new uint8_t[m_pSectionHeaders[i].size];
      memcpy(pSection, &m_pBuffer[m_pSectionHeaders[i].offset], m_pSectionHeaders[i].size);
      m_pSectionHeaders[i].addr = reinterpret_cast<uintptr_t> (pSection);
    }
  }
  
  // Success.
  return true;
}

unsigned int Elf32::getLastAddress()
{
  // TODO
  return 0;
}

const char *Elf32::lookupSymbol(uintptr_t addr, uintptr_t *startAddr)
{
  if (!m_pSymbolTable || !m_pStringTable)
    return 0; // Just return null if we haven't got a symbol table.
  
  Elf32Symbol_t *pSymbol = reinterpret_cast<Elf32Symbol_t *>(m_pSymbolTable->addr);

  const char *pStrtab = reinterpret_cast<const char *>(m_pStringTable->addr);

  for (size_t i = 0; i < m_pSymbolTable->size / sizeof(Elf32Symbol_t); i++)
  {
    // Make sure we're looking at an object or function.
    if (ELF32_ST_TYPE(pSymbol->info) != 0x2 /* function */ &&
        ELF32_ST_TYPE(pSymbol->info) != 0x0 /* notype (asm functions) */)
    {
      pSymbol++;
      continue;
    }
    
    // If we're checking for a symbol that is apparently zero-sized, add one so we can actually
    // count it!
    uint32_t size = pSymbol->size;
    if (size == 0)
      size = 1;
    if ( (addr >= pSymbol->value) &&
         (addr < (pSymbol->value + size)) )
    {
      const char *pStr = pStrtab + pSymbol->name;
      if (startAddr)
        *startAddr = pSymbol->value;
      return pStr;
    }
    pSymbol ++;
  }
  return 0;
  
}

uint32_t Elf32::lookupDynamicSymbolAddress(uint32_t off)
{
  // TODO
  return 0;
}

char *Elf32::lookupDynamicSymbolName(uint32_t off)
{
  // TODO
  return 0;
}

uint32_t Elf32::getGlobalOffsetTable()
{
  // TODO
  return 0;
}

uint32_t Elf32::getEntryPoint()
{
  // TODO
  return 0;
}

void Elf32::setLoadBase(uintptr_t loadBase)
{
  m_LoadBase = loadBase;
}

bool Elf32::relocate()
{
  
}

uintptr_t Elf32::debugFrameTable()
{
  return m_pDebugTable->addr;
}

uintptr_t Elf32::debugFrameTableLength()
{
  return m_pDebugTable->size;
}
