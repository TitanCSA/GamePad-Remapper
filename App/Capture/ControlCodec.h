#pragma once
#include "Interceptor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <tuple>

namespace Remapper {
inline int Percent(int value, int low, int high) noexcept {
    return static_cast<int>(
        std::llround(100.0 * (std::clamp(value, low, high)) /
                     (value < 0 ? -static_cast<double>(low) : static_cast<double>(high))));
}
inline short Axis(int percent) noexcept {
    percent = std::clamp(percent, -100, 100);
    return static_cast<short>(std::llround(percent * (percent < 0 ? 32768.0 : 32767.0) / 100.0));
}
inline unsigned Trigger(int percent, unsigned maximum) noexcept {
    return static_cast<unsigned>(std::llround(std::clamp(percent, 0, 100) * maximum / 100.0));
}
inline constexpr std::array<unsigned short, 16> XusbBits{0x0001, 0x0002, 0x0004, 0x0008, 0x1000, 0x2000,
                                                         0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010,
                                                         0x0040, 0x0080, 0,      0};
inline constexpr std::array<unsigned short, 16> GipBits{0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x0020,
                                                        0x0040, 0x0080, 0x1000, 0x2000, 0x0008, 0x0004,
                                                        0x4000, 0x8000, 0,      0};
inline void ReadNative(const IC_NATIVE_GAMEPAD_STATE &state, bool gip, std::array<int, 256> &values,
                       std::array<std::uint8_t, 256> &available) noexcept {
    values[0] = Percent(state.left_x, -32768, 32767);
    values[1] = Percent(state.left_y, -32768, 32767);
    values[2] = Percent(state.right_x, -32768, 32767);
    values[3] = Percent(state.right_y, -32768, 32767);
    const int triggerMax = gip ? 1023 : 255;
    values[4] = Percent(state.left_trigger, 0, triggerMax);
    values[5] = Percent(state.right_trigger, 0, triggerMax);
    for (unsigned i = 0; i < 6; ++i)
        available[i] = 1;
    const auto &bits = gip ? GipBits : XusbBits;
    for (unsigned i = 0; i < bits.size(); ++i)
        if (bits[i]) {
            values[i + 6] = (state.buttons & bits[i]) ? 100 : 0;
            available[i + 6] = 1;
        }
    values[20] = (state.extra_buttons & 1) ? 100 : 0;
    available[20] = 1;
}
inline void WriteNative(IC_NATIVE_GAMEPAD_STATE &state, bool gip, const std::array<int, 256> &physical,
                        const std::array<int, 256> &output) noexcept {
    short *axes[]{&state.left_x, &state.left_y, &state.right_x, &state.right_y};
    for (unsigned i = 0; i < 4; ++i)
        if (output[i] != physical[i])
            *axes[i] = Axis(output[i]);
    if (output[4] != physical[4])
        state.left_trigger = static_cast<std::uint16_t>(Trigger(output[4], gip ? 1023u : 255u));
    if (output[5] != physical[5])
        state.right_trigger = static_cast<std::uint16_t>(Trigger(output[5], gip ? 1023u : 255u));
    const auto &bits = gip ? GipBits : XusbBits;
    for (unsigned i = 0; i < bits.size(); ++i)
        if (bits[i] && output[i + 6] != physical[i + 6]) {
            if (output[i + 6] > 0)
                state.buttons |= bits[i];
            else
                state.buttons &= static_cast<unsigned short>(~bits[i]);
        }
    if (output[20] != physical[20])
        state.extra_buttons =
            static_cast<std::uint8_t>((state.extra_buttons & ~1u) | (output[20] > 0 ? 1u : 0u));
}

struct FieldKey {
    std::uint16_t page = 0, usage = 0, collection = 0, index = 0;
    std::uint32_t element = 0;
    std::uint8_t report = 0, kind = 0;
    bool operator==(const FieldKey &b) const noexcept {
        return page == b.page && usage == b.usage && collection == b.collection && index == b.index &&
               element == b.element && report == b.report && kind == b.kind;
    }
};
struct KeyHash {
    std::size_t operator()(const FieldKey &key) const noexcept {
        std::size_t hash = 1469598103934665603ull;
        for (const auto v : {static_cast<unsigned>(key.page), static_cast<unsigned>(key.usage),
                             static_cast<unsigned>(key.collection), static_cast<unsigned>(key.index),
                             key.element, static_cast<unsigned>(key.report), static_cast<unsigned>(key.kind)})
            hash = (hash ^ v) * 1099511628211ull;
        return hash;
    }
};
inline FieldKey Key(const Interceptor::Hid_Value &value, unsigned kind) noexcept {
    // Relative and constant-range scalars have no meaningful percentage.
    // Expose their exact integer under VALUE_n rather than clipping AXIS_n.
    if ((kind >= 1 && kind <= 3 && value.relative) ||
        ((kind == 1 || kind == 2) && value.maximum <= value.minimum))
        kind = 4;
    return {value.usage_page, value.usage,     value.link_collection,          value.data_index,
            value.element,    value.report_id, static_cast<std::uint8_t>(kind)};
}
inline FieldKey Key(const Interceptor::Hid_Button &button) noexcept {
    return {
        button.usage_page, button.usage, button.link_collection, button.data_index, 0, button.report_id, 0};
}
// Slots are assigned once per exact report/collection/data-index identity.
// HID usages do not establish a console's A/B/X/Y layout: dynamic slots remain
// authoritative, with no model tables or raw-offset assumptions.
class HidSlots {
    std::unordered_map<FieldKey, unsigned, KeyHash> slots_;
    std::array<unsigned, 5> counts_{};

  public:
    bool truncated = false;
    HidSlots() { slots_.reserve(256); }
    void Seed(std::vector<FieldKey> fields) {
        std::sort(fields.begin(), fields.end(), [](const auto &a, const auto &b) {
            return std::tie(a.kind, a.report, a.collection, a.index, a.element, a.page, a.usage) <
                   std::tie(b.kind, b.report, b.collection, b.index, b.element, b.page, b.usage);
        });
        for (const auto &field : fields)
            (void)Resolve(field);
    }
    unsigned Resolve(const FieldKey &key) {
        const auto found = slots_.find(key);
        if (found != slots_.end())
            return found->second;
        constexpr unsigned bases[]{32, 96, 160, 192, 208}, limits[]{64, 64, 32, 16, 48};
        const unsigned kind = key.kind;
        if (kind >= 5 || counts_[kind] >= limits[kind]) {
            truncated = true;
            return 256;
        }
        const auto slot = bases[kind] + counts_[kind]++;
        slots_.emplace(key, slot);
        return slot;
    }
    unsigned Find(const FieldKey &key) const noexcept {
        const auto found = slots_.find(key);
        return found == slots_.end() ? 256 : found->second;
    }
    static int Read(const Interceptor::Hid_Value &value, unsigned kind) noexcept {
        if (kind == 3) {
            if (value.is_null || value.maximum < value.minimum)
                return -1;
            const auto index = value.value - value.minimum;
            const auto range = value.maximum - value.minimum + 1;
            if (index < 0 || index >= range || range <= 0)
                return -1;
            return static_cast<int>(std::min<std::int64_t>(7, (index * 8) / range));
        }
        if (kind == 1 || kind == 2)
            return value.normalized_valid ? static_cast<int>(std::llround(value.normalized * 100.0)) : 0;
        return static_cast<int>(std::clamp(value.value,
                                           static_cast<std::int64_t>((std::numeric_limits<int>::min)()),
                                           static_cast<std::int64_t>((std::numeric_limits<int>::max)())));
    }
    void Read(const Interceptor::Hid_Decoded_Input &input, std::array<int, 256> &values,
              std::array<std::uint8_t, 256> &available) {
        for (const auto &button : input.state.buttons) {
            const auto slot = Resolve(Key(button));
            if (slot < 256) {
                values[slot] = button.pressed ? 100 : 0;
                available[slot] = 1;
            }
        }
        const std::vector<Interceptor::Hid_Value> *lists[]{&input.state.axes, &input.state.triggers,
                                                           &input.state.hats, &input.state.values};
        for (unsigned i = 0; i < 4; ++i)
            for (const auto &value : *lists[i]) {
                const auto key = Key(value, i + 1);
                const auto slot = Resolve(key);
                if (slot < 256) {
                    values[slot] = Read(value, key.kind);
                    available[slot] = (key.kind == 1 || key.kind == 2)
                                          ? static_cast<std::uint8_t>(value.normalized_valid)
                                          : 1;
                }
            }
    }
    void Write(Interceptor::Hid_Decoded_Input &input, const std::array<int, 256> &physical,
               const std::array<int, 256> &output) const {
        for (auto &button : input.state.buttons) {
            const auto slot = Find(Key(button));
            if (slot < 256 && output[slot] != physical[slot])
                button.pressed = output[slot] > 0;
        }
        std::vector<Interceptor::Hid_Value> *lists[]{&input.state.axes, &input.state.triggers,
                                                     &input.state.hats, &input.state.values};
        for (unsigned i = 0; i < 4; ++i)
            for (auto &value : *lists[i]) {
                const auto key = Key(value, i + 1);
                const auto slot = Find(key);
                if (slot >= 256 || output[slot] == physical[slot])
                    continue;
                if ((key.kind == 1 || key.kind == 2) && value.normalized_valid)
                    value.normalized = std::clamp(output[slot] / 100.0, key.kind == 1 ? -1.0 : 0.0, 1.0);
                else if (key.kind == 3) {
                    if (output[slot] < 0 && value.has_null) {
                        const auto lower = value.minimum < 0 ? -(1ll << (value.bit_size - 1)) : 0;
                        const auto upper = value.minimum < 0
                                               ? (1ll << (value.bit_size - 1)) - 1
                                               : static_cast<std::int64_t>((1ull << value.bit_size) - 1);
                        if (value.maximum < upper)
                            value.value = value.maximum + 1;
                        else if (value.minimum > lower)
                            value.value = value.minimum - 1;
                        value.is_null = true;
                        continue;
                    }
                    value.is_null = false;
                    const auto range = value.maximum - value.minimum + 1;
                    if (range > 0)
                        value.value = std::clamp(value.minimum + (std::clamp(output[slot], 0, 7) * range) / 8,
                                                 value.minimum, value.maximum);
                } else
                    value.value =
                        std::clamp(static_cast<std::int64_t>(output[slot]), value.minimum, value.maximum);
            }
    }
    // A relative scalar describes one movement, not held state. Reusing its last
    // report for an unrelated button/axis macro must not replay that movement.
    void PrepareSynthetic(Interceptor::Hid_Decoded_Input &input, std::vector<unsigned> &relative) const {
        relative.clear();
        std::vector<Interceptor::Hid_Value> *lists[]{&input.state.axes, &input.state.triggers,
                                                     &input.state.hats, &input.state.values};
        for (unsigned kind = 0; kind < 4; ++kind)
            for (auto &value : *lists[kind])
                if (value.relative) {
                    const auto slot = Find(Key(value, kind + 1));
                    if (slot < 256)
                        relative.push_back(slot);
                    value.value = 0;
                    value.is_null = false;
                }
    }
};
inline void ClearRelative(std::array<int, 256> &values, const std::vector<unsigned> &slots) noexcept {
    for (const auto slot : slots)
        values[slot] = 0;
}
inline std::uint64_t SourceKey(unsigned family, unsigned id) noexcept {
    return (static_cast<std::uint64_t>(family) << 32) | id;
}
} // namespace Remapper
