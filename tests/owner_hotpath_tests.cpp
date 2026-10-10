#include "caseelephant/script_probe.hpp"
#include "caseelephant/script_platform.hpp"
#include "support/check.hpp"
#include "support/files.hpp"
#include "caseelephant/constants/script.hpp"
#include "support/host_time.hpp"
#include "test_constants/script.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <algorithm>

using namespace caseelephant;

namespace {
    using caseelephant::testing::format_host_time;
    using caseelephant::testing::read_source_file;
    using caseelephant::testing::require;

    void install(ScriptDomProbe& vm, bool old = false) {
        // old: the platform modules before the owner hot-path optimization, kept as a differential.
        if (old) {
            vm.eval(read_source_file("/tests/fixtures/owner-hotpath/web_host_before.js"));
            vm.eval(read_source_file("/tests/fixtures/owner-hotpath/animation_frame_before.js"));
            return;
        }
        caseelephant::install_script_platform(vm, caseelephant::ScriptPlatformModule::WebHost);
        caseelephant::install_script_platform(vm, caseelephant::ScriptPlatformModule::AnimationFrame);
    }

    void legacy_frame(ScriptDomProbe& vm, double time, bool drawable) {
        vm.eval("__ceAnimationHost.begin(" + format_host_time(time) + "," + (drawable ? "true" : "false") + ")");
        while (vm.eval("__ceAnimationHost.next()") != "0") {
        }
    }

    void scheduling() {
        ScriptDomProbe missing;
        bool absent = false;
        try {
            missing.pump_tasks(0);
        } catch (const std::runtime_error&) {
            absent = true;
        }
        require(absent, "missing optional platform module was accepted");
        install(missing);
        require(missing.pump_tasks(0).ready == 0 && missing.animation_frame(0).pending == 0,
                "missing module poisoned later installation");
        ScriptDomProbe fast({caseelephant::constants::script::probe_vm_stack_bytes,
                             caseelephant::constants::script::max_execution_budget_ms});
        ScriptDomProbe full({caseelephant::constants::script::probe_vm_stack_bytes,
                             caseelephant::constants::script::max_execution_budget_ms});
        install(fast);
        install(full, true);
        const auto script = R"JS(
globalThis.log=[];globalThis.seed=123;globalThis.handles=[];globalThis.raf=[];
globalThis.mutate=()=>{
 seed=(Math.imul(seed,1664525)+1013904223)>>>0;
 const id=seed%9;
 if(id<4){const h=setTimeout(function(a){log.push(['timer',a,performance.now(),this===globalThis]);queueMicrotask(()=>log.push(['micro',a]));if(a%7===0)setTimeout(()=>log.push(['nested',a]),0)},seed%11,seed);handles.push(h)}
 else if(id===4){let n=0;const h=setInterval(()=>{log.push(['interval',h,performance.now()]);if(++n===3)clearInterval(h)},1+seed%7);handles.push(h)}
 else if(id===5)clearTimeout(handles[seed%handles.length]);
 else if(id===6)raf.push(requestAnimationFrame(t=>{log.push(['raf',t]);queueMicrotask(()=>cancelAnimationFrame(raf[seed%raf.length]));requestAnimationFrame(t=>log.push(['next-raf',t]))}));
 else if(id===7)cancelAnimationFrame(raf[seed%raf.length]);
 else document.getElementById('root').setAttribute('data-seed',seed);
};
)JS";
        fast.eval(script);
        full.eval(script);
        const auto before = fast.stats().script_evaluations;
        for (unsigned i = 0; i < caseelephant::test_constants::script::scheduler_differential_frames; ++i) {
            const double time = i * caseelephant::test_constants::script::scheduler_frame_interval_ms +
                                caseelephant::test_constants::script::legacy_rounding_time_ms;
            fast.eval("mutate()");
            full.eval("mutate()");
            auto pump = [&] {
                for (unsigned k = 0; k < caseelephant::constants::script::max_task_pump_turns; ++k) {
                    const auto a = fast.pump_tasks(time);
                    const auto b = full.eval("__ceHost.pump(" + format_host_time(time) + ")");
                    require(std::to_string(a.ready) == b, "native timer batch differs from preserved full module");
                    if (!a.ready) {
                        return;
                    }
                }
                throw std::runtime_error("pump limit");
            };
            pump();
            const bool visible = i % caseelephant::test_constants::script::hidden_frame_period != 0;
            fast.animation_frame(time, visible);
            legacy_frame(full, time, visible);
            pump();
            require(fast.eval("JSON.stringify(log)") == full.eval("JSON.stringify(log)"),
                    "timer/rAF/microtask clock/cancel/order differential");
            require(fast.snapshot_subtree("#root").attributes == full.snapshot_subtree("#root").attributes,
                    "scheduled mutation differs");
            if (i % caseelephant::test_constants::script::scheduler_gc_period == 0) {
                fast.collect_garbage();
                full.collect_garbage();
            }
        }
        require(fast.stats().host_calls > 1000 &&
                    full.stats().script_evaluations > fast.stats().script_evaluations + 1000 &&
                    fast.stats().script_evaluations > before,
                "scheduler never used direct calls");
        for (unsigned cycle = 0; cycle < caseelephant::test_constants::script::reset_cycles; ++cycle) {
            fast.reset_document();
            install(fast);
            fast.eval(
                "let n=document.createElement('div');n.textContent='cycle';document.getElementById('root').appendChild(n);setTimeout(()=>n.setAttribute('done','yes'),0);requestAnimationFrame(()=>n.textContent='frame')");
            require(fast.pump_tasks(0).callbacks == 1 && fast.animation_frame(0).callbacks == 1,
                    "cached host function crossed reset");
            fast.reset_document();
            fast.collect_garbage();
            require(fast.stats().live_nodes == caseelephant::test_constants::script::bootstrap_document_nodes &&
                        fast.stats().native_roots == 1,
                    "cached host functions or journal rooted old nodes");
        }
        install(fast);
        fast.eval("for(let i=0;i<1001;++i)setTimeout(()=>{},0)");
        bool rejected = false;
        try {
            fast.pump_tasks(0);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "native timer ready bound ignored");
        fast.reset_document();
        install(fast);
        fast.eval("setTimeout(()=>{for(let i=0;i<1001;++i)setTimeout(()=>{},0)},0)");
        require(fast.pump_tasks(0).ready == 1001, "post-batch ready count must preserve next batch quota semantics");
        rejected = false;
        try {
            fast.pump_tasks(0);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "next timer batch quota ignored");
        fast.reset_document();
        install(fast);
        fast.eval("setTimeout(()=>{throw Error('timer error')},0)");
        require(fast.pump_tasks(0).callbacks == 1, "throwing timer count");
        rejected = false;
        try {
            fast.pump_tasks(1);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "queued timer error not reported");
        require(fast.pump_tasks(1).ready == 0, "timer error recovery failed");
        rejected = false;
        std::thread thread([&] {
            try {
                fast.pump_tasks(-1);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        thread.join();
        require(rejected, "cross-owner direct call accepted");
        ScriptDomProbe budget({caseelephant::constants::script::probe_vm_stack_bytes,
                               caseelephant::test_constants::script::spin_execution_budget_ms});
        install(budget);
        budget.eval("setTimeout(()=>{for(;;){}},0)");
        rejected = false;
        try {
            budget.pump_tasks(0);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "native timer call escaped execution budget");
        budget.reset_document();
        install(budget);
        require(budget.pump_tasks(0).ready == 0, "budget reset failed");
    }

    void revisions() {
        ScriptDomProbe vm({caseelephant::constants::script::probe_vm_stack_bytes,
                           caseelephant::constants::script::max_execution_budget_ms});
        install(vm);
        vm.eval(
            "globalThis.root=document.getElementById('root');globalThis.side=document.createElement('div');side.id='side';document.body.appendChild(side);globalThis.p=document.createElement('div');p.id='p';root.appendChild(p);globalThis.n=document.createElement('span');n.id='n';p.appendChild(n);globalThis.t=document.createTextNode('x');n.appendChild(t)");
        const auto root = vm.node_identity("#root");
        const auto side = vm.node_identity("#side");
        auto version = vm.subtree_revision(root);
        auto snapshot = vm.snapshot_subtree(root);
        const auto count = vm.stats().snapshot_nodes;
        vm.eval("side.textContent='outside'");
        require(vm.subtree_revision(root) == version && vm.subtree_revision(side) > version,
                "sibling mutation dirtied unrelated subtree");
        require(vm.stats().snapshot_nodes == count, "revision lookup copied a snapshot");
        const char* operations[]{"n.setAttribute('x','1')",
                                 "n.id='renamed'",
                                 "t.nodeValue='value'",
                                 "t.data='data'",
                                 "t.textContent='content'",
                                 "p.removeChild(n)",
                                 "n.setAttribute('detached','yes');p.appendChild(n)",
                                 "p.textContent='replace'",
                                 "p.textContent=''",
                                 "p.appendChild(n)"};
        for (auto operation : operations) {
            vm.eval(operation);
            const auto current = vm.subtree_revision(root);
            require(current != version, "snapshot mutation missed ancestor revision");
            auto full = vm.snapshot_subtree(root);
            require(full != snapshot, "revision counterexample fixture did not change snapshot");
            snapshot = std::move(full);
            version = current;
        }
        version = vm.subtree_revision(root);
        vm.eval("n.setAttributeNS('urn:test','q:color','red')");
        require(vm.subtree_revision(root) > version, "namespace write missed revision");
        version = vm.subtree_revision(root);
        vm.eval("n.removeAttributeNS('urn:test','color')");
        require(vm.subtree_revision(root) > version, "namespace removal missed revision");
        vm.eval("n.setAttribute('same','a')");
        version = vm.subtree_revision(root);
        vm.eval("n.setAttribute('same','a');n.removeAttribute('missing')");
        require(vm.subtree_revision(root) == version, "no-op attribute writes dirtied snapshot");
        vm.eval(
            "globalThis.seed=0x9e3779b9;globalThis.nodes=[];for(let i=0;i<24;++i){let b=document.createElement('div');b.id='b'+i;p.appendChild(b);nodes.push(b)};globalThis.mutate=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;let b=nodes[seed%24];switch((seed>>>8)%7){case 0:b.textContent=''+seed;break;case 1:b.setAttribute('x',seed);break;case 2:b.removeAttribute('x');break;case 3:if(b.parentNode)b.parentNode.removeChild(b);else p.appendChild(b);break;case 4:p.insertBefore(b,p.firstChild);break;case 5:b.setAttributeNS('urn:seed','q:value',seed);b.removeAttributeNS('urn:seed','value');break;case 6:side.setAttribute('outside',seed)}}");
        snapshot = vm.snapshot_subtree(root);
        version = vm.subtree_revision(root);
        unsigned reused{};
        for (unsigned i = 0; i < caseelephant::test_constants::script::mutation_differential_steps; ++i) {
            const auto cursor = vm.changes_since(0).revision;
            vm.eval("mutate()");
            const auto current = vm.subtree_revision(root);
            if (current != version) {
                snapshot = vm.snapshot_subtree(root);
                version = current;
            } else {
                ++reused;
            }
            require(snapshot == vm.snapshot_subtree("#root"),
                    "revision-retained snapshot differs from full random mutation snapshot");
            auto changes = vm.changes_since(cursor);
            require(changes.epoch == vm.epoch() && !changes.full_required, "short journal cursor lost history");
            for (auto id : changes.nodes) {
                require(vm.is_live(id), "journal returned finalized generation");
            }
            if (i % caseelephant::test_constants::script::mutation_gc_period == 0) {
                vm.collect_garbage();
            }
        }
        require(reused > 40, "random differential did not reuse unchanged snapshots");
        // Layout callbacks cannot reenter the owner. Use only captured value state.
        double scroll_x{};
        double scroll_y{};
        vm.install_layout_host([&](const ProbeDomSnapshot& d) {
            auto find = [&](auto&& self, const auto& node) -> const ProbeDomSnapshot* {
                if (node.identity == root) {
                    return &node;
                }
                for (const auto& c : node.children) {
                    if (auto r = self(self, c)) {
                        return r;
                    }
                }
                return nullptr;
            };
            auto n = find(find, d);
            require(n, "layout root missing");
            ProbeElementGeometry g;
            g.identity = root;
            g.rect = {0, 0, 100, 100};
            g.client_width = g.client_height = 100;
            g.scroll_width = 200;
            g.scroll_height = 300;
            g.scroll_x = g.scroll_y = g.wheel_x = g.wheel_y = true;
            g.scroll_left = scroll_x = n->scroll_left;
            g.scroll_top = scroll_y = n->scroll_top;
            return std::vector{g};
        });
        version = vm.subtree_revision(root);
        vm.eval("root.scrollTop=75;root.scrollLeft=20");
        require(vm.subtree_revision(root) > version && vm.snapshot_subtree(root).scroll_top == 75,
                "scroll setters missed revision");
        version = vm.subtree_revision(root);
        vm.dispatch_wheel(root, 0, 23);
        require(vm.subtree_revision(root) > version && vm.snapshot_subtree(root).scroll_top == 98,
                "wheel missed revision");
        const auto cursor = vm.changes_since(0).revision;
        vm.eval("for(let i=0;i<1100;++i)n.setAttribute('overflow',i)");
        require(vm.changes_since(cursor).full_required, "journal overflow did not request full snapshot");
        vm.eval("let c=document.createElement('div');c.id='collect';root.appendChild(c);globalThis.collect=c;c=null");
        const auto old = vm.node_identity("#collect");
        vm.eval("root.removeChild(collect);collect=null");
        vm.collect_garbage();
        require(!vm.is_live(old), "weak dirty journal rooted detached node");
        auto retained = vm.snapshot_subtree(root);
        const auto epoch = vm.epoch();
        vm.reset_document();
        vm.collect_garbage();
        require(vm.epoch() != epoch && !vm.is_live(root) && retained.children.size() > 0,
                "reset invalidated owned snapshot or retained old identity");
        bool rejected = false;
        try {
            vm.subtree_revision(root);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "old-epoch revision accepted");
        require(vm.changes_since(0).full_required, "new epoch did not request baseline snapshot");
    }
} // namespace

int main() {
    try {
        scheduling();
        revisions();
        std::cout
            << "PASS owner hotpath: 320 scheduler differentials, 512 mutation differentials, bounds, weak journal, GC/reset\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
