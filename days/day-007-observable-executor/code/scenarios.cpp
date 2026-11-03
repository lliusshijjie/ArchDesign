#include "scenarios.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "lab.hpp"

namespace archdesign::lab {
namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

TaskSpec task(std::uint64_t id, Milliseconds work = Milliseconds{0},
              Milliseconds timeout = Milliseconds{5000},
              std::size_t bytes = 64, bool fail = false) {
    TaskSpec spec;
    spec.id = id;
    spec.work = work;
    spec.timeout = timeout;
    spec.payload_bytes = bytes;
    spec.fail = fail;
    return spec;
}

class Gate {
public:
    Gate() : future_(promise_.get_future().share()) {}
    ~Gate() { release(); }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    std::shared_future<void> future() const { return future_; }
    void release() {
        if (!released_) {
            promise_.set_value();
            released_ = true;
        }
    }
private:
    std::promise<void> promise_;
    std::shared_future<void> future_;
    bool released_ = false;
};

// 声明在 Pipeline 之后：断言抛异常时先解开故障闸门，再析构管线。
class ReleaseOnExit {
public:
    explicit ReleaseOnExit(Gate& gate) : gate_(gate) {}
    ~ReleaseOnExit() { gate_.release(); }
private:
    Gate& gate_;
};

class CountingSink final : public LogSink {
public:
    std::shared_future<void> gate;
    std::shared_ptr<std::promise<void>> entered;

    void write(const LogRecord&) override {
        std::call_once(first_write_, [this] { if (entered) entered->set_value(); });
        if (gate.valid()) gate.wait();
        ++writes;
    }
    void flush() override {}
    std::atomic<std::uint64_t> writes{0};
private:
    std::once_flag first_write_;
};

struct Results {
    std::vector<Result> items;

    void record(const Result& result) { items.push_back(result); }
    bool has(std::uint64_t id, Status status) const {
        return std::any_of(items.begin(), items.end(), [id, status](const Result& result) {
            return result.id == id && result.status == status;
        });
    }
};

template<class Predicate>
void pump_until(Pipeline& pipeline, Predicate predicate, std::string_view message) {
    const auto until = std::chrono::steady_clock::now() + Milliseconds{3000};
    while (!predicate()) {
        pipeline.pump();
        require(std::chrono::steady_clock::now() < until, message);
        std::this_thread::sleep_for(Milliseconds{1});
    }
}

void verify(const Snapshot& s, const Config& config, const Results& results) {
    const auto rejected = s.ingress_rejected + s.admission_rejected + s.pool_rejected +
                          s.admission_errors + s.stopped_rejected;
    require(s.offered == s.admitted + rejected, "offered accounting mismatch");
    require(s.admitted == s.succeeded + s.failed + s.timed_out, "terminal accounting mismatch");
    require(s.retired == s.admitted, "accepted work was not physically retired");
    require(s.pool.submitted == s.admitted && s.pool.completed == s.admitted,
            "worker pool and pipeline counts differ");
    require(s.ingress == 0 && s.in_flight == 0, "pipeline did not drain");
    require(s.max_ingress <= config.ingress_capacity, "ingress exceeded capacity");
    require(s.max_active <= config.max_in_flight, "in-flight limit exceeded");
    require(s.pool.max_queued <= config.worker_queue, "worker queue exceeded capacity");
    require(s.logger.queue_depth == 0 && s.logger.accepted == s.logger.processed,
            "accepted logs did not drain");
    require(s.logger.sink_errors == 0 && s.observer_errors == 0, "observer failed");
    require(s.admission_errors == 0, "unexpected resource or timer admission error");
    require(s.timer.pending == 0, "timer accounting did not settle");
    require(s.allocator.active == s.allocator_before.active, "pooled allocations remain active");
    require(s.allocator.allocations - s.allocator_before.allocations ==
            s.allocator.deallocations - s.allocator_before.deallocations,
            "pooled allocations and deallocations differ");
    require(results.items.size() == s.offered, "one terminal result per offered task required");
    std::unordered_set<std::uint64_t> ids;
    for (const auto& result : results.items) {
        require(ids.insert(result.id).second, "duplicate terminal result");
    }
}

void report(std::string_view name, const Snapshot& s, const Results& results) {
    std::cout << "\n[" << name << "]\n";
    for (const auto& result : results.items) {
        std::cout << "  id=" << result.id << " status=" << to_string(result.status)
                  << " elapsed_us=" << result.elapsed_us << " checksum=" << result.checksum << '\n';
    }
    std::vector<std::int64_t> elapsed;
    for (const auto& result : results.items) {
        if (result.status == Status::Succeeded || result.status == Status::Failed ||
            result.status == Status::TimedOut) elapsed.push_back(result.elapsed_us);
    }
    std::sort(elapsed.begin(), elapsed.end());
    const auto p99 = elapsed.empty() ? 0 : elapsed[(elapsed.size() * 99 + 99) / 100 - 1];
    std::cout << "  offered=" << s.offered << " admitted=" << s.admitted
              << " succeeded=" << s.succeeded << " failed=" << s.failed
              << " timed_out=" << s.timed_out << " retired=" << s.retired
              << " late_completed=" << s.late_completed << '\n'
              << "  rejected: ingress=" << s.ingress_rejected
              << " admission=" << s.admission_rejected << " pool=" << s.pool_rejected
              << " admission_error=" << s.admission_errors << " stopped=" << s.stopped_rejected << '\n'
              << "  max_ingress=" << s.max_ingress << " max_in_flight=" << s.max_active
              << " max_worker_queued=" << s.pool.max_queued << " pump_rounds=" << s.rounds
              << " sample_p99_us=" << p99 << '\n'
              << "  logs: accepted=" << s.logger.accepted << " dropped=" << s.logger.dropped
              << " processed=" << s.logger.processed << '\n'
              << "  allocator: allocations=" << s.allocator.allocations - s.allocator_before.allocations
              << " deallocations=" << s.allocator.deallocations - s.allocator_before.deallocations
              << " active_delta=" << s.allocator.active - s.allocator_before.active
              << " large=" << s.allocator.large_allocations - s.allocator_before.large_allocations << '\n'
              << "[PASS] " << name << '\n';
}

void normal() {
    Config config;
    CountingSink sink;
    Results results;
    Pipeline pipeline(config, sink, [&results](const Result& result) { results.record(result); });
    for (std::uint64_t id = 1; id <= 6; ++id) {
        require(pipeline.offer(task(id, Milliseconds{1}, Milliseconds{5000},
                                    id == 6 ? 8192 : 64)), "normal ingress rejected");
    }
    pipeline.shutdown();
    const auto s = pipeline.snapshot();
    verify(s, config, results);
    require(s.succeeded == 6 && s.admitted == 6, "normal tasks must all succeed");
    require(s.allocator.large_allocations > s.allocator_before.large_allocations,
            "large allocation fallback was not exercised");
    for (const auto& result : results.items) {
        require(result.checksum == result.id * (result.id == 6 ? 8192 : 64), "payload checksum mismatch");
    }
    report("normal", s, results);
}

void ingress() {
    Config config;
    config.ingress_capacity = 2;
    CountingSink sink;
    Results results;
    Pipeline pipeline(config, sink, [&results](const Result& result) { results.record(result); });
    for (std::uint64_t id = 1; id <= 5; ++id) pipeline.offer(task(id));
    pipeline.shutdown();
    const auto s = pipeline.snapshot();
    verify(s, config, results);
    require(s.ingress_rejected == 3 && s.succeeded == 2, "ingress overflow was not isolated");
    report("ingress", s, results);
}

void overload() {
    Config config;
    config.workers = 1;
    config.worker_queue = 1;
    Gate work_gate;
    CountingSink sink;
    Results results;
    Pipeline pipeline(config, sink, [&results](const Result& result) { results.record(result); });
    ReleaseOnExit release(work_gate);
    auto started = std::make_shared<std::promise<void>>();
    auto ready = started->get_future();
    auto first = task(1);
    first.gate = work_gate.future();
    first.started = started;
    pipeline.offer(std::move(first));
    pump_until(pipeline, [&ready] { return ready.wait_for(Milliseconds{0}) == std::future_status::ready; },
               "worker did not enter gate");
    for (std::uint64_t id = 2; id <= 5; ++id) pipeline.offer(task(id));
    pipeline.pump();
    require(pipeline.snapshot().pool_rejected == 3, "expected exactly three pool rejections");
    work_gate.release();
    pipeline.shutdown();
    const auto s = pipeline.snapshot();
    verify(s, config, results);
    require(s.admitted == 2 && s.succeeded == 2, "running and queued work must survive overload");
    report("overload", s, results);
}

void deadline() {
    Config config;
    config.workers = 1;
    config.worker_queue = 1;
    config.max_in_flight = 1;
    config.ring_capacity = 2;
    Gate work_gate;
    CountingSink sink;
    Results results;
    Pipeline pipeline(config, sink, [&results](const Result& result) { results.record(result); });
    ReleaseOnExit release(work_gate);
    auto first = task(1, Milliseconds{0}, Milliseconds{20});
    first.gate = work_gate.future();
    pipeline.offer(std::move(first));
    pump_until(pipeline, [&results] { return results.has(1, Status::TimedOut); }, "deadline did not fire");
    for (const auto& result : results.items) {
        if (result.id == 1) require(result.elapsed_us >= 20000, "deadline response fired early");
    }
    require(pipeline.snapshot().in_flight == 1 && pipeline.snapshot().retired == 0,
            "timeout must not release physical capacity");
    pipeline.offer(task(2));
    pipeline.pump();
    require(results.has(2, Status::AdmissionRejected), "timed out work still owns its admission slot");
    work_gate.release();
    pipeline.shutdown();
    const auto s = pipeline.snapshot();
    verify(s, config, results);
    require(s.timed_out == 1 && s.late_completed == 1 && s.admitted == 1,
            "late completion must reclaim once without a second response");
    report("deadline", s, results);
}

void slow_log() {
    Config config;
    config.log_capacity = 2;
    Gate sink_gate;
    CountingSink sink;
    sink.gate = sink_gate.future();
    sink.entered = std::make_shared<std::promise<void>>();
    auto entered = sink.entered->get_future();
    Results results;
    Pipeline pipeline(config, sink, [&results](const Result& result) { results.record(result); });
    ReleaseOnExit release(sink_gate);
    pipeline.offer(task(1));
    pump_until(pipeline, [&entered] { return entered.wait_for(Milliseconds{0}) == std::future_status::ready; },
               "logger did not enter sink gate");
    for (std::uint64_t id = 2; id <= 9; ++id) pipeline.offer(task(id));
    require(pipeline.drain_for(Milliseconds{3000}), "business drain stalled behind diagnostic logging");
    require(pipeline.snapshot().logger.dropped > 0, "slow sink must cause diagnostic log drops");
    require(results.items.size() == 9, "business results must arrive while the sink is held");
    sink_gate.release();
    pipeline.shutdown();
    const auto s = pipeline.snapshot();
    verify(s, config, results);
    require(s.succeeded == 9, "diagnostic log loss must not lose task results");
    report("slow-log", s, results);
}

void failure_shutdown() {
    Config config;
    config.workers = 1;
    CountingSink sink;
    Results results;
    Pipeline pipeline(config, sink, [&results](const Result& result) { results.record(result); });
    pipeline.offer(task(1, Milliseconds{0}, Milliseconds{5000}, 64, true));
    pipeline.offer(task(2));
    pipeline.shutdown();
    pipeline.shutdown();
    require(!pipeline.offer(task(3)), "closed pipeline must reject new work");
    const auto s = pipeline.snapshot();
    verify(s, config, results);
    require(s.failed == 1 && s.succeeded == 1 && s.stopped_rejected == 1,
            "failure isolation or shutdown semantics failed");
    require(results.has(1, Status::Failed) && results.has(2, Status::Succeeded),
            "worker must survive injected task failure");
    report("failure-shutdown", s, results);
}

} // namespace

void run_scenario(std::string_view name) {
    if (name == "normal") normal();
    else if (name == "ingress") ingress();
    else if (name == "overload") overload();
    else if (name == "deadline") deadline();
    else if (name == "slow-log") slow_log();
    else if (name == "failure-shutdown") failure_shutdown();
    else if (name == "all") {
        for (const auto scenario : {"normal", "ingress", "overload", "deadline", "slow-log", "failure-shutdown"}) {
            run_scenario(scenario);
        }
    } else throw std::invalid_argument("unknown scenario: " + std::string(name));
}

void run_input_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open input: " + path);
    std::vector<TaskSpec> tasks;
    std::unordered_set<std::uint64_t> ids;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos ||
            line[line.find_first_not_of(" \t\r")] == '#') continue;
        std::istringstream row(line);
        std::int64_t id = 0, work_ms = 0, timeout_ms = 0, bytes = 0;
        int fail = 0;
        std::string extra;
        if (!(row >> id >> work_ms >> timeout_ms >> bytes >> fail) || row >> extra ||
            id <= 0 || work_ms < 0 || work_ms > 60000 || timeout_ms <= 0 || timeout_ms > 60000 ||
            bytes <= 0 || bytes > 8192 || (fail != 0 && fail != 1) || tasks.size() >= 1000) {
            throw std::invalid_argument("invalid task at input line " + std::to_string(line_number));
        }
        require(ids.insert(static_cast<std::uint64_t>(id)).second, "duplicate id in input");
        tasks.push_back(task(static_cast<std::uint64_t>(id), Milliseconds{work_ms},
                             Milliseconds{timeout_ms}, static_cast<std::size_t>(bytes), fail != 0));
    }
    Config config;
    CountingSink sink;
    Results results;
    Pipeline pipeline(config, sink, [&results](const Result& result) { results.record(result); });
    for (auto& spec : tasks) pipeline.offer(std::move(spec));
    pipeline.shutdown();
    const auto s = pipeline.snapshot();
    verify(s, config, results);
    report("input-file", s, results);
}

} // namespace archdesign::lab
