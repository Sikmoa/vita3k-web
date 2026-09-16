// WebGPU-specific SPIR-V lowering. Native Vulkan modules are not modified.
#pragma once
#include <SPIRV/spirv.hpp>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace shader {
// Split combined descriptors: texture binding = 2*n, sampler = 2*n+1.
// Descriptor sets remain 2 (vertex textures) and 3 (fragment textures).
inline void lower_webgpu_spirv(std::vector<uint32_t> &words) {
    using Words = std::vector<uint32_t>;
    struct Combined { uint32_t image_type, sampled_type, sampler; };
    std::map<uint32_t, uint32_t> sampled, pointers;
    std::map<uint32_t, Combined> variables;
    if (words.size() < 5 || words[0] != spv::MagicNumber)
        throw std::runtime_error("Invalid SPIR-V header");
    uint32_t next = words[3];
    for (size_t i = 5; i < words.size();) {
        auto n = words[i] >> 16, op = words[i] & 0xffff;
        if (!n || i + n > words.size()) throw std::runtime_error("Invalid SPIR-V instruction");
        if (op == spv::OpTypeSampledImage) sampled[words[i+1]] = words[i+2];
        if (op == spv::OpTypePointer && sampled.contains(words[i+3]))
            pointers[words[i+1]] = words[i+3];
        if (op == spv::OpVariable && pointers.contains(words[i+1])) {
            auto type = pointers.at(words[i+1]);
            variables[words[i+2]] = { sampled.at(type), type, next++ };
        }
        i += n;
    }
    if (variables.empty()) return;
    uint32_t sampler_type = next++, sampler_ptr = next++;
    Words out(words.begin(), words.begin()+5);
    auto emit = [&](spv::Op op, std::initializer_list<uint32_t> args) {
        out.push_back((uint32_t(args.size()+1) << 16) | op);
        out.insert(out.end(), args);
    };
    bool types_added = false;
    for (size_t i = 5; i < words.size();) {
        auto n = words[i] >> 16, op = words[i] & 0xffff;
        const auto *w = words.data()+i;
        if (!types_added && op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) {
            emit(spv::OpTypeSampler, {sampler_type});
            emit(spv::OpTypePointer, {sampler_ptr, spv::StorageClassUniformConstant, sampler_type});
            types_added = true;
        }
        if (op == spv::OpTypePointer && pointers.contains(w[1])) {
            emit(spv::OpTypePointer, {w[1], w[2], sampled.at(w[3])});
        } else if (op == spv::OpDecorate && variables.contains(w[1]) && w[2] == spv::DecorationBinding) {
            emit(spv::OpDecorate, {w[1], w[2], 2*w[3]});
            emit(spv::OpDecorate, {variables.at(w[1]).sampler, w[2], 2*w[3]+1});
        } else if (op == spv::OpLoad && variables.contains(w[3])) {
            const auto &v = variables.at(w[3]);
            auto image = next++, sampler = next++;
            emit(spv::OpLoad, {v.image_type, image, w[3]});
            emit(spv::OpLoad, {sampler_type, sampler, v.sampler});
            emit(spv::OpSampledImage, {v.sampled_type, w[2], image, sampler});
        } else {
            out.insert(out.end(), w, w+n);
            if (op == spv::OpVariable && variables.contains(w[2]))
                emit(spv::OpVariable, {sampler_ptr, variables.at(w[2]).sampler, spv::StorageClassUniformConstant});
            if (op == spv::OpDecorate && variables.contains(w[1]) && w[2] == spv::DecorationDescriptorSet)
                emit(spv::OpDecorate, {variables.at(w[1]).sampler, w[2], w[3]});
        }
        i += n;
    }
    out[3] = next;
    words = std::move(out);
}
}
