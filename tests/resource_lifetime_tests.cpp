#include "caseelephant/resource_lifetime.hpp"
#include "support/check.hpp"
#include "test_constants/render.hpp"

#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

using namespace caseelephant::render;

namespace {

#define CHECK(expression) CE_REQUIRE(expression)
#define EXPECT(expression, expected) CHECK((expression) == LifetimeResult::expected)

    ResourceId Resource(ResourceLifetime& lifetime, std::uint64_t asset = 1) {
        ResourceId id;
        EXPECT(lifetime.CreateResource({asset, 1, 4096, ResourceKind::Texture}, id), Ok);
        return id;
    }

    PreparedFrameId Prepare(ResourceLifetime& lifetime, ResourceId resource) {
        PreparedFrameId frame;
        const std::array resources{resource};
        EXPECT(lifetime.PrepareFrame(resources, frame), Ok);
        return frame;
    }

    UseTokenId Encode(ResourceLifetime& lifetime, PreparedFrameId frame) {
        UseTokenId token;
        EXPECT(lifetime.EncodeFrame(frame, token), Ok);
        return token;
    }

    ResourceInfo Info(ResourceLifetime& lifetime, ResourceId resource) {
        ResourceInfo info;
        EXPECT(lifetime.InspectResource(resource, info), Ok);
        return info;
    }

    UseTokenInfo TokenInfo(ResourceLifetime& lifetime, UseTokenId token) {
        UseTokenInfo info;
        EXPECT(lifetime.InspectUseToken(token, info), Ok);
        return info;
    }

    void EncodedAndSubmittedPinsSurviveCpuRelease() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto token = Encode(lifetime, frame);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        auto info = Info(lifetime, resource);
        CHECK(info.cpu_leases == 0 && info.prepared_pins == 0 && info.encoded_pins == 1);
        CHECK(info.phase == ResourcePhase::Encoded);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 4), Ok); // unrelated host queue work
        CHECK(Info(lifetime, resource).encoded_pins == 1);
        EXPECT(lifetime.NotifySubmitted(token, 1, 5), Ok);
        info = Info(lifetime, resource);
        CHECK(info.submitted_pins == 1 && info.phase == ResourcePhase::Submitted);
        EXPECT(lifetime.DiscardEncoded(token), InvalidState);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 4), Ok);
        CHECK(Info(lifetime, resource).submitted_pins == 1);
        EXPECT(lifetime.Shutdown(), Busy);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 5), Ok);
        EXPECT(lifetime.InspectResource(resource, info), StaleHandle);
        CHECK(lifetime.Stats().resources == 0 && lifetime.Stats().submitted_tokens == 0);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void CancelledRecordingCannotMakeUploadReady() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto token = Encode(lifetime, frame);
        CHECK(Info(lifetime, resource).phase == ResourcePhase::Encoded);
        EXPECT(lifetime.DiscardEncoded(token), Ok);
        const auto info = Info(lifetime, resource);
        CHECK(info.phase == ResourcePhase::Created && info.encoded_pins == 0);
        EXPECT(lifetime.DiscardEncoded(token), StaleHandle);
        const auto replacement = Encode(lifetime, frame);
        CHECK(TokenInfo(lifetime, replacement).upload_count == 1);
        // An actual backend submission failure uses this same cancellation path.
        EXPECT(lifetime.DiscardEncoded(replacement), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void SeveralTokensCanShareOneSubmission() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto first = Encode(lifetime, frame);
        const auto second = Encode(lifetime, frame);
        CHECK(TokenInfo(lifetime, first).upload_count == 1);
        CHECK(TokenInfo(lifetime, second).upload_count == 1);
        UseTokenId encoded_overflow;
        EXPECT(lifetime.EncodeFrame(frame, encoded_overflow), Busy); // independent unsubmitted bound
        EXPECT(lifetime.NotifySubmitted(first, 1, 7), Ok);
        EXPECT(lifetime.NotifySubmitted(second, 1, 7), Ok);
        CHECK(Info(lifetime, resource).submitted_pins == 2);
        const auto third = Encode(lifetime, frame);
        CHECK(TokenInfo(lifetime, third).upload_count == 0);
        EXPECT(lifetime.DiscardEncoded(third), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 7), Ok);
        CHECK(lifetime.Stats().submitted_tokens == 0);
        CHECK(Info(lifetime, resource).phase == ResourcePhase::Ready);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 7), Ok); // idempotent
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void DroppedFrameDoesNotBreakLaterFrame() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto first = Encode(lifetime, frame);
        const auto second = Encode(lifetime, frame);
        EXPECT(lifetime.DiscardEncoded(first), Ok);
        CHECK(TokenInfo(lifetime, second).upload_count == 1);
        EXPECT(lifetime.NotifySubmitted(second, 1, 3), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 3), Ok);
        CHECK(Info(lifetime, resource).phase == ResourcePhase::Ready);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void ResourceFailurePreservesOutstandingUses() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto submitted = Encode(lifetime, frame);
        const auto encoded = Encode(lifetime, frame);
        EXPECT(lifetime.NotifySubmitted(submitted, 1, 1), Ok);
        EXPECT(lifetime.MarkResourceFailed(resource), Ok);
        CHECK(Info(lifetime, resource).phase == ResourcePhase::Failed);
        CHECK(Info(lifetime, resource).submitted_pins == 1 && Info(lifetime, resource).encoded_pins == 1);
        PreparedFrameId rejected;
        const std::array dependencies{resource};
        EXPECT(lifetime.PrepareFrame(dependencies, rejected), ResourceFailed);
        UseTokenId rejected_token;
        EXPECT(lifetime.EncodeFrame(frame, rejected_token), ResourceFailed);
        EXPECT(lifetime.DiscardEncoded(encoded), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 1), Ok);
        CHECK(Info(lifetime, resource).phase == ResourcePhase::Failed);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void InvalidNotificationsKeepOwnershipAndCanBeCorrected() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto first = Encode(lifetime, frame);
        const auto second = Encode(lifetime, frame);
        EXPECT(lifetime.NotifySubmitted(first, 2, 5), InvalidQueue);
        EXPECT(lifetime.NotifySubmitted(first, 1, 0), InvalidSerial);
        EXPECT(lifetime.NotifySubmitted(first, 1, 5), Ok);
        EXPECT(lifetime.NotifySubmitted(first, 1, 5), InvalidState);
        EXPECT(lifetime.NotifySubmitted(second, 1, 4), InvalidSerial);
        CHECK(TokenInfo(lifetime, second).state == UseState::Encoded);
        auto info = Info(lifetime, resource);
        CHECK(info.encoded_pins == 1 && info.submitted_pins == 1);
        EXPECT(lifetime.NotifySubmitted(second, 1, 5), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(2, 1, 5), StaleDeviceEpoch);
        EXPECT(lifetime.NotifyCompletedSerial(1, 2, 5), InvalidQueue);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 0), InvalidSerial);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 4), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 3), InvalidSerial);
        CHECK(Info(lifetime, resource).submitted_pins == 2);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 5), Ok);
        const auto third = Encode(lifetime, frame);
        EXPECT(lifetime.NotifySubmitted(third, 1, 5), InvalidSerial);
        CHECK(Info(lifetime, resource).encoded_pins == 1);
        EXPECT(lifetime.NotifySubmitted(third, 1, 8), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 9), Ok); // host-only submission after UI
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void CapacityIsReservedBeforeActualSubmission() {
        LifetimeConfig config;
        config.max_resources = 2;
        config.max_prepared_frames = 2;
        config.max_encoded_tokens = 2;
        config.max_submitted_tokens = 2;
        ResourceLifetime lifetime(config);
        const auto resource = Resource(lifetime);
        const auto temporary = Resource(lifetime, 2);
        ResourceId excess;
        EXPECT(lifetime.CreateResource({3, 1, 4, ResourceKind::Buffer}, excess), Busy);
        EXPECT(lifetime.ReleaseResource(temporary), Ok);
        const auto replacement = Resource(lifetime, 3);
        CHECK(temporary.slot == replacement.slot && temporary.generation != replacement.generation);
        const auto frame = Prepare(lifetime, resource);
        const auto extra_frame = Prepare(lifetime, replacement);
        PreparedFrameId blocked_frame;
        EXPECT(lifetime.PrepareFrame({}, blocked_frame), Busy);
        const auto first = Encode(lifetime, frame);
        const auto second = Encode(lifetime, frame);
        UseTokenId blocked;
        EXPECT(lifetime.EncodeFrame(frame, blocked), Busy);
        EXPECT(lifetime.NotifySubmitted(first, 1, 1), Ok);
        EXPECT(lifetime.EncodeFrame(frame, blocked), Busy); // one encoded + one submitted
        EXPECT(lifetime.NotifySubmitted(second, 1, 2), Ok); // cannot fail on capacity
        EXPECT(lifetime.EncodeFrame(frame, blocked), Busy);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 1), Ok);
        const auto third = Encode(lifetime, frame);
        EXPECT(lifetime.NotifySubmitted(third, 1, 3), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 3), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(extra_frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.ReleaseResource(replacement), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void SharedEngineQueueUsesTheActualHostTimeline() {
        ResourceLifetime lifetime;
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 17), Ok); // no prior UI submission
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto first = Encode(lifetime, frame);
        EXPECT(lifetime.NotifySubmitted(first, 1, 16), InvalidSerial);
        CHECK(TokenInfo(lifetime, first).state == UseState::Encoded);
        EXPECT(lifetime.NotifySubmitted(first, 1, 18), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 20), Ok);
        CHECK(Info(lifetime, resource).phase == ResourcePhase::Ready);
        CHECK(lifetime.Stats().last_submitted_serial == 18 && lifetime.Stats().completed_serial == 20);
        const auto second = Encode(lifetime, frame);
        EXPECT(lifetime.NotifySubmitted(second, 1, 19), InvalidSerial);
        EXPECT(lifetime.NotifySubmitted(second, 1, 21), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 23), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void StaleIdentityCannotAliasReusedSlot() {
        ResourceLifetime lifetime;
        const auto old_resource = Resource(lifetime);
        EXPECT(lifetime.ReleaseResource(old_resource), Ok);
        const auto resource = Resource(lifetime, 2);
        CHECK(old_resource.slot == resource.slot && old_resource.generation != resource.generation);
        ResourceInfo info;
        EXPECT(lifetime.InspectResource(old_resource, info), StaleHandle);
        auto forged = resource;
        ++forged.device_epoch;
        EXPECT(lifetime.ReleaseResource(forged), StaleDeviceEpoch);
        PreparedFrameId invalid_frame;
        const std::array invalid_dependencies{resource, old_resource};
        EXPECT(lifetime.PrepareFrame(invalid_dependencies, invalid_frame), StaleHandle);
        CHECK(Info(lifetime, resource).prepared_pins == 0); // validation is transactional
        const auto old_frame = Prepare(lifetime, resource);
        EXPECT(lifetime.ReleasePreparedFrame(old_frame), Ok);
        const auto frame = Prepare(lifetime, resource);
        CHECK(old_frame.slot == frame.slot && old_frame.generation != frame.generation);
        UseTokenId invalid_token;
        EXPECT(lifetime.EncodeFrame(old_frame, invalid_token), StaleHandle);
        const auto old_token = Encode(lifetime, frame);
        EXPECT(lifetime.DiscardEncoded(old_token), Ok);
        const auto token = Encode(lifetime, frame);
        CHECK(old_token.slot == token.slot && old_token.generation != token.generation);
        EXPECT(lifetime.NotifySubmitted(old_token, 1, 1), StaleHandle);
        auto forged_token = token;
        ++forged_token.device_epoch;
        EXPECT(lifetime.DiscardEncoded(forged_token), StaleDeviceEpoch);
        EXPECT(lifetime.DiscardEncoded(token), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void AbandonRequiresExplicitHostConfirmation() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        const auto frame = Prepare(lifetime, resource);
        const auto submitted = Encode(lifetime, frame);
        const auto encoded = Encode(lifetime, frame);
        EXPECT(lifetime.NotifySubmitted(submitted, 1, 9), Ok);
        EXPECT(lifetime.BeginDeviceEpoch(2), InvalidState);
        EXPECT(lifetime.AbandonDeviceEpoch(1, DeviceAbandonConfirmation::NotConfirmed), HostConfirmationRequired);
        CHECK(Info(lifetime, resource).submitted_pins == 1);
        CHECK(Info(lifetime, resource).encoded_pins == 1);
        EXPECT(lifetime.AbandonDeviceEpoch(2, DeviceAbandonConfirmation::HostConfirmedNoFurtherAccess),
               StaleDeviceEpoch);
        EXPECT(lifetime.AbandonDeviceEpoch(1, DeviceAbandonConfirmation::HostConfirmedNoFurtherAccess), Ok);
        CHECK(Info(lifetime, resource).phase == ResourcePhase::Abandoned);
        CHECK(Info(lifetime, resource).submitted_pins == 0 && Info(lifetime, resource).encoded_pins == 0);
        UseTokenInfo token_info;
        EXPECT(lifetime.InspectUseToken(submitted, token_info), StaleHandle);
        EXPECT(lifetime.InspectUseToken(encoded, token_info), StaleHandle);
        ResourceId unavailable;
        EXPECT(lifetime.CreateResource({2, 1, 1, ResourceKind::Buffer}, unavailable), DeviceUnavailable);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 9), DeviceUnavailable);
        EXPECT(lifetime.BeginDeviceEpoch(1), StaleDeviceEpoch);
        EXPECT(lifetime.BeginDeviceEpoch(2), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 9), StaleDeviceEpoch);
        UseTokenId stale;
        EXPECT(lifetime.EncodeFrame(frame, stale), StaleDeviceEpoch);
        PreparedFrameId stale_prepared;
        const std::array old_dependencies{resource};
        EXPECT(lifetime.PrepareFrame(old_dependencies, stale_prepared), StaleDeviceEpoch);
        // Old CPU/prepared leases remain releasable while a new device is active.
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        const auto recreated = Resource(lifetime);
        CHECK(recreated.device_epoch == 2);
        CHECK(Info(lifetime, recreated).phase == ResourcePhase::Created);
        const auto new_frame = Prepare(lifetime, recreated);
        const auto new_token = Encode(lifetime, new_frame);
        CHECK(TokenInfo(lifetime, new_token).upload_count == 1);
        EXPECT(lifetime.NotifySubmitted(new_token, 1, 1), Ok); // serial resets per epoch
        EXPECT(lifetime.NotifyCompletedSerial(2, 1, 1), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(new_frame), Ok);
        EXPECT(lifetime.ReleaseResource(recreated), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void ShutdownDrainsWithoutLosingLateSubmissionAcknowledgement() {
        ResourceLifetime lifetime;
        const auto resource = Resource(lifetime);
        EXPECT(lifetime.RetainResource(resource), Ok);
        CHECK(Info(lifetime, resource).cpu_leases == 2);
        const auto frame = Prepare(lifetime, resource);
        const auto token = Encode(lifetime, frame);
        EXPECT(lifetime.Shutdown(), Busy);
        ResourceId blocked_resource;
        PreparedFrameId blocked_frame;
        UseTokenId blocked_token;
        EXPECT(lifetime.CreateResource({2, 1, 1, ResourceKind::Texture}, blocked_resource), ShuttingDown);
        EXPECT(lifetime.RetainResource(resource), ShuttingDown);
        EXPECT(lifetime.PrepareFrame({}, blocked_frame), ShuttingDown);
        EXPECT(lifetime.EncodeFrame(frame, blocked_token), ShuttingDown);
        EXPECT(lifetime.NotifySubmitted(token, 1, 2), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.ReleaseResource(resource), Ok);
        EXPECT(lifetime.ReleaseResource(resource), InvalidState);
        EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
        EXPECT(lifetime.Shutdown(), Busy);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, 2), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
    }

    void OwnerThreadAndConfigurationAreValidated() {
        ResourceLifetime lifetime;
        LifetimeResult result = LifetimeResult::InternalError;
        std::thread worker([&] {
            ResourceId id;
            result = lifetime.CreateResource({1, 1, 1, ResourceKind::Buffer}, id);
        });
        worker.join();
        CHECK(result == LifetimeResult::WrongThread);
        CHECK(lifetime.Stats().resources == 0);
        EXPECT(lifetime.Shutdown(), Ok);
        LifetimeConfig config;
        config.initial_device_epoch = 0;
        ResourceLifetime invalid(config);
        ResourceId id;
        EXPECT(invalid.CreateResource({1, 1, 1, ResourceKind::Buffer}, id), InvalidArgument);
        EXPECT(invalid.Shutdown(), InvalidArgument);
    }

    void LifetimeStress() {
        std::mt19937 random(caseelephant::test_constants::render::lifetime_stress_seed);
        ResourceLifetime lifetime;
        SubmissionSerial serial = 0;
        for (std::uint64_t cycle = 0; cycle < caseelephant::test_constants::render::lifetime_stress_cycles; ++cycle) {
            const auto resource = Resource(lifetime, cycle + 1);
            const auto immutable_description = Info(lifetime, resource).description;
            const std::array dependencies{resource, resource, resource};
            PreparedFrameId frame;
            EXPECT(lifetime.PrepareFrame(dependencies, frame), Ok);
            const bool early_cpu_release = (random() & 1) != 0;
            if (early_cpu_release) {
                EXPECT(lifetime.ReleaseResource(resource), Ok);
            }
            const auto token = Encode(lifetime, frame);
            CHECK(TokenInfo(lifetime, token).resource_count == 1); // duplicates pin once
            CHECK(Info(lifetime, resource).description == immutable_description);
            EXPECT(lifetime.ReleasePreparedFrame(frame), Ok);
            if (!early_cpu_release) {
                EXPECT(lifetime.ReleaseResource(resource), Ok);
            }
            const bool cancel = (random() % 3) == 0;
            if (cancel) {
                EXPECT(lifetime.DiscardEncoded(token), Ok);
            } else {
                EXPECT(lifetime.NotifySubmitted(token, 1, ++serial), Ok);
                CHECK(Info(lifetime, resource).submitted_pins == 1);
                EXPECT(lifetime.NotifyCompletedSerial(1, 1, serial), Ok);
            }
            const auto stats = lifetime.Stats();
            CHECK(stats.resources == 0 && stats.prepared_frames == 0);
            CHECK(stats.encoded_tokens == 0 && stats.submitted_tokens == 0);
        }
        // Frames containing only procedural geometry still have bounded tokens.
        PreparedFrameId empty;
        EXPECT(lifetime.PrepareFrame({}, empty), Ok);
        const auto empty_token = Encode(lifetime, empty);
        CHECK(TokenInfo(lifetime, empty_token).resource_count == 0);
        EXPECT(lifetime.NotifySubmitted(empty_token, 1, ++serial), Ok);
        EXPECT(lifetime.ReleasePreparedFrame(empty), Ok);
        EXPECT(lifetime.NotifyCompletedSerial(1, 1, serial), Ok);
        EXPECT(lifetime.Shutdown(), Ok);
        std::cout << "lifetime stress: 2000 cycles, seed " << caseelephant::test_constants::render::lifetime_stress_seed
                  << '\n';
    }

} // namespace

int main() {
    try {
        EncodedAndSubmittedPinsSurviveCpuRelease();
        CancelledRecordingCannotMakeUploadReady();
        SeveralTokensCanShareOneSubmission();
        DroppedFrameDoesNotBreakLaterFrame();
        ResourceFailurePreservesOutstandingUses();
        InvalidNotificationsKeepOwnershipAndCanBeCorrected();
        CapacityIsReservedBeforeActualSubmission();
        SharedEngineQueueUsesTheActualHostTimeline();
        StaleIdentityCannotAliasReusedSlot();
        AbandonRequiresExplicitHostConfirmation();
        ShutdownDrainsWithoutLosingLateSubmissionAcknowledgement();
        OwnerThreadAndConfigurationAreValidated();
        LifetimeStress();
        std::cout << "resource lifetime: all 13 test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "resource lifetime test failure: " << error.what() << '\n';
        return 1;
    }
}
