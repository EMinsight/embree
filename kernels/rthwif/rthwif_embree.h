// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-W#pragma-messages"

#if defined(EMBREE_SYCL_NIGHTLY)
#  include <sycl/sycl.hpp>
#else
#  include <CL/sycl.hpp>
#endif

#pragma clang diagnostic pop

#include "../../include/embree4/rtcore.h"
#include "../../include/embree4/rtcore_ray.h"

namespace embree
{
  inline uint8_t mask32_to_mask8( uint32_t mask ) {
    return (mask & 0xFFFFFF80) ? (0x80 | mask) : mask; // bit 7 indicates that some bit >= 7 is set
  }

  struct EmbreeHWAccel
  {
    uint32_t numTimeSegments;
    void* AccelTable[1];
  };
}

SYCL_EXTERNAL void rtcIntersectRTHW(sycl::global_ptr<RTCSceneTy> hscene, sycl::private_ptr<RTCRayQueryContext> context, sycl::private_ptr<RTCRayHit> rayhit, sycl::private_ptr<RTCIntersectArguments> args);
SYCL_EXTERNAL void rtcOccludedRTHW(sycl::global_ptr<RTCSceneTy> hscene, sycl::private_ptr<RTCRayQueryContext> context, sycl::private_ptr<RTCRay> ray, sycl::private_ptr<RTCOccludedArguments> args);
