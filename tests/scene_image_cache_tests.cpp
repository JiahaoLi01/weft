#include "scene/scene_image_cache.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string>

// The image caches of native scene passes: the least recently used values of earlier passes go past the bounds, and
// the values the current pass uses stay, so a pass that holds more than the bounds keeps all of them for the next.
namespace {
    using weft::testing::require;
    using Cache = weft::SceneImageCache<std::string, int>;

    bool has(Cache& cache, const char* key) {
        return cache.find(key) != nullptr;
    }
} // namespace

int main() {
    try {
        // Entries: a pass may hold more than the bound.
        Cache cache(2, 1000);
        cache.begin_pass();
        cache.put("a", 1, 1);
        cache.put("b", 2, 1);
        cache.put("c", 3, 1);
        require(cache.size() == 3 && !cache.evictions(), "a pass keeps every value it puts");
        // The next pass uses a and b, then puts d: c, used by the earlier pass only, goes first, and nothing this pass
        // used goes.
        cache.begin_pass();
        require(has(cache, "a") && has(cache, "b"), "values of the last pass");
        cache.put("d", 4, 1);
        require(cache.size() == 3 && cache.evictions() == 1 && !has(cache, "c"),
                "the least recently used value of an earlier pass goes");
        // A third pass puts e: the earlier pass's values go oldest first until the cache is within its bound.
        cache.begin_pass();
        cache.put("e", 5, 1);
        require(cache.size() == 2 && cache.evictions() == 3 && has(cache, "e") && has(cache, "d") && !has(cache, "a"),
                "oldest first, down to the bound");

        // Using a value makes it the most recently used.
        Cache order(2, 1000);
        order.begin_pass();
        order.put("p", 1, 1);
        order.put("q", 2, 1);
        order.begin_pass();
        require(has(order, "p"), "p used again");
        order.begin_pass();
        order.put("r", 3, 1);
        require(has(order, "p") && !has(order, "q") && has(order, "r"), "the value used later stays");

        // Bytes: the bound evicts as entries do, and one value of the current pass may exceed it alone.
        Cache bytes(100, 10);
        bytes.begin_pass();
        bytes.put("x", 1, 6);
        bytes.begin_pass();
        bytes.put("y", 2, 6);
        require(bytes.bytes() == 6 && !has(bytes, "x") && bytes.evictions() == 1, "byte bound");
        bytes.put("z", 3, 20);
        require(bytes.bytes() == 26 && has(bytes, "y") && has(bytes, "z") && bytes.evictions() == 1,
                "the current pass's values stay over the byte bound");

        // Replacing a value counts its bytes once and evicts nothing; erasing returns its bytes.
        Cache replaced(10, 100);
        replaced.begin_pass();
        replaced.put("k", 1, 6);
        replaced.put("k", 2, 4);
        require(replaced.size() == 1 && replaced.bytes() == 4 && *replaced.find("k") == 2 && !replaced.evictions(),
                "replacement");
        replaced.erase("k");
        replaced.erase("absent");
        require(!replaced.size() && !replaced.bytes() && !has(replaced, "k"), "erase");
        std::cout << "PASS scene image caches: least recently used of earlier passes out, the current pass kept, "
                     "entry and byte bounds, replacement\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
