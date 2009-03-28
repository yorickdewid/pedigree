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
#include <vfs/VFS.h>
#include <ext2/Ext2Filesystem.h>
#include <machine/Device.h>
#include <machine/Disk.h>
#include <Module.h>
#include <processor/Processor.h>
#include <linker/Elf.h>
#include <process/Thread.h>
#include <process/Process.h>
#include <process/Scheduler.h>
#include <processor/PhysicalMemoryManager.h>
#include <processor/VirtualAddressSpace.h>
#include <machine/Machine.h>
#include <linker/DynamicLinker.h>
#include <panic.h>
#include "../../kernel/core/BootIO.h"

#include <network/NetworkStack.h>
#include <network/RoutingTable.h>

extern BootIO bootIO;

static bool probeDisk(Disk *pDisk)
{
  String alias; // Null - gets assigned by the filesystem.
  if (VFS::instance().mount(pDisk, alias))
  {
    // search for the root specifier
    NormalStaticString s;
    s += alias;
    s += ":/.pedigree-root";

    File* f = VFS::instance().find(String(static_cast<const char*>(s)));
    if(f)
    {
      if(f->isValid())
      {
        NOTICE("Mounted " << alias << " successfully as root.");
        VFS::instance().addAlias(alias, String("root"));
      }
    delete f;
    }
    else
    {
      NOTICE("Mounted " << alias << ".");
    }
    return false;
  }
  return false;
}

static bool findDisks(Device *pDev)
{
  for (unsigned int i = 0; i < pDev->getNumChildren(); i++)
  {
    Device *pChild = pDev->getChild(i);
    if (pChild->getNumChildren() == 0 && /* Only check leaf nodes. */
        pChild->getType() == Device::Disk)
    {
      if ( probeDisk(static_cast<Disk*> (pChild)) ) return true;
    }
    else
    {
      // Recurse.
      if (findDisks(pChild)) return true;
    }
  }
  return false;
}

void init()
{
  static HugeStaticString str;

  // Mount all available filesystems.
  if (!findDisks(&Device::root()))
  {
//     FATAL("No disks found!");
  }

  // Build routing tables
  File* routeConfig = VFS::instance().find(String("root:/config/routes"));
  if(routeConfig->isValid())
  {
    NOTICE("Loading route table from file");
    /// \todo Write this...
  }
  else
  {
    // try to find a default configuration that can connect to the outside world
    bool bRouteFound = false;
    for(size_t i = 0; i < NetworkStack::instance().getNumDevices(); i++)
    {
      /// \todo Perhaps try and ping a remote host?
      Network* card = NetworkStack::instance().getDevice(i);
      StationInfo info = card->getStationInfo();
      if(!(info.gateway == 0)) /// \todo write operator !=
      {
        RoutingTable::instance().AddNamed(String("default"), card);
        bRouteFound = true;
        break;
      }
    }

    // otherwise, just assume the default is interface zero
    if(!bRouteFound)
      RoutingTable::instance().AddNamed(String("default"), NetworkStack::instance().getDevice(0));
  }

  str += "Loading init program (root:/applications/bash)\n";
  bootIO.write(str, BootIO::White, BootIO::Black);
  str.clear();

  // Load initial program.
  File* initProg = VFS::instance().find(String("root:/applications/bash"));
  if (!initProg->isValid())
  {
    FATAL("Unable to load init program!");
    return;
  }

  uint8_t *buffer = new uint8_t[initProg->getSize()];
  initProg->read(0, initProg->getSize(), reinterpret_cast<uintptr_t>(buffer));

  Machine::instance().getKeyboard()->setDebugState(false);

  // At this point we're uninterruptible, as we're forking.
  Spinlock lock;
  lock.acquire();

  // Create a new process for the init process.
  Process *pProcess = new Process(Processor::information().getCurrentThread()->getParent());

  pProcess->description().clear();
  pProcess->description().append("init");

  VirtualAddressSpace &oldAS = Processor::information().getVirtualAddressSpace();

  // Switch to the init process' address space.
  Processor::switchAddressSpace(*pProcess->getAddressSpace());

  // That will have forked - we don't want to fork, so clear out all the chaff in the new address space that's not
  // in the kernel address space so we have a clean slate.
  pProcess->getAddressSpace()->revertToKernelAddressSpace();

  static Elf initElf;
  uintptr_t loadBase;

  initElf.create(buffer, initProg->getSize());
  initElf.allocate(buffer, initProg->getSize(), loadBase, 0, pProcess);

  DynamicLinker::instance().setInitProcess(pProcess);
  NOTICE("regElf");
  DynamicLinker::instance().registerElf(&initElf);
NOTICE("needed");
  uintptr_t iter = 0;
  List<char*> neededLibraries = initElf.neededLibraries();
  for (List<char*>::Iterator it = neededLibraries.begin();
       it != neededLibraries.end();
       it++)
  {
    NOTICE("n1");
    if (!DynamicLinker::instance().load(*it, pProcess))
    {
      ERROR("Couldn't open needed file '" << *it << "'");
      FATAL("Init program failed to load!");
      return;
    }
    NOTICE("n2");
  }
NOTICE("bleh");
  initElf.load(buffer, initProg->getSize(), loadBase, initElf.getSymbolTable());
  DynamicLinker::instance().initialiseElf(&initElf);
  DynamicLinker::instance().setInitProcess(0);
  ERROR("Init elf: " << (uintptr_t)&initElf);
  for (int j = 0; j < 0x20000; j += 0x1000)
  {
    physical_uintptr_t phys = PhysicalMemoryManager::instance().allocatePage();
    bool b = Processor::information().getVirtualAddressSpace().map(phys,
                                                                   reinterpret_cast<void*> (j+0x20000000),
                                                                   VirtualAddressSpace::Write);
    if (!b)
      WARNING("map() failed in init");
  }

  // Alrighty - lets create a new thread for this program - -8 as PPC assumes the previous stack frame is available...
  Thread *pThread = new Thread(pProcess, reinterpret_cast<Thread::ThreadStartFunc>(initElf.getEntryPoint()), 0x0 /* parameter */,  reinterpret_cast<void*>(0x20020000-8) /* Stack */);

  // Switch back to the old address space.
  Processor::switchAddressSpace(oldAS);
  
  delete [] buffer;
  
  delete initProg;

  lock.release();

}

void destroy()
{
}

MODULE_NAME("init");
MODULE_ENTRY(&init);
MODULE_EXIT(&destroy);
#ifdef X86_COMMON
MODULE_DEPENDS("VFS", "ext2", "posix", "partition", "TUI", "linker", "vbe", "NetworkStack");
#elif PPC_COMMON
MODULE_DEPENDS("VFS", "ext2", "posix", "partition", "TUI", "linker", "NetworkStack");
#endif

