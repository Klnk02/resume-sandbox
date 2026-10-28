#pragma once
#include <cstddef>
#include <cstdint>
#include <set>

namespace resumelab {
// Model of the proposed protocol. No actual DMA, buffer allocation, or VM access.
enum class EpochPhase { Preparing, Canceled, Failed, Committed };
struct WakeGuard {
    bool live_vm_owned;
    bool rootfs_reversible;
    bool producer_stopped;
    bool independent_staging; // All device source/dest buffers independent of guest.
    bool no_guest_reads_inflight;
};
class SnapshotEpoch {
public:
    SnapshotEpoch(std::uint64_t id, std::size_t chunks) : id_(id), chunks_(chunks) {}
    bool submit(std::size_t chunk) {
        if (phase_ != EpochPhase::Preparing || chunk >= chunks_ || submitted_.contains(chunk)) return false;
        submitted_.insert(chunk); outstanding_.insert(chunk); return true;
    }
    bool complete(std::uint64_t epoch, std::size_t chunk, bool success) {
        if (epoch != id_ || outstanding_.erase(chunk) == 0) return false;
        // Canceled/failed epochs still drain ownership; they never publish data.
        if (phase_ == EpochPhase::Preparing) {
            if (success) completed_.insert(chunk);
            else phase_ = EpochPhase::Failed;
        }
        return true;
    }
    bool cancel(const WakeGuard& guard) {
        if (phase_ != EpochPhase::Preparing || !guard.live_vm_owned || !guard.rootfs_reversible ||
            !guard.producer_stopped || !guard.independent_staging || !guard.no_guest_reads_inflight) return false;
        phase_ = EpochPhase::Canceled;
        return true; // Permits requesting VM resume, not proof it succeeded.
    }
    bool commit() {
        if (phase_ != EpochPhase::Preparing || chunks_ == 0 || completed_.size() != chunks_ || !outstanding_.empty()) return false;
        phase_ = EpochPhase::Committed;
        return true; // Models publication decision; contains no fsync or durable manifest.
    }
    bool can_release_buffers() const { return outstanding_.empty(); }
    EpochPhase phase() const { return phase_; }
    std::size_t outstanding() const { return outstanding_.size(); }
private:
    std::uint64_t id_;
    std::size_t chunks_;
    EpochPhase phase_ = EpochPhase::Preparing;
    std::set<std::size_t> submitted_, outstanding_, completed_;
};
} // namespace resumelab
