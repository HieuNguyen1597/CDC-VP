// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/granule_store.h"

#include <algorithm>
#include <cstring>

namespace fx1::ddr3 {

void GranuleStore::read(std::uint64_t granule, std::uint8_t* out) const {
    const auto it = pages_.find(granule / kPageGranules);
    if (it == pages_.end()) {
        std::memset(out, 0, kGranule);
        return;
    }
    std::memcpy(out, it->second->data() + (granule % kPageGranules) * kGranule, kGranule);
}

void GranuleStore::write(std::uint64_t granule, const std::uint8_t* in, std::uint32_t mask) {
    const std::uint64_t page = granule / kPageGranules;
    auto it = pages_.find(page);
    if (it == pages_.end()) {
        bool nonzero = false;
        for (unsigned b = 0; b < kGranule; ++b) nonzero |= ((mask >> b) & 1u) && in[b] != 0;
        if (!nonzero) return;
        auto p = std::make_unique<Page>();
        p->fill(0);
        it = pages_.emplace(page, std::move(p)).first;
    }
    std::uint8_t* dst = it->second->data() + (granule % kPageGranules) * kGranule;
    for (unsigned b = 0; b < kGranule; ++b)
        if ((mask >> b) & 1u) dst[b] = in[b];
}

void GranuleStore::zero_range(std::uint64_t first, std::uint64_t count) {
    if (count == 0) return;
    const std::uint64_t end = first + count;
    const std::uint64_t first_page = first / kPageGranules;
    const std::uint64_t last_page = (end - 1u) / kPageGranules;
    for (auto it = pages_.lower_bound(first_page); it != pages_.end() && it->first <= last_page;) {
        const std::uint64_t g0 = it->first * kPageGranules;
        const std::uint64_t lo = std::max(first, g0);
        const std::uint64_t hi = std::min(end, g0 + kPageGranules);
        if (lo == g0 && hi == g0 + kPageGranules) {
            it = pages_.erase(it);
            continue;
        }
        std::memset(it->second->data() + (lo - g0) * kGranule, 0, static_cast<std::size_t>(hi - lo) * kGranule);
        // A page left all zero reads the same as an absent one: release it.
        const Page& page = *it->second;
        if (std::all_of(page.begin(), page.end(), [](std::uint8_t b) { return b == 0; })) it = pages_.erase(it);
        else ++it;
    }
}

} // namespace fx1::ddr3
