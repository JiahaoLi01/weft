#pragma once

#include "caseelephant/constants/render.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>
#include <vector>

namespace caseelephant::render {

    using DeviceEpoch = std::uint64_t;
    using QueueId = std::uint64_t;
    using SubmissionSerial = std::uint64_t;

    // These are internal C++ handles, not a public C ABI. Generation and epoch are
    // validated independently; a reused slot never makes an old handle valid again.
    struct ResourceId {
        std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;
        DeviceEpoch device_epoch = 0;
        friend bool operator==(const ResourceId&, const ResourceId&) = default;
    };

    struct PreparedFrameId {
        std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;
        DeviceEpoch device_epoch = 0;
        friend bool operator==(const PreparedFrameId&, const PreparedFrameId&) = default;
    };

    struct UseTokenId {
        std::uint32_t slot = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t generation = 0;
        DeviceEpoch device_epoch = 0;
        friend bool operator==(const UseTokenId&, const UseTokenId&) = default;
    };

    enum class LifetimeResult {
        Ok,
        InvalidArgument,
        WrongThread,
        StaleHandle,
        StaleDeviceEpoch,
        InvalidQueue,
        InvalidSerial,
        InvalidState,
        Busy,
        HostConfirmationRequired,
        DeviceUnavailable,
        ResourceFailed,
        ShuttingDown,
        OutOfMemory,
        InternalError,
    };

    enum class ResourceKind {
        Texture,
        Buffer,
        AtlasPage,
        Staging
    };

    // Immutable for the lifetime of a ResourceId. Replacing an image or atlas page
    // creates a new ResourceId rather than modifying the old record.
    struct ResourceDescription {
        std::uint64_t asset_id = 0;
        std::uint64_t asset_version = 0;
        std::uint64_t byte_size = 0;
        ResourceKind kind = ResourceKind::Texture;
        friend bool operator==(const ResourceDescription&, const ResourceDescription&) = default;
    };

    enum class ResourcePhase {
        Created,
        Encoded,
        Submitted,
        Ready,
        Failed,
        Abandoned
    };
    enum class UseState {
        Encoded,
        Submitted
    };

    struct ResourceInfo {
        ResourceDescription description;
        ResourcePhase phase = ResourcePhase::Created;
        std::size_t cpu_leases = 0;
        std::size_t prepared_pins = 0;
        std::size_t encoded_pins = 0;
        std::size_t submitted_pins = 0;
    };

    struct UseTokenInfo {
        UseState state = UseState::Encoded;
        SubmissionSerial serial = 0;
        std::size_t resource_count = 0;
        std::size_t upload_count = 0;
    };

    struct LifetimeConfig {
        DeviceEpoch initial_device_epoch = 1;
        QueueId queue = 1;
        std::size_t max_resources = constants::render::default_max_resources;
        std::size_t max_prepared_frames = constants::render::default_max_prepared_frames;
        std::size_t max_encoded_tokens = constants::render::default_max_encoded_uses;
        // Bounds encoded plus submitted tokens: encoding reserves the submission slot.
        std::size_t max_submitted_tokens = constants::render::default_max_in_flight_uses;
    };

    struct LifetimeStats {
        std::size_t resources = 0;
        std::size_t prepared_frames = 0;
        std::size_t encoded_tokens = 0;
        std::size_t submitted_tokens = 0;
        DeviceEpoch device_epoch = 0;
        SubmissionSerial last_submitted_serial = 0;
        SubmissionSerial completed_serial = 0;
        bool device_available = false;
        bool shutdown_requested = false;
    };

    enum class DeviceAbandonConfirmation {
        NotConfirmed,
        // The host must first establish that this device can never execute or
        // access these resources again. Surface loss or pause is insufficient.
        HostConfirmedNoFurtherAccess,
    };

    // Renderer-owner-thread lifetime model. It records no native GPU objects and
    // executes no GPU work. An adapter must acquire EncodeFrame's pins BEFORE it
    // records resource use, then call NotifySubmitted only AFTER actual submission.
    // Submit/encode failure must cancel the commands and call DiscardEncoded.
    // A successful destructor is not evidence of GPU completion: callers must
    // obtain Ok from Shutdown before destroying the real runtime/backend.
    class ResourceLifetime final {
    public:
        explicit ResourceLifetime(LifetimeConfig config = {}) noexcept;
        ~ResourceLifetime();
        ResourceLifetime(const ResourceLifetime&) = delete;
        ResourceLifetime& operator=(const ResourceLifetime&) = delete;
        ResourceLifetime(ResourceLifetime&&) = delete;
        ResourceLifetime& operator=(ResourceLifetime&&) = delete;

        LifetimeResult CreateResource(ResourceDescription description, ResourceId& out) noexcept;
        LifetimeResult RetainResource(ResourceId resource) noexcept;
        LifetimeResult ReleaseResource(ResourceId resource) noexcept;
        // Backend create/upload failure does not release any outstanding use pins.
        // Failed versions stay failed; recovery creates a new immutable version.
        LifetimeResult MarkResourceFailed(ResourceId resource) noexcept;

        // Dependencies are the COMPLETE immutable resource set for this frame.
        // Inputs are copied and deduplicated; no prior unsubmitted frame is needed.
        LifetimeResult PrepareFrame(std::span<const ResourceId> resources, PreparedFrameId& out) noexcept;
        LifetimeResult ReleasePreparedFrame(PreparedFrameId frame) noexcept;
        LifetimeResult EncodeFrame(PreparedFrameId frame, UseTokenId& out) noexcept;
        LifetimeResult DiscardEncoded(UseTokenId token) noexcept;

        // One queue only. Several tokens may bind to one actual submission serial.
        // Encode reserves in-flight capacity before the host records/submits work.
        // Invalid notifications retain all pins; correct and retry the notification.
        LifetimeResult NotifySubmitted(UseTokenId token, QueueId queue, SubmissionSerial serial) noexcept;
        // The host supplies the actual completed queue timeline, including engine
        // submissions with no UI tokens. Completion may exceed our last UI serial.
        // Truthful host completion is required; this model cannot inspect a fence.
        LifetimeResult NotifyCompletedSerial(DeviceEpoch epoch, QueueId queue, SubmissionSerial serial) noexcept;
        LifetimeResult AbandonDeviceEpoch(DeviceEpoch epoch, DeviceAbandonConfirmation confirmation) noexcept;
        LifetimeResult BeginDeviceEpoch(DeviceEpoch epoch) noexcept;

        // Starts draining and prevents new resource/frame/token creation,
        // and returns Busy while any CPU lease, prepared frame or use remains.
        // Release, discard, submission acknowledgement, completion and confirmed
        // abandon remain legal. The host stops producers and cancels unsubmitted
        // work; already submitted work must still be acknowledged and completed.
        LifetimeResult Shutdown() noexcept;
        LifetimeResult InspectResource(ResourceId resource, ResourceInfo& out) const noexcept;
        LifetimeResult InspectUseToken(UseTokenId token, UseTokenInfo& out) const noexcept;
        [[nodiscard]] LifetimeStats Stats() const noexcept;

    private:
        struct ResourceSlot;
        struct PreparedSlot;
        struct TokenSlot;

        LifetimeResult CheckCall(bool producing = false) const noexcept;
        LifetimeResult CheckResource(ResourceId id, std::size_t& slot) const noexcept;
        LifetimeResult CheckPrepared(PreparedFrameId id, std::size_t& slot) const noexcept;
        LifetimeResult CheckToken(UseTokenId id, std::size_t& slot) const noexcept;
        void CollectResource(std::size_t slot) noexcept;
        void ReleaseToken(std::size_t slot, bool completed) noexcept;
        ResourcePhase Phase(const ResourceSlot& resource) const noexcept;

        LifetimeConfig config_;
        std::thread::id owner_;
        bool valid_config_ = false;
        LifetimeStats stats_;
        std::vector<ResourceSlot> resources_;
        std::vector<PreparedSlot> prepared_;
        std::vector<TokenSlot> tokens_;
    };

} // namespace caseelephant::render
