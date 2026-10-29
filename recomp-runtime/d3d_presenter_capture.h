#ifndef DOAXBV_RECOMP_D3D_PRESENTER_CAPTURE_H
#define DOAXBV_RECOMP_D3D_PRESENTER_CAPTURE_H

#include "d3d_presenter.h"

#include <cstddef>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

// Guest-side capture work since the last take, for the performance log.
struct D3dCaptureStats {
    uint64_t copied_bytes = 0u;     // payload copied out of guest memory
    uint64_t copied_geometry_bytes = 0u; // the vertex and index part of copied_bytes
    uint64_t compared_bytes = 0u;   // memcmp against an earlier copy in the packet
    uint64_t trusted_bytes = 0u;    // texture reuse accepted without a compare
    uint64_t commands = 0u;
    double add_ms = 0.0;            // time inside add()
};
D3dCaptureStats d3d_capture_take_stats();

class D3dCapturePacket {
public:
    // Texture spans (not vertices or indices) seen earlier in the same packet
    // at the same address, with no texture invalidation or memory release
    // since, are reused without comparing their bytes again. Off by default.
    static void setTrustTextures(bool trust);

    enum Kind : uint32_t { COMMAND, RELEASE, REPORT, INVALIDATE };
    struct Record { Kind kind; uint32_t base; uint32_t size; };

    D3dCapturePacket() = default;
    D3dCapturePacket(const D3dCapturePacket &) = delete;
    D3dCapturePacket &operator=(const D3dCapturePacket &) = delete;
    D3dCapturePacket(D3dCapturePacket &&) noexcept = default;
    D3dCapturePacket &operator=(D3dCapturePacket &&) noexcept = default;

    RecompD3dPresenterError add(const RecompD3dPresenterCommand &command);
    RecompD3dPresenterError addRelease(uint32_t base, uint32_t size);
    RecompD3dPresenterError addInvalidate(uint32_t base, uint32_t size);
    RecompD3dPresenterError addReport();
    void seal();
    size_t count() const;
    const Record &record(size_t index) const;
    const RecompD3dPresenterCommand &command(size_t index) const;
    void clear();
    // Copied commands, records and aligned payload bytes, excluding capacity.
    uint64_t bytes() const;

private:
    // Default-initializes instead of zeroing; Debug builds rebind to proxy types.
    template <typename T>
    struct NoInitAllocator : std::allocator<T> {
        using std::allocator<T>::allocator;
        template <typename U>
        struct rebind { using other = NoInitAllocator<U>; };

        template <typename U>
        void construct(U *pointer)
        {
            ::new (static_cast<void *>(pointer)) U;
        }

        template <typename U, typename... Args>
        void construct(U *pointer, Args &&...args)
        {
            ::new (static_cast<void *>(pointer)) U(std::forward<Args>(args)...);
        }
    };

    struct Entry { Record record; size_t command_index; };
    struct CapturedCommand {
        CapturedCommand() {} // Skip zeroing: add() writes every field it uses.
        alignas(8) RecompD3dPresenterCommand value;
        size_t offsets[8];
    };
    struct Span { size_t size; size_t offset; const void *source; uint32_t epoch; };

    RecompD3dPresenterError addRecord(Kind kind, uint32_t base, uint32_t size);

    std::vector<Entry> entries_;
    std::vector<CapturedCommand> commands_;
    // Word storage keeps indices and vertex data aligned after relocation.
    // resize() appends uninitialized words; add() overwrites the used bytes.
    std::vector<uint64_t, NoInitAllocator<uint64_t>> payload_;
    // Keyed by source, size and sampled bytes; memcmp still confirms each reuse.
    std::unordered_multimap<uint64_t, Span> spans_;
    bool sealed_ = false;
    // Bumped by addInvalidate and addRelease: older spans need a compare.
    uint32_t epoch_ = 0u;
};

#endif
