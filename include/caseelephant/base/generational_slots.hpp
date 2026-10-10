#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace caseelephant {
    // Slot table behind generational handles: slot indices are reused, generations are not.
    //
    // generation(i) is the generation of the latest occupant of slot i (0: never occupied). A
    // handle {index, generation} resolves only while that slot is occupied with that generation.
    // A slot whose generation reaches the maximum is retired and never handed out again, so a stale
    // handle can never alias a later occupant.
    template <class T, class Generation = std::uint32_t>
    class GenerationalSlots {
    public:
        static constexpr Generation max_generation = std::numeric_limits<Generation>::max();

        GenerationalSlots() = default;

        explicit GenerationalSlots(std::size_t size) : slots_(size) {
        }

        std::size_t size() const noexcept {
            return slots_.size();
        }

        // Lowest slot that is neither occupied nor retired, or size() when there is none.
        std::size_t first_free() const noexcept {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (reusable(i)) {
                    return i;
                }
            }
            return slots_.size();
        }

        bool reusable(std::size_t index) const noexcept {
            return !slots_[index].occupied && slots_[index].generation != max_generation;
        }

        // Appends a never-occupied slot and returns its index.
        std::size_t append() {
            slots_.emplace_back();
            return slots_.size() - 1;
        }

        // Starts a new occupancy of a reusable slot and returns its generation. The value is left as
        // is: callers may fill it (and keep its reserved capacity) before or after occupying.
        Generation occupy(std::size_t index) noexcept {
            auto& slot = slots_[index];
            slot.occupied = true;
            return ++slot.generation;
        }

        // Ends the current occupancy; handles to it become stale. The value is left as is.
        void vacate(std::size_t index) noexcept {
            slots_[index].occupied = false;
        }

        bool occupied(std::size_t index) const noexcept {
            return slots_[index].occupied;
        }

        Generation generation(std::size_t index) const noexcept {
            return slots_[index].generation;
        }

        // Occupied value addressed by a handle, or nullptr when the handle is stale or out of range.
        T* find(std::size_t index, Generation generation) noexcept {
            return index < slots_.size() && slots_[index].occupied && slots_[index].generation == generation
                       ? &slots_[index].value
                       : nullptr;
        }

        const T* find(std::size_t index, Generation generation) const noexcept {
            return const_cast<GenerationalSlots*>(this)->find(index, generation);
        }

        // Value of any slot, occupied or not.
        T& operator[](std::size_t index) noexcept {
            return slots_[index].value;
        }

        const T& operator[](std::size_t index) const noexcept {
            return slots_[index].value;
        }

    private:
        struct Slot {
            T value{};
            Generation generation = 0;
            bool occupied = false;
        };

        std::vector<Slot> slots_;
    };
} // namespace caseelephant
