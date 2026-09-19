/**
 * @file rhi_gpu_scope.h
 * @brief `HARUKA_GPU_SCOPE("nombre")`: tramo de GPU medido con timestamps, RAII, anidable.
 *
 * Es el gemelo de `HARUKA_PROFILE` para la GPU. Se pone en los PASES (una docena por frame), no en
 * cada draw: el pool tiene 256 marcas y cada scope gasta dos. Fuera de un frame del device (antes
 * de `beginFrame`, o en un backend sin timestamps) no hace nada.
 */
#pragma once
#include "rhi/rhi_device.h"

namespace Haruka { namespace RHI {
struct GpuScopeGuard {
    explicit GpuScopeGuard(const char* name) { if (Device* d = device()) d->gpuScopeBegin(name); }
    ~GpuScopeGuard() { if (Device* d = device()) d->gpuScopeEnd(); }
    GpuScopeGuard(const GpuScopeGuard&) = delete;
    GpuScopeGuard& operator=(const GpuScopeGuard&) = delete;
};
}}

#define HARUKA_GPU_SCOPE_CAT2(a, b) a##b
#define HARUKA_GPU_SCOPE_CAT(a, b) HARUKA_GPU_SCOPE_CAT2(a, b)
#define HARUKA_GPU_SCOPE(name) ::Haruka::RHI::GpuScopeGuard HARUKA_GPU_SCOPE_CAT(_gpuScope_, __LINE__)(name)
