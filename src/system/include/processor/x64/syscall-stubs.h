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

#ifndef SERVICE
#error syscall-stubs.h requires SERVICE to be defined
#endif
#ifndef SERVICE_ERROR
#error syscall-stubs.h requires SERVICE_ERROR to be defined
#endif
#ifndef SERVICE_INIT
#error syscall-stubs.h requires SERVICE_INIT to be defined
#endif

// If USE_PIC_SYSCALLS is defined, the EBX register will be preserved, as GCC
// refuses to accept it as a clobbered register in inline assembly with -fPIC.

static long syscall0(long function)
{
  long eax = ((SERVICE&0xFFFF) << 16) | (function&0xFFFF);
  long ret;
  SERVICE_INIT;
  asm volatile("syscall" : "=a" (ret), "=b" (SERVICE_ERROR) : "0" (eax) : "rcx", "r11");
  return ret;
}

static long syscall1(long function, long p1)
{
  long eax = ((SERVICE&0xFFFF) << 16) | (function&0xFFFF);
  long ret;
  SERVICE_INIT;
  asm volatile("syscall" : "=a" (ret), "=b" (SERVICE_ERROR) : "0" (eax), "1" (p1) : "rcx", "r11");
  return ret;
}

static long syscall2(long function, long p1, long p2)
{
  long eax = ((SERVICE&0xFFFF) << 16) | (function&0xFFFF);
  long ret;
  SERVICE_INIT;
  asm volatile("syscall" : "=a" (ret), "=b" (SERVICE_ERROR) : "0" (eax), "1" (p1), "d" (p2) : "rcx", "r11");
  return ret;
}

static long syscall3(long function, long p1, long p2, long p3)
{
  long eax = ((SERVICE&0xFFFF) << 16) | (function&0xFFFF);
  long ret;
  SERVICE_INIT;
  asm volatile("syscall" : "=a" (ret), "=b" (SERVICE_ERROR) : "0" (eax), "1" (p1), "d" (p2), "S" (p3) : "rcx", "r11");
  return ret;
}

static long syscall4(long function, long p1, long p2, long p3, long p4)
{
  long eax = ((SERVICE&0xFFFF) << 16) | (function&0xFFFF);
  long ret;
  SERVICE_INIT;
  asm volatile("syscall" : "=a" (ret), "=b" (SERVICE_ERROR) : "0" (eax), "1" (p1), "d" (p2), "S" (p3), "D" (p4) : "rcx", "r11");
  return ret;
}

static long syscall5(long function, long p1, long p2, long p3, long p4, long p5)
{
  long eax = ((SERVICE&0xFFFF) << 16) | (function&0xFFFF);
  long ret;
  SERVICE_INIT;
  asm volatile("mov %7, %%r8; syscall" : "=a" (ret), "=b" (SERVICE_ERROR) : "0" (eax), "1" (p1), "d" (p2), "S" (p3), "D" (p4), "m" (p5) : "rcx", "r8", "r11");
  return ret;
}
