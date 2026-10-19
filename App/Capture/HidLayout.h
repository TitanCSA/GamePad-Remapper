#pragma once
#include "ControlCodec.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <unordered_set>

namespace Remapper {
// Preseed dynamic slot identities from the Windows descriptor. Consequently a
// saved AXIS_n refers to the same field even if HID report arrival order changes.
// This never reads raw guessed offsets and does not encode reports.
inline bool SeedHidLayout(HidSlots &slots, const std::vector<std::uint8_t> &descriptor) {
    if (descriptor.empty())
        return false;
    auto data = reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<std::uint8_t *>(descriptor.data()));
    HIDP_CAPS caps{};
    if (HidP_GetCaps(data, &caps) != HIDP_STATUS_SUCCESS || caps.NumberInputValueCaps > 1024 ||
        caps.NumberInputButtonCaps > 1024)
        return false;
    std::vector<HIDP_LINK_COLLECTION_NODE> nodes(caps.NumberLinkCollectionNodes);
    ULONG nodeCount = static_cast<ULONG>(nodes.size());
    if (nodeCount && (HidP_GetLinkCollectionNodes(nodes.data(), &nodeCount, data) != HIDP_STATUS_SUCCESS ||
                      nodeCount > nodes.size()))
        return false;
    nodes.resize(nodeCount);
    std::vector<std::uint8_t> trigger(nodes.size());
    std::vector<unsigned> path;
    for (unsigned root = 0; root < nodes.size(); ++root) {
        if (trigger[root])
            continue;
        path.clear();
        auto link = root;
        bool isTrigger = false;
        while (link < nodes.size() && !trigger[link]) {
            trigger[link] = 3;
            path.push_back(link);
            const auto &node = nodes[link];
            if (node.LinkUsagePage == 5 && node.LinkUsage == 0x39) {
                isTrigger = true;
                break;
            }
            if (!link || node.Parent == link)
                break;
            link = node.Parent;
        }
        if (link < nodes.size() && trigger[link] == 2)
            isTrigger = true;
        for (auto item : path)
            trigger[item] = isTrigger ? 2 : 1;
    }
    std::vector<FieldKey> fields;
    fields.reserve(256);
    USHORT count = caps.NumberInputValueCaps;
    std::vector<HIDP_VALUE_CAPS> values(count);
    if (count && (HidP_GetValueCaps(HidP_Input, values.data(), &count, data) != HIDP_STATUS_SUCCESS ||
                  count > values.size()))
        return false;
    values.resize(count);
    std::unordered_set<std::uint64_t> seen;
    for (const auto &cap : values) {
        if (cap.IsAlias || cap.BitSize > 32)
            continue;
        if (!cap.BitSize || !cap.ReportCount ||
            (cap.IsRange &&
             (cap.Range.UsageMax < cap.Range.UsageMin || cap.Range.DataIndexMax < cap.Range.DataIndexMin ||
              cap.Range.UsageMax - cap.Range.UsageMin != cap.Range.DataIndexMax - cap.Range.DataIndexMin)))
            return false;
        const unsigned first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage,
                       last = cap.IsRange ? cap.Range.UsageMax : first;
        for (unsigned usage = first; usage <= last; ++usage) {
            const auto index = cap.IsRange ? static_cast<USHORT>(cap.Range.DataIndexMin + usage - first)
                                           : cap.NotRange.DataIndex;
            if (!seen.insert((static_cast<std::uint64_t>(cap.ReportID) << 32) | index).second)
                continue;
            unsigned kind = 4;
            if ((cap.UsagePage == 2 && usage == 0xc0) ||
                (cap.LinkCollection < trigger.size() && trigger[cap.LinkCollection] == 2))
                kind = 2;
            else if (cap.UsagePage == 1 && usage >= 0x30 && usage <= 0x38)
                kind = 1;
            else if (cap.UsagePage == 1 && usage == 0x39)
                kind = 3;
            const std::int64_t minimum = cap.LogicalMin,
                               maximum = cap.LogicalMin < 0 ? static_cast<std::int64_t>(cap.LogicalMax)
                                                            : static_cast<std::uint32_t>(cap.LogicalMax);
            if ((kind >= 1 && kind <= 3 && !cap.IsAbsolute) ||
                ((kind == 1 || kind == 2) && maximum <= minimum))
                kind = 4;
            const unsigned elements = cap.IsRange ? 1u : cap.ReportCount;
            if (fields.size() + elements > 65536)
                return false;
            for (unsigned element = 0; element < elements; ++element)
                fields.push_back({cap.UsagePage, static_cast<USHORT>(usage), cap.LinkCollection, index,
                                  element, cap.ReportID, static_cast<std::uint8_t>(kind)});
        }
    }
    seen.clear();
    count = caps.NumberInputButtonCaps;
    std::vector<HIDP_BUTTON_CAPS> buttons(count);
    if (count && (HidP_GetButtonCaps(HidP_Input, buttons.data(), &count, data) != HIDP_STATUS_SUCCESS ||
                  count > buttons.size()))
        return false;
    buttons.resize(count);
    for (const auto &cap : buttons) {
        if (cap.IsAlias)
            continue;
        if (cap.IsRange &&
            (cap.Range.UsageMax < cap.Range.UsageMin || cap.Range.DataIndexMax < cap.Range.DataIndexMin ||
             cap.Range.UsageMax - cap.Range.UsageMin != cap.Range.DataIndexMax - cap.Range.DataIndexMin))
            return false;
        const unsigned first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage,
                       last = cap.IsRange ? cap.Range.UsageMax : first;
        for (unsigned usage = first; usage <= last; ++usage) {
            const auto index = cap.IsRange ? static_cast<USHORT>(cap.Range.DataIndexMin + usage - first)
                                           : cap.NotRange.DataIndex;
            if (!seen.insert((static_cast<std::uint64_t>(cap.ReportID) << 32) | index).second)
                continue;
            if (fields.size() >= 65536)
                return false;
            fields.push_back(
                {cap.UsagePage, static_cast<USHORT>(usage), cap.LinkCollection, index, 0, cap.ReportID, 0});
        }
    }
    slots.Seed(std::move(fields));
    return true;
}
} // namespace Remapper
