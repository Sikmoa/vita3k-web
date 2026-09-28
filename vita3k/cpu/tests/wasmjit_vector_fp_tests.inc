// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Cross-platform checks: native portable FP versus actual helper Wasm, plus
// independently calculated scalar FMA/NaN cases in extended_fp_tests.inc.
void vector_fp_lowerings(Suite &suite) {
    using namespace vita3k::wasmjit;
    struct Spec { Opcode op; FPOperation math; unsigned bits, result_bits; bool unsigned_ = false, paired = false, lower = false; };
    const Spec specs[]{
        {Opcode::FPVectorAdd64,FPOperation::Add,64,64},
        {Opcode::FPVectorSub64,FPOperation::Sub,64,64},
        {Opcode::FPVectorMul64,FPOperation::Mul,64,64},
        {Opcode::FPVectorDiv32,FPOperation::Div,32,32},
        {Opcode::FPVectorDiv64,FPOperation::Div,64,64},
        {Opcode::FPVectorEqual16,FPOperation::Equal,16,16},
        {Opcode::FPVectorEqual64,FPOperation::Equal,64,64},
        {Opcode::FPVectorGreater64,FPOperation::Greater,64,64},
        {Opcode::FPVectorGreaterEqual64,FPOperation::GreaterEqual,64,64},
        {Opcode::FPVectorMin64,FPOperation::Min,64,64},
        {Opcode::FPVectorMax64,FPOperation::Max,64,64},
        {Opcode::FPVectorMinNumeric32,FPOperation::MinNumeric,32,32},
        {Opcode::FPVectorMinNumeric64,FPOperation::MinNumeric,64,64},
        {Opcode::FPVectorMaxNumeric32,FPOperation::MaxNumeric,32,32},
        {Opcode::FPVectorMaxNumeric64,FPOperation::MaxNumeric,64,64},
        {Opcode::FPVectorMulAdd16,FPOperation::MulAdd,16,16},
        {Opcode::FPVectorMulAdd32,FPOperation::MulAdd,32,32},
        {Opcode::FPVectorMulAdd64,FPOperation::MulAdd,64,64},
        {Opcode::FPVectorMulX32,FPOperation::MulX,32,32},
        {Opcode::FPVectorMulX64,FPOperation::MulX,64,64},
        {Opcode::FPVectorRecipEstimate16,FPOperation::RecipEstimate,16,16},
        {Opcode::FPVectorRecipEstimate64,FPOperation::RecipEstimate,64,64},
        {Opcode::FPVectorRecipStepFused16,FPOperation::RecipStep,16,16},
        {Opcode::FPVectorRecipStepFused64,FPOperation::RecipStep,64,64},
        {Opcode::FPVectorRSqrtEstimate16,FPOperation::RSqrtEstimate,16,16},
        {Opcode::FPVectorRSqrtEstimate64,FPOperation::RSqrtEstimate,64,64},
        {Opcode::FPVectorRSqrtStepFused16,FPOperation::RSqrtStep,16,16},
        {Opcode::FPVectorRSqrtStepFused64,FPOperation::RSqrtStep,64,64},
        {Opcode::FPVectorRoundInt16,FPOperation::RoundInt,16,16},
        {Opcode::FPVectorRoundInt32,FPOperation::RoundInt,32,32},
        {Opcode::FPVectorRoundInt64,FPOperation::RoundInt,64,64},
        {Opcode::FPVectorSqrt32,FPOperation::Sqrt,32,32},
        {Opcode::FPVectorSqrt64,FPOperation::Sqrt,64,64},
        {Opcode::FPVectorToSignedFixed16,FPOperation::ToFixed,16,16,false},
        {Opcode::FPVectorToSignedFixed64,FPOperation::ToFixed,64,64,false},
        {Opcode::FPVectorToUnsignedFixed16,FPOperation::ToFixed,16,16,true},
        {Opcode::FPVectorToUnsignedFixed64,FPOperation::ToFixed,64,64,true},
        {Opcode::FPVectorFromSignedFixed64,FPOperation::FromFixed,64,64,false},
        {Opcode::FPVectorFromUnsignedFixed64,FPOperation::FromFixed,64,64,true},
        {Opcode::FPVectorPairedAdd32,FPOperation::Add,32,32,false,true,false},
        {Opcode::FPVectorPairedAdd64,FPOperation::Add,64,64,false,true,false},
        {Opcode::FPVectorPairedAddLower32,FPOperation::Add,32,32,false,true,true},
        {Opcode::FPVectorPairedAddLower64,FPOperation::Add,64,64,false,true,true},
        {Opcode::FPVectorFromHalf32,FPOperation::Convert,16,32},
        {Opcode::FPVectorToHalf32,FPOperation::Convert,32,16},
    };
    std::mt19937_64 random{0xfc76549};
    for (const auto &spec : specs) for (bool controlled : {false,true}) {
        auto block=blank();
        const auto a=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q0}});
        const auto b=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q1}});
        const auto c=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q2}});
        Value result;
        const auto math=spec.math;
        const bool fixed=math==FPOperation::FromFixed || math==FPOperation::ToFixed;
        if (fixed) result=append(block,spec.op,{a,Value{uint8_t(spec.bits/2)},Value{uint8_t(3)},Value{controlled}});
        else if (math==FPOperation::RoundInt) result=append(block,spec.op,{a,Value{uint8_t(4)},Value{true},Value{controlled}});
        else if (math==FPOperation::Convert) result=append(block,spec.op,{a,Value{uint8_t(0)},Value{controlled}});
        else if (math==FPOperation::MulAdd) result=append(block,spec.op,{a,b,c,Value{controlled}});
        else if (IR::GetNumArgsOf(spec.op)==2) result=append(block,spec.op,{a,Value{controlled}});
        else result=append(block,spec.op,{a,b,Value{controlled}});
        append(block,Opcode::A32SetVector,{Value{A32::ExtReg::Q3},result});
        std::vector<Case> cases;
        for (unsigned trial=0;trial<64;++trial) {
            auto in=initial();in.fpscr=0;
            for (unsigned i=0;i<12;++i) in.fpu[i]=random();
            for (unsigned i=0;i<6;++i) in.fp_arguments[i]=random();
            auto out=next(in);
            const unsigned count=(spec.lower ? 64 : 128)/std::max(spec.bits,spec.result_bits);
            const auto read=[&](unsigned arg,unsigned i) {
                uint64_t x=in.fpu[4*arg+i*spec.bits/32];
                if (spec.bits==64) x|=uint64_t(in.fpu[4*arg+i*2+1])<<32;
                if (spec.bits==16) x=(x>>((i%2)*16))&0xffff;
                return x;
            };
            for (unsigned i=0;i<count;++i) {
                uint64_t operands[]{read(0,i),read(1,i),read(2,i)};
                if (spec.paired) {
                    if (spec.lower && spec.bits==64) {operands[0]=read(0,0);operands[1]=read(1,0);}
                    else for (unsigned j=0;j<2;++j) operands[j]=read(i/(count/2),(i%(count/2))*2+j);
                }
                if (fixed) {operands[1]=spec.bits/2;operands[2]=3;}
                if (math==FPOperation::RoundInt) {operands[1]=4;operands[2]=1;}
                if (math==FPOperation::Convert) operands[1]=0;
                const auto r=fp_extended_arithmetic(fp_operation(math,spec.bits,spec.result_bits,spec.unsigned_),
                    operands[0],operands[1],operands[2],controlled ? 0 : 0x03000000);
                out.fpscr|=r.flags;
                if (spec.result_bits==16) out.fpu[12+i/2]|=uint32_t(r.bits&0xffff)<<((i%2)*16);
                else {
                    out.fpu[12+i*spec.result_bits/32]=uint32_t(r.bits);
                    if (spec.result_bits==64) out.fpu[13+i*2]=uint32_t(r.bits>>32);
                }
            }
            cases.emplace_back(in,out);
        }
        suite.add("fp_vector_new_"+IR::GetNameOf(spec.op)+"_"+std::to_string(controlled),block,cases);
    }
}
