// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vector>
#include "xstudio/timeline/timeline.hpp"
#include "xstudio/utility/uuid.hpp"

namespace xstudio::timeline {

class TransientSelectionTimeline {
  public:
    TransientSelectionTimeline()  = default;
    ~TransientSelectionTimeline() = default;

    // Synthesizes a new Timeline object based on the source playlist and current
    // selection, preserving playlist order.
    //
    // An empty selection yields an EMPTY timeline, not the whole playlist. The
    // caller is expected to treat that as "nothing to show" and leave the viewport
    // alone: synthesizing the entire playlist here meant that merely clearing the
    // selection force-bound the viewport to a sequence of every item in the bin.
    [[nodiscard]] static Timeline synthesize(
        const utility::UuidListContainer &playlist_media, const utility::UuidSet &selection) {

        Timeline t("Transient Selection");
        for (const auto &m : playlist_media.uuids()) {
            if (selection.count(m)) {
                t.insert_media(m);
            }
        }
        return t;
    }
};

} // namespace xstudio::timeline
