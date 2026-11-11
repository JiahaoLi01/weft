#include "bridge/engine_channel.hpp"

#include "bridge/bridge_value.hpp"
#include "constants/bridge.hpp"

#include <stdexcept>

namespace weft::bridge {
    namespace {
        // A value converted from JSON text; empty for text that is not JSON or past the bounds.
        std::vector<std::uint8_t> from_json(std::string_view json) {
            try {
                return value_from_json(json);
            } catch (const std::invalid_argument&) {
                return {};
            }
        }
    } // namespace

    bool EngineChannel::valid_name(std::string_view name) {
        return !name.empty() && name.size() <= constants::bridge::max_name_bytes && utf8(name);
    }

    ChannelStatus EngineChannel::emit(std::string_view name, std::span<const std::uint8_t> value, bool latest) {
        if (!valid_name(name) || !valid_value(value)) {
            return ChannelStatus::Invalid;
        }
        const std::lock_guard lock(mutex_);
        if (latest) {
            if (const auto found = last_event_.find(std::string(name)); found != last_event_.end()) {
                auto& event = inputs_[found->second];
                if (bytes_ - event.value.size() + value.size() > constants::bridge::max_pending_bytes) {
                    return ChannelStatus::Full;
                }
                bytes_ = bytes_ - event.value.size() + value.size();
                event.value.assign(value.begin(), value.end());
                return ChannelStatus::Ok;
            }
        }
        if (inputs_.size() >= constants::bridge::max_pending_inputs ||
            bytes_ + value.size() > constants::bridge::max_pending_bytes) {
            return ChannelStatus::Full;
        }
        inputs_.push_back({EngineInput::Kind::Event, std::string(name), 0, false, {value.begin(), value.end()}});
        last_event_[inputs_.back().name] = inputs_.size() - 1;
        bytes_ += value.size();
        return ChannelStatus::Ok;
    }

    ChannelStatus EngineChannel::emit_json(std::string_view name, std::string_view json, bool latest) {
        const auto value = from_json(json);
        return value.empty() ? ChannelStatus::Invalid : emit(name, value, latest);
    }

    ChannelStatus EngineChannel::complete(std::uint64_t call, bool succeeded, std::span<const std::uint8_t> value) {
        if (!valid_value(value)) {
            return ChannelStatus::Invalid;
        }
        const std::lock_guard lock(mutex_);
        if (!open_.contains(call)) {
            return ChannelStatus::Stale;
        }
        if (inputs_.size() >= constants::bridge::max_pending_inputs ||
            bytes_ + value.size() > constants::bridge::max_pending_bytes) {
            return ChannelStatus::Full;
        }
        open_.erase(call);
        inputs_.push_back({EngineInput::Kind::Completion, {}, call, succeeded, {value.begin(), value.end()}});
        bytes_ += value.size();
        return ChannelStatus::Ok;
    }

    ChannelStatus EngineChannel::complete_json(std::uint64_t call, bool succeeded, std::string_view json) {
        const auto value = from_json(json);
        return value.empty() ? ChannelStatus::Invalid : complete(call, succeeded, value);
    }

    ChannelStatus EngineChannel::set_model(std::string_view model, std::span<const std::uint8_t> fields) {
        if (!valid_name(model) || !valid_value(fields)) {
            return ChannelStatus::Invalid;
        }
        // Each field's last value in the map (a later key replaces an earlier one), as its bytes.
        std::map<std::string_view, std::span<const std::uint8_t>> entries;
        ValueReader reader(fields);
        const auto map = reader.next();
        if (map.type != ValueReader::Type::Map) {
            return ChannelStatus::Invalid;
        }
        for (std::uint32_t i = 0; i < map.count; ++i) {
            const auto key = reader.next();
            if (!valid_name(key.text)) {
                return ChannelStatus::Invalid;
            }
            entries.insert_or_assign(key.text, reader.skip());
        }
        const std::lock_guard lock(mutex_);
        auto& pending = models_[std::string(model)];
        std::size_t bytes = bytes_;
        for (const auto& [field, value] : entries) {
            const auto found = pending.find(field);
            bytes = bytes - (found == pending.end() ? 0 : found->second.size()) + value.size();
        }
        if (bytes > constants::bridge::max_pending_bytes) {
            if (pending.empty()) {
                models_.erase(std::string(model));
            }
            return ChannelStatus::Full;
        }
        for (const auto& [field, value] : entries) {
            pending[std::string(field)].assign(value.begin(), value.end());
        }
        bytes_ = bytes;
        return ChannelStatus::Ok;
    }

    ChannelStatus EngineChannel::set_model_json(std::string_view model, std::string_view json) {
        const auto value = from_json(json);
        return value.empty() ? ChannelStatus::Invalid : set_model(model, value);
    }

    std::uint64_t EngineChannel::open_call() {
        const std::lock_guard lock(mutex_);
        if (open_.size() >= constants::bridge::max_open_calls) {
            return 0;
        }
        const auto call = next_call_++;
        open_.insert(call);
        return call;
    }

    void EngineChannel::close_call(std::uint64_t call) {
        const std::lock_guard lock(mutex_);
        open_.erase(call);
    }

    EnginePending EngineChannel::take() {
        EnginePending out;
        const std::lock_guard lock(mutex_);
        for (auto& [name, fields] : models_) {
            auto& model = out.models.emplace_back();
            model.name = name;
            for (auto& [field, value] : fields) {
                model.fields.emplace_back(field, std::move(value));
            }
        }
        models_.clear();
        out.inputs = std::move(inputs_);
        inputs_.clear();
        last_event_.clear();
        bytes_ = 0;
        return out;
    }

    void EngineChannel::reset() {
        const std::lock_guard lock(mutex_);
        inputs_.clear();
        last_event_.clear();
        models_.clear();
        bytes_ = 0;
        open_.clear();
    }

    std::size_t EngineChannel::open_calls() const {
        const std::lock_guard lock(mutex_);
        return open_.size();
    }

#if WEFT_DEVTOOLS
    EngineChannel::Checkpoint EngineChannel::checkpoint() const {
        const std::lock_guard lock(mutex_);
        return {inputs_, models_, open_};
    }

    void EngineChannel::restore(Checkpoint checkpoint) {
        const std::lock_guard lock(mutex_);
        auto since = std::move(inputs_);
        inputs_ = std::move(checkpoint.inputs);
        inputs_.insert(inputs_.end(), std::make_move_iterator(since.begin()), std::make_move_iterator(since.end()));
        // A field set since is the later value.
        for (auto& [model, fields] : models_) {
            auto& kept = checkpoint.models[model];
            for (auto& [field, value] : fields) {
                kept[field] = std::move(value);
            }
        }
        models_ = std::move(checkpoint.models);
        open_ = std::move(checkpoint.open);
        // The bytes that wait are the values', as emit, complete and set_model count them.
        last_event_.clear();
        bytes_ = 0;
        for (std::size_t i = 0; i < inputs_.size(); ++i) {
            bytes_ += inputs_[i].value.size();
            if (inputs_[i].kind == EngineInput::Kind::Event) {
                last_event_[inputs_[i].name] = i;
            }
        }
        for (const auto& [model, fields] : models_) {
            for (const auto& [field, value] : fields) {
                bytes_ += value.size();
            }
        }
    }
#endif
} // namespace weft::bridge
