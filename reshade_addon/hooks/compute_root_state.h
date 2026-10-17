#pragma once
#include <array>
#include <cstdint>
#include <algorithm>

// Independent of the graphics bindings: switching graphics roots must not erase
// compute roots. Tracks only values actually defined by the application.
struct ComputeRootState {
    enum class Kind { none, table, cbv, srv, uav, constants, acceleration_structure };
    struct Arg {
        Kind kind = Kind::none;
        uint64_t value = 0;
        std::array<uint32_t, 64> words{};
        uint64_t defined = 0;
    };
    uint64_t layout = 0;
    std::array<Arg, 64> args{};
    bool complete = true;

    void Signature(uint64_t next)
    {
        if (layout == next) return;
        layout = next;
        args = {};
        complete = true;
    }
    void Descriptor(uint32_t param, Kind kind, uint64_t value)
    {
        if (param >= args.size()) { complete = false; return; }
        args[param] = {};
        args[param].kind = kind;
        args[param].value = value;
    }
    void Constants(uint32_t param, uint32_t first, uint32_t count, const uint32_t* values)
    {
        if (param >= args.size() || first > 64 || count > 64 - first || (count && !values)) {
            complete = false;
            return;
        }
        auto& a = args[param];
        if (a.kind != Kind::constants) a = {};
        a.kind = Kind::constants;
        for (uint32_t i = 0; i < count; ++i) {
            a.words[first + i] = values[i];
            a.defined |= uint64_t(1) << (first + i);
        }
    }
};
