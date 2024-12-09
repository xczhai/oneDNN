/*******************************************************************************
* Copyright 2020-2021 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "cpu/x64/jit_generator.hpp"
#include "cpu/x64/injectors/jit_uni_eltwise_injector.hpp"
#include "cpu/x64/injectors/jit_uni_binary_injector.hpp"
#include "cpu/x64/injectors/jit_uni_depthwise_injector.hpp"

#include "cpu/x64/jit_gemm_convolution_utils.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {
namespace gemm_convolution_utils {

using namespace dnnl::impl::cpu::gemm_convolution_utils;

template <cpu_isa_t isa>
struct jit_pp_kernel_t : pp_kernel_t, public jit_generator {
    DECLARE_CPU_JIT_AUX_FUNCTIONS(
            gemm_convolution_utils::jit_pp_kernel_t);

    jit_pp_kernel_t(const convolution_pd_t *pd, const conv_gemm_conf_t &jcp)
            : pp_kernel_t(pd, jcp), jit_generator(jit_name()), idx_compute_vreg_start_(0), idx_compute_vreg_max_(isa == avx512_core ? 31 : 15) {
        if (utils::one_of(isa, avx2, sse41)) {
            idx_compute_vreg_start_ += 1;   //  Vmm(0) - for masks
        }
        size_t prelu_tmp_vmm_idx = 0;
        if (post_ops_.find(primitive_kind::prelu))  {
            // prelu need a temp vmm
            prelu_tmp_vmm_idx = idx_compute_vreg_start_;
            idx_compute_vreg_start_++;
        }

        bool only_eltwise = true;
        bool with_binary = false;
        for (int i = 0; i < post_ops_.len(); i++) {
            auto &post_op = post_ops_.entry_[i];
            if (post_op.is_eltwise()) {
                jit_eltwise_injectors_.push_back(new jit_uni_eltwise_injector<isa>(
                        this, post_op.eltwise, data_type::f32, true, eltwise_reserved_1_, eltwise_reserved_2_));
            } else if (post_op.is_binary()) {
                with_binary = true;
                only_eltwise = false;
            } else if (post_op.is_depthwise()) {
                only_eltwise = false;
                jit_depthwise_injectors_.push_back(new jit_uni_depthwise_injector_f32<isa>(
                        this, post_op, depthwise_reserved_2_));
            } else {
                only_eltwise = false;
            }
        }
        if (with_binary) {
#define PARAM_OFF(field) offsetof(ker_args_t, field)
            static constexpr bool preserve_gpr = true;
            static constexpr bool preserve_vmm = true;
            static constexpr size_t helper_vmm_idx = 15;
            static constexpr size_t tail_size = 0;
            static constexpr bool use_exact_tail_scalar_bcast = false;
            const binary_injector::rhs_arg_static_params_t rhs_sp {
                helper_vmm_idx, r13, r14, r15, preserve_gpr,
                preserve_vmm, PARAM_OFF(post_ops_binary_rhs_arg_vec),
                PARAM_OFF(dst_orig), memory_desc_wrapper(pd->dst_md()),
                tail_size, kreg_rem_mask, use_exact_tail_scalar_bcast, prelu_tmp_vmm_idx};
#undef PARAM_OFF
            const binary_injector::static_params_t bsp {this->reg_abi_bak, rhs_sp};
            jit_binary_injector_ = utils::make_unique<
                    binary_injector::jit_uni_binary_injector_t<isa>>(
                    this, bsp);            
        }
        if (post_ops_.len() > 0 && !only_eltwise) {
            vreg_d_weights = Vmm(idx_compute_vreg_max_--);
            vreg_d_bias = Vmm(idx_compute_vreg_max_--);
        }
        if (utils::one_of(isa, avx2, sse41)) {
            vreg_zero = Vmm(idx_compute_vreg_start_++);
            vreg_tmp = Vmm(idx_compute_vreg_start_++);
        }
    }
    ~jit_pp_kernel_t() {
        for (auto inj : jit_eltwise_injectors_)
            delete inj;
        jit_eltwise_injectors_.clear();
        for (auto inj : jit_depthwise_injectors_)
            delete inj;
        jit_depthwise_injectors_.clear();
    }

    status_t create_kernel() override { return jit_generator::create_kernel(); }

    void operator()(float *dst, const float *bias, const int len, const int oc_start, const int oc_work, const int oc_stride,
                    const std::vector<const void *>& post_ops_binary_rhs_arg_vec) const override {
        for (int oc = 0; oc < oc_work; oc++) {
            ker_args_t args;
            args.dst = dst + oc * oc_stride;
            args.dst_orig = dst;
            args.bias = bias + oc_start + oc;
            args.len = len;
            args.oc_offset = oc_start + oc;
            args.post_ops_binary_rhs_arg_vec = post_ops_binary_rhs_arg_vec.data();
            jit_generator::operator()(&args);
        }
    }

    static bool post_ops_ok(const convolution_pd_t *pd) {
        const auto& post_ops = pd->attr()->post_ops_;
        auto dst_md = pd->dst_md();
        for (int i = 0; i < post_ops.len(); i++) {
            const auto &post_op = post_ops.entry_[i];
            if (post_op.is_binary()) {
                if (!binary_injector::is_supported(isa,
                        binary_injector::get_src1_desc(post_op, *dst_md),
                        *dst_md,
                        default_strategies())) {
                        return false;
                }
            }
        }
        return true;
    }

private:
    void generate() override;

    struct ker_args_t {
        float *dst;
        const float *bias;
        size_t len;
        size_t oc_offset;
        const void *post_ops_binary_rhs_arg_vec;
        float *dst_orig;
    };

    nstl::vector<jit_uni_eltwise_injector<isa> *> jit_eltwise_injectors_;
    std::unique_ptr<binary_injector::jit_uni_binary_injector_t<isa>>
            jit_binary_injector_;
    nstl::vector<jit_uni_depthwise_injector_f32<isa> *> jit_depthwise_injectors_;

    using Vmm = typename cpu_isa_traits<isa>::Vmm;
    static const size_t vlen = cpu_isa_traits<isa>::vlen / sizeof(float);

    Xbyak::Reg64 reg_param = abi_param1;
    Xbyak::Reg64 reg_dst = rdx;
    Xbyak::Reg64 reg_bias = rbx;

    Xbyak::Reg64 reg_len = r8;
    Xbyak::Reg64 reg_tmp = rcx; // intentional for shifting purposes
    Xbyak::Reg64 reg_abi_bak = rsi;
    Xbyak::Reg64 reg_oc_offset = r9;
    Xbyak::Reg64 reg_rem_mask = r10;
    Xbyak::Opmask kreg_rem_mask = k1;

    //  sse41/avx2
    Xbyak::Reg64 reg_ptr_maskmovdqu_dst = rdi; // sse41: store destination - must be rdi
    Xbyak::Label l_table;
    Xbyak::Reg64 reg_table = r12;
    Xbyak::Reg64 reg_shift_table = r13;
    Vmm vreg_mask = Vmm(0); //  sse41: mask for blendvps must be in xmm0
    Vmm vreg_zero;
    Vmm vreg_tmp;     //  post_ops

    //  post_ops
    Xbyak::Reg64 eltwise_reserved_1_ = r11;
    Xbyak::Opmask eltwise_reserved_2_ = k2;
    Xbyak::Opmask depthwise_reserved_2_ = k2;
    Xbyak::Reg64 reg_d_weights = r14;
    Xbyak::Reg64 reg_d_bias = r15;
    Xbyak::Reg64 reg_post_ops_data = rax;
    Vmm vreg_d_weights, vreg_d_bias;

    int idx_compute_vreg_start_;
    int idx_compute_vreg_max_;

    int idx_vreg_dst(int iter) {
        int idx = idx_compute_vreg_start_ + 0;
        assert(idx <= idx_compute_vreg_max_);
        return idx;
    }
    int idx_vreg_bias(int iter) {
        int idx = idx_compute_vreg_start_ + 1;
        assert(idx <= idx_compute_vreg_max_);
        return idx;
    }

    Vmm vreg_dst(int idx) { return Vmm(idx_vreg_dst(idx)); };
    Vmm vreg_bias(int idx) { return Vmm(idx_vreg_bias(idx)); };
};

template <cpu_isa_t isa>
void jit_pp_kernel_t<isa>::generate() {
    using namespace Xbyak;
    using namespace utils;

    preamble();

#define PARAM_OFF(x) offsetof(ker_args_t, x)
    mov(reg_abi_bak, reg_param);
    mov(reg_dst, ptr[reg_param + PARAM_OFF(dst)]);
    mov(reg_bias, ptr[reg_param + PARAM_OFF(bias)]);
    mov(reg_len, ptr[reg_param + PARAM_OFF(len)]);
    mov(reg_oc_offset, ptr[reg_param + PARAM_OFF(oc_offset)]);
    mov(reg_post_ops_data, ptr[reg_param + PARAM_OFF(post_ops_binary_rhs_arg_vec)]);
#undef PARAM_OFF

    if (utils::one_of(isa, avx2, sse41)) {
        uni_vpxor(vreg_zero, vreg_zero, vreg_zero);
        mov(reg_table, l_table);
    }

    auto apply_post_ops = [&](bool apply_mask) {
        int eltwise_inj_idx = 0;
        int binary_inj_idx = 0;
        int depthwise_inj_idx = 0;
        std::size_t post_ops_data_offset = 0;
        auto vreg_dst_ = vreg_dst(0);
        for (int i = 0; i < post_ops_.len(); i++) {
            auto &post_op = post_ops_.entry_[i];
            // todo: antonvor: sum?
            if (post_op.is_eltwise()) {
                jit_eltwise_injectors_[eltwise_inj_idx]->compute_vector(vreg_dst_.getIdx());
                eltwise_inj_idx++;
            } else if (post_op.is_binary()) {
                binary_injector::rhs_arg_dynamic_params_t rhs_arg_params;
                rhs_arg_params.vmm_idx_to_out_reg.emplace(vreg_dst_.getIdx(), reg_dst);
                rhs_arg_params.vmm_idx_to_out_elem_off_val.emplace(
                        vreg_dst_.getIdx(), 0 * sizeof(float));
                if (mayiuse(avx512_core) && apply_mask) 
                    rhs_arg_params.vmm_tail_idx_.emplace(vreg_dst_.getIdx());
                jit_binary_injector_->compute_vector(
                        vreg_dst_.getIdx(), binary_inj_idx, post_op, rhs_arg_params);

                binary_inj_idx++;
            } else if (post_op.is_depthwise()) {
                mov(reg_d_weights, ptr[reg_post_ops_data + post_ops_data_offset]);
                lea(reg_d_weights, ptr[reg_d_weights + reg_oc_offset * sizeof(float)]);
                jit_depthwise_injectors_[depthwise_inj_idx]->compute_vector_range(vreg_dst_.getIdx(), vreg_dst_.getIdx() + 1,
                                                                                  reg_d_weights, reg_d_weights, true);
                post_ops_data_offset += jit_depthwise_injectors_[depthwise_inj_idx]->memoryStep();
                binary_inj_idx++;
                depthwise_inj_idx++;
            } else if (post_op.is_quantization()) {
                bool do_dequantization = post_op.quantization.alg == alg_kind::quantization_quantize_dequantize;
                bool do_rounding = true;

                size_t crop_low_off = post_op.quantization.offset[post_op.quantization.crop_low] * sizeof(float);
                size_t crop_high_off = post_op.quantization.offset[post_op.quantization.crop_high] * sizeof(float);
                mov(reg_d_weights, ptr[reg_post_ops_data + post_ops_data_offset]);
                if (post_op.quantization.per_channel[post_op.quantization.crop_low]) {
                    uni_vpbroadcastd(vreg_d_weights, ptr[reg_d_weights + reg_oc_offset * sizeof(float) + crop_low_off]);
                } else {
                    uni_vpbroadcastd(vreg_d_weights, ptr[reg_d_weights + crop_low_off]);
                }

                if (post_op.quantization.per_channel[post_op.quantization.crop_high]) {
                    uni_vpbroadcastd(vreg_d_bias, ptr[reg_d_weights + reg_oc_offset * sizeof(float) + crop_high_off]);
                } else {
                    uni_vpbroadcastd(vreg_d_bias, ptr[reg_d_weights + crop_high_off]);
                }

                uni_vmaxps(vreg_dst_, vreg_dst_, vreg_d_weights);
                uni_vminps(vreg_dst_, vreg_dst_, vreg_d_bias);

                size_t inp_scale_off = post_op.quantization.offset[post_op.quantization.inp_scale] * sizeof(float);
                size_t inp_shift_off = post_op.quantization.offset[post_op.quantization.inp_shift] * sizeof(float);
                if (post_op.quantization.per_channel[post_op.quantization.inp_scale]) {
                    uni_vpbroadcastd(vreg_d_weights, ptr[reg_d_weights + reg_oc_offset * sizeof(float) + inp_scale_off]);
                } else {
                    uni_vpbroadcastd(vreg_d_weights, ptr[reg_d_weights + inp_scale_off]);
                }

                if (post_op.quantization.per_channel[post_op.quantization.inp_shift]) {
                    uni_vpbroadcastd(vreg_d_bias, ptr[reg_d_weights + reg_oc_offset * sizeof(float) + inp_shift_off]);
                } else {
                    uni_vpbroadcastd(vreg_d_bias, ptr[reg_d_weights + inp_shift_off]);
                }

                uni_vfmadd213ps(vreg_dst_, vreg_d_weights, vreg_d_bias);

                if (do_rounding)
                    uni_vroundps(vreg_dst_, vreg_dst_, 0);

                size_t output_scale_off = post_op.quantization.offset[post_op.quantization.output_scale] * sizeof(float);
                size_t output_shift_off = post_op.quantization.offset[post_op.quantization.output_shift] * sizeof(float);
                if (do_dequantization) {
                    if (post_op.quantization.per_channel[post_op.quantization.output_scale]) {
                        uni_vpbroadcastd(vreg_d_weights, ptr[reg_d_weights + reg_oc_offset * sizeof(float) + output_scale_off]);
                    } else {
                        uni_vpbroadcastd(vreg_d_weights, ptr[reg_d_weights + output_scale_off]);
                    }

                    if (post_op.quantization.per_channel[post_op.quantization.output_shift]) {
                        uni_vpbroadcastd(vreg_d_bias, ptr[reg_d_weights + reg_oc_offset * sizeof(float) + output_shift_off]);
                    } else {
                        uni_vpbroadcastd(vreg_d_bias, ptr[reg_d_weights + output_shift_off]);
                    }

                    uni_vfmadd213ps(vreg_dst_, vreg_d_weights, vreg_d_bias);
                }

                post_ops_data_offset += sizeof(float*);
                binary_inj_idx++;
            }
        }
    };

    // Load accumulated value, convert to float, apply bias (if any), scaling,
    // and eltwise (if any); then convert to destination type and store
    auto compute = [&](bool apply_mask) {
        auto dst_addr = ptr[reg_dst];
        auto vreg_dst_ = vreg_dst(0);
        if (isa == avx512_core) {
            if (apply_mask)
                vreg_dst_ = vreg_dst_ | kreg_rem_mask;
            uni_vmovups(vreg_dst_, dst_addr);
        } else {
            if (apply_mask) {
                if (isa != sse41) {
                    vmaskmovps(vreg_tmp, vreg_mask, dst_addr);
                    uni_vblendvps(vreg_dst_, vreg_zero, vreg_tmp, vreg_mask);
                } else {
                    uni_vmovups(vreg_dst_, dst_addr);
                }
            } else {
                uni_vmovups(vreg_dst_, dst_addr);
            }
        }

        if (do_bias_) {
            auto vreg_bias_ = vreg_bias(0);
            if (isa == avx512_core && apply_mask)
                vreg_bias_ = vreg_bias_ | kreg_rem_mask;

            uni_vpbroadcastd(vreg_bias_, ptr[reg_bias]);
            uni_vaddps(vreg_dst_, vreg_dst_, vreg_bias_);
        }

        apply_post_ops(apply_mask);

        if (isa == avx512_core) {
            uni_vmovups(dst_addr, vreg_dst_);
        } else {
            if (apply_mask) {
                if (isa != sse41) {
                    vmaskmovps(dst_addr, vreg_mask, vreg_dst_);
                } else {
                    lea(reg_ptr_maskmovdqu_dst, dst_addr);
                    maskmovdqu(vreg_dst_, vreg_mask);
                }
            } else {
                uni_vmovups(dst_addr, vreg_dst_);
            }
        }
    };

    Label loop_end;
    {
        cmp(reg_len, 0);
        je(loop_end, T_NEAR);

        Label loop, loop_tail;
        cmp(reg_len, vlen);
        jl(loop_tail, T_NEAR);
        L(loop); {
            compute(false);
            sub(reg_len, vlen);
            add(reg_dst, vlen * sizeof(float));
            cmp(reg_len, vlen);
            jge(loop, T_NEAR);
        }

        L(loop_tail);
        mov(reg_tmp, reg_len); // reg_tmp is rcx, and we need cl for the shift
        if (isa == avx512_core) {
            mov(reg_rem_mask, 1);
            shl(reg_rem_mask, cl); // reg_tmp == rcx and reg_tail < vlen == 16
            sub(reg_rem_mask, 1);
            jz(loop_end, T_NEAR);
            kmovq(kreg_rem_mask, reg_rem_mask);
        } else {
            mov(reg_shift_table, vlen);
            sub(reg_shift_table, reg_tmp);
            uni_vmovups(vreg_mask, ptr[reg_table + reg_shift_table * sizeof(float)]);
        }
        compute(true);
    }
    L(loop_end);

    postamble();

    for (auto& inj : jit_eltwise_injectors_)
        inj->prepare_table();

    if (utils::one_of(isa, avx2, sse41)) {
        align(64);
        L(l_table);
        for (size_t i = 0; i < vlen; i++) dd(0xFFFFFFFF);
        for (size_t i = 0; i < vlen; i++) dd(0x00000000);
    }
}

pp_kernel_t *jit_pp_kernel_create(
        const convolution_pd_t *pd, const conv_gemm_conf_t &jcp) {
    if (mayiuse(avx512_core) && jit_pp_kernel_t<avx512_core>::post_ops_ok(pd)) {
        return new jit_pp_kernel_t<avx512_core>(pd, jcp);
    } else if (mayiuse(avx2) && jit_pp_kernel_t<avx2>::post_ops_ok(pd)) {
        return new jit_pp_kernel_t<avx2>(pd, jcp);
    } else if (mayiuse(sse41) && jit_pp_kernel_t<sse41>::post_ops_ok(pd)) {
        return new jit_pp_kernel_t<sse41>(pd, jcp);
    }
    return nullptr;
}

} // namespace gemm_convolution_utils
} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl
