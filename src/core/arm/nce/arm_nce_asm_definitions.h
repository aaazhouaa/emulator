/* SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project */
/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

// Shared with arm_nce.s: no kernel uapi headers (their sigset_t conflicts with libc's).
// Values are stable arm64 Linux ABI; checked against libc below when compiled as C++.

#define ReturnToRunCodeByExceptionLevelChangeSignal 12 /* SIGUSR2 */
#define BreakFromRunCodeSignal 23                      /* SIGURG */
#define GuestAccessFaultSignal 11                      /* SIGSEGV */
#define GuestAlignmentFaultSignal 7                    /* SIGBUS */

#ifdef __ASSEMBLER__
#define __NR_tkill 130
#else
#include <csignal>
#include <sys/syscall.h>

static_assert(ReturnToRunCodeByExceptionLevelChangeSignal == SIGUSR2);
static_assert(BreakFromRunCodeSignal == SIGURG);
static_assert(GuestAccessFaultSignal == SIGSEGV);
static_assert(GuestAlignmentFaultSignal == SIGBUS);
#ifdef __aarch64__
static_assert(__NR_tkill == 130);
#endif
#endif

#define GuestContextSp 0xF8
#define GuestContextHostContext 0x320

#define HostContextSpTpidrEl0 0xE0
#define HostContextTpidrEl0 0xE8
#define HostContextRegs 0x0
#define HostContextVregs 0x60

#define TpidrEl0NativeContext 0x10
#define TpidrEl0Lock 0x18
#define TpidrEl0TlsMagic 0x20
#define TlsMagic 0x555a5559

#define SpinLockLocked 0
#define SpinLockUnlocked 1
