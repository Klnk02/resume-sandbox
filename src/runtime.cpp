#include "resumelab/runtime.hpp"
#include <stdexcept>

namespace resumelab {
namespace {
State target(Action action) {
    switch (action) {
    case Action::Create: case Action::Resume: return State::Running;
    case Action::Pause: return State::Paused;
    case Action::Destroy: return State::Deleted;
    }
    throw std::logic_error("invalid action");
}
bool allowed(State state, Action action) {
    switch (action) {
    case Action::Create: return state == State::Missing;
    case Action::Pause: return state == State::Running || state == State::Paused;
    case Action::Resume: return state == State::Paused || state == State::Running;
    case Action::Destroy: return state == State::Running || state == State::Paused || state == State::Deleted;
    }
    return false;
}
bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (const unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}
}
std::string name(State state) {
    switch (state) {
    case State::Missing: return "Missing";
    case State::Running: return "Running";
    case State::Paused: return "Paused";
    case State::Deleted: return "Deleted";
    case State::Unknown: return "Unknown";
    }
    return "Invalid";
}
std::string name(Status status) {
    switch (status) {
    case Status::Succeeded: return "Succeeded";
    case Status::Failed: return "Failed";
    case Status::Uncertain: return "Uncertain";
    case Status::Conflict: return "Conflict";
    }
    return "Invalid";
}
Sandbox Controller::get(const std::string& id) const {
    const auto it = sandboxes_.find(id);
    return it == sandboxes_.end() ? Sandbox{} : it->second;
}
Result Controller::submit(const Request& request) {
    const auto current = get(request.sandbox_id);
    if (!valid_id(request.request_id) || !valid_id(request.sandbox_id))
        return {Status::Conflict, current.observed, current.revision, "invalid identifier"};
    if (const auto it = operations_.find(request.request_id); it != operations_.end()) {
        if (it->second.request == request) return it->second.result;
        return {Status::Conflict, current.observed, current.revision, "request ID reused with different payload"};
    }
    if (current.pending || current.revision != request.expected_revision || !allowed(current.observed, request.action))
        return {Status::Conflict, current.observed, current.revision, "pending operation, stale revision, or invalid transition"};

    auto& record = sandboxes_[request.sandbox_id];
    const State desired = target(request.action);
    if (record.observed == desired) {
        Result result{Status::Succeeded, record.observed, record.revision, "already at target; no backend mutation"};
        operations_.emplace(request.request_id, Operation{request, result});
        return result;
    }
    const State before = record.observed;
    record.desired = desired;
    record.pending = request.request_id;
    ++record.revision;
    record.observed = State::Unknown;
    // Durable intent-before-dispatch is a future M2 gate. v0 survives NO restart.
    BackendResult response{Effect::Uncertain, "backend exception; outcome unknown"};
    try { response = backend_.apply(request); } catch (...) { /* never assume rollback */ }
    Status status = Status::Uncertain;
    if (response.effect == Effect::Applied) {
        record.observed = desired;
        record.pending.reset();
        status = Status::Succeeded;
    } else if (response.effect == Effect::Rejected) {
        record.observed = before;
        record.desired = before;
        record.pending.reset();
        status = Status::Failed;
    }
    Result result{status, record.observed, record.revision, response.detail};
    operations_.emplace(request.request_id, Operation{request, result});
    return result;
}
Result Controller::reconcile(const std::string& id) {
    const auto it = sandboxes_.find(id);
    if (it == sandboxes_.end()) return {Status::Conflict, State::Missing, 0, "unknown logical sandbox"};
    auto& record = it->second;
    if (!record.pending)
        return {Status::Succeeded, record.observed, record.revision, "no pending operation; no fresh health check"};
    State observed = State::Unknown;
    try { observed = backend_.inspect(id); } catch (...) { /* keep pending */ }
    auto& operation = operations_.at(*record.pending);
    // Target match is sufficient only under the v0 single-writer mock contract.
    // A live adapter must establish identity, generation and terminal completion.
    if (observed == record.desired) {
        record.observed = observed;
        record.pending.reset();
        ++record.revision;
        operation.result = {Status::Succeeded, observed, record.revision, "target observed; operation reconciled"};
    } else {
        operation.result = {Status::Uncertain, State::Unknown, record.revision, "target unconfirmed; do not replay mutation"};
    }
    return operation.result;
}
BackendResult MockBackend::apply(const Request& request) {
    ++calls_;
    const auto fault = next_fault_;
    next_fault_ = Fault::None;
    if (fault == Fault::RejectBeforeApply) return {Effect::Rejected, "injected rejection before mutation"};
    if (fault == Fault::UnknownBeforeApply) return {Effect::Uncertain, "injected unknown outcome before mutation"};
    states_[request.sandbox_id] = target(request.action);
    if (fault == Fault::LoseReplyAfterApply) return {Effect::Uncertain, "injected lost reply after mutation"};
    return {Effect::Applied, "mock target applied"};
}
State MockBackend::inspect(const std::string& id) {
    if (fail_inspection_) return State::Unknown;
    const auto it = states_.find(id);
    return it == states_.end() ? State::Missing : it->second;
}
} // namespace resumelab
