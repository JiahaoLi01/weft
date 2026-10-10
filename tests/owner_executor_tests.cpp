#include "core/owner_executor.hpp"
#include "support/check.hpp"
#include "script/script_dom.hpp"
#include "test_constants/script.hpp"
#include <atomic>
#include <iostream>
#include <latch>
#include <future>
#include <vector>
using namespace weft;

namespace {
    using weft::testing::require;

    struct Request {
        unsigned value{};
    };

    struct Input {
        unsigned value{};
    };

    struct Frame {
        DomSnapshot dom;
        std::string log;
    };

    using Owner = OwnerExecutor<Request, Input, Frame>;

    struct State {
        const std::thread::id thread = std::this_thread::get_id();
        ScriptDom vm;
        std::atomic<unsigned>& destroyed;

        State(std::atomic<unsigned>& d) : destroyed(d) {
            install();
        }

        void install() {
            vm.eval(
                "globalThis.root=document.createElement('div');root.id='root';document.body.appendChild(root);globalThis.n=document.createElement('div');n.id='n';document.getElementById('root').appendChild(n);globalThis.log=[]");
        }

        void input(Input v) {
            require(std::this_thread::get_id() == thread, "input changed owner");
            vm.eval("log.push(" + std::to_string(v.value) + ");n.textContent='v" + std::to_string(v.value) +
                    "';n.setAttribute('value'," + std::to_string(v.value) + ")");
            if (v.value % weft::test_constants::script::owner_gc_input_period == 0) {
                vm.collect_garbage();
            }
        }

        std::shared_ptr<const Frame> frame(Request r) {
            require(std::this_thread::get_id() == thread, "frame changed owner");
            vm.eval("n.setAttribute('tick'," + std::to_string(r.value) + ")");
            return std::make_shared<const Frame>(Frame{vm.snapshot_subtree("#n"), vm.eval("JSON.stringify(log)")});
        }

        void reset() {
            vm.reset_document();
            install();
        }

        ~State() {
            require(std::this_thread::get_id() == thread, "VM destructor crossed thread");
            vm.reset_document();
            vm.collect_garbage();
            require(vm.stats().live_nodes == weft::test_constants::script::bootstrap_document_nodes,
                    "owner VM teardown");
            ++destroyed;
        }
    };

    void differential() {
        std::atomic<unsigned> destroyed{};
        Owner owner([&] {
            auto s = std::make_shared<State>(destroyed);
            return Owner::Hooks{[s](Input i) {
                                    s->input(i);
                                },
                                [s](Request r) {
                                    return s->frame(r);
                                },
                                [s](std::uint64_t) {
                                    s->reset();
                                },
                                [] {
                                }};
        });
        State full(destroyed);
        std::shared_ptr<const Frame> frozen;
        std::uint64_t seq{};
        for (unsigned i = 0; i < weft::test_constants::script::owner_differential_inputs; ++i) {
            require(owner.input(1, {i}) == Owner::Accepted, "input not accepted");
            full.input({i});
            seq = owner.request(1, {i});
            if (i % weft::test_constants::script::owner_wait_period ==
                weft::test_constants::script::owner_wait_period - 1) {
                auto p = owner.wait(1, seq);
                auto reference = full.frame({i});
                require(*p.value->dom.attributes.find("value") == *reference->dom.attributes.find("value") &&
                            p.value->dom.data == reference->dom.data &&
                            p.value->dom.children == reference->dom.children && p.value->log == reference->log,
                        "owner copied output differs from serial VM");
                if (i == 15) {
                    frozen = p.value;
                }
            }
        }
        require(frozen && frozen->dom.attributes.at("value") == "15", "old owning snapshot was mutated");
        require(owner.reset(2) == Owner::Accepted && owner.input(1, {900}) == Owner::Stale, "epoch accepted old input");
        require(owner.input(2, {7}) == Owner::Accepted, "new epoch input");
        auto value = owner.wait(2, owner.request(2, {1}));
        require(value.epoch == 2 && value.value->log == "[7]" &&
                    value.value->dom.identity.view_epoch != frozen->dom.identity.view_epoch,
                "reset reused weak identity or old publication");
        owner.close();
        auto work = owner.work();
        require(work.accepted_inputs == work.processed_inputs && work.processed_inputs == 513 && work.resets == 1 &&
                    destroyed == 1,
                "reliable drain or worker destruction failed");
        require(owner.input(2, {0}) == Owner::Closed, "closed input accepted");
    }

    void bounded() {
        std::latch entered(1);
        std::latch release(1);
        std::atomic<unsigned> received{};
        std::atomic<unsigned> frames{};
        std::atomic<unsigned> closed{};
        Owner owner(
            [&] {
                return Owner::Hooks{[&](Input) {
                                        ++received;
                                    },
                                    [&](Request r) {
                                        if (r.value == 0) {
                                            entered.count_down();
                                            release.wait();
                                        }
                                        ++frames;
                                        return std::make_shared<const Frame>();
                                    },
                                    [](std::uint64_t) {
                                    },
                                    [&] {
                                        ++closed;
                                    }};
            },
            2);
        auto first = owner.request(1, {0});
        (void)first;
        entered.wait();
        require(owner.input(1, {1}) == Owner::Accepted && owner.input(1, {2}) == Owner::Accepted &&
                    owner.input(1, {3}) == Owner::Full,
                "bounded reliable queue did not apply backpressure");
        require(owner.reset(2) == Owner::Full, "reset bypassed reliable queue bound");
        std::uint64_t last{};
        for (unsigned i = 1; i <= 100; ++i) {
            last = owner.request(1, {i});
        }
        release.count_down();
        owner.wait(1, last);
        auto work = owner.work();
        require(work.coalesced_requests >= 99 && received == 2 && frames == 2,
                "refresh requests did not coalesce independently of inputs");
        for (unsigned i = 0; i < 6; ++i) {
            last = owner.request(1, {i + 101});
            owner.wait(1, last);
        }
        owner.close();
        require(closed == 1, "close executed twice");
    }

    void sampled_moves() {
        std::latch entered(1);
        std::latch release(1);
        std::vector<unsigned> received;
        Owner owner(
            [&] {
                return Owner::Hooks{[&](Input v) {
                                        received.push_back(v.value);
                                    },
                                    [&](Request r) {
                                        if (r.value == 0) {
                                            entered.count_down();
                                            release.wait();
                                        }
                                        return std::make_shared<const Frame>();
                                    },
                                    [](std::uint64_t) {
                                    },
                                    [] {
                                    }};
            },
            3);
        owner.request(1, {0});
        entered.wait();
        auto compatible = [](Input a, Input b) {
            return a.value >= 100 && b.value >= 100;
        };
        require(owner.input_latest(1, {1}, compatible) == Owner::Accepted, "sampled down rejected");
        for (unsigned i = 100; i < 1100; ++i) {
            require(owner.input_latest(1, {i}, compatible) == Owner::Accepted,
                    "pending motion failed bounded sampling");
        }
        require(owner.input_latest(1, {2}, compatible) == Owner::Accepted, "sampled up rejected");
        require(owner.input_latest(1, {2000}, compatible) == Owner::Full, "motion crossed release event");
        auto serial = owner.request(1, {1});
        release.count_down();
        auto publication = owner.wait(1, serial);
        owner.close();
        auto work = owner.work();
        require(received == std::vector<unsigned>{1, 1099, 2} && work.accepted_inputs == 1002 &&
                    work.coalesced_inputs == 999 && work.processed_inputs == 3 && work.peak_pending_inputs == 3 &&
                    publication.input_sequence == 1002,
                "sampled motion lost final position/edge ordering/accounting");
    }

    void stale_publication() {
        std::latch entered(1);
        std::latch release(1);
        std::atomic<unsigned> resets{};
        Owner owner([&] {
            return Owner::Hooks{[](Input) {
                                },
                                [&](Request r) {
                                    if (r.value == 0) {
                                        entered.count_down();
                                        release.wait();
                                    }
                                    return std::make_shared<const Frame>();
                                },
                                [&](std::uint64_t) {
                                    ++resets;
                                },
                                [] {
                                }};
        });
        owner.request(1, {0});
        entered.wait();
        require(owner.reset(2) == Owner::Accepted, "epoch reset rejected");
        auto next = owner.request(2, {1});
        release.count_down();
        auto p = owner.wait(2, next);
        require(p.epoch == 2 && resets == 1 && owner.work().stale_publications == 1,
                "running old epoch published after reset");
        owner.close();
    }

    void destruction_outside_queue() {
        struct SlowFrame {
            unsigned value;
            std::function<void()> retire;

            SlowFrame(unsigned v, std::function<void()> fn) : value(v), retire(std::move(fn)) {
            }

            ~SlowFrame() {
                if (value == 0) {
                    retire();
                }
            }
        };

        using Queue = OwnerExecutor<Request, Input, SlowFrame>;
        std::latch entered(1);
        std::latch release(1);
        Queue owner([&] {
            return Queue::Hooks{[](Input) {
                                },
                                [&](Request r) {
                                    return std::make_shared<const SlowFrame>(r.value, [&] {
                                        entered.count_down();
                                        release.wait();
                                    });
                                },
                                [](std::uint64_t) {
                                },
                                [] {
                                }};
        });
        owner.wait(1, owner.request(1, {0}));
        owner.request(1, {1});
        entered.wait();
        std::promise<std::uint64_t> result;
        auto future = result.get_future();
        std::jthread game([&] {
            try {
                result.set_value(owner.request(1, {2}));
            } catch (...) {
                result.set_exception(std::current_exception());
            }
        });
        const bool ready =
            future.wait_for(std::chrono::seconds(weft::test_constants::script::queue_api_timeout_seconds)) ==
            std::future_status::ready;
        release.count_down();
        game.join();
        require(ready && future.get() > 0, "snapshot destruction blocked game queue API");
        owner.close();
    }

    void faults() {
        bool caught{};
        try {
            Owner owner([]() -> Owner::Hooks {
                throw std::runtime_error("init");
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        require(caught, "init error did not join/rethrow");
        std::atomic<unsigned> closed{};
        Owner owner([&] {
            return Owner::Hooks{[](Input) {
                                },
                                [](Request) -> std::shared_ptr<const Frame> {
                                    throw std::runtime_error("VM deadline");
                                },
                                [](std::uint64_t) {
                                },
                                [&] {
                                    ++closed;
                                }};
        });
        caught = false;
        try {
            owner.wait(1, owner.request(1, {1}));
        } catch (const std::runtime_error&) {
            caught = true;
        }
        require(caught && closed == 1, "worker exception did not propagate/close");
        try {
            owner.close();
        } catch (const std::runtime_error&) {
        }
    }
} // namespace

int main() {
    try {
        differential();
        bounded();
        sampled_moves();
        stale_publication();
        destruction_outside_queue();
        faults();
        std::cout
            << "PASS owner worker: 512 serial DOM/GC differentials, bounded reliable inputs, coalesced ticks, independent snapshots, epoch reset, fault and shutdown\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
