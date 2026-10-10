#include "caseelephant/resource_lifetime.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace weft::render {

    struct ResourceLifetime::ResourceSlot {
        DeviceEpoch epoch = 0;
        ResourceDescription description;
        std::size_t cpu_leases = 0;
        std::size_t prepared_pins = 0;
        std::size_t encoded_pins = 0;
        std::size_t submitted_pins = 0;
        std::size_t encoded_uploads = 0;
        std::size_t submitted_uploads = 0;
        bool ready = false;
        bool failed = false;
        bool abandoned = false;
    };

    struct ResourceLifetime::PreparedSlot {
        DeviceEpoch epoch = 0;
        std::vector<ResourceId> resources;
    };

    struct ResourceLifetime::TokenSlot {
        struct ResourceUse {
            ResourceId id;
            bool upload = false;
        };

        DeviceEpoch epoch = 0;
        UseState state = UseState::Encoded;
        SubmissionSerial serial = 0;
        std::vector<ResourceUse> resources;
    };

    namespace {

        bool ResourceLess(const ResourceId& a, const ResourceId& b) noexcept {
            if (a.slot != b.slot) {
                return a.slot < b.slot;
            }
            if (a.generation != b.generation) {
                return a.generation < b.generation;
            }
            return a.device_epoch < b.device_epoch;
        }

    } // namespace

    ResourceLifetime::ResourceLifetime(LifetimeConfig config) noexcept : config_(config) {
        constexpr auto max_slots = std::numeric_limits<std::uint32_t>::max();
        valid_config_ = config.initial_device_epoch != 0 && config.queue != 0 && config.max_resources != 0 &&
                        config.max_resources <= max_slots && config.max_prepared_frames != 0 &&
                        config.max_prepared_frames <= max_slots && config.max_encoded_tokens != 0 &&
                        config.max_encoded_tokens <= max_slots && config.max_submitted_tokens != 0 &&
                        config.max_submitted_tokens <= max_slots - config.max_encoded_tokens;
        stats_.device_epoch = config.initial_device_epoch;
        stats_.device_available = valid_config_;
    }

    ResourceLifetime::~ResourceLifetime() = default;

    LifetimeResult ResourceLifetime::CheckCall(bool producing) const noexcept {
        if (!owner_.is_current()) {
            return LifetimeResult::WrongThread;
        }
        if (!valid_config_) {
            return LifetimeResult::InvalidArgument;
        }
        if (producing && stats_.shutdown_requested) {
            return LifetimeResult::ShuttingDown;
        }
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::CheckResource(ResourceId id, std::size_t& slot) const noexcept {
        const auto* item = resources_.find(id.slot, id.generation);
        if (!item) {
            return LifetimeResult::StaleHandle;
        }
        if (id.device_epoch != item->epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        slot = id.slot;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::CheckPrepared(PreparedFrameId id, std::size_t& slot) const noexcept {
        const auto* item = prepared_.find(id.slot, id.generation);
        if (!item) {
            return LifetimeResult::StaleHandle;
        }
        if (id.device_epoch != item->epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        slot = id.slot;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::CheckToken(UseTokenId id, std::size_t& slot) const noexcept {
        const auto* item = tokens_.find(id.slot, id.generation);
        if (!item) {
            return LifetimeResult::StaleHandle;
        }
        if (id.device_epoch != item->epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        slot = id.slot;
        return LifetimeResult::Ok;
    }

    void ResourceLifetime::CollectResource(std::size_t slot) noexcept {
        auto& item = resources_[slot];
        if (resources_.occupied(slot) && item.cpu_leases == 0 && item.prepared_pins == 0 && item.encoded_pins == 0 &&
            item.submitted_pins == 0) {
            resources_.vacate(slot);
            --stats_.resources;
        }
    }

    ResourcePhase ResourceLifetime::Phase(const ResourceSlot& item) const noexcept {
        if (item.abandoned) {
            return ResourcePhase::Abandoned;
        }
        if (item.failed) {
            return ResourcePhase::Failed;
        }
        if (item.ready) {
            return ResourcePhase::Ready;
        }
        if (item.submitted_uploads != 0) {
            return ResourcePhase::Submitted;
        }
        if (item.encoded_uploads != 0) {
            return ResourcePhase::Encoded;
        }
        return ResourcePhase::Created;
    }

    LifetimeResult ResourceLifetime::CreateResource(ResourceDescription description, ResourceId& out) noexcept {
        out = {};
        if (const auto result = CheckCall(true); result != LifetimeResult::Ok) {
            return result;
        }
        if (!stats_.device_available) {
            return LifetimeResult::DeviceUnavailable;
        }
        if (description.asset_id == 0 || description.asset_version == 0) {
            return LifetimeResult::InvalidArgument;
        }
        switch (description.kind) {
            case ResourceKind::Texture:
            case ResourceKind::Buffer:
            case ResourceKind::AtlasPage:
            case ResourceKind::Staging:
                break;
            default:
                return LifetimeResult::InvalidArgument;
        }
        if (stats_.resources == config_.max_resources) {
            return LifetimeResult::Busy;
        }
        try {
            const auto slot = resources_.first_free();
            if (slot == std::numeric_limits<std::uint32_t>::max()) {
                return LifetimeResult::Busy;
            }
            if (slot == resources_.size()) {
                resources_.append();
            }
            auto& item = resources_[slot];
            item = ResourceSlot{};
            const auto generation = resources_.occupy(slot);
            item.epoch = stats_.device_epoch;
            item.description = description;
            item.cpu_leases = 1;
            ++stats_.resources;
            out = {static_cast<std::uint32_t>(slot), generation, item.epoch};
            return LifetimeResult::Ok;
        } catch (const std::bad_alloc&) {
            return LifetimeResult::OutOfMemory;
        } catch (...) {
            return LifetimeResult::InternalError;
        }
    }

    LifetimeResult ResourceLifetime::RetainResource(ResourceId resource) noexcept {
        if (const auto result = CheckCall(true); result != LifetimeResult::Ok) {
            return result;
        }
        std::size_t slot = 0;
        if (const auto result = CheckResource(resource, slot); result != LifetimeResult::Ok) {
            return result;
        }
        auto& item = resources_[slot];
        if (item.cpu_leases == std::numeric_limits<std::size_t>::max()) {
            return LifetimeResult::Busy;
        }
        ++item.cpu_leases;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::ReleaseResource(ResourceId resource) noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        std::size_t slot = 0;
        if (const auto result = CheckResource(resource, slot); result != LifetimeResult::Ok) {
            return result;
        }
        auto& item = resources_[slot];
        if (item.cpu_leases == 0) {
            return LifetimeResult::InvalidState;
        }
        --item.cpu_leases;
        CollectResource(slot);
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::MarkResourceFailed(ResourceId resource) noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        if (!stats_.device_available) {
            return LifetimeResult::DeviceUnavailable;
        }
        std::size_t slot = 0;
        if (const auto result = CheckResource(resource, slot); result != LifetimeResult::Ok) {
            return result;
        }
        if (resource.device_epoch != stats_.device_epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        resources_[slot].failed = true;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::PrepareFrame(std::span<const ResourceId> resources,
                                                  PreparedFrameId& out) noexcept {
        out = {};
        if (const auto result = CheckCall(true); result != LifetimeResult::Ok) {
            return result;
        }
        if (!stats_.device_available) {
            return LifetimeResult::DeviceUnavailable;
        }
        if (stats_.prepared_frames == config_.max_prepared_frames) {
            return LifetimeResult::Busy;
        }
        try {
            // Validate the entire input BEFORE changing ownership. In particular,
            // a stale dependency must not partly pin the other dependencies.
            for (const auto id : resources) {
                std::size_t slot = 0;
                if (const auto result = CheckResource(id, slot); result != LifetimeResult::Ok) {
                    return result;
                }
                if (id.device_epoch != stats_.device_epoch) {
                    return LifetimeResult::StaleDeviceEpoch;
                }
                if (resources_[slot].failed) {
                    return LifetimeResult::ResourceFailed;
                }
            }
            std::vector<ResourceId> dependencies(resources.begin(), resources.end());
            std::sort(dependencies.begin(), dependencies.end(), ResourceLess);
            dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
            const auto slot = prepared_.first_free();
            if (slot == std::numeric_limits<std::uint32_t>::max()) {
                return LifetimeResult::Busy;
            }
            if (slot == prepared_.size()) {
                prepared_.append();
            }
            auto& frame = prepared_[slot];
            const auto generation = prepared_.occupy(slot);
            frame.epoch = stats_.device_epoch;
            frame.resources = std::move(dependencies);
            for (const auto id : frame.resources) {
                ++resources_[id.slot].prepared_pins;
            }
            ++stats_.prepared_frames;
            out = {static_cast<std::uint32_t>(slot), generation, frame.epoch};
            return LifetimeResult::Ok;
        } catch (const std::bad_alloc&) {
            return LifetimeResult::OutOfMemory;
        } catch (...) {
            return LifetimeResult::InternalError;
        }
    }

    LifetimeResult ResourceLifetime::ReleasePreparedFrame(PreparedFrameId frame) noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        std::size_t slot = 0;
        if (const auto result = CheckPrepared(frame, slot); result != LifetimeResult::Ok) {
            return result;
        }
        auto& item = prepared_[slot];
        for (const auto id : item.resources) {
            --resources_[id.slot].prepared_pins;
            CollectResource(id.slot);
        }
        item.resources.clear();
        prepared_.vacate(slot);
        --stats_.prepared_frames;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::EncodeFrame(PreparedFrameId frame, UseTokenId& out) noexcept {
        out = {};
        if (const auto result = CheckCall(true); result != LifetimeResult::Ok) {
            return result;
        }
        if (!stats_.device_available) {
            return LifetimeResult::DeviceUnavailable;
        }
        std::size_t frame_slot = 0;
        if (const auto result = CheckPrepared(frame, frame_slot); result != LifetimeResult::Ok) {
            return result;
        }
        if (frame.device_epoch != stats_.device_epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        for (const auto id : prepared_[frame_slot].resources) {
            if (resources_[id.slot].failed) {
                return LifetimeResult::ResourceFailed;
            }
        }
        if (stats_.encoded_tokens == config_.max_encoded_tokens) {
            return LifetimeResult::Busy;
        }
        // Reserve a submission slot BEFORE real GPU encoding. A successful submit
        // notification cannot subsequently fail because a bounded queue is full.
        if (stats_.encoded_tokens + stats_.submitted_tokens >= config_.max_submitted_tokens) {
            return LifetimeResult::Busy;
        }
        try {
            std::vector<TokenSlot::ResourceUse> dependencies;
            dependencies.reserve(prepared_[frame_slot].resources.size());
            for (const auto id : prepared_[frame_slot].resources) {
                const auto& item = resources_[id.slot];
                // An upload in another unsubmitted token may be cancelled. Each
                // independent encoded frame therefore includes its own upload.
                // Once submitted, same-queue ordering makes that upload reusable.
                dependencies.push_back({id, !item.ready && item.submitted_uploads == 0});
            }
            const auto slot = tokens_.first_free();
            if (slot == std::numeric_limits<std::uint32_t>::max()) {
                return LifetimeResult::Busy;
            }
            if (slot == tokens_.size()) {
                tokens_.append();
            }
            auto& token = tokens_[slot];
            const auto generation = tokens_.occupy(slot);
            token.epoch = stats_.device_epoch;
            token.state = UseState::Encoded;
            token.serial = 0;
            token.resources = std::move(dependencies);
            for (const auto& use : token.resources) {
                auto& item = resources_[use.id.slot];
                ++item.encoded_pins;
                if (use.upload) {
                    ++item.encoded_uploads;
                }
            }
            ++stats_.encoded_tokens;
            out = {static_cast<std::uint32_t>(slot), generation, token.epoch};
            return LifetimeResult::Ok;
        } catch (const std::bad_alloc&) {
            return LifetimeResult::OutOfMemory;
        } catch (...) {
            return LifetimeResult::InternalError;
        }
    }

    void ResourceLifetime::ReleaseToken(std::size_t slot, bool completed) noexcept {
        auto& token = tokens_[slot];
        for (const auto& use : token.resources) {
            auto& item = resources_[use.id.slot];
            if (token.state == UseState::Encoded) {
                --item.encoded_pins;
                if (use.upload) {
                    --item.encoded_uploads;
                }
            } else {
                --item.submitted_pins;
                if (use.upload) {
                    --item.submitted_uploads;
                    if (completed) {
                        item.ready = true;
                    }
                }
            }
            CollectResource(use.id.slot);
        }
        if (token.state == UseState::Encoded) {
            --stats_.encoded_tokens;
        } else {
            --stats_.submitted_tokens;
        }
        token.resources.clear();
        tokens_.vacate(slot);
    }

    LifetimeResult ResourceLifetime::DiscardEncoded(UseTokenId token) noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        std::size_t slot = 0;
        if (const auto result = CheckToken(token, slot); result != LifetimeResult::Ok) {
            return result;
        }
        if (tokens_[slot].state != UseState::Encoded) {
            return LifetimeResult::InvalidState;
        }
        ReleaseToken(slot, false);
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::NotifySubmitted(UseTokenId token, QueueId queue,
                                                     SubmissionSerial serial) noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        if (!stats_.device_available) {
            return LifetimeResult::DeviceUnavailable;
        }
        if (queue != config_.queue) {
            return LifetimeResult::InvalidQueue;
        }
        std::size_t slot = 0;
        if (const auto result = CheckToken(token, slot); result != LifetimeResult::Ok) {
            return result;
        }
        if (token.device_epoch != stats_.device_epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        auto& item = tokens_[slot];
        if (item.state != UseState::Encoded) {
            return LifetimeResult::InvalidState;
        }
        if (serial == 0 || serial < stats_.last_submitted_serial || serial <= stats_.completed_serial) {
            return LifetimeResult::InvalidSerial;
        }
        for (const auto& use : item.resources) {
            auto& resource = resources_[use.id.slot];
            --resource.encoded_pins;
            ++resource.submitted_pins;
            if (use.upload) {
                --resource.encoded_uploads;
                ++resource.submitted_uploads;
            }
        }
        item.state = UseState::Submitted;
        item.serial = serial;
        --stats_.encoded_tokens;
        ++stats_.submitted_tokens;
        stats_.last_submitted_serial = serial;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::NotifyCompletedSerial(DeviceEpoch epoch, QueueId queue,
                                                           SubmissionSerial serial) noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        if (epoch != stats_.device_epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        if (!stats_.device_available) {
            return LifetimeResult::DeviceUnavailable;
        }
        if (queue != config_.queue) {
            return LifetimeResult::InvalidQueue;
        }
        // The queue belongs to the host. Its completed timeline can advance past
        // the last submission containing UI work; do not invent a separate fence.
        if (serial == 0 || serial < stats_.completed_serial) {
            return LifetimeResult::InvalidSerial;
        }
        for (std::size_t i = 0; i < tokens_.size(); ++i) {
            if (tokens_.occupied(i) && tokens_[i].state == UseState::Submitted && tokens_[i].serial <= serial) {
                ReleaseToken(i, true);
            }
        }
        stats_.completed_serial = serial;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::AbandonDeviceEpoch(DeviceEpoch epoch,
                                                        DeviceAbandonConfirmation confirmation) noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        if (epoch != stats_.device_epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        if (!stats_.device_available) {
            return LifetimeResult::DeviceUnavailable;
        }
        if (confirmation != DeviceAbandonConfirmation::HostConfirmedNoFurtherAccess) {
            return LifetimeResult::HostConfirmationRequired;
        }
        for (std::size_t i = 0; i < resources_.size(); ++i) {
            if (resources_.occupied(i) && resources_[i].epoch == epoch) {
                resources_[i].abandoned = true;
            }
        }
        for (std::size_t i = 0; i < tokens_.size(); ++i) {
            if (tokens_.occupied(i) && tokens_[i].epoch == epoch) {
                ReleaseToken(i, false);
            }
        }
        stats_.device_available = false;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::BeginDeviceEpoch(DeviceEpoch epoch) noexcept {
        if (const auto result = CheckCall(true); result != LifetimeResult::Ok) {
            return result;
        }
        if (epoch == 0 || epoch <= stats_.device_epoch) {
            return LifetimeResult::StaleDeviceEpoch;
        }
        if (stats_.device_available) {
            return LifetimeResult::InvalidState;
        }
        stats_.device_epoch = epoch;
        stats_.last_submitted_serial = 0;
        stats_.completed_serial = 0;
        stats_.device_available = true;
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::Shutdown() noexcept {
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        stats_.shutdown_requested = true;
        if (stats_.resources != 0 || stats_.prepared_frames != 0 || stats_.encoded_tokens != 0 ||
            stats_.submitted_tokens != 0) {
            return LifetimeResult::Busy;
        }
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::InspectResource(ResourceId resource, ResourceInfo& out) const noexcept {
        out = {};
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        std::size_t slot = 0;
        if (const auto result = CheckResource(resource, slot); result != LifetimeResult::Ok) {
            return result;
        }
        const auto& item = resources_[slot];
        out = {item.description,   Phase(item),       item.cpu_leases,
               item.prepared_pins, item.encoded_pins, item.submitted_pins};
        return LifetimeResult::Ok;
    }

    LifetimeResult ResourceLifetime::InspectUseToken(UseTokenId token, UseTokenInfo& out) const noexcept {
        out = {};
        if (const auto result = CheckCall(); result != LifetimeResult::Ok) {
            return result;
        }
        std::size_t slot = 0;
        if (const auto result = CheckToken(token, slot); result != LifetimeResult::Ok) {
            return result;
        }
        const auto& item = tokens_[slot];
        std::size_t uploads = 0;
        for (const auto& use : item.resources) {
            uploads += use.upload ? 1 : 0;
        }
        out = {item.state, item.serial, item.resources.size(), uploads};
        return LifetimeResult::Ok;
    }

    LifetimeStats ResourceLifetime::Stats() const noexcept {
        return stats_;
    }

} // namespace weft::render
