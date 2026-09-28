// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
void portable_aot_fixture(const std::filesystem::path &path) {
    using namespace vita3k::wasmjit;
    auto block = blank();
    const auto converted = append(block, Opcode::FPFixedU32ToDouble,
        {reg(block, Reg::R0), Value{uint8_t(16)}, Value{uint8_t(0)}});
    set(block, Reg::R6, append(block, Opcode::LeastSignificantWord, {converted}));
    set(block, Reg::R7, append(block, Opcode::MostSignificantWord, {converted}));
    set(block, Reg::R5, append(block, Opcode::FPMulAdd32,
        {reg(block, Reg::R2), reg(block, Reg::R3), reg(block, Reg::R4)}));
    set(block, Reg::PC, Value{uint32_t(0x1004)});
    append(block, Opcode::A32CallSupervisor, {Value{uint32_t(42)}});
    block.ReplaceTerminal(IR::Term::ReturnToDispatch{});
    const std::vector<RegionBlockMeta> meta{{0x1000, A32::LocationDescriptor::CPSR_MODE_MASK,
        loc().CPSR().Value() & A32::LocationDescriptor::CPSR_MODE_MASK, 1}};
    for (unsigned mode = 0; mode < 4; ++mode) {
        RegionStateOptions options{};
        options.promote_flags = (mode & 1) != 0;
        options.promote_accounting = (mode & 2) != 0;
        const auto body = emit_aot_function({&block}, meta, options);
        CHECK(!body.empty());
        const auto module = assemble_aot_module({body}, {{0x1000, 4}}, {}, options);
        CHECK(!module.empty());
        std::ofstream output(path / ("portable_aot_" + std::to_string(mode) + ".wasm"), std::ios::binary);
        output.write(reinterpret_cast<const char *>(module.data()), module.size());
        CHECK(output.good());
    }
    auto in = initial(); in.fpscr = 0; in.regs[0] = 0xffffffff;
    in.regs[2] = 0xbf800000; in.regs[3] = 0x3f800001; in.regs[4] = 0x3f7ffffe;
    for (unsigned i = 0; i < 6; ++i) in.fp_arguments[i] = 0x12340000 + i;
    std::ofstream state(path / "portable_aot_state.json");
    json_state(state, in);
}

void portable_boundaries(Suite &suite) {
    std::mt19937_64 rng{0x20260928};
    for (const auto op : {Opcode::VectorSignedMultiply16, Opcode::VectorSignedMultiply32,
             Opcode::VectorUnsignedMultiply16, Opcode::VectorUnsignedMultiply32}) {
        const bool wide = op == Opcode::VectorSignedMultiply32 || op == Opcode::VectorUnsignedMultiply32;
        const bool sign = op == Opcode::VectorSignedMultiply16 || op == Opcode::VectorSignedMultiply32;
        const unsigned bits = wide ? 32 : 16;
        auto block = blank();
        const auto product = append(block, op, {
            append(block, Opcode::A32GetVector, {Value{A32::ExtReg::Q0}}),
            append(block, Opcode::A32GetVector, {Value{A32::ExtReg::Q1}})});
        append(block, Opcode::A32SetVector, {Value{A32::ExtReg::Q2}, append(block, Opcode::GetLowerFromOp, {product})});
        append(block, Opcode::A32SetVector, {Value{A32::ExtReg::Q3}, append(block, Opcode::GetUpperFromOp, {product})});
        std::vector<Case> cases;
        const uint64_t mask = (uint64_t(1) << bits) - 1;
        for (unsigned trial = 0; trial < 256; ++trial) {
            auto in = initial();
            for (unsigned i = 0; i < 8; ++i) in.fpu[i] = trial < 4 ? (0xffffffffu >> trial) : rng();
            auto out = next(in);
            for (unsigned i = 0; i < 128 / bits; ++i) {
                const unsigned word = i * bits / 32, shift = i * bits % 32;
                const auto lane = [&](unsigned base) -> int64_t {
                    const uint64_t x = (in.fpu[base + word] >> shift) & mask;
                    return sign && (x >> (bits - 1)) ? int64_t(x) - (int64_t(1) << bits) : int64_t(x);
                };
                const uint64_t product = uint64_t(lane(0)) * uint64_t(lane(4));
                out.fpu[8 + word] |= uint32_t(product & mask) << shift;
                out.fpu[12 + word] |= uint32_t((product >> bits) & mask) << shift;
            }
            cases.emplace_back(in, out);
        }
        suite.add("portable_" + IR::GetNameOf(op), block, cases);
    }
    for (unsigned tables = 1; tables <= 4; ++tables) {
        auto block = blank();
        std::array<Value, 4> registers{};
        for (unsigned i = 0; i < tables; ++i)
            registers[i] = append(block, Opcode::A32GetVector, {Value{A32::ExtReg(unsigned(A32::ExtReg::Q0) + i)}});
        const auto table = append(block, Opcode::VectorTable, {registers[0], registers[1], registers[2], registers[3]});
        const auto result = append(block, Opcode::VectorTableLookup128, {
            append(block, Opcode::A32GetVector, {Value{A32::ExtReg::Q4}}), table,
            append(block, Opcode::A32GetVector, {Value{A32::ExtReg::Q5}})});
        append(block, Opcode::A32SetVector, {Value{A32::ExtReg::Q6}, result});
        std::vector<Case> cases;
        for (unsigned trial = 0; trial < 256; ++trial) {
            auto in = initial();
            for (unsigned i = 0; i < 20; ++i) in.fpu[i] = rng();
            for (unsigned i = 0; i < 16; ++i) in.fpu[20 + i / 4] |= ((trial + i) & 255) << (8 * (i % 4));
            auto out = next(in);
            for (unsigned i = 0; i < 16; ++i) {
                const unsigned index = (trial + i) & 255;
                const unsigned byte = index < 16 * tables
                    ? (in.fpu[index / 4] >> (8 * (index % 4))) & 255
                    : (in.fpu[16 + i / 4] >> (8 * (i % 4))) & 255;
                out.fpu[24 + i / 4] |= byte << (8 * (i % 4));
            }
            cases.emplace_back(in, out);
        }
        suite.add("portable_table128_" + std::to_string(tables), block, cases);
    }
    for (const auto op : {Opcode::VectorBroadcastLower8, Opcode::VectorBroadcastLower16, Opcode::VectorBroadcastLower32}) {
        const unsigned bits = op == Opcode::VectorBroadcastLower8 ? 8 : op == Opcode::VectorBroadcastLower16 ? 16 : 32;
        auto block = blank();
        auto input = reg(block, Reg::R0);
        if (bits < 32) input = append(block, bits == 8 ? Opcode::LeastSignificantByte : Opcode::LeastSignificantHalf, {input});
        append(block, Opcode::A32SetVector, {Value{A32::ExtReg::Q0}, append(block, op, {input})});
        std::vector<Case> cases;
        for (unsigned trial = 0; trial < 128; ++trial) {
            auto in = initial(); in.regs[0] = rng();
            std::fill_n(in.fpu, 4, 0xffffffff);
            auto out = next(in);
            uint32_t word = 0;
            for (unsigned shift = 0; shift < 32; shift += bits)
                word |= (in.regs[0] & uint32_t((uint64_t(1) << bits) - 1)) << shift;
            out.fpu[0] = out.fpu[1] = word; out.fpu[2] = out.fpu[3] = 0;
            cases.emplace_back(in, out);
        }
        suite.add("portable_" + IR::GetNameOf(op), block, cases);
    }
    const std::array<Opcode, 8> shifts{Opcode::LogicalShiftLeft64, Opcode::LogicalShiftRight64,
        Opcode::ArithmeticShiftRight64, Opcode::RotateRight64, Opcode::LogicalShiftLeftMasked64,
        Opcode::LogicalShiftRightMasked64, Opcode::ArithmeticShiftRightMasked64, Opcode::RotateRightMasked64};
    for (unsigned kind = 0; kind < shifts.size(); ++kind) {
        auto block = blank();
        const auto a = append(block, Opcode::Pack2x32To1x64, {reg(block, Reg::R0), reg(block, Reg::R1)});
        const auto amount = kind < 4 ? append(block, Opcode::LeastSignificantByte, {reg(block, Reg::R2)})
            : append(block, Opcode::Pack2x32To1x64, {reg(block, Reg::R2), reg(block, Reg::R3)});
        const auto result = append(block, shifts[kind], {a, amount});
        set(block, Reg::R4, append(block, Opcode::LeastSignificantWord, {result}));
        set(block, Reg::R5, append(block, Opcode::MostSignificantWord, {result}));
        std::vector<Case> cases;
        for (unsigned count = 0; count < 260; ++count) {
            auto in = initial(); const uint64_t x = rng();
            in.regs[0] = x; in.regs[1] = x >> 32; in.regs[2] = count; in.regs[3] = rng();
            unsigned shift = kind < 4 ? uint8_t(count) : count % 64;
            uint64_t result = 0;
            if (kind % 4 == 3) result = std::rotr(x, int(shift));
            else if (kind % 4 == 2) result = uint64_t(int64_t(x) >> std::min(shift, 63u));
            else if (shift < 64) result = kind % 4 == 0 ? x << shift : x >> shift;
            auto out = next(in); out.regs[4] = result; out.regs[5] = result >> 32;
            cases.emplace_back(in, out);
        }
        suite.add("portable_" + IR::GetNameOf(shifts[kind]), block, cases);
    }
    for (const auto op : {Opcode::A32SetCpsr, Opcode::A32SetCpsrNZCVQ, Opcode::A32SetGEFlagsCompressed}) {
        auto block = blank();
        append(block, op, {reg(block, Reg::R0)});
        set(block, Reg::R1, append(block, Opcode::A32GetCpsr, {}));
        std::vector<Case> cases;
        for (unsigned trial = 0; trial < 256; ++trial) {
            auto in = initial(); in.regs[0] = trial < 2 ? (trial ? 0xffffffff : 0) : rng();
            auto out = next(in);
            const uint32_t mask = op == Opcode::A32SetCpsr ? 0xffffffff : op == Opcode::A32SetCpsrNZCVQ ? 0xf8000000 : 0x000f0000;
            out.cpsr = (in.cpsr & ~mask) | (in.regs[0] & mask & (op == Opcode::A32SetCpsr ? 0xf90f03ff : 0xffffffff));
            out.regs[1] = out.cpsr;
            // The static LinkBlock terminal publishes its destination's T/E/IT mode.
            out.cpsr &= ~A32::LocationDescriptor::CPSR_MODE_MASK;
            out.cpsr |= loc().CPSR().Value() & A32::LocationDescriptor::CPSR_MODE_MASK;
            cases.emplace_back(in, out);
        }
        suite.add("portable_" + IR::GetNameOf(op), block, cases);
    }
}
