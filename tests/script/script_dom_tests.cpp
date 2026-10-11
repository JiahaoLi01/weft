#include "script/script_dom.hpp"
#include "script/script_platform.hpp"
#include "../support/check.hpp"
#include "../support/files.hpp"
#include "../test_constants/script.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    using weft::testing::read_file;
    using weft::testing::require;

    // The platform modules are compiled into the library: the embedded text equals the checked-in
    // source (CRLF normalized), and they install in order without any file access.
    void embedded_platform_modules() {
        using weft::ScriptPlatformModule;
        for (const auto module : {ScriptPlatformModule::WebHost, ScriptPlatformModule::AnimationFrame,
                                  ScriptPlatformModule::ScenePlatform}) {
            auto source = read_file(std::string(WEFT_SOURCE_DIR) + "/src/script/js/" +
                                    std::string(weft::script_platform_file(module)));
            std::erase(source, '\r');
            require(weft::script_platform_source(module) == source, "embedded platform module differs from source");
        }
        weft::ScriptDom vm;
        weft::install_script_platform(vm, ScriptPlatformModule::WebHost);
        weft::install_script_platform(vm, ScriptPlatformModule::AnimationFrame);
        weft::install_script_platform(vm, ScriptPlatformModule::ScenePlatform);
        require(vm.eval("[typeof setTimeout, typeof requestAnimationFrame, typeof ResizeObserver].join()") ==
                    "function,function,function",
                "embedded platform modules install");
    }

    void wrapper_identity() {
        weft::ScriptDom probe;
        const auto result = probe.eval(R"JS(
(() => {
 const n = document.createElement('div'); n.setAttribute('id', 'identity');
 document.body.appendChild(n);
 if (document.querySelector('#identity') !== n || document.body.childNodes[0] !== n)
   throw Error('native wrapper identity changed');
 const data = { n, answer: 42 }; n.__data__ = data;
 const symbol = Symbol('native expando'); n[symbol] = n;
 Object.defineProperty(n, 'calculated', { get() { return this.__data__.answer; }, configurable: true });
 if (n.calculated !== 42 || n[symbol] !== n || !('__data__' in n)) throw Error('expandos missing');
 delete n.__data__; delete n[symbol]; delete n.calculated;
 if ('__data__' in n) throw Error('expando delete failed');
 return 'identity/expando passed';
})()
)JS");
        require(result == "identity/expando passed", "wrapper/expando result");
        probe.collect_garbage();
        require(probe.eval("document.querySelector('#identity') === document.body.firstChild") == "true",
                "identity after GC");
    }

    void native_roots_and_cycles() {
        weft::ScriptDom probe;
        const auto baseline = probe.stats().live_nodes;
        probe.eval(R"JS(
(() => {
 const parent = document.createElement('div'); parent.setAttribute('id', 'cycle');
 const child = document.createElement('span'); parent.appendChild(child); document.body.appendChild(parent);
 parent.addEventListener('cycle', () => child.parentNode === parent);
 parent.marker = 1;
 child.__data__ = { parent, listener: () => child };
 globalThis.detached = child;
})()
)JS");
        const auto identity = probe.node_identity("#cycle");
        const auto root = probe.retain_node(identity);
        probe.eval("document.body.removeChild(document.querySelector('#cycle')); globalThis.detached = null;");
        probe.collect_garbage();
        require(probe.is_live(identity), "native root did not keep detached listener/expando graph alive");
        require(probe.stats().live_nodes == baseline + 2, "unexpected retained graph size");
        probe.release_node(root);
        probe.collect_garbage();
        require(!probe.is_live(identity), "unreachable detached subtree/listener/expando cycle leaked");
        require(probe.stats().live_nodes == baseline, "cycles were not fully reclaimed");
        probe.eval("document.body.appendChild(document.createElement('div')).setAttribute('id','replacement')");
        const auto replacement = probe.node_identity("#replacement");
        require(replacement != identity, "generation was reused without revocation");
        // The slot's next occupant starts without the collected node's listeners or own properties.
        const auto calls = probe.stats().listener_calls;
        require(replacement.slot == identity.slot &&
                    probe.eval("(()=>{const r=document.querySelector('#replacement');r.dispatchEvent(new "
                               "Event('cycle'));return Reflect.ownKeys(r).length})()") == "0" &&
                    probe.stats().listener_calls == calls,
                "the next occupant of a node slot inherited script state");
        bool stale_rejected{};
        try {
            (void)probe.retain_node(identity);
        } catch (const std::runtime_error&) {
            stale_rejected = true;
        }
        require(stale_rejected, "stale Node identity accepted");
        probe.eval("globalThis.document = null");
        probe.collect_garbage();
        require(probe.is_live(replacement), "native Document root was not visible to collector");
        require(probe.dump_document().find("replacement") != std::string::npos,
                "Document graph lost after JS global root cleared");
    }

    void js_root_preserves_detached() {
        weft::ScriptDom probe;
        probe.eval(R"JS(
globalThis.heldNode = document.createElement('div');
heldNode.setAttribute('id','held'); document.body.appendChild(heldNode);
heldNode.textContent = '中文 detached';
)JS");
        const auto identity = probe.node_identity("#held");
        probe.eval("document.body.removeChild(heldNode)");
        probe.collect_garbage();
        require(probe.is_live(identity), "JS wrapper did not retain detached Node");
        require(probe.eval("heldNode.textContent") == "中文 detached", "detached contents lost");
        probe.eval("globalThis.heldNode = null");
        probe.collect_garbage();
        require(!probe.is_live(identity), "released detached JS wrapper leaked");
    }

    void event_path_and_listener_snapshot() {
        weft::ScriptDom probe;
        require(probe.eval(R"JS(
(() => {
 const parent = document.createElement('div'), child = document.createElement('span');
 document.body.appendChild(parent); parent.appendChild(child); const calls = [];
 parent.addEventListener('test', () => calls.push('capture'), true);
 const removed = () => calls.push('removed');
 child.addEventListener('test', () => {
   calls.push('target'); child.removeEventListener('test', removed);
   child.addEventListener('test', () => calls.push('new'));
   parent.removeChild(child); document.body.removeChild(parent);
 });
 child.addEventListener('test', removed);
 parent.addEventListener('test', e => {
   if (e.currentTarget !== parent || e.target !== child) throw Error('event targets');
   calls.push('bubble'); e.preventDefault();
 });
 const event = new Event('test', { bubbles: true, cancelable: true });
 const accepted = child.dispatchEvent(event);
 if (calls.join(',') !== 'capture,target,bubble' || accepted || !event.defaultPrevented)
   throw Error('fixed path/listener snapshot mismatch: ' + calls);
 if (event.currentTarget !== null || event.eventPhase !== 0) throw Error('dispatch cleanup');
 return 'event path passed';
})()
)JS") == "event path passed",
                "event path result");
        require(probe.stats().listener_exceptions == 0, "event path callback assertion failed");
        require(probe.eval(R"JS(
(() => {
 const target = document.createElement('div'); let nestedRejected = false, count = 0;
 target.addEventListener('same', e => {
   try { target.dispatchEvent(e); } catch (_) { nestedRejected = true; }
   target.dispatchEvent(new Event('other'));
 }, { once: true });
 target.addEventListener('other', () => ++count);
 target.dispatchEvent(new Event('same')); target.dispatchEvent(new Event('same'));
 if (!nestedRejected || count !== 1) throw Error('event active/once/nesting mismatch');
 return true;
})()
)JS") == "true",
                "event nesting result");
        // A listener that creates many nodes grows the per-node script state; the target's later listeners still run.
        require(probe.eval(R"JS(
(() => {
 const target = document.createElement('div'); const order = [];
 target.addEventListener('grow', () => {
   globalThis.grown = Array.from({ length: 4096 }, () => document.createElement('span')); order.push('grow');
 }, { once: true });
 target.addEventListener('grow', () => order.push('after'));
 target.dispatchEvent(new Event('grow')); globalThis.grown = null;
 return order.join();
})()
)JS") == "grow,after",
                "listeners after node creation in a listener");
    }

    void random_tree_mutations() {
        weft::ScriptDom probe;
        require(probe.eval(R"JS(
(() => {
 let seed = 0x51a37;
 const random = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return seed >>> 0; };
 const parent = document.createElement('div'); document.body.appendChild(parent);
 const nodes = [], reference = [];
 for (let i = 0; i < 48; ++i) {
   const node = document.createElement('span'); node.setAttribute('id', 'node-' + i); nodes.push(node);
 }
 for (let step = 0; step < 1200; ++step) {
   const node = nodes[random() % nodes.length]; const old = reference.indexOf(node);
   if ((random() % 4) === 0 && old >= 0) {
     parent.removeChild(node); reference.splice(old, 1);
   } else {
     if (old >= 0) reference.splice(old, 1);
     const position = random() % (reference.length + 1);
     parent.insertBefore(node, reference[position] || null); reference.splice(position, 0, node);
   }
   const observed = Array.from(parent.childNodes);
   if (observed.length !== reference.length || observed.some((n, i) => n !== reference[i]))
     throw Error('seed 0x51a37 order mismatch at step ' + step);
   if (reference.some(n => n.parentNode !== parent)) throw Error('parent edge mismatch at ' + step);
   for (let i = 1; i < reference.length; ++i)
     if (!(reference[i - 1].compareDocumentPosition(reference[i]) & 4)) throw Error('position mismatch');
 }
 const before = parent.childNodes.length; let rejected = false;
 try { parent.appendChild(document.body); } catch (_) { rejected = true; }
 if (!rejected || before !== parent.childNodes.length || parent.parentNode !== document.body)
   throw Error('invalid insertion changed tree');
 document.body.removeChild(parent);
 return true;
})()
)JS") == "true",
                "random mutation result");
        probe.collect_garbage();
        require(probe.stats().live_nodes == weft::test_constants::script::bootstrap_document_nodes,
                "random mutation graph leaked");
    }

    void reentrant_attribute_conversion() {
        weft::ScriptDom probe;
        require(probe.eval(R"JS(
(() => {
 const n = document.createElement('div'); n.setAttribute('existing', 'before');
 n.setAttribute('existing', { toString() {
   for (let i = 0; i < 100; ++i) n.setAttribute('k' + i, String(i));
   return 'after';
 }});
 if (n.getAttribute('existing') !== 'after' || n.getAttribute('k99') !== '99')
   throw Error('reentrant attribute conversion invalidated native iterator');
 n.setAttributeNS('urn:test', 'test:attr', 'before');
 n.setAttributeNS('urn:test', 'test:attr', { toString() {
   n.removeAttributeNS('urn:test', 'attr');
   for (let i = 0; i < 100; ++i) n.setAttribute('z' + i, String(i));
   return 'after namespace';
 }});
 if (n.getAttributeNS('urn:test', 'attr') !== 'after namespace')
   throw Error('reentrant namespaced conversion did not restore attribute');
 return true;
})()
)JS") == "true",
                "reentrant attribute conversion result");
    }

    void conversion_exceptions() {
        weft::ScriptDom probe;
        require(probe.eval(R"JS(
(() => {
 const n = document.createElement('div'); n.setAttribute('existing', 'before');
 const sentinel = { name: 'exact thrown object' }; let rejected = false, calls = 0;
 try { n.addEventListener('x', () => ++calls, { get capture() { throw sentinel; } }); }
 catch (e) { rejected = e === sentinel; }
 n.dispatchEvent(new Event('x'));
 if (!rejected || calls) throw Error('capture getter exception swallowed or registration occurred');
 rejected = false;
 try { n.addEventListener('x', () => ++calls, { capture: false, get once() { throw sentinel; } }); }
 catch (e) { rejected = e === sentinel; }
 n.dispatchEvent(new Event('x'));
 if (!rejected || calls) throw Error('once getter exception swallowed or registration occurred');
 rejected = false;
 try { n.setAttribute('existing', { toString() { throw sentinel; } }); }
 catch (e) { rejected = e === sentinel; }
 if (!rejected || n.getAttribute('existing') !== 'before') throw Error('attribute conversion lost exception or mutated');
 rejected = false;
 try { n.querySelector({ toString() { throw sentinel; } }); }
 catch (e) { rejected = e === sentinel; }
 if (!rejected) throw Error('selector conversion lost exception');
 rejected = false;
 const event = new Event('x'); Object.defineProperty(event, 'type', { get() { throw sentinel; } });
 try { n.dispatchEvent(event); } catch (e) { rejected = e === sentinel; }
 if (!rejected) throw Error('event getter exception swallowed');
 rejected = false;
 try { document.createElementNS('http://www.w3.org/2000/svg', 'svg:rect'); }
 catch (e) { rejected = String(e).includes('WEFT_UNSUPPORTED_DOM'); }
 if (!rejected) throw Error('prefixed unsupported element names silently changed semantics');
 n.addEventListener('ordinary-error', () => { throw sentinel; });
 n.addEventListener('ordinary-error', () => ++calls);
 n.dispatchEvent(new Event('ordinary-error'));
 if (calls !== 1) throw Error('ordinary listener exception prevented subsequent listener');
 return true;
})()
)JS") == "true",
                "JS conversion exception identity result");
        require(probe.stats().listener_exceptions == 1, "ordinary listener exception diagnostic missing");
    }

    void lifetime_pressure() {
        weft::ScriptDom probe;
        for (int round = 0; round < weft::test_constants::script::lifetime_pressure_rounds; ++round) {
            probe.eval(R"JS(
(() => {
 for (let i = 0; i < 64; ++i) {
   const a = document.createElement('div'), b = document.createElement('span');
   a.appendChild(b); document.body.appendChild(a);
   a.addEventListener('retainer', () => b);
   b.__data__ = { a, b, callback: () => a };
   document.body.removeChild(a);
 }
})()
)JS");
            probe.collect_garbage();
            require(probe.stats().live_nodes == weft::test_constants::script::bootstrap_document_nodes,
                    "lifecycle stress cycle leak");
        }
        require(probe.stats().created_nodes ==
                    probe.stats().finalized_nodes + weft::test_constants::script::bootstrap_document_nodes,
                "native node accounting mismatch");
    }

    void owner_and_epoch() {
        weft::ScriptDom probe;
        std::atomic_bool rejected{};
        std::thread wrong_thread([&] {
            try {
                (void)probe.eval("document.body");
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        wrong_thread.join();
        require(rejected, "non-owner script operation accepted");
        const auto old_epoch = probe.epoch();
        probe.queue_callback(old_epoch, "globalThis.oldCallbackExecuted = true");
        probe.reset_document();
        require(probe.epoch() != old_epoch, "View epoch did not change");
        probe.queue_callback(probe.epoch(), "globalThis.newCallbackExecuted = true");
        require(probe.drain_callbacks() == 1, "late callback was executed");
        require(probe.stats().rejected_callbacks == 1, "late callback rejection was not recorded");
        require(probe.eval("typeof oldCallbackExecuted === 'undefined' && newCallbackExecuted") == "true",
                "callback crossed reload epoch");
    }

    void execution_limits() {
        weft::ScriptDom probe;
        bool recursion_rejected{};
        try {
            (void)probe.eval("(function recurse(){ return recurse(); })()", "stack-limit-fixture.js");
        } catch (const std::runtime_error& e) {
            const std::string message(e.what());
            recursion_rejected =
                message.find("call stack") != std::string::npos || message.find("stack overflow") != std::string::npos;
        }
        require(recursion_rejected, "deep recursion did not become a recoverable JS stack exception");
        require(probe.eval("1 + 1") == "2", "owner process/VM did not survive deep recursion");
        bool interrupted{};
        bool faulted{};
        try {
            (void)probe.eval("while (true) {}", "infinite-loop-fixture.js");
        } catch (const std::runtime_error& e) {
            interrupted = std::string(e.what()).find("WEFT_VIEW_FAULTED") != std::string::npos;
        }
        require(interrupted, "infinite loop was not interrupted and faulted");
        try {
            (void)probe.eval("1 + 1");
        } catch (const std::runtime_error& e) {
            faulted = std::string(e.what()).find("WEFT_VIEW_FAULTED") != std::string::npos;
        }
        require(faulted, "interrupted View resumed without reset");
        probe.reset_document();
        require(probe.eval("1 + 1") == "2", "reset did not recover faulted View");
        bool microtask_limit{};
        try {
            (void)probe.eval("Promise.resolve().then(function forever(){ Promise.resolve().then(forever); });");
        } catch (const std::runtime_error& e) {
            const std::string message(e.what());
            microtask_limit = message.find("WEFT_MICROTASK_LIMIT") != std::string::npos ||
                              message.find("WEFT_VIEW_FAULTED") != std::string::npos;
        }
        require(microtask_limit, "unbounded microtasks were not faulted");
        bool microtask_sticky_fault{};
        try {
            (void)probe.eval("1 + 1");
        } catch (const std::runtime_error& e) {
            microtask_sticky_fault = std::string(e.what()).find("WEFT_VIEW_FAULTED") != std::string::npos;
        }
        require(microtask_sticky_fault, "microtask View resumed without reset");
        probe.reset_document();
        require(probe.eval("document.body.childNodes.length") == "0", "microtasks survived VM reset");
        bool caught_oom_faulted{};
        try {
            (void)probe.eval("try { new ArrayBuffer(80 * 1024 * 1024); } catch (_) {}", "caught-oom-fixture.js");
        } catch (const std::runtime_error& e) {
            caught_oom_faulted = std::string(e.what()).find("WEFT_VIEW_FAULTED") != std::string::npos;
        }
        require(caught_oom_faulted, "caught VM OOM did not fault View");
        probe.reset_document();
        require(probe.eval("1") == "1", "caught-OOM reset did not recover");
    }
} // namespace

int main(int argc, char** argv) {
    try {
        embedded_platform_modules();
        wrapper_identity();
        native_roots_and_cycles();
        js_root_preserves_detached();
        event_path_and_listener_snapshot();
        random_tree_mutations();
        reentrant_attribute_conversion();
        conversion_exceptions();
        lifetime_pressure();
        owner_and_epoch();
        execution_limits();
        std::string d3_path;
        std::string fixture_path;
        if (argc == 3) {
            d3_path = argv[1];
            fixture_path = argv[2];
        }
#ifdef WEFT_SOURCE_DIR
        else if (argc == 1) {
            d3_path = std::string(WEFT_SOURCE_DIR) + "/third_party/d3/d3.js";
            fixture_path = std::string(WEFT_SOURCE_DIR) + "/tests/fixtures/d3_probe.js";
        }
#endif
        else
            throw std::runtime_error("Usage: script_dom_tests <d3.js> <d3_probe.js>");
        weft::ScriptDom probe;
        probe.eval(read_file(d3_path), d3_path);
        const auto report = probe.eval(read_file(fixture_path), fixture_path);
        probe.collect_garbage();
        std::cout
            << "PASS native DOM identity, visible roots, cyclic GC, events, seed 0x51a37, 1536 lifecycle graphs, epochs, owner thread\n";
        std::cout << report << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
