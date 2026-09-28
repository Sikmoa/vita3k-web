// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
void crypto_lowerings(Suite &suite) {
    namespace Crypto = Dynarmic::Common::Crypto;
    std::mt19937_64 rng{0x73ac64};
    for (Opcode op : {Opcode::AESEncryptSingleRound,Opcode::AESDecryptSingleRound,Opcode::AESMixColumns,Opcode::AESInverseMixColumns,
        Opcode::VectorPolynomialMultiplyLong64,Opcode::VectorUnsignedRecipEstimate,Opcode::VectorUnsignedRecipSqrtEstimate}) {
        auto block=blank();
        const auto a=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q0}});
        const auto b=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q1}});
        const auto r=op==Opcode::VectorPolynomialMultiplyLong64 ? append(block,op,{a,b}) : append(block,op,{a});
        append(block,Opcode::A32SetVector,{Value{A32::ExtReg::Q2},r});
        std::vector<Case> cases;
        for (unsigned trial=0;trial<512;++trial) {
            auto in=initial();for (unsigned i=0;i<8;++i) in.fpu[i]=rng();
            if (op==Opcode::VectorUnsignedRecipEstimate||op==Opcode::VectorUnsignedRecipSqrtEstimate)
                for(unsigned i=0;i<4;++i)in.fpu[i]=(trial<<23)|uint32_t(rng()&0x7fffff);
            auto out=next(in);
            Crypto::AES::State input{},output{};
            for(unsigned i=0;i<16;++i)input[i]=uint8_t(in.fpu[i/4]>>((i%4)*8));
            switch(op) {
            case Opcode::AESEncryptSingleRound: Crypto::AES::EncryptSingleRound(output,input); break;
            case Opcode::AESDecryptSingleRound: Crypto::AES::DecryptSingleRound(output,input); break;
            case Opcode::AESMixColumns: Crypto::AES::MixColumns(output,input); break;
            case Opcode::AESInverseMixColumns: Crypto::AES::InverseMixColumns(output,input); break;
            case Opcode::VectorPolynomialMultiplyLong64: {
                const uint64_t x=uint64_t(in.fpu[0])|(uint64_t(in.fpu[1])<<32), y=uint64_t(in.fpu[4])|(uint64_t(in.fpu[5])<<32);
                __uint128_t product=0;for(unsigned i=0;i<64;++i)if((y>>i)&1)product^=__uint128_t(x)<<i;
                for(unsigned i=0;i<16;++i)output[i]=uint8_t(product>>(8*i));break;
            }
            default:
                for(unsigned i=0;i<4;++i) {
                    const uint32_t x=in.fpu[i];const bool root=op==Opcode::VectorUnsignedRecipSqrtEstimate;
                    const uint32_t result=x<(root ? 0x40000000u : 0x80000000u) ? UINT32_MAX
                        : (0x100u|(root ? Dynarmic::Common::RecipSqrtEstimate(x>>23) : Dynarmic::Common::RecipEstimate(x>>23)))<<23;
                    for(unsigned byte=0;byte<4;++byte)output[4*i+byte]=uint8_t(result>>(byte*8));
                }
            }
            for(unsigned i=0;i<16;++i)out.fpu[8+i/4]|=uint32_t(output[i])<<((i%4)*8);
            cases.emplace_back(in,out);
        }
        suite.add("crypto_"+IR::GetNameOf(op),block,cases);
    }
    {
        auto block=blank();auto input=append(block,Opcode::LeastSignificantByte,{reg(block,Reg::R0)});
        auto r=append(block,Opcode::SM4AccessSubstitutionBox,{input});set(block,Reg::R1,append(block,Opcode::ZeroExtendByteToWord,{r}));
        std::vector<Case> cases;
        for(unsigned i=0;i<256;++i) {auto in=initial();in.regs[0]=i;auto out=next(in);out.regs[1]=Crypto::SM4::AccessSubstitutionBox(uint8_t(i));cases.emplace_back(in,out);}
        suite.add("crypto_SM4",block,cases);
    }
    const Opcode crc[2][4]{{Opcode::CRC32ISO8,Opcode::CRC32ISO16,Opcode::CRC32ISO32,Opcode::CRC32ISO64},
        {Opcode::CRC32Castagnoli8,Opcode::CRC32Castagnoli16,Opcode::CRC32Castagnoli32,Opcode::CRC32Castagnoli64}};
    for(unsigned family=0;family<2;++family)for(unsigned size=0;size<4;++size) {
        auto block=blank();const auto a=reg(block,Reg::R0);auto b=reg(block,Reg::R1);
        if(size==3)b=append(block,Opcode::Pack2x32To1x64,{b,reg(block,Reg::R2)});
        set(block,Reg::R3,append(block,crc[family][size],{a,b}));
        std::vector<Case> cases;
        for(unsigned i=0;i<256;++i) {
            auto in=initial();in.regs[0]=rng();in.regs[1]=rng();in.regs[2]=rng();auto out=next(in);
            const uint64_t value=uint64_t(in.regs[1])|(uint64_t(in.regs[2])<<32);
            out.regs[3]=family ? Crypto::CRC32::ComputeCRC32Castagnoli(in.regs[0],value,1<<size) : Crypto::CRC32::ComputeCRC32ISO(in.regs[0],value,1<<size);
            cases.emplace_back(in,out);
        }
        suite.add("crypto_"+IR::GetNameOf(crc[family][size]),block,cases);
    }
    for(Opcode opcode:{Opcode::SHA256Hash,Opcode::SHA256MessageSchedule0,Opcode::SHA256MessageSchedule1})for(bool first:{false,true}) {
        if(opcode!=Opcode::SHA256Hash&&first)continue;
        auto block=blank();const auto a=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q0}});
        const auto b=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q1}}),c=append(block,Opcode::A32GetVector,{Value{A32::ExtReg::Q2}});
        const auto r=opcode==Opcode::SHA256Hash ? append(block,opcode,{a,b,c,Value{first}})
            : opcode==Opcode::SHA256MessageSchedule0 ? append(block,opcode,{a,b}) : append(block,opcode,{a,b,c});
        append(block,Opcode::A32SetVector,{Value{A32::ExtReg::Q3},r});
        std::vector<Case> cases;
        for(unsigned trial=0;trial<128;++trial) {
            auto in=initial();for(unsigned i=0;i<12;++i)in.fpu[i]=rng();auto out=next(in);
            const auto s0=[](uint32_t x){return std::rotr(x,7)^std::rotr(x,18)^(x>>3);};
            const auto s1=[](uint32_t x){return std::rotr(x,17)^std::rotr(x,19)^(x>>10);};
            if(opcode==Opcode::SHA256Hash) {
                std::array<uint32_t,8> v;std::copy_n(in.fpu,8,v.begin());
                for(unsigned i=0;i<4;++i) {
                    const uint32_t t1=v[7]+(std::rotr(v[4],6)^std::rotr(v[4],11)^std::rotr(v[4],25))+((v[4]&v[5])^(~v[4]&v[6]))+in.fpu[8+i];
                    const uint32_t t2=(std::rotr(v[0],2)^std::rotr(v[0],13)^std::rotr(v[0],22))+((v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2]));
                    v={t1+t2,v[0],v[1],v[2],v[3]+t1,v[4],v[5],v[6]};
                }
                std::copy_n(v.begin()+(first?0:4),4,out.fpu+12);
            } else for(unsigned i=0;i<4;++i)out.fpu[12+i]=opcode==Opcode::SHA256MessageSchedule0
                ? in.fpu[i]+s0(in.fpu[i+1]) : in.fpu[i]+in.fpu[5+i]+s1(i<2?in.fpu[10+i]:out.fpu[10+i]);
            cases.emplace_back(in,out);
        }
        suite.add("crypto_"+IR::GetNameOf(opcode)+"_"+std::to_string(first),block,cases);
    }
}
