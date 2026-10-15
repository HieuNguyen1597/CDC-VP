// SPDX-License-Identifier: Apache-2.0
#pragma once
// Sparse DRAM backing store keyed by physical granule index (data and companion ECC
// granules share it). Same semantics as fx1::SparseStorage (4 KiB pages allocated on the
// first nonzero write, unwritten bytes read 0), reimplemented here so the IP package
// stays standalone and can erase ranges without touching every page: the startup scrub
// zeroes up to 2^27 granules by dropping resident pages only. Pure C++17.
#include <array>
#include <cstdint>
#include <map>
#include <memory>

namespace fx1::ddr3 {

class GranuleStore {
public:
    static constexpr unsigned kGranule = 32;
    static constexpr unsigned kPageGranules = 128;  // 4 KiB pages

    void read(std::uint64_t granule, std::uint8_t* out) const;  // 32 bytes
    // Writes the bytes whose bit is set in `mask` (bit b = byte b). Zero data aimed at a
    // page that does not exist allocates nothing.
    void write(std::uint64_t granule, const std::uint8_t* in, std::uint32_t mask = 0xFFFFFFFFu);
    // Zeroes granules [first, first + count): whole pages, and pages left all zero, are dropped.
    void zero_range(std::uint64_t first, std::uint64_t count);
    void clear() noexcept { pages_.clear(); }
    std::size_t resident_pages() const noexcept { return pages_.size(); }

private:
    using Page = std::array<std::uint8_t, kGranule * kPageGranules>;
    std::map<std::uint64_t, std::unique_ptr<Page>> pages_;
};

} // namespace fx1::ddr3
