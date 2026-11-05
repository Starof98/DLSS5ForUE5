#pragma once
namespace DLSS5NRMRQ
{
// Read from render thread; owned by the lifetime of the MRQ output setting.
bool IsFinalFrameRouteActive();
} // namespace DLSS5NRMRQ
