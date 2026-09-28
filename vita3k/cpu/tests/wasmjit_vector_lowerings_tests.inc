// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
void vector_lowerings(Suite &suite) {
    struct Spec { Opcode op; unsigned bits; enum Kind { PairAdd, PairMin, PairMax, HalfSub, Min, Max, Greater, Abs, Equal, Extend, Clz, Reduce, Transpose, Deinterleave } kind; bool sign = false, lower = false; };
    const std::vector<Spec> specs{
        {Opcode::VectorPairedAdd8, 8, Spec::PairAdd},
        {Opcode::VectorReduceAdd8, 8, Spec::Reduce},
        {Opcode::VectorTranspose8, 8, Spec::Transpose},
        {Opcode::VectorCountLeadingZeros8, 8, Spec::Clz},
        {Opcode::VectorHalvingSubS8, 8, Spec::HalfSub, true},
        {Opcode::VectorPairedMinS8, 8, Spec::PairMin, true, false},
        {Opcode::VectorPairedMinLowerS8, 8, Spec::PairMin, true, true},
        {Opcode::VectorPairedMaxS8, 8, Spec::PairMax, true, false},
        {Opcode::VectorPairedMaxLowerS8, 8, Spec::PairMax, true, true},
        {Opcode::VectorHalvingSubU8, 8, Spec::HalfSub, false},
        {Opcode::VectorPairedMinU8, 8, Spec::PairMin, false, false},
        {Opcode::VectorPairedMinLowerU8, 8, Spec::PairMin, false, true},
        {Opcode::VectorPairedMaxU8, 8, Spec::PairMax, false, false},
        {Opcode::VectorPairedMaxLowerU8, 8, Spec::PairMax, false, true},
        {Opcode::VectorPairedAdd16, 16, Spec::PairAdd},
        {Opcode::VectorReduceAdd16, 16, Spec::Reduce},
        {Opcode::VectorTranspose16, 16, Spec::Transpose},
        {Opcode::VectorCountLeadingZeros16, 16, Spec::Clz},
        {Opcode::VectorHalvingSubS16, 16, Spec::HalfSub, true},
        {Opcode::VectorPairedMinS16, 16, Spec::PairMin, true, false},
        {Opcode::VectorPairedMinLowerS16, 16, Spec::PairMin, true, true},
        {Opcode::VectorPairedMaxS16, 16, Spec::PairMax, true, false},
        {Opcode::VectorPairedMaxLowerS16, 16, Spec::PairMax, true, true},
        {Opcode::VectorHalvingSubU16, 16, Spec::HalfSub, false},
        {Opcode::VectorPairedMinU16, 16, Spec::PairMin, false, false},
        {Opcode::VectorPairedMinLowerU16, 16, Spec::PairMin, false, true},
        {Opcode::VectorPairedMaxU16, 16, Spec::PairMax, false, false},
        {Opcode::VectorPairedMaxLowerU16, 16, Spec::PairMax, false, true},
        {Opcode::VectorPairedAdd32, 32, Spec::PairAdd},
        {Opcode::VectorReduceAdd32, 32, Spec::Reduce},
        {Opcode::VectorTranspose32, 32, Spec::Transpose},
        {Opcode::VectorCountLeadingZeros32, 32, Spec::Clz},
        {Opcode::VectorHalvingSubS32, 32, Spec::HalfSub, true},
        {Opcode::VectorPairedMinS32, 32, Spec::PairMin, true, false},
        {Opcode::VectorPairedMinLowerS32, 32, Spec::PairMin, true, true},
        {Opcode::VectorPairedMaxS32, 32, Spec::PairMax, true, false},
        {Opcode::VectorPairedMaxLowerS32, 32, Spec::PairMax, true, true},
        {Opcode::VectorHalvingSubU32, 32, Spec::HalfSub, false},
        {Opcode::VectorPairedMinU32, 32, Spec::PairMin, false, false},
        {Opcode::VectorPairedMinLowerU32, 32, Spec::PairMin, false, true},
        {Opcode::VectorPairedMaxU32, 32, Spec::PairMax, false, false},
        {Opcode::VectorPairedMaxLowerU32, 32, Spec::PairMax, false, true},
        {Opcode::VectorPairedAdd64, 64, Spec::PairAdd},
        {Opcode::VectorReduceAdd64, 64, Spec::Reduce},
        {Opcode::VectorTranspose64, 64, Spec::Transpose},
        {Opcode::VectorMinS64, 64, Spec::Min, true},
        {Opcode::VectorMinU64, 64, Spec::Min, false},
        {Opcode::VectorMaxS64, 64, Spec::Max, true},
        {Opcode::VectorMaxU64, 64, Spec::Max, false},
        {Opcode::VectorGreaterS64,64,Spec::Greater,true},
        {Opcode::VectorAbs64,64,Spec::Abs,true},
        {Opcode::VectorEqual128,64,Spec::Equal},
        {Opcode::VectorSignExtend64,64,Spec::Extend,true},
        {Opcode::VectorDeinterleaveEven64,64,Spec::Deinterleave},
        {Opcode::VectorDeinterleaveOdd64,64,Spec::Deinterleave,true},
    };
    std::mt19937_64 rng{0x8126345};
    for (const auto &spec : specs) for (unsigned part = 0; part < (spec.kind == Spec::Transpose ? 2u : 1u); ++part) {
        auto block = blank();
        const auto a = append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q0}});
        const auto b = append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q1}});
        Value r;
        if (spec.kind == Spec::Transpose) r = append(block,spec.op,{a,b,Value{bool(part)}});
        else if (spec.kind == Spec::Abs || spec.kind == Spec::Extend || spec.kind == Spec::Clz || spec.kind == Spec::Reduce)
            r = append(block,spec.op,{a});
        else r = append(block,spec.op,{a,b});
        append(block,Opcode::A32SetVector,{Value{A32::ExtReg::Q2},r});
        std::vector<Case> cases;
        for (unsigned trial = 0; trial < 128; ++trial) {
            auto in = initial();
            for (unsigned i = 0; i < 8; ++i) in.fpu[i] = trial < 8 ? std::array<uint32_t,8>{0,1,0xffffffff,0x80000000,0x7fffffff,0x80808080,0x7f7f7f7f,0xaaaa5555}[(i+trial)%8] : rng();
            if (trial % 3 == 0) std::copy_n(in.fpu,4,in.fpu+4);
            auto out = next(in);
            const uint64_t mask = spec.bits == 64 ? UINT64_MAX : (uint64_t(1) << spec.bits) - 1;
            const auto lane = [&](unsigned operand, unsigned index) -> __int128 {
                const unsigned position = index * spec.bits;
                uint64_t x = in.fpu[operand*4 + position/32];
                if (spec.bits == 64) x |= uint64_t(in.fpu[operand*4 + position/32+1]) << 32;
                else x >>= position % 32;
                x &= mask;
                return spec.sign && (x & (uint64_t(1) << (spec.bits-1))) ? __int128(x)-(__int128(1)<<spec.bits) : __int128(x);
            };
            const unsigned lanes = 128 / spec.bits, count = spec.lower ? lanes/2 : lanes;
            for (unsigned i = 0; i < count; ++i) {
                __int128 result = 0;
                const auto x = lane(0,i), y = lane(1,i);
                switch (spec.kind) {
                case Spec::PairAdd: case Spec::PairMin: case Spec::PairMax: {
                    const auto l = lane(i/(count/2),2*(i%(count/2))), h = lane(i/(count/2),2*(i%(count/2))+1);
                    result = spec.kind == Spec::PairAdd ? l+h : spec.kind == Spec::PairMin ? std::min(l,h) : std::max(l,h); break;
                }
                case Spec::HalfSub: result = (x-y)>>1; break;
                case Spec::Min: result = std::min(x,y); break;
                case Spec::Max: result = std::max(x,y); break;
                case Spec::Greater: result = x>y ? -1 : 0; break;
                case Spec::Abs: result = x<0 ? -x : x; break;
                case Spec::Equal: result = std::equal(in.fpu,in.fpu+4,in.fpu+4) ? -1 : 0; break;
                case Spec::Extend: result = i ? (lane(0,0)<0 ? -1 : 0) : x; break;
                case Spec::Clz: result = std::countl_zero(uint64_t(x)) - (64-spec.bits); break;
                case Spec::Reduce: if (!i) for (unsigned j=0;j<lanes;++j) result += lane(0,j); break;
                case Spec::Transpose: result = lane(i%2,2*(i/2)+part); break;
                case Spec::Deinterleave: result = lane(i,unsigned(spec.sign)); break;
                }
                const uint64_t packed = uint64_t(result)&mask;
                const unsigned position = i*spec.bits;
                out.fpu[8+position/32] |= uint32_t(packed) << (position%32);
                if (spec.bits==64) out.fpu[9+position/32] = packed>>32;
            }
            cases.emplace_back(in,out);
        }
        suite.add("vector_new_"+IR::GetNameOf(spec.op)+"_"+std::to_string(part),block,cases);
    }
}
