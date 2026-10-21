#pragma once
#include "Service.h"
#include "Interceptor.h"
#include <functional>

namespace gpr {
// Private application transport boundary. Production uses only the read-only
// telemetry IOCTLs; deterministic tests supply an event-backed transport.
struct MonitorCapabilities {
    std::uint64_t Session = 0, TimestampFrequency = 0;
    std::uint32_t Capacity = 0;
};
struct MonitorFrame {
    std::uint64_t Sequence = 0, Timestamp = 0;
    std::uint32_t Family = 0, Instance = 0, Direction = 0, Flags = 0, Status = 0;
    IC_NATIVE_GAMEPAD_STATE Native{};
    std::vector<std::uint8_t> Raw;
};
struct MonitorBatch {
    std::uint64_t Session = 0, NextSequence = 0, Dropped = 0;
    std::vector<MonitorFrame> Frames;
};
struct MonitorMetadata {
    IC_DEVICE_INFO Info{};
    std::vector<std::uint8_t> Preparsed;
    std::wstring Instance;
    std::string ConsumerError;
    std::uint32_t Flags = 0;
};
class MonitorTransport {
  public:
    virtual ~MonitorTransport() = default;
    virtual MonitorCapabilities Open(HANDLE changedEvent) = 0;
    virtual MonitorBatch Read(unsigned maximum) = 0;
    virtual MonitorMetadata Device(unsigned family, unsigned instance, bool preparsed) = 0;
    // Ordinal metadata enumeration never creates a capture or starts the trial.
    virtual bool Enumerate(unsigned family, unsigned ordinal, MonitorMetadata &metadata) = 0;
    virtual void Removed(unsigned, unsigned) {}
};
struct MonitorSnapshot {
    bool Requested = false, Running = false;
    std::string Status, Error;
    std::uint64_t Frames = 0, Dropped = 0, TimestampFrequency = 0;
    std::vector<ServiceDevice> Devices;
};
class DriverMonitor final {
  public:
    using Factory = std::function<std::unique_ptr<MonitorTransport>()>;
    explicit DriverMonitor(std::function<void()> changed, Factory factory = {});
    ~DriverMonitor();
    DriverMonitor(const DriverMonitor &) = delete;
    DriverMonitor &operator=(const DriverMonitor &) = delete;
    // Async, no capture, no installation, no automatic retry on errors.
    void Start();
    void Stop() noexcept;
    MonitorSnapshot Snapshot();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gpr
