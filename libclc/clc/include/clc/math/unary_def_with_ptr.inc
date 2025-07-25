//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <clc/utils.h>

#ifndef __IMPL_FUNCTION
#define __IMPL_FUNCTION(x) __CLC_CONCAT(__clc_, x)
#endif

_CLC_OVERLOAD _CLC_DEF __CLC_GENTYPE FUNCTION(__CLC_GENTYPE x,
                                              private __CLC_GENTYPE *ptr) {
  return __IMPL_FUNCTION(FUNCTION)(x, ptr);
}

_CLC_OVERLOAD _CLC_DEF __CLC_GENTYPE FUNCTION(__CLC_GENTYPE x,
                                              global __CLC_GENTYPE *ptr) {
  return __IMPL_FUNCTION(FUNCTION)(x, ptr);
}

_CLC_OVERLOAD _CLC_DEF __CLC_GENTYPE FUNCTION(__CLC_GENTYPE x,
                                              local __CLC_GENTYPE *ptr) {
  return __IMPL_FUNCTION(FUNCTION)(x, ptr);
}

#if _CLC_DISTINCT_GENERIC_AS_SUPPORTED
_CLC_OVERLOAD _CLC_DEF __CLC_GENTYPE FUNCTION(__CLC_GENTYPE x,
                                              generic __CLC_GENTYPE *ptr) {
  return __IMPL_FUNCTION(FUNCTION)(x, ptr);
}
#endif
