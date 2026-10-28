#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace resumelab {
// This is a control-plane contract model, NOT a VM or security boundary.
enum class State { Missing, Running, Paused, Deleted, Unknown };
enum class Action { Create, Pause, Resume, Destroy };
enum class Effect { Applied, Rejected, Uncertain };
enum class Status { Succeeded, Failed, Uncertain, Conflict };
std::string name(State);
std::string name(Status);

struct Request {
    std::string request_id;
    std::string sandbox_id; // Logical project ID; real backend mapping is M1 work.
    Action action;
    std::uint64_t expected_revision;
    bool operator==(const Request&) const = default;
};
struct BackendResult { Effect effect; std::string detail; };
struct Result { Status status; State observed; std::uint64_t revision; std::string detail; };
struct Sandbox {
    State observed = State::Missing;
    State desired = State::Missing;
    std::uint64_t revision = 0;
    std::optional<std::string> pending;
};

class Backend {
public:
    virtual ~Backend() = default;
    // Applied must mean completed + observed at target, NOT merely HTTP 202.
    // Rejected must guarantee no mutation; transport errors map to Uncertain.
    virtual BackendResult apply(const Request&) = 0;
    // Unknown covers inaccessible backend and intermediate states.
    virtual State inspect(const std::string& sandbox_id) = 0;
};

class Controller {
public:
    explicit Controller(Backend& backend) : backend_(backend) {}
    Result submit(const Request&);
    Result reconcile(const std::string& sandbox_id);
    Sandbox get(const std::string& sandbox_id) const;
private:
    struct Operation { Request request; Result result; };
    Backend& backend_;
    std::map<std::string, Sandbox> sandboxes_;
    std::map<std::string, Operation> operations_;
    // Single thread, single writer, process-local ledger only in v0.
};

enum class Fault { None, RejectBeforeApply, LoseReplyAfterApply, UnknownBeforeApply };
class MockBackend final : public Backend {
public:
    BackendResult apply(const Request&) override;
    State inspect(const std::string&) override;
    void inject(Fault fault) { next_fault_ = fault; }
    void fail_inspection(bool value) { fail_inspection_ = value; }
    std::size_t calls() const { return calls_; }
private:
    std::map<std::string, State> states_;
    Fault next_fault_ = Fault::None;
    bool fail_inspection_ = false;
    std::size_t calls_ = 0;
};
} // namespace resumelab
