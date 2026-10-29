// The kernel vblank wait d3d_vblank.cpp calls; the host test never paces.
#include <cstdint>
extern "C" void recomp_kernel_wait_for_vblank(uint64_t) {}
