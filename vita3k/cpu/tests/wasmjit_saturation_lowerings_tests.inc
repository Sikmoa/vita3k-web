// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
void saturation_lowerings(Suite &suite) {
    struct Spec { Opcode op; unsigned bits; enum Kind { Round, Shift, ShiftUnsigned, Mixed, MulHigh, MulRound, MulLong } kind; bool sign; };
    const Spec specs[]{
        {Opcode::VectorRoundingShiftLeftS8,8,Spec::Round,true},
        {Opcode::VectorRoundingShiftLeftU8,8,Spec::Round,false},
        {Opcode::VectorSignedSaturatedShiftLeft8,8,Spec::Shift,true},
        {Opcode::VectorSignedSaturatedAccumulateUnsigned8,8,Spec::Mixed,true},
        {Opcode::VectorUnsignedSaturatedShiftLeft8,8,Spec::Shift,false},
        {Opcode::VectorUnsignedSaturatedAccumulateSigned8,8,Spec::Mixed,false},
        {Opcode::VectorSignedSaturatedShiftLeftUnsigned8,8,Spec::ShiftUnsigned,true},
        {Opcode::VectorRoundingShiftLeftS16,16,Spec::Round,true},
        {Opcode::VectorRoundingShiftLeftU16,16,Spec::Round,false},
        {Opcode::VectorSignedSaturatedShiftLeft16,16,Spec::Shift,true},
        {Opcode::VectorSignedSaturatedAccumulateUnsigned16,16,Spec::Mixed,true},
        {Opcode::VectorUnsignedSaturatedShiftLeft16,16,Spec::Shift,false},
        {Opcode::VectorUnsignedSaturatedAccumulateSigned16,16,Spec::Mixed,false},
        {Opcode::VectorSignedSaturatedShiftLeftUnsigned16,16,Spec::ShiftUnsigned,true},
        {Opcode::VectorRoundingShiftLeftS32,32,Spec::Round,true},
        {Opcode::VectorRoundingShiftLeftU32,32,Spec::Round,false},
        {Opcode::VectorSignedSaturatedShiftLeft32,32,Spec::Shift,true},
        {Opcode::VectorSignedSaturatedAccumulateUnsigned32,32,Spec::Mixed,true},
        {Opcode::VectorUnsignedSaturatedShiftLeft32,32,Spec::Shift,false},
        {Opcode::VectorUnsignedSaturatedAccumulateSigned32,32,Spec::Mixed,false},
        {Opcode::VectorSignedSaturatedShiftLeftUnsigned32,32,Spec::ShiftUnsigned,true},
        {Opcode::VectorRoundingShiftLeftS64,64,Spec::Round,true},
        {Opcode::VectorRoundingShiftLeftU64,64,Spec::Round,false},
        {Opcode::VectorSignedSaturatedShiftLeft64,64,Spec::Shift,true},
        {Opcode::VectorSignedSaturatedAccumulateUnsigned64,64,Spec::Mixed,true},
        {Opcode::VectorUnsignedSaturatedShiftLeft64,64,Spec::Shift,false},
        {Opcode::VectorUnsignedSaturatedAccumulateSigned64,64,Spec::Mixed,false},
        {Opcode::VectorSignedSaturatedShiftLeftUnsigned64,64,Spec::ShiftUnsigned,true},
        {Opcode::VectorSignedSaturatedDoublingMultiplyHigh16,16,Spec::MulHigh,true},
        {Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding16,16,Spec::MulRound,true},
        {Opcode::VectorSignedSaturatedDoublingMultiplyLong16,16,Spec::MulLong,true},
        {Opcode::VectorSignedSaturatedDoublingMultiplyHigh32,32,Spec::MulHigh,true},
        {Opcode::VectorSignedSaturatedDoublingMultiplyHighRounding32,32,Spec::MulRound,true},
        {Opcode::VectorSignedSaturatedDoublingMultiplyLong32,32,Spec::MulLong,true},
    };
    std::mt19937_64 random{0x77358432};
    for (const auto &spec : specs) for (unsigned amount : {0u, spec.bits-1}) {
        if (spec.kind != Spec::ShiftUnsigned && amount != 0) continue;
        auto block=blank();
        const auto a=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q0}});
        const auto b=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q1}});
        const auto r=append(block,spec.op,{a,spec.kind==Spec::ShiftUnsigned ? Value{uint8_t(amount)} : b});
        append(block,Opcode::A32SetVector,{Value{A32::ExtReg::Q2},r});
        std::vector<Case> cases;
        for (unsigned trial=0;trial<512;++trial) {
            auto in=initial();in.fpscr=trial%5==0 ? 0x0800001f : 0;
            for (unsigned i=0;i<8;++i) in.fpu[i]=random();
            const unsigned bits=spec.bits, result_bits=spec.kind==Spec::MulLong ? bits*2 : bits;
            const uint64_t mask=bits==64 ? UINT64_MAX : (uint64_t(1)<<bits)-1;
            const uint64_t out_mask=result_bits==64 ? UINT64_MAX : (uint64_t(1)<<result_bits)-1;
            const auto put=[&](uint32_t *words,unsigned i,unsigned width,uint64_t value) {
                const unsigned position=i*width, word=position/32, shift=position%32;
                if (width>=32) words[word]=uint32_t(value);
                else words[word]=(words[word]&~(((1u<<width)-1)<<shift))|((uint32_t(value)&((1u<<width)-1))<<shift);
                if (width==64) words[word+1]=value>>32;
            };
            const auto read=[&](unsigned operand,unsigned lane,bool sign) -> __int128 {
                const unsigned position=lane*bits;
                uint64_t value=in.fpu[operand*4+position/32]>>(position%32);
                if (bits==64) value|=uint64_t(in.fpu[operand*4+position/32+1])<<32;
                value&=mask;
                return sign&&(value&(uint64_t(1)<<(bits-1))) ? __int128(value)-(__int128(1)<<bits) : __int128(value);
            };
            for (unsigned i=0;i<128/bits;++i) {
                if (trial<64) {
                    const uint64_t values[]{0,1,mask,mask>>1,uint64_t(1)<<(bits-1)};
                    put(in.fpu,i,bits,values[(trial+i)%5]);
                    put(in.fpu+4,i,bits,values[(trial/5+i)%5]);
                }
                if (spec.kind==Spec::Round||spec.kind==Spec::Shift) {
                    const int shifts[]{0,1,7,8,15,16,31,32,63,64,127,-128,-65,-64,-63,-33,-32,-31,-17,-16,-15,-9,-8,-7,-1};
                    uint64_t value=uint64_t(read(1,i,false));
                    value=(value&~uint64_t(0xff))|uint8_t(shifts[(trial+i)%25]);put(in.fpu+4,i,bits,value);
                }
            }
            auto out=next(in);
            const bool signed_output=spec.sign && spec.kind!=Spec::ShiftUnsigned;
            const __int128 low=signed_output ? -(__int128(1)<<(result_bits-1)) : 0;
            const __int128 high=signed_output ? (__int128(1)<<(result_bits-1))-1 : (__int128(1)<<result_bits)-1;
            for (unsigned i=0;i<128/result_bits;++i) {
                const __int128 x=read(0,i,spec.kind==Spec::Mixed ? !spec.sign : spec.sign);
                const __int128 y=read(1,i,spec.sign);
                __int128 exact;
                if (spec.kind==Spec::Mixed) exact=x+y;
                else if (spec.kind==Spec::MulLong) exact=2*x*y;
                else if (spec.kind==Spec::MulHigh||spec.kind==Spec::MulRound)
                    exact=(x*y+(spec.kind==Spec::MulRound ? __int128(1)<<(bits-2) : 0))>>(bits-1);
                else {
                    const int shift=spec.kind==Spec::ShiftUnsigned ? int(amount) : int(int8_t(uint8_t(read(1,i,false))));
                    if (shift>=0) {
                        if (shift>=int(bits)) exact=spec.kind==Spec::Round ? 0 : x==0 ? 0 : x<0 ? low-1 : high+1;
                        else exact=x*(__int128(1)<<shift);
                    } else if (spec.kind==Spec::Round) {
                        exact=-shift>int(bits) ? 0 : (x+(__int128(1)<<(-shift-1)))>>-shift;
                    } else exact=x>>std::min(-shift,127);
                }
                if (spec.kind!=Spec::Round) {
                    if (exact<low||exact>high) out.fpscr|=0x08000000;
                    exact=std::clamp(exact,low,high);
                }
                put(out.fpu+8,i,result_bits,uint64_t(exact)&out_mask);
            }
            cases.emplace_back(in,out);
        }
        suite.add("saturation_new_"+IR::GetNameOf(spec.op)+"_"+std::to_string(amount),block,cases);
    }
}
