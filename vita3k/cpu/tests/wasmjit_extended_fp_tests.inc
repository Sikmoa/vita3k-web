// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
void extended_fp_lowerings(Suite &suite) {
    const auto get = [](IR::Block &block, unsigned bits, Reg low, Reg high) {
        auto value = reg(block, low);
        if (bits == 16) value = append(block, Opcode::LeastSignificantHalf, {value});
        if (bits == 64) value = append(block, Opcode::Pack2x32To1x64, {value, reg(block, high)});
        return value;
    };
    const auto put = [](IR::Block &block, unsigned bits, Value result) {
        if (bits == 16) result = append(block, Opcode::ZeroExtendHalfToWord, {result});
        if (bits == 64) {
            set(block, Reg::R7, append(block, Opcode::MostSignificantWord, {result}));
            result = append(block, Opcode::LeastSignificantWord, {result});
        }
        set(block, Reg::R6, result);
    };
    const auto run = [&](const std::string &name, Opcode op, unsigned bits,
        std::initializer_list<std::array<uint64_t, 5>> data, bool ternary = false) {
        auto block = blank();
        auto a = get(block, bits, Reg::R0, Reg::R1), b = get(block, bits, Reg::R2, Reg::R3);
        auto c = get(block, bits, Reg::R4, Reg::R5);
        const auto result = ternary ? append(block, op, {a,b,c}) : append(block, op, {a,b});
        put(block, bits, result);
        std::vector<Case> cases;
        for (const auto &row : data) {
            auto in = initial(); in.fpscr = 0;
            for (unsigned i = 0; i < 3; ++i) { in.regs[2*i] = row[i]; in.regs[2*i+1] = row[i] >> 32; }
            // Scratch preservation is part of the helper ABI.
            for (unsigned i = 0; i < 6; ++i) in.fp_arguments[i] = 0xabab0000 + i;
            auto out = next(in); out.regs[6] = row[3]; if (bits == 64) out.regs[7] = row[3] >> 32;
            out.fpscr = row[4]; cases.emplace_back(in,out);
        }
        suite.add("exact_fp_" + name, block, cases);
    };
    // Single rounding matters: (-1) + (1+epsilon)*(1-epsilon) is -epsilon^2.
    run("fma32", Opcode::FPMulAdd32, 32, {{0xbf800000,0x3f800001,0x3f7ffffe,0xa8800000,0}}, true);
    run("fma64", Opcode::FPMulAdd64, 64, {{0xbff0000000000000,0x3ff0000000000001,0x3feffffffffffffe,0xb970000000000000,0}}, true);
    run("fma16", Opcode::FPMulAdd16, 16, {{0xbc00,0x3c01,0x3bfe,0x8010,0}}, true);
    run("fms32", Opcode::FPMulSub32, 32, {{0x3f800000,0x3f800001,0x3f7ffffe,0x28800000,0}}, true);
    run("min32", Opcode::FPMin32, 32, {{0,0x80000000,0,0x80000000,0}, {0x7f800123,0x3f800000,0,0x7fc00123,1}});
    run("max32", Opcode::FPMax32, 32, {{0x80000000,0,0,0,0}, {0xffc00042,0x3f800000,0,0xffc00042,0}});
    run("minnum32", Opcode::FPMinNumeric32, 32, {{0x7fc00123,0x3f800000,0,0x3f800000,0}, {0x7f800123,0x3f800000,0,0x7fc00123,1}});
    run("mulx64", Opcode::FPMulX64, 64, {{0,0x7ff0000000000000,0,0x4000000000000000,0}, {0x8000000000000000,0x7ff0000000000000,0,0xc000000000000000,0}});
    for (uint8_t fraction : {uint8_t(0),uint8_t(16),uint8_t(32)}) {
        auto block = blank();
        auto r = append(block, Opcode::FPFixedU32ToDouble, {reg(block,Reg::R0),Value{fraction},Value{uint8_t(0)}});
        put(block,64,r);
        std::vector<Case> cases;
        for (uint32_t a : {0u,1u,0x80000000u,0xffffffffu}) {
            auto in = initial(); in.regs[0] = a;
            auto out = next(in); const uint64_t result = std::bit_cast<uint64_t>(std::ldexp(double(a),-int(fraction)));
            out.regs[6] = result; out.regs[7] = result >> 32; cases.emplace_back(in,out);
        }
        suite.add("exact_fp_U32ToDouble_"+std::to_string(fraction),block,cases);
    }
    for (unsigned bits : {32u,64u}) {
        auto block=blank();
        const auto r=append(block,bits==32 ? Opcode::FPMulAdd32 : Opcode::FPMulAdd64,
            {get(block,bits,Reg::R0,Reg::R1),get(block,bits,Reg::R2,Reg::R3),get(block,bits,Reg::R4,Reg::R5)});
        put(block,bits,r);
        std::vector<Case> cases;
        std::mt19937_64 rng{0xab321234};
        for (unsigned i=0;i<1024;++i) {
            auto in=initial();in.fpscr=0x9f;
            uint64_t x[3];
            for(unsigned j=0;j<3;++j) {
                x[j]=rng();
                if(bits==32) {
                    x[j]=uint32_t(x[j]);
                    if((x[j]&0x7f800000)==0x7f800000)x[j]&=~uint64_t(0x40000000);
                } else if((x[j]&0x7ff0000000000000)==0x7ff0000000000000)x[j]&=~uint64_t(0x4000000000000000);
                in.regs[2*j]=x[j];in.regs[2*j+1]=x[j]>>32;
            }
            const uint64_t expected=bits==32
                ? uint64_t(std::bit_cast<uint32_t>(std::fma(std::bit_cast<float>(uint32_t(x[1])),std::bit_cast<float>(uint32_t(x[2])),std::bit_cast<float>(uint32_t(x[0])))))
                : std::bit_cast<uint64_t>(std::fma(std::bit_cast<double>(x[1]),std::bit_cast<double>(x[2]),std::bit_cast<double>(x[0])));
            auto out=next(in);out.regs[6]=expected;if(bits==64)out.regs[7]=expected>>32;cases.emplace_back(in,out);
        }
        suite.add("exact_fp_fma_host_oracle_"+std::to_string(bits),block,cases);
    }

}
