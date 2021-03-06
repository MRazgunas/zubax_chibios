/*
 * Copyright (c) 2014-2017 Zubax, zubax.com
 * Distributed under the MIT License, available in the file LICENSE.
 * Author: Pavel Kirienko <pavel.kirienko@zubax.com>
 */

#include "sys.hpp"
#include <chprintf.h>
#include <ch.hpp>
#include <unistd.h>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cstdarg>
#include <type_traits>

#if !CH_CFG_USE_REGISTRY
# pragma message "CH_CFG_USE_REGISTRY is disabled, panic reports will be incomplete"
#endif

namespace os
{

extern void emergencyPrint(const char* str);

__attribute__((weak))
void applicationHaltHook(void) { }

void sleepUntilChTime(systime_t sleep_until)
{
    chSysLock();
    sleep_until -= chVTGetSystemTimeX();
    if (static_cast<std::make_signed<systime_t>::type>(sleep_until) > 0)
    {
        chThdSleepS(sleep_until);
    }
    chSysUnlock();

#if defined(DEBUG_BUILD) && DEBUG_BUILD
    if (static_cast<std::make_signed<systime_t>::type>(sleep_until) < 0)
    {
#if CH_CFG_USE_REGISTRY
        const char* const name = chThdGetSelfX()->name;
#else
        const char* const name = "<?>";
#endif
        DEBUG_LOG("%s: Lag %d ts\n", name,
                  static_cast<int>(static_cast<std::make_signed<systime_t>::type>(sleep_until)));
    }
#endif
}

static bool reboot_request_flag = false;

void requestReboot()
{
    reboot_request_flag = true;
}

bool isRebootRequested()
{
    return reboot_request_flag;
}

} // namespace os

extern "C"
{

__attribute__((weak))
void* __dso_handle;

__attribute__((weak))
int* __errno()
{
    static int en;
    return &en;
}


void zchSysHaltHook(const char* msg)
{
    using namespace os;

    applicationHaltHook();

    /*
     * Printing the general panic message
     */
    port_disable();
    emergencyPrint("\r\nPANIC [");
#if CH_CFG_USE_REGISTRY
    const thread_t *pthread = chThdGetSelfX();
    if (pthread && pthread->name)
    {
        emergencyPrint(pthread->name);
    }
#endif
    emergencyPrint("] ");

    if (msg != NULL)
    {
        emergencyPrint(msg);
    }
    emergencyPrint("\r\n");

#if !defined(AGGRESSIVE_SIZE_OPTIMIZATION) || (AGGRESSIVE_SIZE_OPTIMIZATION == 0)
    static const auto print_register = [](const char* name, std::uint32_t value)
        {
            emergencyPrint(name);
            emergencyPrint("\t");
            char buffer[20];
            chsnprintf(&buffer[0], sizeof(buffer), "%08x", value);
            emergencyPrint(&buffer[0]);
            emergencyPrint("\r\n");
        };

    static const auto print_stack = [](const std::uint32_t* const ptr)
        {
            print_register("Pointer", reinterpret_cast<std::uint32_t>(ptr));
            print_register("R0",      ptr[0]);
            print_register("R1",      ptr[1]);
            print_register("R2",      ptr[2]);
            print_register("R3",      ptr[3]);
            print_register("R12",     ptr[4]);
            print_register("R14[LR]", ptr[5]);
            print_register("R15[PC]", ptr[6]);
            print_register("PSR",     ptr[7]);
        };

    /*
     * Printing registers
     */
    emergencyPrint("\r\nCore registers:\r\n");
#define PRINT_CORE_REGISTER(name)       print_register(#name, __get_##name())
    PRINT_CORE_REGISTER(CONTROL);
    PRINT_CORE_REGISTER(IPSR);
    PRINT_CORE_REGISTER(APSR);
    PRINT_CORE_REGISTER(xPSR);
    PRINT_CORE_REGISTER(PRIMASK);
#if __CORTEX_M >= 3
    PRINT_CORE_REGISTER(BASEPRI);
    PRINT_CORE_REGISTER(FAULTMASK);
#endif
#if __CORTEX_M >= 4
    PRINT_CORE_REGISTER(FPSCR);
#endif
#undef PRINT_CORE_REGISTER

    emergencyPrint("\r\nProcess stack:\r\n");
    print_stack(reinterpret_cast<std::uint32_t*>(__get_PSP()));

    emergencyPrint("\r\nMain stack:\r\n");
    print_stack(reinterpret_cast<std::uint32_t*>(__get_MSP()));

    emergencyPrint("\r\nSCB:\r\n");
#define PRINT_SCB_REGISTER(name)        print_register(#name, SCB->name)
    PRINT_SCB_REGISTER(AIRCR);
    PRINT_SCB_REGISTER(SCR);
    PRINT_SCB_REGISTER(CCR);
    PRINT_SCB_REGISTER(SHCSR);
    PRINT_SCB_REGISTER(CFSR);
    PRINT_SCB_REGISTER(HFSR);
    PRINT_SCB_REGISTER(DFSR);
    PRINT_SCB_REGISTER(MMFAR);
    PRINT_SCB_REGISTER(BFAR);
    PRINT_SCB_REGISTER(AFSR);
#undef PRINT_SCB_REGISTER
#endif      // AGGRESSIVE_SIZE_OPTIMIZATION

    /*
     * Emulating a breakpoint if we're in debug mode
     */
#if defined(DEBUG_BUILD) && DEBUG_BUILD && defined(CoreDebug_DHCSR_C_DEBUGEN_Msk)
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk)
    {
        __asm volatile ("bkpt #0\n"); // Break into the debugger
    }
#endif
}

/**
 * Overrides the weak handler defined in the OS.
 * This is required because the weak handler doesn't halt the OS, which is very dangerous!
 * More context: http://www.chibios.com/forum/viewtopic.php?f=35&t=3819&p=28555#p28555
 */
void _unhandled_exception()
{
    chSysHalt("UNDEFINED IRQ");
}


void __assert_func(const char* file, int line, const char* func, const char* expr)
{
    port_disable();

    // We're using static here in order to avoid overflowing the stack in the event of assertion panic
    // Keeping the stack intact allows us to connect a debugger later and observe the state postmortem
    static char buffer[200]{};
    chsnprintf(&buffer[0], sizeof(buffer), "%s:%d:%s:%s",
               file, line, (func == nullptr) ? "" : func, expr);
    chSysHalt(&buffer[0]);

    while (true) { }
}

/// From unistd
int usleep(useconds_t useconds)
{
    assert((((uint64_t)useconds * (uint64_t)CH_CFG_ST_FREQUENCY + 999999ULL) / 1000000ULL)
           < (1ULL << CH_CFG_ST_RESOLUTION));
    // http://pubs.opengroup.org/onlinepubs/7908799/xsh/usleep.html
    if (useconds > 0)
    {
        chThdSleepMicroseconds(useconds);
    }
    return 0;
}

/// From unistd
unsigned sleep(unsigned int seconds)
{
    assert(((uint64_t)seconds * (uint64_t)CH_CFG_ST_FREQUENCY) < (1ULL << CH_CFG_ST_RESOLUTION));
    // http://pubs.opengroup.org/onlinepubs/7908799/xsh/sleep.html
    if (seconds > 0)
    {
        chThdSleepSeconds(seconds);
    }
    return 0;
}

void* malloc(size_t sz)
{
    (void) sz;
    assert(sz == 0);                    // We want debug builds to fail loudly; release builds are given a pass
    return nullptr;
}

void* calloc(size_t num, size_t sz)
{
    (void) num;
    (void) sz;
    assert((num == 0) || (sz == 0));    // We want debug builds to fail loudly; release builds are given a pass
    return nullptr;
}

void* realloc(void*, size_t sz)
{
    (void) sz;
    assert(sz == 0);                    // We want debug builds to fail loudly; release builds are given a pass
    return nullptr;
}

void free(void* p)
{
    /*
     * Certain stdlib functions, like mktime(), may call free() with zero argument, which can be safely ignored.
     */
    if (p != nullptr)
    {
        chSysHalt("free");
    }
}

}
