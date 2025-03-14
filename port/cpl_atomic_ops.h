/**********************************************************************
 *
 * Name:     cpl_atomic_ops.h
 * Project:  CPL - Common Portability Library
 * Purpose:  Atomic operation functions.
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 **********************************************************************
 * Copyright (c) 2009-2010, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef CPL_ATOMIC_OPS_INCLUDED
#define CPL_ATOMIC_OPS_INCLUDED

#include "cpl_port.h"

CPL_C_START

/** Add a value to a pointed integer in a thread and SMP-safe way
 * and return the resulting value of the operation.
 *
 * This function, which in most cases is implemented by a few
 * efficient machine instructions, guarantees that the value pointed
 * by ptr will be incremented in a thread and SMP-safe way.
 * The variables for this function must be aligned on a 32-bit boundary.
 *
 * Depending on the platforms, this function can also act as a
 * memory barrier, but this should not be assumed.
 *
 * Current platforms/architectures where an efficient implementation
 * exists are MacOSX, MS Windows, i386/x86_64 with GCC and platforms
 * supported by GCC 4.1 or higher. For other platforms supporting
 * the pthread library, and when GDAL is configured with thread-support,
 * the atomicity will be done with a mutex, but with
 * reduced efficiency. For the remaining platforms, a simple addition
 * with no locking will be done...
 *
 * @param ptr a pointer to an integer to increment
 * @param increment the amount to add to the pointed integer
 * @return the pointed value AFTER the result of the addition
 */
int CPL_DLL CPLAtomicAdd(volatile int *ptr, int increment);

/** Increment of 1 the pointed integer in a thread and SMP-safe way
 * and return the resulting value of the operation.
 *
 * @see CPLAtomicAdd for the details and guarantees of this atomic
 *      operation
 *
 * @param ptr a pointer to an integer to increment
 * @return the pointed value AFTER the operation: *ptr + 1
 */
#define CPLAtomicInc(ptr) CPLAtomicAdd(ptr, 1)

/** Decrement of 1 the pointed integer in a thread and SMP-safe way
 * and return the resulting value of the operation.
 *
 * @see CPLAtomicAdd for the details and guarantees of this atomic
 *      operation
 *
 * @param ptr a pointer to an integer to decrement
 * @return the pointed value AFTER the operation: *ptr - 1
 */
#define CPLAtomicDec(ptr) CPLAtomicAdd(ptr, -1)

/** Compares *ptr with oldval. If *ptr == oldval, then *ptr is assigned
 * newval and TRUE is returned. Otherwise nothing is done, and FALSE is
 * returned.
 *
 * Current platforms/architectures where an efficient implementation
 * exists are MacOSX, MS Windows, i386/x86_64 with GCC and platforms
 * supported by GCC 4.1 or higher. For other platforms supporting
 * the pthread library, and when GDAL is configured with thread-support,
 * the atomicity will be done with a mutex, but with
 * reduced efficiency. For the remaining platforms, a simple compare and
 * exchange with no locking will be done...
 *
 * @param ptr a pointer to an integer (aligned on 32bit boundary).
 * @param oldval old value
 * @param newval new value
 * @return TRUE if the exchange has been done
 */
int CPLAtomicCompareAndExchange(volatile int *ptr, int oldval, int newval);

CPL_C_END

#endif /* CPL_ATOMIC_OPS_INCLUDED */
