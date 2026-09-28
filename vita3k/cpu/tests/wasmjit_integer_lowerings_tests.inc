// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Independent widened-integer references. Every case executes emitted Wasm
// in the Node runner, including all promoted-state region policies.
void integer_lowerings(Suite &suite) {
    std::mt19937_64 rng{0x642032};
    const std::array<uint64_t, 12> edge{0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff,
        0x8000, 0x7fffffff, 0x80000000, 0x8000000000000000ull, UINT64_MAX};
    const auto input = [&](unsigned i) { return i < edge.size() ? edge[i] : rng(); };
    const auto get64 = [](IR::Block &b, Reg low, Reg high) {
        return append(b, Opcode::Pack2x32To1x64, {reg(b, low), reg(b, high)});
    };
    const auto put64 = [](IR::Block &b, Value result) {
        set(b, Reg::R4, append(b, Opcode::LeastSignificantWord, {result}));
        set(b, Reg::R5, append(b, Opcode::MostSignificantWord, {result}));
    };
    for (bool sign : {false,true}) {
        auto block = blank();
        const auto a = get64(block,Reg::R0,Reg::R1), b = get64(block,Reg::R2,Reg::R3);
        const auto opcode = sign ? Opcode::SignedMultiplyHigh64 : Opcode::UnsignedMultiplyHigh64;
        put64(block,append(block,opcode,{a,b}));
        std::vector<Case> cases;
        for (unsigned i=0;i<80;++i) for (unsigned j=0;j<20;++j) {
            const uint64_t x=input(i), y=input(j);
            auto in=initial(); in.regs[0]=x;in.regs[1]=x>>32;in.regs[2]=y;in.regs[3]=y>>32;
            auto out=next(in);
            const uint64_t result = sign ? uint64_t((__int128(int64_t(x))*__int128(int64_t(y)))>>64)
                : uint64_t((__uint128_t(x)*__uint128_t(y))>>64);
            out.regs[4]=result;out.regs[5]=result>>32;cases.emplace_back(in,out);
        }
        suite.add("integer_"+IR::GetNameOf(opcode),block,cases);
    }
    struct Packed { Opcode opcode; unsigned bits; bool sign, sub, half, exchange; };
    const Packed packed[]{
        {Opcode::PackedAddS8,8,true,false,false,false},
        {Opcode::PackedHalvingAddS8,8,true,false,true,false},
        {Opcode::PackedSubS8,8,true,true,false,false},
        {Opcode::PackedHalvingSubS8,8,true,true,true,false},
        {Opcode::PackedAddU8,8,false,false,false,false},
        {Opcode::PackedHalvingAddU8,8,false,false,true,false},
        {Opcode::PackedSubU8,8,false,true,false,false},
        {Opcode::PackedHalvingSubU8,8,false,true,true,false},
        {Opcode::PackedAddS16,16,true,false,false,false},
        {Opcode::PackedHalvingAddS16,16,true,false,true,false},
        {Opcode::PackedSubS16,16,true,true,false,false},
        {Opcode::PackedHalvingSubS16,16,true,true,true,false},
        {Opcode::PackedAddSubS16,16,true,true,false,true},
        {Opcode::PackedHalvingAddSubS16,16,true,true,true,true},
        {Opcode::PackedSubAddS16,16,true,false,false,true},
        {Opcode::PackedHalvingSubAddS16,16,true,false,true,true},
        {Opcode::PackedAddU16,16,false,false,false,false},
        {Opcode::PackedHalvingAddU16,16,false,false,true,false},
        {Opcode::PackedSubU16,16,false,true,false,false},
        {Opcode::PackedHalvingSubU16,16,false,true,true,false},
        {Opcode::PackedAddSubU16,16,false,true,false,true},
        {Opcode::PackedHalvingAddSubU16,16,false,true,true,true},
        {Opcode::PackedSubAddU16,16,false,false,false,true},
        {Opcode::PackedHalvingSubAddU16,16,false,false,true,true},
    };
    for (const auto &spec : packed) {
        auto block=blank();
        const auto a=reg(block,Reg::R0),b=reg(block,Reg::R1);
        const auto r=append(block,spec.opcode,{a,b});set(block,Reg::R2,r);
        if (!spec.half) {
            auto ge=append(block,Opcode::GetGEFromOp,{r}); set(block,Reg::R3,ge);
            append(block,Opcode::A32SetGEFlags,{ge});
        }
        std::vector<Case> cases;
        for (unsigned i=0;i<256;++i) {
            auto in=initial();in.regs[0]=input(i);in.regs[1]=input(i/4);
            auto out=next(in);uint32_t packed_result=0,flags=0,compressed=0;
            const uint32_t mask=(1u<<spec.bits)-1;
            const auto lane=[&](unsigned arg,unsigned i) {
                int32_t v=(in.regs[arg]>>(i*spec.bits))&mask;
                if (spec.sign && (v&(1u<<(spec.bits-1)))) v-=1<<spec.bits;
                return v;
            };
            for (unsigned i=0;i<32/spec.bits;++i) {
                const bool sub=spec.exchange ? (i==0 ? spec.sub : !spec.sub) : spec.sub;
                const int32_t a=lane(0,i), b=lane(1,spec.exchange ? i^1 : i);
                const int32_t exact=sub ? a-b : a+b;
                const bool ge=spec.sign||sub ? exact>=0 : exact>int32_t(mask);
                if (ge) { flags|=mask<<(i*spec.bits);compressed|=((1u<<(spec.bits/8))-1)<<(i*spec.bits/8); }
                packed_result|=(uint32_t(spec.half ? exact>>1 : exact)&mask)<<(i*spec.bits);
            }
            out.regs[2]=packed_result;
            if (!spec.half) {out.regs[3]=flags;out.cpsr=(out.cpsr&~0xf0000)|(compressed<<16);}
            cases.emplace_back(in,out);
        }
        suite.add("integer_"+IR::GetNameOf(spec.opcode),block,cases);
    }
    for (bool wide : {false, true}) for (bool sign : {false, true}) {
        auto block = blank();
        const auto a = wide ? get64(block, Reg::R0, Reg::R1) : reg(block, Reg::R0);
        const auto b = wide ? get64(block, Reg::R2, Reg::R3) : reg(block, Reg::R2);
        const auto op = wide ? (sign ? Opcode::SignedDiv64 : Opcode::UnsignedDiv64)
                            : (sign ? Opcode::SignedDiv32 : Opcode::UnsignedDiv32);
        const auto r = append(block, op, {a, b});
        if (wide) put64(block, r); else set(block, Reg::R4, r);
        std::vector<Case> cases;
        for (unsigned i = 0; i < 64; ++i) for (unsigned j = 0; j < edge.size(); ++j) {
            uint64_t x = input(i), y = input(j);
            if (!wide) { x = uint32_t(x); y = uint32_t(y); }
            auto in = initial();
            in.regs[0] = x; in.regs[1] = x >> 32; in.regs[2] = y; in.regs[3] = y >> 32;
            auto out = next(in);
            __int128 sx = wide ? __int128(int64_t(x)) : __int128(int32_t(x));
            __int128 sy = wide ? __int128(int64_t(y)) : __int128(int32_t(y));
            uint64_t q = !y ? 0 : sign ? uint64_t(sx / sy) : x / y;
            out.regs[4] = q; if (wide) out.regs[5] = q >> 32;
            cases.emplace_back(in, out);
        }
        suite.add("integer_" + std::string(IR::GetNameOf(op)), block, cases);
    }
    for (unsigned bits : {8u, 16u, 32u, 64u}) for (bool sign : {false, true}) for (bool add : {false, true}) {
        const Opcode ops[4][4] = {
            {Opcode::UnsignedSaturatedSub8, Opcode::UnsignedSaturatedAdd8, Opcode::SignedSaturatedSub8, Opcode::SignedSaturatedAdd8},
            {Opcode::UnsignedSaturatedSub16, Opcode::UnsignedSaturatedAdd16, Opcode::SignedSaturatedSub16, Opcode::SignedSaturatedAdd16},
            {Opcode::UnsignedSaturatedSub32, Opcode::UnsignedSaturatedAdd32, Opcode::SignedSaturatedSub32, Opcode::SignedSaturatedAdd32},
            {Opcode::UnsignedSaturatedSub64, Opcode::UnsignedSaturatedAdd64, Opcode::SignedSaturatedSub64, Opcode::SignedSaturatedAdd64}};
        const unsigned row = bits == 8 ? 0 : bits == 16 ? 1 : bits == 32 ? 2 : 3;
        const auto op = ops[row][unsigned(sign) * 2 + unsigned(add)];
        auto block = blank();
        const auto narrow = [&](Reg r) {
            auto v = reg(block, r);
            if (bits < 32) v = append(block, bits == 8 ? Opcode::LeastSignificantByte : Opcode::LeastSignificantHalf, {v});
            return v;
        };
        auto a = bits == 64 ? get64(block, Reg::R0, Reg::R1) : narrow(Reg::R0);
        auto b = bits == 64 ? get64(block, Reg::R2, Reg::R3) : narrow(Reg::R2);
        auto r = append(block, op, {a,b});
        if (bits == 64) put64(block,r);
        else {
            if (bits < 32) r = append(block, bits == 8 ? Opcode::ZeroExtendByteToWord : Opcode::ZeroExtendHalfToWord, {r});
            set(block, Reg::R4, r);
        }
        std::vector<Case> cases;
        const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
        const __int128 lo = sign ? -(__int128(1) << (bits - 1)) : 0;
        const __int128 hi = sign ? (__int128(1) << (bits - 1)) - 1 : (__int128(1) << bits) - 1;
        const auto extend = [&](uint64_t x) -> __int128 {
            x &= mask;
            return sign && (x & (uint64_t(1) << (bits - 1))) ? __int128(x) - (__int128(1) << bits) : x;
        };
        for (unsigned i = 0; i < 64; ++i) for (unsigned j = 0; j < edge.size(); ++j) {
            uint64_t x = input(i), y = input(j);
            auto in = initial(); in.fpscr = i & 1 ? 0x08000000 : 0;
            in.regs[0] = x; in.regs[1] = x >> 32; in.regs[2] = y; in.regs[3] = y >> 32;
            auto out = next(in);
            const __int128 exact = add ? extend(x) + extend(y) : extend(x) - extend(y);
            const uint64_t result = uint64_t(std::clamp(exact, lo, hi)) & mask;
            out.regs[4] = result; if (bits == 64) out.regs[5] = result >> 32;
            if (exact < lo || exact > hi) out.fpscr |= 0x08000000;
            cases.emplace_back(in,out);
        }
        suite.add("integer_" + std::string(IR::GetNameOf(op)), block, cases);
    }
}
