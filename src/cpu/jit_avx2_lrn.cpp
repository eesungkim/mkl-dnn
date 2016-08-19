/*******************************************************************************
* Copyright 2016 Intel Corporation
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

#include "mkldnn_types.h"

#include "c_types_map.hpp"
#include "jit_avx2_lrn.hpp"
#include "type_helpers.hpp"

#if 1
#include "jit_generator.hpp"

class xbyak_lrn : public mkldnn::impl::cpu::jit_generator
{
public:
    Xbyak::Reg64 src = rax;
    Xbyak::Reg64 dst = r8;
    Xbyak::Reg64 scratch = rdx;
    Xbyak::Reg64 hw = r9;
    Xbyak::Reg64 t = rsp;
    Xbyak::Reg64 imm_addr64 = rbx;

    Xbyak::Ymm yalpha = ymm0;
    Xbyak::Ymm ysrc_prev = ymm1;
    Xbyak::Ymm ysrc = ymm2;
    Xbyak::Ymm ysrc_next = ymm3;
    Xbyak::Ymm ya = ymm4;
    Xbyak::Ymm yb = ymm5;
    Xbyak::Ymm yc = ymm2;
    Xbyak::Ymm yd = ymm6;
    Xbyak::Ymm ye = ymm7;
    Xbyak::Ymm yone = ymm8;
    Xbyak::Ymm ysum = ymm9;
    Xbyak::Ymm ysum2 = ymm10;
    Xbyak::Ymm ydst = ymm11;
    Xbyak::Ymm ybase = ymm12;

    xbyak_lrn(
        float *run_time_ptr_alpha,
        float *run_time_ptr_one,
        uint32_t compile_time_HW,
        int version, // -1 channels 0..7, 1 channels C-8 .. C-1, 0 -- other channels
        void* code_ptr = nullptr,
        size_t code_size = 1 * Xbyak::DEFAULT_MAX_CODE_SIZE)
        :
        jit_generator(code_ptr, code_size)
    {
            if (compile_time_HW == 0)
            {
                ret();
                return;
            }
            this->preamble();

            mov(src, ptr[this->param1 + 0]);
            mov(dst, ptr[this->param1 + 8]);
            mov(scratch, ptr[this->param1+ 16]);
            sub(t, 96);
            mov(imm_addr64, reinterpret_cast<size_t>(run_time_ptr_alpha));
            vbroadcastss(yalpha, ptr[imm_addr64]);
            mov(imm_addr64, reinterpret_cast<size_t>(run_time_ptr_one));
            vbroadcastss(yone, ptr[imm_addr64]);
            if (version == -1)
            {
                vxorps(ysrc_prev, ysrc_prev, ysrc_prev);
                vmovups(ptr[t + 0], ysrc_prev);
            }
            if (version == +1)
            {
                vxorps(ysrc_next, ysrc_next, ysrc_next);
                vmovups(ptr[t + 64], ysrc_next);
            }

            mov(hw, compile_time_HW);
            L(".lrn_loop");

            if (version != -1) vmovups(ysrc_prev, ptr[src - compile_time_HW * 32]);
            vmovups(ysrc, ptr[src]);
            if (version != +1) vmovups(ysrc_next, ptr[src + compile_time_HW * 32]);

            if (version != -1) vmovups(ptr[t + 0], ysrc_prev);
            vmovups(ptr[t + 32], ysrc);
            if (version != +1) vmovups(ptr[t + 64], ysrc_next);

            vmovups(ya, ptr[t + 32 - 8]);
            vmovups(yb, ptr[t + 32 - 4]);
            vmovups(yd, ptr[t + 32 + 4]);
            vmovups(ye, ptr[t + 32 + 8]);
            vmulps(ysum, yc, yc);
            vfmadd231ps(ysum, ya, ya); // ysum <- ysum + ya*ya
            vfmadd231ps(ysum, yb, yb);
            vfmadd231ps(ysum, yd, yd);
            vfmadd231ps(ysum, ye, ye);

            vfmadd132ps(ysum, yone, yalpha); // ysum <- ysum*yalpha+yone
            vmovaps(ybase, ysum);
            vmulps(ysum2, ysum, ysum);
            vmulps(ysum, ysum, ysum2); // ysum = ybase^3;
            vsqrtps(ysum, ysum);
            vsqrtps(ysum, ysum); // ysum = ybase^0.75
            vdivps(ydst, ysrc, ysum); // ydst = ysrc / ysum
            vmulps(ysum, ysum, ybase); // ysum = ybase ^ 1.75 -- for back prop
            vmovups(ptr[dst], ydst);
            vmovups(ptr[scratch], ysum);

            add(src, 32);
            add(dst, 32);
            add(scratch, 32);
            dec(hw);
            cmp(hw, 0);
            jne(".lrn_loop", T_NEAR);

            add(t, 96);
            this->postamble();
            return;
        }
};
#endif

namespace mkldnn { namespace impl { namespace cpu {

using namespace mkldnn::impl::status;
using namespace mkldnn::impl::prop_kind;
using namespace mkldnn::impl::alg_kind;
using namespace mkldnn::impl::precision;
using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::primitive_kind;

enum { VECTOR_LENGTH = 8 };
typedef struct {
    const float *src;
    float *dst, *scratch;
} jit_args_t;

template <impl::precision_t prec>
jit_avx2_lrn<prec>::jit_avx2_lrn(const lrn_primitive_desc_t &lpd,
    const primitive_at_t *inputs, const primitive *outputs[])
    : lrn<jit_avx2_lrn<prec>>(lpd, inputs, outputs)
    , jit_alpha(lpd.lrn_desc.alpha/lpd.lrn_desc.local_size)
    , jit_one(1.0) {
    uint32_t H = this->_lpd.src_primitive_desc.memory_desc.tensor_desc.dims[2];
    uint32_t W = this->_lpd.src_primitive_desc.memory_desc.tensor_desc.dims[3];

    typedef void (*kernel_t)(const void *);
    this->jit_lrn = new xbyak_lrn(&this->jit_alpha, &this->jit_one, H*W, 0);
    this->ker_hw8 = reinterpret_cast<kernel_t>(
            const_cast<uint8_t*>(this->jit_lrn->getCode()));

    this->jit_lrn_first =
        new xbyak_lrn(&this->jit_alpha, &this->jit_one, H*W, -1);
    this->ker_hw8_first = reinterpret_cast<kernel_t>(
            const_cast<uint8_t*>(this->jit_lrn_first->getCode()));

    this->jit_lrn_last =
        new xbyak_lrn(&this->jit_alpha, &this->jit_one, H*W, +1);
    this->ker_hw8_last = reinterpret_cast<kernel_t>(
            const_cast<uint8_t*>(this->jit_lrn_last->getCode()));
}

template <impl::precision_t prec>
jit_avx2_lrn<prec>::~jit_avx2_lrn() {
    delete this->jit_lrn;
    delete this->jit_lrn_first;
    delete this->jit_lrn_last;
}

template <impl::precision_t prec>
status_t jit_avx2_lrn<prec>::execute_forward() {
    auto src = reinterpret_cast<const data_t *>(
            this->input()[0].primitive->output()[
            this->input()[0].output_index]->memory_const());
    auto scratch = reinterpret_cast<data_t *>(
            this->input()[1].primitive->output()[
            this->input()[1].output_index]->memory());
    auto dst = reinterpret_cast<data_t *>(this->output()[0]->memory());

    const memory_desc_wrapper src_d(this->_lpd.src_primitive_desc.memory_desc);

    const uint32_t C = src_d.dims()[1];
    const uint32_t HW = src_d.dims()[2]*src_d.dims()[3];

    const uint32_t N = src_d.dims()[0];
#   pragma omp parallel for collapse(2)
    for (uint32_t n = 0; n < N; ++n) {
        for (uint32_t c8 = 0; c8 < C / VECTOR_LENGTH; ++c8) {
            jit_args_t args;
            args.src     = &src    [n*HW*C + c8 * HW * VECTOR_LENGTH];
            args.dst     = &dst    [n*HW*C + c8 * HW * VECTOR_LENGTH];
            args.scratch = &scratch[n*HW*C + c8 * HW * VECTOR_LENGTH];
            if (c8 == 0)
                ker_hw8_first(&args);
            else if (c8 == C / VECTOR_LENGTH - 1)
                ker_hw8_last(&args);
            else
                ker_hw8(&args);
        }
    }

    return success;
}

template <impl::precision_t prec>
status_t jit_avx2_lrn<prec>::set_default_parameters(lrn_desc_t &lrn_d) {
    if (lrn_d.src_desc.format == any)
        CHECK(lrn_set_default_format<prec>(lrn_d.src_desc, nChw8c));
    if (lrn_d.dst_desc.format == any)
        CHECK(lrn_set_default_format<prec>(lrn_d.dst_desc, nChw8c));
    return status::success;
}

template <impl::precision_t prec>
status_t jit_avx2_lrn<prec>::constraint(const lrn_desc_t &lrn_d) {
    const memory_desc_wrapper src_d(lrn_d.src_desc);

    bool args_ok = true
        && lrn_d.alg_kind == lrn_across_channels
        && src_d.ndims() == 4
        && src_d.dims()[1] % VECTOR_LENGTH == 0
        && src_d.dims()[1] >= 2*VECTOR_LENGTH
        && lrn_d.beta == 0.75
        && lrn_d.local_size == 5
        && src_d.format() == nChw8c
        && lrn_d.dst_desc.format == nChw8c;

    return args_ok ? success : unimplemented;
}

template <impl::precision_t prec>
const primitive_impl jit_avx2_lrn<prec>::implementation = {
    jit_avx2_lrn<prec>::create
};

template class jit_avx2_lrn<precision::f32>;

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
