// Lane-rearranging, widening and pairwise NEON IR (VPADD, VPADDL, VZIP,
// VUZP, VPMIN/VPMAX, VMOVL, VHADD, VSHL register, VMULL/VMLAL, VMUL.P8,
// VQMOVN.U, VQ(R)SHRN.U64).
// References are byte-lane models of Dynarmic's IR definitions
// (backend/x64/emit_x64_vector.cpp and ir/opt/polyfill_pass.cpp),
// independent of the emitter's i32-word lowering. Guest cases run real
// ARM/Thumb encodings through Dynarmic's translator and the production JIT.
namespace vector_lane_tests {
using namespace vector_tests;

uint64_t truncate(uint64_t value, unsigned bits) {
    return bits == 64 ? value : value & ((uint64_t(1) << bits) - 1);
}
int64_t sext(uint64_t value, unsigned bits) {
    return bits == 64 ? int64_t(value) : int64_t(value << (64 - bits)) >> (64 - bits);
}
uint64_t paired_add_lower(const JitState &s, unsigned bits, unsigned i) {
    const unsigned per = 64 / bits;
    const auto x = [&](unsigned k) { return k < per ? lane(s, bits, k, 0) : lane(s, bits, k - per, 1); };
    return i < per ? x(2 * i) + x(2 * i + 1) : 0;
}
uint64_t interleave(const JitState &s, unsigned bits, bool upper, unsigned j) {
    return lane(s, bits, (upper ? 64 / bits : 0) + j / 2, j % 2);
}
uint64_t deinterleave(const JitState &s, unsigned bits, bool odd, bool lower, unsigned i) {
    const unsigned per = (lower ? 32 : 64) / bits;
    if (i >= 2 * per) return 0;
    return lane(s, bits, 2 * (i % per) + (odd ? 1 : 0), i / per);
}
uint64_t paired_add_widen(const JitState &s, unsigned bits, bool is_signed, unsigned i) {
    const auto a = lane(s, bits, 2 * i), b = lane(s, bits, 2 * i + 1);
    return is_signed ? uint64_t(sext(a, bits) + sext(b, bits)) : a + b;
}
uint64_t halving_add(uint64_t a, uint64_t b, unsigned bits, bool is_signed) {
    return is_signed ? uint64_t((sext(a, bits) + sext(b, bits)) >> 1) : (a + b) >> 1;
}
uint64_t logical_vshift(uint64_t a, uint64_t b, unsigned bits) {
    const int amount = int8_t(uint8_t(b));
    if (amount >= int(bits) || amount <= -int(bits)) return 0;
    return amount >= 0 ? a << amount : a >> -amount;
}
uint64_t multiply_widen(uint64_t a, uint64_t b, unsigned bits, bool is_signed) {
    return is_signed ? uint64_t(sext(a, bits) * sext(b, bits)) : a * b;
}
uint64_t polynomial_multiply(uint64_t a, uint64_t b) {
    uint64_t result = 0;
    for (unsigned k = 0; k < 8; ++k)
        if ((b >> k) & 1) result ^= a << k;
    return result;
}

void ir_lanes() {
    Harness h;
    const auto binary = [&](Op operation, unsigned out_bits, auto reference) {
        h.test(operation, [&](auto &b) {
            write_q(b, append(b, operation, {read_q(b), read_q(b, 1)}));
        }, [&](auto &s) {
            const auto before = s;
            for (unsigned i = 0; i < 128 / out_bits; ++i)
                set_lane(s, out_bits, i, truncate(reference(before, i), out_bits));
        });
    };
    const std::array<unsigned, 3> sizes{8, 16, 32};
    const Op paired[] = {Op::VectorPairedAddLower8, Op::VectorPairedAddLower16, Op::VectorPairedAddLower32};
    const Op even[] = {Op::VectorDeinterleaveEvenLower8, Op::VectorDeinterleaveEvenLower16, Op::VectorDeinterleaveEvenLower32};
    const Op odd[] = {Op::VectorDeinterleaveOddLower8, Op::VectorDeinterleaveOddLower16, Op::VectorDeinterleaveOddLower32};
    const Op even_full[] = {Op::VectorDeinterleaveEven8, Op::VectorDeinterleaveEven16, Op::VectorDeinterleaveEven32};
    const Op odd_full[] = {Op::VectorDeinterleaveOdd8, Op::VectorDeinterleaveOdd16, Op::VectorDeinterleaveOdd32};
    const Op paddl_s[] = {Op::VectorPairedAddSignedWiden8, Op::VectorPairedAddSignedWiden16, Op::VectorPairedAddSignedWiden32};
    const Op paddl_u[] = {Op::VectorPairedAddUnsignedWiden8, Op::VectorPairedAddUnsignedWiden16, Op::VectorPairedAddUnsignedWiden32};
    const Op hadd_s[] = {Op::VectorHalvingAddS8, Op::VectorHalvingAddS16, Op::VectorHalvingAddS32};
    const Op hadd_u[] = {Op::VectorHalvingAddU8, Op::VectorHalvingAddU16, Op::VectorHalvingAddU32};
    const Op widen_s[] = {Op::VectorMultiplySignedWiden8, Op::VectorMultiplySignedWiden16, Op::VectorMultiplySignedWiden32};
    const Op widen_u[] = {Op::VectorMultiplyUnsignedWiden8, Op::VectorMultiplyUnsignedWiden16, Op::VectorMultiplyUnsignedWiden32};
    const Op extend[] = {Op::VectorSignExtend8, Op::VectorSignExtend16, Op::VectorSignExtend32};
    for (unsigned k = 0; k < sizes.size(); ++k) {
        const unsigned bits = sizes[k];
        binary(paired[k], bits, [&](const JitState &s, unsigned i) { return paired_add_lower(s, bits, i); });
        for (bool lower : {false, true}) for (bool is_odd : {false, true}) {
            const Op operation = lower ? (is_odd ? odd[k] : even[k]) : (is_odd ? odd_full[k] : even_full[k]);
            binary(operation, bits, [&](const JitState &s, unsigned i) { return deinterleave(s, bits, is_odd, lower, i); });
        }
        for (bool is_signed : {true, false}) {
            const Op operation = is_signed ? paddl_s[k] : paddl_u[k];
            h.test(operation, [&](auto &b) {
                write_q(b, append(b, operation, {read_q(b)}));
            }, [&](auto &s) {
                const auto before = s;
                for (unsigned i = 0; i < 64 / bits; ++i)
                    set_lane(s, 2 * bits, i, truncate(paired_add_widen(before, bits, is_signed, i), 2 * bits));
            });
        }
        for (bool is_signed : {true, false}) {
            binary(is_signed ? hadd_s[k] : hadd_u[k], bits, [&](const JitState &s, unsigned i) {
                return halving_add(lane(s, bits, i, 0), lane(s, bits, i, 1), bits, is_signed);
            });
            binary(is_signed ? widen_s[k] : widen_u[k], 2 * bits, [&](const JitState &s, unsigned i) {
                return multiply_widen(lane(s, bits, i, 0), lane(s, bits, i, 1), bits, is_signed);
            });
        }
        h.test(extend[k], [&](auto &b) {
            write_q(b, append(b, extend[k], {read_q(b)}));
        }, [&](auto &s) {
            const auto before = s;
            for (unsigned i = 0; i < 64 / bits; ++i)
                set_lane(s, 2 * bits, i, truncate(uint64_t(sext(lane(before, bits, i), bits)), 2 * bits));
        });
    }
    const Op lower[] = {Op::VectorInterleaveLower8, Op::VectorInterleaveLower16, Op::VectorInterleaveLower32, Op::VectorInterleaveLower64};
    const Op upper[] = {Op::VectorInterleaveUpper8, Op::VectorInterleaveUpper16, Op::VectorInterleaveUpper32, Op::VectorInterleaveUpper64};
    const Op vshift[] = {Op::VectorLogicalVShift8, Op::VectorLogicalVShift16, Op::VectorLogicalVShift32, Op::VectorLogicalVShift64};
    for (unsigned k = 0; k < 4; ++k) {
        const unsigned bits = 8u << k;
        binary(lower[k], bits, [&](const JitState &s, unsigned j) { return interleave(s, bits, false, j); });
        binary(upper[k], bits, [&](const JitState &s, unsigned j) { return interleave(s, bits, true, j); });
        binary(vshift[k], bits, [&](const JitState &s, unsigned i) {
            return logical_vshift(lane(s, bits, i, 0), lane(s, bits, i, 1), bits);
        });
        // Every boundary amount (0, +-1, +-(bits-1), +-bits, the byte
        // extremes) against a constant lane pattern, beyond random bytes.
        const std::array<int, 11> amounts{0, 1, -1, int(bits) - 1, 1 - int(bits), int(bits), -int(bits),
            int(bits) + 1, -int(bits) - 1, 127, -128};
        for (unsigned first = 0; first < amounts.size(); first += 128 / bits) {
            std::array<uint64_t, 2> shift{};
            JitState model{};
            for (unsigned i = 0; i < 128 / bits; ++i) {
                const uint64_t amount = uint8_t(amounts[(first + i) % amounts.size()])
                    | (bits > 8 ? uint64_t(0xa5) << 8 : 0); // only the low byte counts
                set_lane(model, bits, i, truncate(amount, bits), 1);
            }
            shift[0] = lane(model, 64, 0, 1);
            shift[1] = lane(model, 64, 1, 1);
            h.test(vshift[k], [&](auto &b) {
                const auto amounts_q = append(b, Op::Pack2x64To1x128, {Value{shift[0]}, Value{shift[1]}});
                write_q(b, append(b, vshift[k], {read_q(b), amounts_q}));
            }, [&](auto &s) {
                const auto before = s;
                for (unsigned i = 0; i < 128 / bits; ++i)
                    set_lane(s, bits, i, truncate(logical_vshift(lane(before, bits, i), lane(model, bits, i, 1), bits), bits));
            });
        }
    }
    // VQMOVN.U: clamp to the narrow range; any clamp sets the sticky QC.
    // Harness pattern 0 starts with FPSCR clear, so constant saturating
    // inputs prove QC is set and non-saturating ones that it is left alone.
    const Op narrow[] = {Op::VectorUnsignedSaturatedNarrow16, Op::VectorUnsignedSaturatedNarrow32,
        Op::VectorUnsignedSaturatedNarrow64};
    for (unsigned k = 0; k < 3; ++k) {
        const unsigned bits = 8u << k; // result lane width
        const uint64_t max = truncate(~uint64_t(0), bits);
        const auto expect_narrow = [&](JitState &s, const JitState &source) {
            bool saturated = false;
            for (unsigned i = 0; i < 128 / bits; ++i) {
                const uint64_t wide = i < 64 / bits ? lane(source, 2 * bits, i) : 0;
                saturated |= wide > max;
                set_lane(s, bits, i, std::min(wide, max));
            }
            if (saturated) s.fpscr |= 0x08000000u;
        };
        h.test(narrow[k], [&](auto &b) {
            write_q(b, append(b, narrow[k], {read_q(b)}));
        }, [&](auto &s) { const auto before = s; expect_narrow(s, before); });
        const std::array<std::array<uint64_t, 2>, 2> constants{{
            {0x0001fffe00ff0100ull, 0x0000ffff00000000ull}, // every width saturates
            {0x00000000000000ffull, 0x0000000000000001ull}}}; // every width fits
        for (const auto &words : constants) {
            h.test(narrow[k], [&](auto &b) {
                write_q(b, append(b, narrow[k], {append(b, Op::Pack2x64To1x128, {Value{words[0]}, Value{words[1]}})}));
            }, [&](auto &s) {
                JitState source{};
                set_lane(source, 64, 0, words[0]);
                set_lane(source, 64, 1, words[1]);
                expect_narrow(s, source);
            });
        }
        h.test(narrow[k], [&](auto &b) {
            write_q(b, append(b, narrow[k], {append(b, Op::Pack2x64To1x128,
                {Value{0x0000000100000000ull}, Value{0ull}})}));
        }, [&](auto &s) {
            JitState source{};
            set_lane(source, 64, 0, 0x0000000100000000ull);
            expect_narrow(s, source);
        });
    }
    binary(Op::VectorEqual64, 64, [](const JitState &s, unsigned i) {
        return lane(s, 64, i, 0) == lane(s, 64, i, 1) ? ~uint64_t(0) : 0;
    });
    h.test(Op::VectorEqual64, [&](auto &b) {
        write_q(b, append(b, Op::VectorEqual64, {read_q(b), read_q(b)}));
    }, [&](auto &s) { set_lane(s, 64, 0, ~uint64_t(0)); set_lane(s, 64, 1, ~uint64_t(0)); });
    binary(Op::VectorPolynomialMultiply8, 8, [](const JitState &s, unsigned i) {
        return polynomial_multiply(lane(s, 8, i, 0), lane(s, 8, i, 1));
    });
    binary(Op::VectorPolynomialMultiplyLong8, 16, [](const JitState &s, unsigned i) {
        return polynomial_multiply(lane(s, 8, i, 0), lane(s, 8, i, 1));
    });
    std::printf("NEON lane/widen/pairwise IR: %u fixtures; %zu missing routes\n", h.cases, h.missing.size());
    CHECK(h.missing.empty());
}

uint32_t neon3(uint32_t base, unsigned d, unsigned n, unsigned m) {
    return base | ((d >> 4) << 22) | ((d & 15) << 12) | ((n >> 4) << 7) | ((n & 15) << 16)
        | ((m >> 4) << 5) | (m & 15);
}
uint64_t dlane(const JitState &s, unsigned d, unsigned bits, unsigned i) {
    return lane(s, bits, (d & 1) * 64 / bits + i, d / 2);
}
void set_dlane(JitState &s, unsigned d, unsigned bits, unsigned i, uint64_t value) {
    set_lane(s, bits, (d & 1) * 64 / bits + i, truncate(value, bits), d / 2);
}

void guest_lanes(MemState &mem) {
    CPUState parent{};
    parent.mem = &mem;
    WasmJitCPU jit(&parent, 0);
    jit.set_instruction_budget(16);
    // Encodings from A32/decoder/asimd.inc; the Limbo instances are
    // vpadd.i32, vshl.u64 (register), vhadd.s8/u16, vpmin.s32, vmovl.s16,
    // vmull.p8/u8, vmlal.s32 and vzip.16 (Q).
    const unsigned d = 20, n = 5, m = 31; // D20 (Q10 low), D5 (Q2 high), D31 (Q15 high)
    unsigned cases = 0;
    for (bool region : {false, true}) {
        jit.set_region_mode(region);
        for (bool thumb : {false, true}) {
            const auto run = [&](uint32_t opcode, auto expect) {
                guest_case(mem, jit, parent, thumb, opcode, expect);
                ++cases;
            };
            for (unsigned size = 0; size < 3; ++size) {
                const unsigned bits = 8u << size;
                // VPADD.I<bits> Dd, Dn, Dm
                run(neon3(0xf2000b10 | (size << 20), d, n, m), [&](auto &s) {
                    const auto before = s;
                    for (unsigned i = 0; i < 64 / bits; ++i) {
                        const unsigned half = 32 / bits, source = i < half ? n : m, k = 2 * (i % half);
                        set_dlane(s, d, bits, i, dlane(before, source, bits, k) + dlane(before, source, bits, k + 1));
                    }
                });
                for (bool u : {false, true}) {
                    // VHADD.<S|U><bits> Dd, Dn, Dm
                    run(neon3(0xf2000000 | (u ? 1u << 24 : 0) | (size << 20), d, n, m), [&](auto &s) {
                        const auto before = s;
                        for (unsigned i = 0; i < 64 / bits; ++i)
                            set_dlane(s, d, bits, i, halving_add(dlane(before, n, bits, i), dlane(before, m, bits, i), bits, !u));
                    });
                    // VMULL.<S|U><bits> Qd, Dn, Dm and VMLAL.<S|U><bits> Qd, Dn, Dm
                    for (bool accumulate : {false, true}) {
                        run(neon3((accumulate ? 0xf2800800 : 0xf2800c00) | (u ? 1u << 24 : 0) | (size << 20), d, n, m), [&](auto &s) {
                            const auto before = s;
                            for (unsigned i = 0; i < 64 / bits; ++i) {
                                const auto product = multiply_widen(dlane(before, n, bits, i), dlane(before, m, bits, i), bits, !u);
                                const auto base = accumulate ? lane(before, 2 * bits, i, d / 2) : 0;
                                set_lane(s, 2 * bits, i, truncate(base + product, 2 * bits), d / 2);
                            }
                        });
                    }
                    // VPMIN/VPMAX.<S|U><bits> Dd, Dn, Dm
                    for (bool minimum : {false, true}) {
                        run(neon3(0xf2000a00 | (minimum ? 0x10u : 0) | (u ? 1u << 24 : 0) | (size << 20), d, n, m), [&](auto &s) {
                            const auto before = s;
                            for (unsigned i = 0; i < 64 / bits; ++i) {
                                const unsigned half = 32 / bits, source = i < half ? n : m, k = 2 * (i % half);
                                const auto a = dlane(before, source, bits, k), b = dlane(before, source, bits, k + 1);
                                const bool a_less = u ? a < b : sext(a, bits) < sext(b, bits);
                                set_dlane(s, d, bits, i, (a_less == minimum) ? a : b);
                            }
                        });
                    }
                }
                // VMOVL.S<bits> Qd, Dm (VSHLL #0): imm3 has the single bit for bits.
                run(neon3(0xf2800a10 | ((bits / 8) << 19), d, 0, m), [&](auto &s) {
                    const auto before = s;
                    for (unsigned i = 0; i < 64 / bits; ++i)
                        set_lane(s, 2 * bits, i, truncate(uint64_t(sext(dlane(before, m, bits, i), bits)), 2 * bits), d / 2);
                });
                // VZIP.<bits> Qd, Qm and, below 32 bits, VUZP.<bits> Dd, Dm.
                run(neon3(0xf3b20180 | (size << 18) | 0x40, 4, 0, 30), [&](auto &s) {
                    const auto before = s;
                    for (unsigned j = 0; j < 128 / bits; ++j) {
                        set_lane(s, bits, j, lane(before, bits, j / 2, j % 2 ? 15 : 2), 2);
                        set_lane(s, bits, j, lane(before, bits, 64 / bits + j / 2, j % 2 ? 15 : 2), 15);
                    }
                });
                // VUZP.<bits> Qd, Qm
                run(neon3(0xf3b20100 | (size << 18) | 0x40, 4, 0, 30), [&](auto &s) {
                    const auto before = s;
                    for (unsigned i = 0; i < 128 / bits; ++i) {
                        const unsigned half = 64 / bits, source = i < half ? 2 : 15, k = 2 * (i % half);
                        set_lane(s, bits, i, lane(before, bits, k, source), 2);
                        set_lane(s, bits, i, lane(before, bits, k + 1, source), 15);
                    }
                });
                // VPADDL/VPADAL.<S|U><bits> Qd, Qm
                for (bool u : {false, true}) for (bool accumulate : {false, true}) {
                    run(neon3((accumulate ? 0xf3b00600 : 0xf3b00200) | (u ? 0x80u : 0) | (size << 18) | 0x40, 4, 0, 30), [&](auto &s) {
                        const auto before = s;
                        for (unsigned i = 0; i < 64 / bits; ++i) {
                            const auto a = lane(before, bits, 2 * i, 15), b = lane(before, bits, 2 * i + 1, 15);
                            const auto sum = u ? a + b : uint64_t(sext(a, bits) + sext(b, bits));
                            const auto base = accumulate ? lane(before, 2 * bits, i, 2) : 0;
                            set_lane(s, 2 * bits, i, truncate(base + sum, 2 * bits), 2);
                        }
                    });
                }
                if (bits < 32) {
                    run(neon3(0xf3b20100 | (size << 18), 4, 0, 31), [&](auto &s) {
                        const auto before = s;
                        for (unsigned i = 0; i < 64 / bits; ++i) {
                            const unsigned half = 32 / bits, source = i < half ? 4 : 31, k = 2 * (i % half);
                            set_dlane(s, 4, bits, i, dlane(before, source, bits, k));
                            set_dlane(s, 31, bits, i, dlane(before, source, bits, k + 1));
                        }
                    });
                }
            }
            // VSHL.U<bits> Dd, Dm, Dn: Dn holds the per-lane shift amounts.
            for (unsigned size = 0; size < 4; ++size) {
                const unsigned bits = 8u << size;
                run(neon3(0xf3000400 | (size << 20), d, n, m), [&](auto &s) {
                    const auto before = s;
                    for (unsigned i = 0; i < 64 / bits; ++i)
                        set_dlane(s, d, bits, i, logical_vshift(dlane(before, m, bits, i), dlane(before, n, bits, i), bits));
                });
            }
            // VQMOVN.U<2*bits> Dd, Qm; VQSHRN/VQRSHRN.U64 Dd, Qm, #5. QC is
            // already set in the harness state; the IR fixtures pin setting it.
            for (unsigned size = 0; size < 3; ++size) {
                const unsigned bits = 8u << size;
                run(neon3(0xf3b202c0u | (size << 18), d, 0, 4), [&](auto &s) {
                    const auto before = s;
                    for (unsigned i = 0; i < 64 / bits; ++i)
                        set_dlane(s, d, bits, i, std::min(lane(before, 2 * bits, i, 2), truncate(~uint64_t(0), bits)));
                });
            }
            for (bool rounding : {false, true}) {
                run(neon3((rounding ? 0xf3800950u : 0xf3800910u) | (59u << 16), d, 0, 4), [&](auto &s) {
                    const auto before = s;
                    for (unsigned i = 0; i < 2; ++i) {
                        const uint64_t x = lane(before, 64, i, 2);
                        const uint64_t shifted = (x >> 5) + (rounding ? (x >> 4) & 1 : 0);
                        set_dlane(s, d, 32, i, std::min<uint64_t>(shifted, 0xffffffffu));
                    }
                });
            }
            // VMUL.P8 Qd, Qn, Qm
            run(neon3(0xf3000910 | 0x40, 4, 30, 6), [&](auto &s) {
                const auto before = s;
                for (unsigned i = 0; i < 16; ++i)
                    set_lane(s, 8, i, polynomial_multiply(lane(before, 8, i, 15), lane(before, 8, i, 3)), 2);
            });
            // VMULL.P8 Qd, Dn, Dm
            run(neon3(0xf2800e00, d, n, m), [&](auto &s) {
                const auto before = s;
                for (unsigned i = 0; i < 8; ++i)
                    set_lane(s, 16, i, polynomial_multiply(dlane(before, n, 8, i), dlane(before, m, 8, i)), d / 2);
            });
        }
    }
    std::printf("NEON lane/widen/pairwise guest encodings: %u cases passed\n", cases);
}
} // namespace vector_lane_tests
