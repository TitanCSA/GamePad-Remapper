#include "../Capture/ControlCodec.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
void NativeExactness() {
    for (int value = -32768; value <= 32767; ++value) {
        IC_NATIVE_GAMEPAD_STATE state{};
        state.left_x = static_cast<short>(value);
        state.left_y = static_cast<short>(~value);
        state.right_x = static_cast<short>(value / 2);
        state.right_y = static_cast<short>(-value / 2);
        state.buttons = 0x8f3fu;
        state.left_trigger = static_cast<unsigned short>(value & 255);
        state.right_trigger = static_cast<unsigned short>((value >> 1) & 255);
        state.extra_buttons = 0x89;
        state.packet = 0x12345678;
        std::array<int, 256> physical{};
        std::array<std::uint8_t, 256> available{};
        Remapper::ReadNative(state, false, physical, available);
        auto exact = state;
        Remapper::WriteNative(exact, false, physical, physical);
        Check(!std::memcmp(&state, &exact, sizeof(state)), "No edit must preserve every native state byte");
        auto edited = physical;
        edited[0] = 100;
        Remapper::WriteNative(exact, false, physical, edited);
        Check(exact.left_x == (physical[0] == 100 ? state.left_x : 32767),
              "XUSB full-scale edit with exact no-op preservation");
        exact.left_x = state.left_x;
        Check(!std::memcmp(&state, &exact, sizeof(state)),
              "One axis edit must preserve every unrelated field");
    }
    for (bool gip : {false, true})
        for (int value = 0; value <= 100; ++value) {
            IC_NATIVE_GAMEPAD_STATE state{};
            std::array<int, 256> physical{}, output{};
            std::array<std::uint8_t, 256> available{};
            Remapper::ReadNative(state, gip, physical, available);
            output = physical;
            output[4] = value;
            output[5] = 100 - value;
            Remapper::WriteNative(state, gip, physical, output);
            const int limit = gip ? 1023 : 255;
            Check(state.left_trigger <= limit && state.right_trigger <= limit,
                  "Native triggers remain in wire range");
        }
    IC_NATIVE_GAMEPAD_STATE gip{};
    gip.buttons = 0x8014;
    gip.extra_buttons = 1;
    std::array<int, 256> values{}, output{};
    std::array<std::uint8_t, 256> available{};
    Remapper::ReadNative(gip, true, values, available);
    Check(values[10] == 100 && values[17] == 100 && values[19] == 100 && values[20] == 100,
          "GIP button bit layout");
    output = values;
    output[11] = 100;
    output[10] = 0;
    Remapper::WriteNative(gip, true, values, output);
    Check((gip.buttons & 0x30) == 0x20 && gip.extra_buttons == 1, "GIP edits preserve guide");
    IC_NATIVE_GAMEPAD_STATE xusb{};
    xusb.buttons = 0x0400;
    xusb.extra_buttons = 0x83;
    Remapper::ReadNative(xusb, false, values, available);
    Check(values[20] == 100, "XUSB guide comes from the dedicated extra-buttons field");
    output = values;
    output[20] = 0;
    Remapper::WriteNative(xusb, false, values, output);
    Check(xusb.extra_buttons == 0x82 && xusb.buttons == 0x0400,
          "Guide edits preserve unrelated protocol bits");
    Interceptor::Controller_GIP_Raw_Input raw{};
    raw.report_length = 50;
    for (unsigned i = 0; i < 50; ++i)
        raw.report[i] = static_cast<std::uint8_t>(i ^ 0xa6);
    auto original = raw;
    Remapper::WriteNative(raw.input.state, true, values, output);
    Check(raw.report_length == 50 && !std::memcmp(raw.report, original.report, 64),
          "Opaque GIP envelope and tail remain immutable");
}
void HidIdentities() {
    Remapper::HidSlots slots;
    Interceptor::Hid_Decoded_Input input;
    Interceptor::Hid_Value axis;
    axis.kind = Interceptor::Hid_Value_Kind::Axis;
    axis.usage_page = 1;
    axis.usage = 0x30;
    axis.minimum = -32768;
    axis.maximum = 32767;
    axis.bit_size = 16;
    axis.normalized_valid = true;
    axis.normalized = 0.25;
    input.state.axes.push_back(axis);
    axis.link_collection = 2;
    axis.normalized = -0.75;
    input.state.axes.push_back(axis);
    axis.report_id = 3;
    axis.normalized = 0.5;
    input.state.axes.push_back(axis);
    axis.element = 1;
    input.state.axes.push_back(axis);
    std::array<int, 256> physical{}, output{};
    std::array<std::uint8_t, 256> available{};
    slots.Read(input, physical, available);
    Check(physical[96] == 25 && physical[97] == -75 && physical[98] == 50 && physical[99] == 50,
          "Repeated X usages remain four distinct dynamic controls");
    Check(!available[0] && !available[10], "A HID X usage does not guess a console face layout");
    output = physical;
    output[97] = 100;
    slots.Write(input, physical, output);
    Check(input.state.axes[0].normalized == 0.25 && input.state.axes[1].normalized == 1 &&
              input.state.axes[2].normalized == 0.5,
          "An edit reaches its exact HID collection");
    Check(slots.Resolve(Remapper::Key(input.state.axes[0], 1)) == 96,
          "Identity stays stable when report order changes");
    Interceptor::Hid_Value hat;
    hat.kind = Interceptor::Hid_Value_Kind::Hat;
    hat.minimum = 0;
    hat.maximum = 7;
    hat.bit_size = 4;
    hat.value = 8;
    hat.has_null = true;
    hat.is_null = true;
    input.state.hats.push_back(hat);
    slots.Read(input, physical, available);
    Check(physical[192] == -1, "HID hat null decodes as neutral");
    input.state.hats[0].is_null = false;
    input.state.hats[0].value = 2;
    slots.Read(input, physical, available);
    output = physical;
    output[192] = -1;
    slots.Write(input, physical, output);
    Check(input.state.hats[0].value == 8, "HID neutral is written as an actual representable null value");
    for (unsigned i = 0; i < 65; ++i) {
        auto key = Remapper::Key(axis, 1);
        key.index = static_cast<std::uint16_t>(i + 200);
        slots.Resolve(key);
    }
    Check(slots.truncated, "Excess controls are explicitly diagnosed");
    Check(Remapper::SourceKey(3, 1) != Remapper::SourceKey(4, 1),
          "Source IDs are isolated by transport family");
    std::vector<Remapper::FieldKey> schema{
        Remapper::Key(input.state.axes[3], 1), Remapper::Key(input.state.axes[0], 1),
        Remapper::Key(input.state.axes[2], 1), Remapper::Key(input.state.axes[1], 1)};
    Remapper::HidSlots ordered;
    ordered.Seed(schema);
    std::reverse(schema.begin(), schema.end());
    Remapper::HidSlots reversed;
    reversed.Seed(schema);
    for (const auto &key : schema)
        Check(ordered.Find(key) == reversed.Find(key),
              "Descriptor preseed is independent of report arrival order");
    Interceptor::Hid_Decoded_Input relative;
    axis.relative = true;
    axis.value = 12000;
    axis.normalized_valid = false;
    relative.state.axes.push_back(axis);
    Remapper::HidSlots rawSlots;
    available.fill(0);
    rawSlots.Read(relative, physical, available);
    Check(available[208] && physical[208] == 12000 && !available[96],
          "A relative HID axis is exposed as raw VALUE_1");
    output = physical;
    output[208] = -9000;
    rawSlots.Write(relative, physical, output);
    Check(relative.state.axes[0].value == -9000, "Raw relative axes retain full signed integer editing");
}
void RelativeReports() {
    Remapper::HidSlots slots;
    Interceptor::Hid_Decoded_Input actual;
    Interceptor::Hid_Value dial;
    dial.relative = true;
    dial.bit_size = 16;
    dial.minimum = -32768;
    dial.maximum = 32767;
    dial.usage_page = 1;
    dial.usage = 0x37;
    dial.value = 17;
    actual.state.axes.push_back(dial);
    auto hat = dial;
    hat.usage = 0x39;
    hat.data_index = 1;
    hat.value = -3;
    actual.state.hats.push_back(hat);
    Interceptor::Hid_Button button;
    button.usage_page = 9;
    button.usage = 1;
    actual.state.buttons.push_back(button);
    std::array<int, 256> physical{}, output{};
    std::array<std::uint8_t, 256> available{};
    slots.Read(actual, physical, available);
    Check(physical[208] == 17 && physical[209] == -3 && !available[192],
          "Every relative scalar, including a relative hat, retains its raw delta");
    auto synthetic = actual;
    std::vector<unsigned> relative;
    slots.PrepareSynthetic(synthetic, relative);
    Check(relative.size() == 2 && synthetic.state.axes[0].value == 0 && synthetic.state.hats[0].value == 0,
          "Synthetic templates neutralize previous relative movements");
    Check(actual.state.axes[0].value == 17 && actual.state.hats[0].value == -3,
          "Neutralizing a template cannot alter the original physical packet");
    Remapper::ClearRelative(physical, relative);
    output = physical;
    output[32] = 100;
    slots.Write(synthetic, physical, output);
    Check(synthetic.state.buttons[0].pressed && synthetic.state.axes[0].value == 0 &&
              synthetic.state.hats[0].value == 0,
          "An unrelated timed button press does not replay either previous delta");
    for (int i = 0; i < 1000; ++i) {
        synthetic = actual;
        slots.PrepareSynthetic(synthetic, relative);
        output = physical;
        output[209] = -1000 + i;
        slots.Write(synthetic, physical, output);
        Check(synthetic.state.hats[0].value == -1000 + i && synthetic.state.axes[0].value == 0,
              "Explicit relative output uses raw units without hat-direction conversion");
    }
}
void ForwardedStates() {
    using namespace Interceptor;
    Remapper::HidSlots slots;
    Hid_Decoded_Input input;
    Hid_Value axis;
    axis.kind = Hid_Value_Kind::Axis;
    axis.usage_page = 1;
    axis.usage = 0x30;
    axis.minimum = -2;
    axis.maximum = 2;
    axis.bit_size = 3;
    axis.normalized_valid = true;
    input.state.axes.push_back(axis);
    Hid_Value hat;
    hat.kind = Hid_Value_Kind::Hat;
    hat.usage_page = 1;
    hat.usage = 0x39;
    hat.minimum = 0;
    hat.maximum = 3;
    hat.bit_size = 3;
    input.state.hats.push_back(hat);
    Hid_Button button;
    button.usage_page = 9;
    button.usage = 1;
    input.state.buttons.push_back(button);
    std::array<int, 256> physical{}, requested{}, forwarded{};
    std::array<std::uint8_t, 256> available{};
    slots.Read(input, physical, available);
    for (int percent = -100; percent <= 100; ++percent) {
        auto edited = input;
        requested = physical;
        requested[96] = percent;
        requested[192] = 7;
        requested[32] = 1;
        slots.Write(edited, physical, requested);
        slots.ReadForwarded(edited, physical, requested, forwarded);
        const auto raw = std::llround(std::clamp(percent, -100, 100) * 2.0 / 100);
        Check(forwarded[96] == raw * 50,
              "Low-resolution HID monitor shows representable output rather than requested percentage");
        Check(forwarded[192] == 6 && forwarded[32] == 100,
              "Forwarded HID hat and button display reflect the actual encoded direction and press");
    }
    // Preserve actual relative movements for the last physical report. The
    // synthetic template is neutralized separately, without erasing this view.
    input.state.axes[0].relative = true;
    input.state.axes[0].normalized_valid = false;
    input.state.axes[0].minimum = -1000;
    input.state.axes[0].maximum = 1000;
    input.state.axes[0].value = 17;
    slots.Read(input, physical, available);
    requested = physical;
    std::array<std::uint8_t, 256> known{};
    slots.ReadForwarded(input, physical, requested, forwarded, &known);
    Check(forwarded[208] == 17, "Physical relative reports retain their real delta in the monitor");
    std::vector<unsigned> relative;
    slots.PrepareSynthetic(input, relative);
    Remapper::ClearRelative(physical, relative);
    Remapper::ClearRelative(forwarded, relative);
    Hid_Decoded_Input other;
    button.report_id = 2;
    button.pressed = true;
    other.state.buttons.push_back(button);
    slots.Read(other, physical, available);
    requested = physical;
    slots.ReadForwarded(other, physical, requested, forwarded, &known);
    Check(forwarded[208] == 0 && physical[208] == 0 && known[33] && forwarded[33] == 100,
          "An unrelated accepted HID report does not replay or invent a previous relative movement");
    other.state.buttons[0].report_id = 3;
    slots.Read(other, physical, available);
    // The driver rejects this report: do not commit ReadForwarded. Other
    // successfully delivered reports must not give the new control validity.
    Check(available[34] && !known[34] && known[33],
          "A newly observed but unaccepted report stays unknown independently of accepted reports");
    for (bool gip : {false, true}) {
        IC_NATIVE_GAMEPAD_STATE native{};
        Remapper::ReadNative(native, gip, physical, available);
        requested = physical;
        requested[0] = 999;
        requested[4] = -999;
        Remapper::WriteNative(native, gip, physical, requested);
        Remapper::ReadNative(native, gip, forwarded, available);
        Check(forwarded[0] == 100 && forwarded[4] == 0,
              "Native output monitor reflects saturation on both XUSB and GIP, not an invalid request");
    }
}
} // namespace
int main() {
    try {
        NativeExactness();
        HidIdentities();
        RelativeReports();
        ForwardedStates();
        std::printf("Native pure checks passed: %u; no driver access\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native check failed after %u: %s\n", checks, error.what());
        return 1;
    }
}
