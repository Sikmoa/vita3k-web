// Included by wasmjit_backend_test.cpp after its shared CHECK/put helpers.
// Independent IR fixtures collect all missing family routes before failing;
// guest tests below then exercise the real ARM/Thumb translator and dispatcher.
namespace vector_tests {
namespace A32 = Dynarmic::A32;
namespace IR = Dynarmic::IR;
using Op = IR::Opcode;
using Value = IR::Value;
using Reason = vita3k::wasmjit::ExitReason;

IR::Block blank(uint32_t pc = code) {
    const A32::LocationDescriptor here{pc, A32::PSR{0x10}, A32::FPSCR{0}};
    const A32::LocationDescriptor next{pc + 4, A32::PSR{0x10}, A32::FPSCR{0}};
    IR::Block block{here};
    block.SetEndLocation(next);
    block.SetTerminal(IR::Term::LinkBlock{next});
    block.CycleCount() = 1;
    return block;
}
Value append(IR::Block &block, Op op, std::initializer_list<Value> args = {}) {
    block.AppendNewInst(op, args);
    return Value{&block.back()};
}
Value read_q(IR::Block &block, unsigned q = 0) {
    return append(block, Op::A32GetVector, {Value{A32::ExtReg::Q0 + q}});
}
Value read_d(IR::Block &block, unsigned d = 0) {
    return append(block, Op::A32GetExtendedRegister64, {Value{A32::ExtReg::D0 + d}});
}
void write_q(IR::Block &block, Value value, unsigned q = 0) {
    append(block, Op::A32SetVector, {Value{A32::ExtReg::Q0 + q}, value});
}
Value scalar_input(IR::Block &block, unsigned bits) {
    if (bits == 64) return read_d(block, 2);
    const auto r1 = append(block, Op::A32GetRegister, {Value{A32::Reg::R1}});
    if (bits == 8) return append(block, Op::LeastSignificantByte, {r1});
    if (bits == 16) return append(block, Op::LeastSignificantHalf, {r1});
    return r1;
}

JitState initial(unsigned pattern) {
    JitState state{};
    uint32_t random = 0x13579bdf;
    for (auto &word : state.fpu) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        word = pattern == 0 ? 0 : pattern == 1 ? UINT32_MAX : random;
    }
    for (unsigned r = 0; r < 15; ++r) state.regs[r] = 0xabc00000 + r;
    state.regs[1] = pattern == 0 ? 0 : pattern == 1 ? UINT32_MAX : 0xa58173df;
    state.regs[15] = code;
    state.cpsr = pattern == 0 ? 0x10 : 0xf80f0010; // NZCV, Q and GE must survive.
    state.fpscr = pattern == 0 ? 0 : 0x08009f9f; // Check both clear and set QC/sticky/trap bits.
    return state;
}
// Byte-based reference, independent of the emitter's i32-word operations.
uint64_t lane(const JitState &state, unsigned bits, unsigned index, unsigned q = 0) {
    std::array<uint8_t, 16> bytes{};
    std::memcpy(bytes.data(), state.fpu + q * 4, bytes.size());
    uint64_t result = 0;
    for (unsigned i = 0; i < bits / 8; ++i)
        result |= uint64_t(bytes[index * (bits / 8) + i]) << (i * 8);
    return result;
}
void set_lane(JitState &state, unsigned bits, unsigned index, uint64_t value, unsigned q = 0) {
    std::array<uint8_t, 16> bytes{};
    std::memcpy(bytes.data(), state.fpu + q * 4, bytes.size());
    for (unsigned i = 0; i < bits / 8; ++i)
        bytes[index * (bits / 8) + i] = uint8_t(value >> (i * 8));
    std::memcpy(state.fpu + q * 4, bytes.data(), bytes.size());
}
uint64_t scalar_input(const JitState &state, unsigned bits) {
    return bits == 64 ? lane(state, 64, 0, 1) : state.regs[1];
}

std::vector<uint8_t> emit(const IR::Block &block, bool region) {
    if (!region) return vita3k::wasmjit::emit_block(block);
    const A32::LocationDescriptor at(block.Location());
    return vita3k::wasmjit::emit_region({&block}, {{at.PC(), PSR_DISPATCH_MASK,
        at.CPSR().Value() & PSR_DISPATCH_MASK, 1}});
}
void check_architecture(const JitState &state, const JitState &expected, unsigned ticks = 1) {
    for (unsigned i = 0; i < 64; ++i) CHECK(state.fpu[i] == expected.fpu[i]);
    for (unsigned i = 0; i < 15; ++i) CHECK(state.regs[i] == expected.regs[i]);
    CHECK(state.cpsr == expected.cpsr && state.fpscr == expected.fpscr);
    CHECK(state.regs[15] == code + ticks * 4 && state.executed == ticks);
}

struct Harness {
    std::vector<Op> missing;
    unsigned cases = 0;
    template <typename Build, typename Expect>
    void test(Op operation, Build build, Expect expect) {
        ++cases;
        auto block = blank();
        build(block);
        for (bool region : {false, true}) {
            const auto bytes = emit(block, region);
            if (bytes.empty()) {
                if (std::find(missing.begin(), missing.end(), operation) == missing.end()) {
                    missing.push_back(operation);
                    std::fprintf(stderr, "Vector family missing: %s\n", IR::GetNameOf(operation).c_str());
                }
                continue; // No guest execution through rejected instructions.
            }
            const int slot = region
                ? vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write)
                : vita3k_jit_install(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
            CHECK(slot >= 0);
            for (unsigned pattern = 0; pattern < 3; ++pattern) {
                auto state = initial(pattern), expected = state;
                expect(expected);
                const auto reason = static_cast<Reason>(region
                    ? vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 1)
                    : vita3k_jit_call(slot, reinterpret_cast<uintptr_t>(&state)));
                CHECK(reason == (region ? Reason::Miss : Reason::Continue));
                check_architecture(state, expected);
            }
            if (region) vita3k_jit_release_region(slot);
            else vita3k_jit_release(slot);
        }
    }
};

void ir_data_movement() {
    Harness h;
    const std::array<unsigned, 4> widths{8, 16, 32, 64};
    const std::array<Op, 4> broadcasts{Op::VectorBroadcast8, Op::VectorBroadcast16,
        Op::VectorBroadcast32, Op::VectorBroadcast64};
    const std::array<Op, 4> duplicates{Op::VectorBroadcastElement8, Op::VectorBroadcastElement16,
        Op::VectorBroadcastElement32, Op::VectorBroadcastElement64};
    const std::array<Op, 4> gets{Op::VectorGetElement8, Op::VectorGetElement16,
        Op::VectorGetElement32, Op::VectorGetElement64};
    const std::array<Op, 4> sets{Op::VectorSetElement8, Op::VectorSetElement16,
        Op::VectorSetElement32, Op::VectorSetElement64};
    for (unsigned w = 0; w < widths.size(); ++w) {
        const unsigned bits = widths[w];
        h.test(broadcasts[w], [&](auto &b) {
            write_q(b, append(b, broadcasts[w], {scalar_input(b, bits)}));
        }, [&](auto &s) {
            const auto value = scalar_input(s, bits);
            for (unsigned i = 0; i < 128 / bits; ++i) set_lane(s, bits, i, value);
        });
        // Constants must preserve the second word of 64-bit values too.
        h.test(broadcasts[w], [&](auto &b) {
            const uint64_t value = 0xfedcba98765432e1ull;
            const auto immediate = bits == 64 ? Value{value} : bits == 32 ? Value{uint32_t(value)}
                : bits == 16 ? Value{uint16_t(value)} : Value{uint8_t(value)};
            write_q(b, append(b, broadcasts[w], {immediate}));
        }, [&](auto &s) {
            for (unsigned i = 0; i < 128 / bits; ++i) set_lane(s, bits, i, 0xfedcba98765432e1ull);
        });
        for (unsigned index = 0; index < 128 / bits; ++index) {
            h.test(duplicates[w], [&](auto &b) {
                write_q(b, append(b, duplicates[w], {read_q(b), Value{uint8_t(index)}}));
            }, [&](auto &s) {
                const auto value = lane(s, bits, index);
                for (unsigned i = 0; i < 128 / bits; ++i) set_lane(s, bits, i, value);
            });
            h.test(gets[w], [&](auto &b) {
                auto value = append(b, gets[w], {read_q(b), Value{uint8_t(index)}});
                if (bits == 64) append(b, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D0}, value});
                else {
                    if (bits == 8) value = append(b, Op::ZeroExtendByteToWord, {value});
                    if (bits == 16) value = append(b, Op::ZeroExtendHalfToWord, {value});
                    append(b, Op::A32SetRegister, {Value{A32::Reg::R0}, value});
                }
            }, [&](auto &s) {
                const auto value = lane(s, bits, index);
                if (bits == 64) set_lane(s, bits, 0, value);
                else s.regs[0] = uint32_t(value);
            });
            h.test(sets[w], [&](auto &b) {
                write_q(b, append(b, sets[w], {read_q(b), Value{uint8_t(index)}, scalar_input(b, bits)}));
            }, [&](auto &s) { set_lane(s, bits, index, scalar_input(s, bits)); });
        }
    }
    // D-form modified-immediate Boolean operations use U64 scalar IR.
    for (const auto op : {Op::And64, Op::AndNot64, Op::Or64, Op::Eor64, Op::Not64}) {
        h.test(op, [&](auto &b) {
            const auto a = read_d(b), other = read_d(b, 3);
            const auto value = op == Op::Not64 ? append(b, op, {a}) : append(b, op, {a, other});
            append(b, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D0}, value});
        }, [&](auto &s) {
            const auto a = lane(s, 64, 0), b = lane(s, 64, 1, 1);
            set_lane(s, 64, 0, op == Op::And64 ? a & b : op == Op::AndNot64 ? a & ~b
                : op == Op::Or64 ? a | b : op == Op::Eor64 ? a ^ b : ~a);
        });
    }
    for (const auto op : {Op::VectorAnd, Op::VectorAndNot, Op::VectorOr, Op::VectorEor, Op::VectorNot}) {
        h.test(op, [&](auto &b) {
            const auto a = read_q(b), other = read_q(b, 1);
            write_q(b, op == Op::VectorNot ? append(b, op, {a}) : append(b, op, {a, other}));
        }, [&](auto &s) {
            for (unsigned i = 0; i < 4; ++i) {
                const auto a = s.fpu[i], b = s.fpu[i + 4];
                s.fpu[i] = op == Op::VectorAnd ? a & b : op == Op::VectorAndNot ? a & ~b
                    : op == Op::VectorOr ? a | b : op == Op::VectorEor ? a ^ b : ~a;
            }
        });
    }
    for (const auto op : {Op::ZeroVector, Op::VectorZeroUpper, Op::ZeroExtendLongToQuad,
             Op::Pack2x64To1x128, Op::Identity}) {
        h.test(op, [&](auto &b) {
            Value result;
            if (op == Op::ZeroVector) result = append(b, op);
            else if (op == Op::Pack2x64To1x128) result = append(b, op, {read_d(b), read_d(b, 3)});
            else if (op == Op::ZeroExtendLongToQuad) result = append(b, op, {read_d(b)});
            else result = append(b, op, {read_q(b)});
            write_q(b, result);
        }, [&](auto &s) {
            if (op == Op::ZeroVector) std::fill_n(s.fpu, 4, 0);
            else if (op == Op::VectorZeroUpper || op == Op::ZeroExtendLongToQuad) s.fpu[2] = s.fpu[3] = 0;
            else if (op == Op::Pack2x64To1x128) { s.fpu[2] = s.fpu[6]; s.fpu[3] = s.fpu[7]; }
        });
    }
    h.test(Op::Identity, [&](auto &b) {
        append(b, Op::A32SetExtendedRegister64, {Value{A32::ExtReg::D0},
            append(b, Op::Identity, {read_d(b, 3)})});
    }, [&](auto &s) { set_lane(s, 64, 0, lane(s, 64, 1, 1)); });
    for (const auto op : {Op::ZeroExtendLongToQuad, Op::Pack2x64To1x128}) {
        h.test(op, [&](auto &b) {
            const Value low{uint64_t(0x0123456789abcdef)}, high{uint64_t(0xfedcba9876543210)};
            write_q(b, op == Op::ZeroExtendLongToQuad ? append(b, op, {low}) : append(b, op, {low, high}));
        }, [&](auto &s) {
            set_lane(s, 64, 0, 0x0123456789abcdef);
            set_lane(s, 64, 1, op == Op::ZeroExtendLongToQuad ? 0 : 0xfedcba9876543210);
        });
    }
    for (bool lower : {false, true}) {
        const auto op = lower ? Op::VectorExtractLower : Op::VectorExtract;
        const unsigned bytes = lower ? 8 : 16;
        for (unsigned position = 0; position <= bytes; ++position) {
            h.test(op, [&](auto &b) {
                write_q(b, append(b, op, {read_q(b), read_q(b, 1), Value{uint8_t(position * 8)}}));
            }, [&](auto &s) {
                std::array<uint8_t, 32> joined{};
                std::memcpy(joined.data(), s.fpu, bytes);
                std::memcpy(joined.data() + bytes, s.fpu + 4, bytes);
                std::fill_n(s.fpu, 4, 0);
                std::memcpy(s.fpu, joined.data() + position, bytes);
            });
        }
    }
    std::fprintf(stderr, "Vector family: %u independent IR fixtures; %zu missing routes\n", h.cases, h.missing.size());
    CHECK(h.missing.empty());

    // Malformed lane/bit positions must reject the entire module, never
    // silently wrap WebAssembly's modulo-32 shift or corrupt a neighbour.
    for (unsigned w = 0; w < widths.size(); ++w) for (const auto op : {gets[w], sets[w], duplicates[w]}) {
        for (uint8_t index : {uint8_t(128 / widths[w]), uint8_t(255)}) {
            auto b = blank();
            const auto a = read_q(b);
            if (op == sets[w]) write_q(b, append(b, op, {a, Value{index}, scalar_input(b, widths[w])}));
            else append(b, op, {a, Value{index}});
            CHECK(emit(b, false).empty() && emit(b, true).empty());
        }
        auto b = blank();
        const auto a = read_q(b), dynamic = append(b, Op::LeastSignificantByte,
            {append(b, Op::A32GetRegister, {Value{A32::Reg::R1}})});
        if (op == sets[w]) write_q(b, append(b, op, {a, dynamic, scalar_input(b, widths[w])}));
        else append(b, op, {a, dynamic});
        CHECK(emit(b, false).empty() && emit(b, true).empty());
    }
    for (const auto op : {Op::VectorExtract, Op::VectorExtractLower}) {
        for (uint8_t position : {uint8_t(1), uint8_t(op == Op::VectorExtract ? 136 : 72), uint8_t(255)}) {
            auto b = blank();
            append(b, op, {read_q(b), read_q(b, 1), Value{position}});
            CHECK(emit(b, false).empty() && emit(b, true).empty());
        }
        auto b = blank();
        const auto dynamic = append(b, Op::LeastSignificantByte,
            {append(b, Op::A32GetRegister, {Value{A32::Reg::R1}})});
        append(b, op, {read_q(b), read_q(b, 1), dynamic});
        CHECK(emit(b, false).empty() && emit(b, true).empty());
    }
}

void region_slot_reuse() {
    // Each member's second SSA slot aliases the previous member's output.
    // A wide producer poisons it, then narrower producers must clear rather
    // than expose old high words. Exercise all flag/accounting representations.
    std::vector<IR::Block> blocks;
    for (unsigned i = 0; i < 5; ++i) {
        blocks.push_back(blank(code + i * 4));
        auto &b = blocks.back();
        const auto source = read_q(b);
        Value result;
        switch (i) {
        case 0: result = append(b, Op::VectorBroadcast64, {Value{uint64_t(0x88776655deadbeef)}}); break;
        case 1: result = append(b, Op::VectorZeroUpper, {source}); break;
        case 2: result = append(b, Op::ZeroExtendLongToQuad, {Value{uint64_t(0x0102030405060708)}}); break;
        case 3: result = append(b, Op::ZeroVector); break;
        case 4: result = append(b, Op::VectorExtractLower, {source, source, Value{uint8_t(56)}}); break;
        }
        write_q(b, result, i + 1);
    }
    std::vector<const IR::Block *> members;
    std::vector<vita3k::wasmjit::RegionBlockMeta> meta;
    for (const auto &b : blocks) {
        members.push_back(&b);
        const A32::LocationDescriptor at(b.Location());
        meta.push_back({at.PC(), PSR_DISPATCH_MASK, at.CPSR().Value() & PSR_DISPATCH_MASK, 1});
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        const auto bytes = vita3k::wasmjit::emit_region(members, meta,
            {(mode & 1) != 0, (mode & 2) != 0, false});
        CHECK(!bytes.empty());
        const int slot = vita3k_jit_install_region(bytes.data(), bytes.size(), checked_memory_read, checked_memory_write);
        CHECK(slot >= 0);
        auto state = initial(2), expected = state;
        set_lane(expected, 64, 0, 0x88776655deadbeef, 1);
        set_lane(expected, 64, 1, 0x88776655deadbeef, 1);
        set_lane(expected, 64, 0, lane(state, 64, 0), 2); set_lane(expected, 64, 1, 0, 2);
        set_lane(expected, 64, 0, 0x0102030405060708, 3); set_lane(expected, 64, 1, 0, 3);
        set_lane(expected, 64, 0, 0, 4); set_lane(expected, 64, 1, 0, 4);
        for (unsigned i = 0; i < 8; ++i) set_lane(expected, 8, i, lane(state, 8, (i + 7) % 8), 5);
        set_lane(expected, 64, 1, 0, 5);
        CHECK(static_cast<Reason>(vita3k_jit_run(slot, reinterpret_cast<uintptr_t>(&state), 5)) == Reason::Miss);
        check_architecture(state, expected, 5);
        CHECK(state.next_pc == code + 20);
        vita3k_jit_release_region(slot);
    }
}

uint32_t thumb_word(uint32_t arm) {
    // Unconditional Advanced SIMD F2/F3 space maps to EF/FF in Thumb-2.
    // VFP core transfers use the same EE halfwords in both encodings.
    if ((arm & 0xfe000000) == 0xf2000000)
        arm = (arm & 0x00ffffff) | ((arm & 0x01000000) ? 0xff000000 : 0xef000000);
    else if ((arm & 0xff000000) == 0xf4000000)
        arm = (arm & 0x00ffffff) | 0xf9000000; // structure loads/stores
    return (arm << 16) | (arm >> 16);
}

template <typename Expect>
void guest_case(MemState &mem, WasmJitCPU &jit, CPUState &parent, bool thumb,
    uint32_t opcode, Expect expect) {
    if (thumb) put_thumb(mem, jit, {thumb_word(opcode), 0xbf00df00});
    else put(mem, jit, {opcode, 0xef000000});
    auto s = initial(2);
    auto context = jit.save_context();
    std::copy_n(s.regs, 15, context.cpu_registers.begin());
    std::memcpy(context.fpu_registers.data(), s.fpu, sizeof(s.fpu));
    context.cpsr = s.cpsr | (thumb ? 0x20 : 0);
    context.fpscr = s.fpscr;
    jit.load_context(context);
    expect(s);
    if (jit.run() != 0) {
        std::fprintf(stderr, "NEON guest failed: %08x (%s): %s\n", opcode,
            thumb ? "Thumb" : "ARM", jit.get_last_error().c_str());
        CHECK(false);
    }
    CHECK(parent.svc_called);
    const auto result = jit.save_context();
    for (unsigned i = 0; i < 15; ++i) CHECK(result.cpu_registers[i] == s.regs[i]);
    CHECK(std::memcmp(result.fpu_registers.data(), s.fpu, sizeof(s.fpu)) == 0);
    CHECK(result.cpsr == context.cpsr && result.fpscr == context.fpscr);
    CHECK(jit.get_pc() == code + (thumb ? 6 : 8));
}

void guest_data_movement(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    // Representative encoding checks, assembled with VitaSDK armv7-a/neon.
    CHECK(thumb_word(0xeee01b10) == 0x1b10eee0); // vdup.8 q0,r1
    CHECK(thumb_word(0xf3bf0c41) == 0x0c41ffbf); // vdup.8 q0,d1[7]
    CHECK(thumb_word(0xf2b00f42) == 0x0f42efb0); // vext.8 q0,q0,q1,#15
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (bool thumb : {false, true}) {
            if (!thumb) {
                // VDUPNE must not write any lane when Z=1.
                guest_case(mem, jit, parent, false, 0x1ee01b10, [](auto &) {});
            }
            for (unsigned bits : {8u, 16u, 32u}) {
                for (bool quad : {false, true}) for (unsigned d : {0u, 16u, 30u}) {
                    const unsigned reg = quad ? d : d + 1; // odd D/high Q aliases
                    const uint32_t opcode = 0xee801b10 | (quad ? 1u << 21 : 0)
                        | (bits == 8 ? 1u << 22 : bits == 16 ? 1u << 5 : 0)
                        | ((reg & 15) << 16) | ((reg >> 4) << 7);
                    guest_case(mem, jit, parent, thumb, opcode, [&](auto &s) {
                        const auto value = s.regs[1];
                        for (unsigned i = 0; i < (quad ? 128u : 64u) / bits; ++i)
                            set_lane(s, bits, i + (quad ? 0 : (reg & 1) * 64 / bits), value, reg / 2);
                    });
                }
                // Every scalar lane; D31 checks the last architectural word.
                for (unsigned index = 0; index < 64 / bits; ++index) {
                    const unsigned shift = bits == 8 ? 2 : bits == 16 ? 1 : 0;
                    const uint32_t lane_bits = ((index >> shift) << 21)
                        | ((index & ((1u << shift) - 1)) << (bits == 8 ? 5 : 6));
                    const uint32_t insert = 0xee0f1b90 | lane_bits | (bits == 8 ? 1u << 22 : bits == 16 ? 1u << 5 : 0);
                    guest_case(mem, jit, parent, thumb, insert, [&](auto &s) {
                        set_lane(s, bits, 64 / bits + index, s.regs[1], 15);
                    });
                    for (bool sign : {false, true}) {
                        if (bits == 32 && sign) continue;
                        const uint32_t extract = insert | (1u << 20) | (bits != 32 && !sign ? 1u << 23 : 0);
                        guest_case(mem, jit, parent, thumb, extract, [&](auto &s) {
                            uint32_t value = uint32_t(lane(s, bits, 64 / bits + index, 15));
                            if (sign && (value & (1u << (bits - 1)))) value |= UINT32_MAX << bits;
                            s.regs[1] = value;
                        });
                    }
                    for (bool quad : {false, true}) {
                        const unsigned imm4 = (index * 2 + 1) * (bits / 8);
                        // q15/d31 duplicate from d31: destination overlaps source.
                        const uint32_t dup = 0xf3f00c2f | (imm4 << 16) | ((quad ? 14u : 15u) << 12) | (quad ? 1u << 6 : 0);
                        guest_case(mem, jit, parent, thumb, dup, [&](auto &s) {
                            const auto value = lane(s, bits, 64 / bits + index, 15);
                            for (unsigned i = 0; i < (quad ? 128u : 64u) / bits; ++i)
                                set_lane(s, bits, i + (quad ? 0 : 64 / bits), value, 15);
                        });
                    }
                }
            }
            for (const auto &[opcode, value] : std::array<std::pair<uint32_t, uint64_t>, 5>{{
                     {0xf3800e51, 0x8181818181818181ull}, // vmov.i8
                     {0xf3800a51, 0x8100810081008100ull}, // vmov.i16
                     {0xf387047f, 0xff00ffffff00ffffull}, // vmvn.i32
                     {0xf3820e73, 0xff00ff000000ffffull}, // vmov.i64, unequal words
                     {0xf3800670, 0x7fffffff7fffffffull}}}) {
                guest_case(mem, jit, parent, thumb, opcode, [&](auto &s) {
                    set_lane(s, 64, 0, value); set_lane(s, 64, 1, value);
                });
            }
            for (bool bic : {false, true}) {
                guest_case(mem, jit, parent, thumb, bic ? 0xf3800770 : 0xf3800750, [&](auto &s) {
                    for (unsigned i = 0; i < 4; ++i)
                        s.fpu[i] = bic ? s.fpu[i] & 0x7fffffff : s.fpu[i] | 0x80000000;
                });
                guest_case(mem, jit, parent, thumb, bic ? 0xf3c0fb31 : 0xf3c0fb11, [&](auto &s) {
                    for (unsigned i = 62; i < 64; ++i)
                        s.fpu[i] = bic ? s.fpu[i] & ~0x81008100u : s.fpu[i] | 0x81008100;
                });
            }
            guest_case(mem, jit, parent, thumb, 0xf3c2fe33, [](auto &s) {
                set_lane(s, 64, 1, 0xff00ff000000ffff, 15); // D31 immediate, preserve D30
            });
            guest_case(mem, jit, parent, thumb, 0xf3f0f5ae, [](auto &s) {
                set_lane(s, 64, 1, ~lane(s, 64, 0, 15), 15); // vmvn d31,d30
            });
            for (unsigned operation = 0; operation < 11; ++operation) {
                const std::array<uint32_t, 11> opcodes{0xf2220152, 0xf3b005c2,
                    0xf2000152, 0xf2100152, 0xf2200152, 0xf2300152, 0xf3000152,
                    0xf3120154, 0xf3220154, 0xf3320154, 0xf3b20042};
                guest_case(mem, jit, parent, thumb, opcodes[operation], [&](auto &s) {
                    for (unsigned i = 0; i < 4; ++i) {
                        const auto a = s.fpu[i], b = s.fpu[i + 4], c = s.fpu[i + 8];
                        switch (operation) {
                        case 0: s.fpu[i] = b; break; // vmov (vorr alias)
                        case 1: s.fpu[i] = ~b; break;
                        case 2: s.fpu[i] = a & b; break;
                        case 3: s.fpu[i] = a & ~b; break;
                        case 4: s.fpu[i] = a | b; break;
                        case 5: s.fpu[i] = a | ~b; break;
                        case 6: s.fpu[i] = a ^ b; break;
                        case 7: s.fpu[i] = (b & a) | (c & ~a); break; // vbsl
                        case 8: s.fpu[i] = (b & c) | (a & ~c); break; // vbit
                        case 9: s.fpu[i] = (a & c) | (b & ~c); break; // vbif
                        case 10: s.fpu[i] = b; s.fpu[i + 4] = a; break; // vswp
                        }
                    }
                });
            }
            for (bool quad : {false, true}) for (unsigned offset = 0; offset < (quad ? 16u : 8u); ++offset) {
                const uint32_t opcode = (quad ? 0xf2b00042 : 0xf2fff0ae) | (offset << 8);
                guest_case(mem, jit, parent, thumb, opcode, [&](auto &s) {
                    std::array<uint8_t, 32> joined{};
                    const unsigned bytes = quad ? 16 : 8, dst = quad ? 0 : 62;
                    std::memcpy(joined.data(), s.fpu + dst, bytes);
                    std::memcpy(joined.data() + bytes, s.fpu + (quad ? 4 : 60), bytes);
                    std::memcpy(s.fpu + dst, joined.data() + offset, bytes);
                });
            }
        }
    }
}
void guest_structure_lanes(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    CHECK(thumb_word(0xf4e0f0ef) == 0xf0eff9e0);
    const std::array<uint32_t, 3> loads{0xf4e0f0ef, 0xf4e0f4cf, 0xf4e0f88f};
    const std::array<uint32_t, 3> stores{0xf4c2f0ef, 0xf4c2f4cf, 0xf4c2f88f};
    const std::array<uint32_t, 3> replicates{0xf4e0fc0f, 0xf4e0fc4f, 0xf4e0fc8f};
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (bool thumb : {false, true}) for (unsigned width = 0; width < 3; ++width) {
            const unsigned bytes = 1u << width, bits = bytes * 8;
            for (bool replicate : {false, true}) {
                const auto load = replicate ? replicates[width] : loads[width];
                if (thumb) put_thumb(mem, jit, {thumb_word(load), thumb_word(stores[width]), 0xbf00df00});
                else put(mem, jit, {load, stores[width], 0xef000000});
                auto state = initial(2);
                state.regs[0] = data + page - 1; // deliberately crosses a page
                state.regs[2] = data + page + 35;
                auto context = jit.save_context();
                std::copy_n(state.regs, 15, context.cpu_registers.begin());
                std::memcpy(context.fpu_registers.data(), state.fpu, sizeof(state.fpu));
                context.cpsr = state.cpsr | (thumb ? 0x20 : 0);
                context.fpscr = state.fpscr;
                jit.load_context(context);
                const uint32_t input = 0xa58173df;
                std::array<uint8_t, 8> output{}, expected;
                expected.fill(0x55);
                CHECK(mem_write(mem, state.regs[0], &input, sizeof(input)));
                CHECK(mem_write(mem, state.regs[2] - 1, expected.data(), expected.size()));
                for (unsigned i = replicate ? 64 / bits : 128 / bits - 1; i < 128 / bits; ++i)
                    set_lane(state, bits, i, input, 15);
                std::memcpy(expected.data() + 1, &input, bytes);
                CHECK(jit.run() == 0 && parent.svc_called);
                const auto result = jit.save_context();
                CHECK(std::memcmp(result.fpu_registers.data(), state.fpu, sizeof(state.fpu)) == 0);
                for (unsigned i = 0; i < 15; ++i) CHECK(result.cpu_registers[i] == state.regs[i]);
                CHECK(result.cpsr == context.cpsr && result.fpscr == context.fpscr);
                CHECK(jit.get_pc() == code + (thumb ? 10 : 12));
                CHECK(mem_read(mem, state.regs[2] - 1, output.data(), output.size()));
                CHECK(output == expected);
            }
        }
    }
}
} // namespace vector_tests
