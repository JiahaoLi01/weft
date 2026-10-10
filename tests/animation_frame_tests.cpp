#include "caseelephant/animation_frame_probe.hpp"
#include "caseelephant/constants/script.hpp"
#include "../probes/host_time.hpp"
#include "test_constants/script.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <sstream>
#include <thread>

namespace {
    void require(bool value, const char* message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    }

    std::string read(const char* name) {
        std::ifstream f(std::string(CE_SOURCE_DIR) + name, std::ios::binary);
        require(bool(f), "bootstrap missing");
        std::ostringstream s;
        s << f.rdbuf();
        return s.str();
    }

    void install(caseelephant::ScriptDomProbe& vm) {
        vm.eval(read("/probes/web_host_bootstrap.js"));
        vm.eval(read("/probes/animation_frame_bootstrap.js"));
    }
} // namespace

int main() {
    try {
        caseelephant::ScriptProbeConfig config{caseelephant::constants::script::probe_vm_stack_bytes};
#ifdef CE_REACT_GC_STRESS
        config.execution_budget_ms = caseelephant::constants::script::max_execution_budget_ms;
#endif
        caseelephant::ScriptDomProbe vm(config);
        const auto baseline_nodes = vm.stats().live_nodes;
        const auto baseline_roots = vm.stats().native_roots;
        install(vm);
        require(vm.eval(R"JS(
      globalThis.log=[];
      requestAnimationFrame(function(t){log.push(['A',t,this===globalThis,performance.now()]);queueMicrotask(()=>{log.push(['micro']);cancelAnimationFrame(cancelled);requestAnimationFrame(t=>log.push(['nested',t]));});});
      const cancelled=requestAnimationFrame(()=>log.push(['cancelled']));
      requestAnimationFrame(t=>{log.push(['throws',t]);throw new Error('sample error')});
      requestAnimationFrame(t=>{log.push(['last',t]);setTimeout(()=>log.push(['timer']),0)});
      log.length;
    )JS") == "0",
                "rAF ran synchronously");
        auto result = caseelephant::run_animation_frame_probe(vm, 16.25);
        require(result.callbacks == 3 && result.pending == 1 && result.exceptions == 1,
                "batch cancellation/error counts");
        require(result.exception_messages_json == "[\"Error: sample error\"]", "callback error diagnostics");
        require(vm.eval("JSON.stringify(log)") ==
                    "[[\"A\",16.25,true,16.25],[\"micro\"],[\"throws\",16.25],[\"last\",16.25]]",
                "microtask checkpoint / common timestamp / error continuation");
        result = caseelephant::run_animation_frame_probe(vm, 32.5);
        require(result.callbacks == 1 && result.exceptions == 0 && result.pending == 0,
                "nested callback ran in same batch");
        require(vm.eval("log[log.length-1][0]") == "nested", "nested callback missing");
        vm.eval("__ceHost.pump(32.5)");
        require(vm.eval("log[log.length-1][0]") == "timer", "timer crossed frame checkpoint");
        bool rejected = false;
        try {
            caseelephant::run_animation_frame_probe(vm, 32.4);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected && vm.eval("performance.now()") == "32.5", "backward frame partially advanced clock");
        for (double invalid :
             {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            rejected = false;
            try {
                caseelephant::run_animation_frame_probe(vm, invalid);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected, "invalid frame time");
        }
        require(
            vm.eval(
                "(()=>{let n=0;for(const value of [null,3,{}]){try{requestAnimationFrame(value)}catch(e){n+=e instanceof TypeError}}return n===3})()") ==
                "true",
            "noncallable rAF accepted");
        require(
            vm.eval(
                "(()=>{let n=0;for(const value of [1n,Object(1n),Symbol()]){try{cancelAnimationFrame(value)}catch(e){n+=e instanceof TypeError}}return n===3})()") ==
                "true",
            "cancel used Number coercion instead of WebIDL ToNumber");
        vm.eval(
            "globalThis.directCalls=0;const direct=()=>++directCalls;direct.call=()=>{throw Error('own call should not run')};requestAnimationFrame(direct)");
        require(caseelephant::run_animation_frame_probe(vm, 32.75).callbacks == 1 && vm.eval("directCalls") == "1",
                "callback invocation used overridden function.call");
        vm.eval(
            "globalThis.loops=0;globalThis.loop=t=>{++loops;requestAnimationFrame(loop)};requestAnimationFrame(loop)");
        for (unsigned i = 0; i < 32; ++i) {
            result = caseelephant::run_animation_frame_probe(vm, 33 + i * .25);
            require(result.callbacks == 1 && result.pending == 1, "self scheduling escaped snapshot");
        }
        require(vm.eval("loops") == "32", "fractional opportunities / shared timestamp");
        result = caseelephant::run_animation_frame_probe(vm, 4000, false);
        require(result.callbacks == 0 && result.pending == 1, "hidden frame dispatched or lost callback");
        result = caseelephant::run_animation_frame_probe(vm, 9000);
        require(result.callbacks == 1 && result.pending == 1, "restore replayed historical frames");
        bool wrong_thread = false;
        std::thread worker([&] {
            try {
                caseelephant::run_animation_frame_probe(vm, 9001);
            } catch (const std::logic_error&) {
                wrong_thread = true;
            }
        });
        worker.join();
        require(wrong_thread, "cross-thread frame host accepted");
        vm.reset_document();
        install(vm);
        require(caseelephant::run_animation_frame_probe(vm, 0).callbacks == 0, "rAF crossed reset epoch");
        // Counterexample from the visible Canvas host: legacy formatting rounds UP.
        const double fractional = caseelephant::test_constants::script::legacy_rounding_time_ms;
        vm.eval("__ceHost.pump(" + std::to_string(fractional) + ")");
        rejected = false;
        try {
            vm.animation_frame(fractional);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "legacy rounded timer counterexample no longer rejects backward rAF");
        vm.reset_document();
        install(vm);
        vm.eval(
            "globalThis.clockFrames=0;globalThis.clockLoop=t=>{if(t!==performance.now())throw Error('clock disagreement');++clockFrames;requestAnimationFrame(clockLoop)};requestAnimationFrame(clockLoop)");
        for (unsigned i = 0; i < 1024; ++i) {
            const double time = i * caseelephant::test_constants::script::clock_frame_interval_ms + fractional;
            const auto stamp = probe_host_time(time);
            require(std::stod(stamp) == time, "host timestamp did not round-trip");
            vm.eval("__ceHost.pump(" + stamp + ")");
            const auto frame = vm.animation_frame(time);
            vm.eval("__ceHost.pump(" + stamp + ")");
            require(frame.callbacks == 1 && !frame.exceptions, "timer/rAF fractional clock regression");
        }
        require(vm.eval("clockFrames") == "1024", "fractional clock lost callbacks");
        vm.reset_document();
        install(vm);
        vm.eval("globalThis.ids=[];for(let i=0;i<1001;++i)ids.push(requestAnimationFrame(()=>{}));");
        rejected = false;
        try {
            caseelephant::run_animation_frame_probe(vm, 1);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected && vm.eval("__ceAnimationHost.pending()===1001 && performance.now()===0") == "true",
                "ready batch limit must preserve pending/clock");
        vm.eval("cancelAnimationFrame(ids.pop())");
        require(caseelephant::run_animation_frame_probe(vm, 1).callbacks == 1000,
                "failed ready batch could not recover");
        vm.eval("ids=[];for(let i=0;i<4096;++i)ids.push(requestAnimationFrame(()=>{}));");
        require(
            vm.eval(
                "(()=>{try{requestAnimationFrame(()=>{});return false}catch(e){return __ceAnimationHost.pending()===4096}})()") ==
                "true",
            "pending callback quota");
        vm.eval("ids.forEach(cancelAnimationFrame)");
        require(caseelephant::run_animation_frame_probe(vm, 2).pending == 0, "cancelled capacity not recovered");
        vm.eval(
            "requestAnimationFrame(()=>{throw {toString(){throw 1}}});requestAnimationFrame(()=>{globalThis.afterBadError=true})");
        result = caseelephant::run_animation_frame_probe(vm, 3);
        require(result.callbacks == 2 && result.exceptions == 1 && vm.eval("afterBadError") == "true",
                "throwing error formatter stopped other callbacks");
        vm.eval("for(let i=0;i<70;++i)requestAnimationFrame(()=>{throw new Error('x'.repeat(600))});");
        result = caseelephant::run_animation_frame_probe(vm, 4);
        require(
            result.exceptions == 70 &&
                vm.eval(
                    "__ceAnimationHost.errors().length===64 && __ceAnimationHost.errors().every(x=>x.length<=512)") ==
                    "true",
            "unbounded exception diagnostics");
        vm.eval(
            "requestAnimationFrame(()=>{try{__ceAnimationHost.begin(5)}catch(e){globalThis.reentryRejected=true}})");
        caseelephant::run_animation_frame_probe(vm, 5);
        require(vm.eval("reentryRejected") == "true", "nested frame consumed pending batch");
        vm.eval(
            "globalThis.stamps=[];requestAnimationFrame(t=>{stamps.push(t);performance.now=()=>99999});requestAnimationFrame(t=>stamps.push(t))");
        caseelephant::run_animation_frame_probe(vm, 5.5);
        require(vm.eval("JSON.stringify(stamps)") == "[5.5,5.5]",
                "callback changed batch timestamp through public clock");
        for (unsigned cycle = 0; cycle < caseelephant::test_constants::script::reset_cycles; ++cycle) {
            vm.reset_document();
            install(vm);
            vm.collect_garbage();
            const auto installed_nodes = vm.stats().live_nodes;
            vm.eval(
                "(()=>{const n=document.createElement('div');n.id='detached';document.body.appendChild(n);document.body.removeChild(n);requestAnimationFrame(()=>{n.textContent='alive';globalThis.detachedResult=n.textContent})})()");
            vm.collect_garbage();
            require(caseelephant::run_animation_frame_probe(vm, 0).callbacks == 1 &&
                        vm.eval("detachedResult") == "alive",
                    "pending closure lost detached node");
            vm.collect_garbage();
            require(vm.stats().live_nodes == installed_nodes, "invoked frame retained detached closure until reset");
            vm.eval(
                "(()=>{const n=document.createElement('div');globalThis.cancelHandle=requestAnimationFrame(()=>{globalThis.cancelledValue=n.textContent})})()");
            vm.collect_garbage();
            require(vm.stats().live_nodes == installed_nodes + 1, "pending closure was not traced");
            vm.eval("cancelAnimationFrame(cancelHandle)");
            vm.collect_garbage();
            require(vm.stats().live_nodes == installed_nodes, "cancelled frame retained detached closure until reset");
            vm.collect_garbage();
            vm.reset_document();
            vm.collect_garbage();
            require(vm.stats().native_roots == baseline_roots && vm.stats().live_nodes == baseline_nodes,
                    "frame closure leaked after reset");
        }
#ifdef CE_REACT_GC_STRESS
        const unsigned batch_budget = 1000;
        const unsigned spin_ms = 35;
#else
        const unsigned batch_budget = 20;
        const unsigned spin_ms = 3;
#endif
        caseelephant::ScriptDomProbe limited({caseelephant::constants::script::probe_vm_stack_bytes, batch_budget});
        install(limited);
        limited.eval("for(let i=0;i<40;++i)requestAnimationFrame(()=>{const until=Date.now()+" +
                     std::to_string(spin_ms) + ";while(Date.now()<until){}})");
        rejected = false;
        try {
            caseelephant::run_animation_frame_probe(limited, 0);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "callbacks reset the shared batch deadline");
        rejected = false;
        try {
            limited.eval("1");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "budget failure must fault the view");
        limited.reset_document();
        install(limited);
        require(caseelephant::run_animation_frame_probe(limited, 0).callbacks == 0,
                "faulted residual callbacks survived reset");
        limited.eval(
            "requestAnimationFrame(()=>{Promise.resolve().then(function loop(){Promise.resolve().then(loop)})})");
        rejected = false;
        try {
            caseelephant::run_animation_frame_probe(limited, 1);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "infinite rAF microtasks not bounded");
        limited.reset_document();
        install(limited);
        require(caseelephant::run_animation_frame_probe(limited, 0).callbacks == 0, "microtasks survived fault/reset");
        std::cout
            << "PASS owner animation frames: snapshots, microtasks, cancellation, clocks, visibility, errors, limits, epoch/GC\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
