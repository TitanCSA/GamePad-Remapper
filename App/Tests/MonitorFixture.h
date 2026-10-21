#pragma once
#include "../Capture/DriverMonitor.h"
#include "../Capture/DriverMonitorProtocol.h"
#include <atomic>
#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>

namespace monitor_fixture {
struct State {
    std::mutex Gate;
    HANDLE Event = nullptr;
    std::deque<gpr::MonitorFrame> Queue;
    std::vector<gpr::MonitorMetadata> Metadata;
    std::uint64_t Session = 71, Next = 1, Cursor = 1, Dropped = 0;
    std::atomic<unsigned> Opens{0}, Closes{0}, Reads{0}, Queries{0};
    std::string OpenError, ReadError;
    void Push(gpr::MonitorFrame frame) {
        std::lock_guard<std::mutex> lock(Gate);
        if (!frame.Sequence) frame.Sequence = Next++;
        else Next = std::max(Next, frame.Sequence + 1);
        if (!frame.Timestamp) frame.Timestamp = frame.Sequence * 1000;
        Queue.push_back(std::move(frame));
        if (Event) SetEvent(Event);
    }
};
inline gpr::MonitorMetadata Metadata(unsigned family, unsigned instance, bool hid = false) {
    gpr::MonitorMetadata result;
    result.Info.type = family; result.Info.instance_token = instance; result.Info.id = instance;
    const wchar_t name[] = L"Contr\u00f4leur de test";
    std::copy(std::begin(name), std::end(name), result.Info.name);
    if (hid) result.Preparsed.resize(1024);
    return result;
}
class Transport final : public gpr::MonitorTransport {
    std::shared_ptr<State> state_;
    bool opened_ = false;
  public:
    explicit Transport(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Transport() override {
        std::lock_guard<std::mutex> lock(state_->Gate);
        if (opened_) { state_->Event = nullptr; ++state_->Closes; }
    }
    gpr::MonitorCapabilities Open(HANDLE changed) override {
        std::lock_guard<std::mutex> lock(state_->Gate);
        ++state_->Opens;
        if (!state_->OpenError.empty()) throw std::runtime_error(state_->OpenError);
        opened_ = true; state_->Event = changed;
        if (!state_->Queue.empty()) SetEvent(changed);
        return {state_->Session, 10000000, IC_MONITOR_CAPACITY};
    }
    gpr::MonitorBatch Read(unsigned maximum) override {
        std::lock_guard<std::mutex> lock(state_->Gate);
        ++state_->Reads;
        if (!maximum || maximum > IC_MONITOR_MAX_READ) throw std::runtime_error("Unbounded monitor read");
        if (!state_->ReadError.empty()) throw std::runtime_error(state_->ReadError);
        gpr::MonitorBatch result;
        result.Session = state_->Session; result.Dropped = state_->Dropped;
        while (!state_->Queue.empty() && result.Frames.size() < maximum) {
            result.Frames.push_back(std::move(state_->Queue.front()));
            state_->Queue.pop_front();
            state_->Cursor = result.Frames.back().Sequence + 1;
        }
        result.NextSequence = state_->Cursor;
        if (state_->Queue.empty()) ResetEvent(state_->Event);
        return result;
    }
    gpr::MonitorMetadata Device(unsigned family, unsigned instance, bool preparsed) override {
        std::lock_guard<std::mutex> lock(state_->Gate);
        ++state_->Queries;
        for (const auto &metadata : state_->Metadata)
            if (metadata.Info.type == family && metadata.Info.instance_token == instance) {
                auto result = metadata;
                if (!preparsed) result.Preparsed.clear();
                return result;
            }
        throw std::runtime_error("Fixture metadata not found");
    }
    bool Enumerate(unsigned family, unsigned ordinal, gpr::MonitorMetadata &result) override {
        std::lock_guard<std::mutex> lock(state_->Gate);
        unsigned index = 0;
        for (const auto &metadata : state_->Metadata)
            if (metadata.Info.type == family && ++index == ordinal) {
                result = metadata; result.Preparsed.clear(); return true;
            }
        return false;
    }
};
inline gpr::DriverMonitor::Factory Factory(const std::shared_ptr<State> &state) {
    return [state] { return std::make_unique<Transport>(state); };
}
inline gpr::MonitorFrame Native(unsigned family, unsigned instance, unsigned direction, int axis) {
    gpr::MonitorFrame frame;
    frame.Family = family; frame.Instance = instance; frame.Direction = direction;
    frame.Flags = IC_MONITOR_NATIVE_VALID | (family == IC_TYPE_CONTROLLER_XUSB ? IC_MONITOR_GUIDE_VALID : 0u);
    frame.Native.left_x = static_cast<short>(axis);
    frame.Native.left_y = 933;
    frame.Raw.resize(family == IC_TYPE_CONTROLLER_XUSB ? 20 : 50, 0xa5);
    return frame;
}
inline gpr::MonitorFrame Hid(unsigned family, unsigned instance, unsigned direction,
                             unsigned report, unsigned value, unsigned buttons = 0) {
    gpr::MonitorFrame frame;
    frame.Family = family; frame.Instance = instance; frame.Direction = direction; frame.Flags = IC_MONITOR_RAW;
    frame.Raw = {static_cast<std::uint8_t>(report), static_cast<std::uint8_t>(value),
                 static_cast<std::uint8_t>(buttons), 0x5a};
    return frame;
}
} // namespace monitor_fixture
