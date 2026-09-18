#include "../src/guest_fiber_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

using Scheduler = vita3k::web::GuestFiberScheduler;
using State = Scheduler::State;
#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::abort(); } } while (false)

static void check_state(const Scheduler &s, Scheduler::TaskId id, State state, bool failed = false) {
    auto status = s.status(id);
    CHECK(status && status->state == state && status->failed == failed);
}

struct Nested {
    int seed;
    int visits = 0;
    int destructors = 0;
};
struct Guard {
    int &count;
    ~Guard() { ++count; }
};

// Address-taken volatile locals force distinct C stacks, while recursive return
// paths and scalar accumulators also exercise Asyncify's saved Wasm frames.
__attribute__((noinline)) static int nested(Scheduler &s, Nested &data, int depth) {
    volatile int local[17];
    for (int i = 0; i < 17; ++i)
        local[i] = data.seed + depth * 100 + i;
    Guard guard{data.destructors};
    int result = depth;
    if (depth) {
        result += nested(s, data, depth - 1);
    } else {
        for (int i = 0; i < 3; ++i) {
            ++data.visits;
            CHECK(s.yield());
            CHECK(data.destructors == 0);
        }
    }
    for (int i = 0; i < 17; ++i)
        CHECK(local[i] == data.seed + depth * 100 + i);
    return result;
}

static void nested_task(Scheduler &s, void *arg) {
    CHECK(nested(s, *static_cast<Nested *>(arg), 5) == 15);
}

static void enqueue_and_nested_test() {
    Scheduler s;
    Nested a{7}, b{901};
    auto aid = s.enqueue(0, nested_task, &a);
    auto bid = s.enqueue(0, nested_task, &b);
    CHECK(aid != bid && aid != Scheduler::invalid_task);
    CHECK(a.visits == 0 && b.visits == 0); // enqueue is inert
    CHECK(!s.teardown()); // even never-started tasks are live
    CHECK(s.resume(0) == 0);
    CHECK(!s.yield() && !s.park());
    CHECK(s.enqueue(0, nullptr) == Scheduler::invalid_task);
    CHECK(!s.status(Scheduler::invalid_task));
    CHECK(!s.wake(aid));
    for (int i = 1; i <= 3; ++i) {
        CHECK(s.resume(1) == 1);
        CHECK(a.visits == i && b.visits == i - 1);
        CHECK(s.resume(1) == 1);
        CHECK(a.visits == i && b.visits == i);
        CHECK(a.destructors == 0 && b.destructors == 0);
        CHECK(!s.teardown()); // suspended nested frames remain intact
        check_state(s, aid, State::runnable);
    }
    CHECK(s.resume(1) == 1);
    check_state(s, aid, State::completed);
    CHECK(a.destructors == 6 && b.destructors == 0);
    CHECK(!s.teardown()); // completion of one cannot dispose the other
    CHECK(s.resume(100) == 1);
    check_state(s, bid, State::completed);
    CHECK(b.destructors == 6);
    CHECK(!s.wake(aid) && s.resume(100) == 0);
    CHECK(s.teardown() && s.teardown());
    CHECK(!s.status(aid));
}

struct Rotation { std::vector<int> &trace; int tag; };
static void rotating_task(Scheduler &s, void *arg) {
    auto &data = *static_cast<Rotation *>(arg);
    for (int i = 0; i < 3; ++i) {
        data.trace.push_back(data.tag);
        if (i != 2)
            CHECK(s.yield());
    }
}

static void priority_test() {
    Scheduler s;
    std::vector<int> trace;
    Rotation low{trace, 9}, a{trace, 1}, b{trace, 2}, first{trace, 0};
    s.enqueue(10, rotating_task, &low);
    s.enqueue(0, rotating_task, &a);
    s.enqueue(0, rotating_task, &b);
    s.enqueue(-10, rotating_task, &first);
    CHECK(s.resume(100) == 12);
    CHECK((trace == std::vector<int>{0, 0, 0, 1, 2, 1, 2, 1, 2, 9, 9, 9}));
    CHECK(s.teardown());
}

struct Parked {
    Scheduler::TaskId id = Scheduler::invalid_task;
    int stage = 0;
    int destructors = 0;
    std::vector<int> trace;
};
static void parked_task(Scheduler &s, void *arg) {
    auto &data = *static_cast<Parked *>(arg);
    Guard guard{data.destructors};
    volatile int preserved = 312;
    data.stage = 1;
    CHECK(!s.teardown() && s.resume(99) == 0);
    CHECK(s.park());
    CHECK(preserved == 312 && data.destructors == 0);
    data.stage = 2;
    data.trace.push_back(2);
    CHECK(s.park());
    CHECK(preserved == 312);
    data.stage = 3;
}
static void waker_task(Scheduler &s, void *arg) {
    auto &data = *static_cast<Parked *>(arg);
    CHECK(s.wake(data.id));
    CHECK(!s.wake(data.id));
    CHECK(data.stage == 1); // wake is also inert
    data.trace.push_back(1);
}
static void peer_task(Scheduler &, void *arg) {
    static_cast<Parked *>(arg)->trace.push_back(3);
}

static void park_wake_test() {
    Scheduler s;
    Parked data;
    data.id = s.enqueue(0, parked_task, &data);
    CHECK(s.resume(99) == 1);
    check_state(s, data.id, State::parked);
    CHECK(!s.teardown() && data.destructors == 0);
    CHECK(s.resume(99) == 0); // parked is not runnable
    CHECK(!s.wake(123456));
    s.enqueue(0, waker_task, &data);
    s.enqueue(0, peer_task, &data);
    CHECK(s.resume(3) == 3);
    CHECK((data.trace == std::vector<int>{1, 3, 2})); // wake joins FIFO tail
    CHECK(data.stage == 2 && data.destructors == 0);
    check_state(s, data.id, State::parked);
    CHECK(s.wake(data.id)); // root can also wake
    CHECK(data.stage == 2);
    CHECK(s.resume(10) == 1);
    CHECK(data.stage == 3 && data.destructors == 1);
    check_state(s, data.id, State::completed);
    CHECK(s.teardown());
}

static void throwing_task(Scheduler &s, void *arg) {
    Guard guard{*static_cast<int *>(arg)};
    CHECK(s.yield());
    throw std::runtime_error("synthetic callback failure");
}
static void count_task(Scheduler &, void *arg) { ++*static_cast<int *>(arg); }
static void exception_test() {
    Scheduler s;
    int destructors = 0, count = 0;
    auto failed = s.enqueue(0, throwing_task, &destructors);
    auto normal = s.enqueue(0, count_task, &count);
    CHECK(s.resume(10) == 3);
    check_state(s, failed, State::completed, true);
    check_state(s, normal, State::completed);
    CHECK(destructors == 1 && count == 1);
    CHECK(s.teardown());
    auto reused = s.enqueue(0, count_task, &count);
    CHECK(reused > normal);
    CHECK(s.resume(1) == 1 && count == 2);
    // Destructor accepts completed tasks even without explicit teardown.
}

struct EnqueueInside { Scheduler &other; int count = 0; };
static void spawning_task(Scheduler &s, void *arg) {
    auto &data = *static_cast<EnqueueInside *>(arg);
    auto id = s.enqueue(-1, count_task, &data.count);
    CHECK(data.count == 0);
    CHECK(data.other.resume(1) == 0); // cross-scheduler nested dispatch refused
    CHECK(!data.other.teardown());
    CHECK(s.yield());
    check_state(s, id, State::completed);
    CHECK(data.count == 1);
}
static void reentrancy_test() {
    Scheduler s, other;
    EnqueueInside data{other};
    int count = 0;
    other.enqueue(0, count_task, &count);
    s.enqueue(0, spawning_task, &data);
    CHECK(s.resume(3) == 3);
    CHECK(count == 0);
    CHECK(other.resume(1) == 1 && count == 1);
}

int main(int argc, char **argv) {
    // Separate-process negative tests; custom terminate handler proves refusal
    // occurs before member destruction. Expected process status is 86.
    if (argc == 2) {
        std::set_terminate([] { std::_Exit(86); });
        Scheduler s;
        if (std::strcmp(argv[1], "--destroy-parked") == 0) {
            static Parked data;
            s.enqueue(0, parked_task, &data);
            CHECK(s.resume(1) == 1);
        } else if (std::strcmp(argv[1], "--destroy-live") == 0) {
            static int count;
            s.enqueue(0, count_task, &count);
        } else {
            return 2;
        }
        return 0; // s destructor must terminate, not discard a live continuation
    }
    enqueue_and_nested_test();
    priority_test();
    park_wake_test();
    exception_test();
    reentrancy_test();
    bool rejected = false;
    try { Scheduler invalid(1025); } catch (const std::invalid_argument &) { rejected = true; }
    CHECK(rejected);
    std::printf("guest_fiber_scheduler: 6 test groups passed (pointer bits=%zu)\n", sizeof(void *) * 8);
}
