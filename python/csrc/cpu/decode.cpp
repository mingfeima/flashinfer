#include "flashinfer_ops.h"
#include "common.h"
#include "vec.h"

namespace {

// [NOTE] TODO list for this kernel:
//   1. tune the value for BLOCK_N
//   2. planning for {batches, num_heads, num_kv_splits}
//      and use actual num_kv_splits for small seq length
//   3. try fast impl of `.tanh()`
//   4. provide amx kernel for index_gemm_kernel_nn when M = 16
//

inline void fill_stub(float* __restrict__ out, float val, int size) {
  using Vec = at::vec::Vectorized<float>;
  const Vec data_vec(val);
  at::vec::map<float>([data_vec](Vec out) { return out = data_vec; }, out, out, size);
}

template <typename scalar_t>
inline void copy_stub(scalar_t* __restrict__ out, const float* __restrict__ acc, float s, int size) {
  using bVec = at::vec::Vectorized<scalar_t>;
  using fVec = at::vec::Vectorized<float>;
  const fVec s_fvec = fVec(s);
  int d = 0;
  for (; d <= size - bVec::size(); d += bVec::size()) {
    fVec a_fvec0 = fVec::loadu(acc + d) * s_fvec;
    fVec a_fvec1 = fVec::loadu(acc + d + fVec::size()) * s_fvec;
    bVec out_bvec = convert_from_float_ext<scalar_t>(a_fvec0, a_fvec1);
    out_bvec.store(out + d);
  }
  for (; d < size; ++d) {
    out[d] = static_cast<scalar_t>(acc[d] * s);
  }
}

// GEMM handles query @ key (indexed) x scale
//   A : [M, K]
//   B : [N, K] indexed
//   C : [M, N]
//
template <typename scalar_t, typename index_t, int BLOCK_M, int BLOCK_N>
struct tinygemm_kernel_nt {
  static inline void apply(
      const scalar_t* __restrict__ A, const scalar_t* __restrict__ B,
      float* __restrict__ C, const index_t* __restrict__ indices,
      float scale, int lda, int ldb, int ldc, int K, int max_tokens) {

    for (int m = 0; m < BLOCK_M; ++m) {
      for (int n = 0; n < BLOCK_N; ++n) {
        float sum = 0.f;
        int b_idx = indices[n];
        TORCH_CHECK(b_idx < max_tokens, "token index out of scope!");
        for (int k = 0; k < K; ++k) {
          sum += scale * static_cast<float>(A[m * lda + k]) * static_cast<float>(B[b_idx * ldb + k]);
        }
        C[m * ldc + n] = sum;
      }
    }
  }
};

#if defined(CPU_CAPABILITY_AVX512)
template <typename index_t, int BLOCK_M, int BLOCK_N>
struct tinygemm_kernel_nt<at::BFloat16, index_t, BLOCK_M, BLOCK_N> {
  static inline void apply(
      const at::BFloat16* __restrict__ A, const at::BFloat16* __restrict__ B,
      float* __restrict__ C, const index_t* __restrict__ indices,
      float scale, int lda, int ldb, int ldc, int K, int max_tokens) {

    constexpr int ROWS = BLOCK_M;
    constexpr int COLS = BLOCK_N;

    __m512bh va;
    __m512bh vb[COLS];
    __m512 vc[ROWS * COLS];
    __m512 vscale = _mm512_set1_ps(scale);

    auto loadc = [&](auto i) {
      vc[i] = _mm512_setzero_ps();
    };
    Unroll<ROWS * COLS>{}(loadc);

    // for main loop
    auto compute = [&](auto i, int k) {
      constexpr int row = i / COLS;
      constexpr int col = i % COLS;

      if constexpr (col == 0) {
        va = (__m512bh)(_mm512_loadu_si512(A + row * lda + k));
      }
      if constexpr (row == 0) {
        if constexpr (col + 1 < COLS) {
          int b_idx_prefetch = indices[col + 1];
           _mm_prefetch(B + b_idx_prefetch * ldb + k, _MM_HINT_T0);
        }
        int b_idx = indices[col];
        TORCH_CHECK(b_idx < max_tokens, "token index out of scope!");
        vb[col] = (__m512bh)(_mm512_loadu_si512(B + b_idx * ldb + k));
      }
      vc[i] = _mm512_dpbf16_ps(vc[i], va, vb[col]);
    };

    // for remainder
    auto compute2 = [&](auto i, int k, __mmask32 mask) {
      constexpr int row = i / COLS;
      constexpr int col = i % COLS;

      if constexpr (col == 0) {
        va = (__m512bh)(_mm512_maskz_loadu_epi16(mask, A + row * lda + k));
      }
      if constexpr (row == 0) {
        int b_idx = indices[col];
        TORCH_CHECK(b_idx < max_tokens, "token index out of scope!");
        vb[col] = (__m512bh)(_mm512_maskz_loadu_epi16(mask, B + b_idx * ldb + k));
      }
      vc[i] = _mm512_dpbf16_ps(vc[i], va, vb[col]);
    };

    int k = 0;
    for (; k <= K - 32; k += 32) {
      Unroll<ROWS * COLS>{}(compute, k);
    }
    int count = K - k;
    if (count > 0) {
      __mmask32 mask = (1ULL << count) - 1;
      Unroll<ROWS * COLS>{}(compute2, k, mask);
    }

    auto storec = [&](auto i) {
      constexpr int row = i / COLS;
      constexpr int col = i % COLS;
      C[row * ldc + col] = _mm512_reduce_add_ps(_mm512_mul_ps(vc[i], vscale));
    };
    Unroll<ROWS * COLS>{}(storec);
  }
};
#endif

#define LAUNCH_TINYGEMM_KERNEL_NT(MB_SIZE, NB_SIZE)                         \
    tinygemm_kernel_nt<scalar_t, index_t, MB_SIZE, NB_SIZE>::apply(         \
        A + mb_start * lda, B, C + mb_start * ldc + nb_start,               \
        indices + nb_start, scale,                                          \
        lda, ldb, ldc, K, max_tokens);


// this is used when N isn't multiple of 16,
// N corresponds to `head_size_v` which should be 16x
template <typename scalar_t, typename index_t>
inline void tinygemm_kernel_nn_scalar(
    const float* __restrict__ A, const scalar_t* __restrict__ B,
    float* __restrict__ C, const index_t* __restrict__ indices, const float* __restrict__ scale,
    int M, int N, int K, int lda, int ldb, int ldc, int max_tokens) {

  for (int m = 0; m < M; ++m) {
    for (int n = 0; n < N; ++n) {
      C[m * ldc + n] *= scale[m];
      for (int k = 0; k < K; ++k) {
        int b_idx = indices[k];
        TORCH_CHECK(b_idx < max_tokens, "token index out of scope!");
        C[m * ldc + n] += A[m * lda + k] * static_cast<float>(B[b_idx * ldb + n]);
      }
    }
  }
}

// GEMM handles v' * scale + attn @ value (indexed)
//   A : [M, K]
//   B : [K, N] indexed
//   C ：[M, N]
//
template <typename scalar_t, typename index_t, int BLOCK_M, int BLOCK_N>
struct tinygemm_kernel_nn {
  static inline void apply(
      const float* __restrict__ A, const scalar_t* __restrict__ B,
      float* __restrict__ C, const index_t* __restrict__ indices, const float* __restrict__ scale,
      int lda, int ldb, int ldc, int K, int max_tokens) {
    tinygemm_kernel_nn_scalar(A, B, C, indices, scale, BLOCK_M, BLOCK_N, K, lda, ldb, ldc, max_tokens);
  }
};

#if defined(CPU_CAPABILITY_AVX512)
template <typename index_t, int BLOCK_M, int BLOCK_N>
struct tinygemm_kernel_nn<at::BFloat16, index_t, BLOCK_M, BLOCK_N> {
  static inline void apply(
      const float* __restrict__ A, const at::BFloat16* __restrict__ B,
      float* __restrict__ C, const index_t* __restrict__ indices, const float* __restrict__ scale,
      int lda, int ldb, int ldc, int K, int max_tokens) {

    constexpr int ROWS = BLOCK_M;
    constexpr int COLS = BLOCK_N / 16;

    __m512 va;
    __m512 vb[COLS];
    __m512 vc[ROWS * COLS];
    __m512 vscale;

    auto loadc = [&](auto i) {
      constexpr int row = i / COLS;
      constexpr int col = i % COLS;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
      if constexpr (col == 0) {
        vscale = _mm512_set1_ps(scale[row]);
      }
#pragma GCC diagnostic pop
      vc[i] = _mm512_loadu_ps(C + row * ldc + col * 16);
      vc[i] = _mm512_mul_ps(vc[i], vscale);
    };
    Unroll<ROWS * COLS>{}(loadc);

    auto compute = [&](auto i, int k) {
      constexpr int row = i / COLS;
      constexpr int col = i % COLS;

      if constexpr (col == 0) {
        va = _mm512_set1_ps(A[row * lda + k]);
      }
      if constexpr (row == 0) {
        if (k + 1 < K) {
          int b_idx_prefetch = indices[k + 1];
          _mm_prefetch(B + b_idx_prefetch * ldb + col * 16, _MM_HINT_T0);
        }
        int b_idx = indices[k];
        TORCH_CHECK(b_idx < max_tokens, "token index out of scope!");

        // for COLS = 2, 4, 6, 8 use 512 bit load
        // for COLS = 1, 3, 5, 7 use 256 bit load
        if constexpr (COLS % 2 == 0) {
          if constexpr (col % 2  == 0) {
            __m512i b16 = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(B + b_idx * ldb + col * 16));
            vb[col + 0] = CVT_BF16_TO_FP32(_mm512_extracti32x8_epi32(b16, 0));
            vb[col + 1] = CVT_BF16_TO_FP32(_mm512_extracti32x8_epi32(b16, 1));
          }
        } else {
          __m256i b16 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(B + b_idx * ldb + col * 16));
          vb[col] = CVT_BF16_TO_FP32(b16);
        }
      }
      vc[i] = _mm512_fmadd_ps(va, vb[col], vc[i]);
    };

    for (int k = 0; k < K; ++k) {
      Unroll<ROWS * COLS>{}(compute, k);
    }

    auto storec = [&](auto i) {
      constexpr int row = i / COLS;
      constexpr int col = i % COLS;
      _mm512_storeu_ps(C + row * ldc + col * 16, vc[i]);
    };
    Unroll<ROWS * COLS>{}(storec);
  }
};
#endif

#define LAUNCH_TINYGEMM_KERNEL_NN(MB_SIZE, NB_SIZE)                         \
    tinygemm_kernel_nn<scalar_t, index_t, MB_SIZE, NB_SIZE>::apply(         \
        A + mb_start * lda, B + nb_start, C + mb_start * ldc + nb_start,    \
        indices, scale + mb_start,                                          \
        lda, ldb, ldc, K, max_tokens);


template <typename scalar_t, typename index_t>
void index_gemm_kernel_nt(
    const scalar_t* __restrict__ A, const scalar_t* __restrict__ B,
    float* __restrict__ C, const index_t* __restrict__ indices,
    float scale, int M, int N, int K, int lda, int ldb, int ldc, int max_tokens) {

  // pattern: 1-8-8
  if (M == 1) {
    constexpr int BLOCK_N = 8;
    const int NB = div_up(N, BLOCK_N);
    int mb_start = 0, lda = 1, ldc = 1;

    for (int nb = 0; nb < NB; ++nb) {
      int nb_start = nb * BLOCK_N;
      int nb_size = std::min(BLOCK_N, N - nb_start);

      switch(nb_size) {
        case 1: LAUNCH_TINYGEMM_KERNEL_NT(1, 1); break;
        case 2: LAUNCH_TINYGEMM_KERNEL_NT(1, 2); break;
        case 3: LAUNCH_TINYGEMM_KERNEL_NT(1, 3); break;
        case 4: LAUNCH_TINYGEMM_KERNEL_NT(1, 4); break;
        case 5: LAUNCH_TINYGEMM_KERNEL_NT(1, 5); break;
        case 6: LAUNCH_TINYGEMM_KERNEL_NT(1, 6); break;
        case 7: LAUNCH_TINYGEMM_KERNEL_NT(1, 7); break;
        case 8: LAUNCH_TINYGEMM_KERNEL_NT(1, 8); break;
        default: TORCH_CHECK(false, "Unexpected block size, 1x", "nb_size");
      }
    }
    return;
  }

  // pattern: 1-6-24
  constexpr int BLOCK_M = 4;
  constexpr int BLOCK_N = 6;
  const int MB = div_up(M, BLOCK_M);
  const int NB = div_up(N, BLOCK_N);

  for (int mb = 0; mb < MB; ++mb) {
    int mb_start = mb * BLOCK_M;
    int mb_size = std::min(BLOCK_M, M - mb_start);
    for (int nb = 0; nb < NB; ++nb) {
      int nb_start = nb * BLOCK_N;
      int nb_size = std::min(BLOCK_N, N - nb_start);

      switch(mb_size << 4 | nb_size) {
        // mb_size = 1
        // case 0x11: LAUNCH_TINYGEMM_KERNEL_NT(1, 1); break;
        // case 0x12: LAUNCH_TINYGEMM_KERNEL_NT(1, 2); break;
        // case 0x13: LAUNCH_TINYGEMM_KERNEL_NT(1, 3); break;
        // case 0x14: LAUNCH_TINYGEMM_KERNEL_NT(1, 4); break;
        // case 0x15: LAUNCH_TINYGEMM_KERNEL_NT(1, 5); break;
        // case 0x16: LAUNCH_TINYGEMM_KERNEL_NT(1, 6); break;
        // mb_size = 2
        case 0x21: LAUNCH_TINYGEMM_KERNEL_NT(2, 1); break;
        case 0x22: LAUNCH_TINYGEMM_KERNEL_NT(2, 2); break;
        case 0x23: LAUNCH_TINYGEMM_KERNEL_NT(2, 3); break;
        case 0x24: LAUNCH_TINYGEMM_KERNEL_NT(2, 4); break;
        case 0x25: LAUNCH_TINYGEMM_KERNEL_NT(2, 5); break;
        case 0x26: LAUNCH_TINYGEMM_KERNEL_NT(2, 6); break;
        // mb_size = 3
        case 0x31: LAUNCH_TINYGEMM_KERNEL_NT(3, 1); break;
        case 0x32: LAUNCH_TINYGEMM_KERNEL_NT(3, 2); break;
        case 0x33: LAUNCH_TINYGEMM_KERNEL_NT(3, 3); break;
        case 0x34: LAUNCH_TINYGEMM_KERNEL_NT(3, 4); break;
        case 0x35: LAUNCH_TINYGEMM_KERNEL_NT(3, 5); break;
        case 0x36: LAUNCH_TINYGEMM_KERNEL_NT(3, 6); break;
        // mb_size = 4
        case 0x41: LAUNCH_TINYGEMM_KERNEL_NT(4, 1); break;
        case 0x42: LAUNCH_TINYGEMM_KERNEL_NT(4, 2); break;
        case 0x43: LAUNCH_TINYGEMM_KERNEL_NT(4, 3); break;
        case 0x44: LAUNCH_TINYGEMM_KERNEL_NT(4, 4); break;
        case 0x45: LAUNCH_TINYGEMM_KERNEL_NT(4, 5); break;
        case 0x46: LAUNCH_TINYGEMM_KERNEL_NT(4, 6); break;
        default: TORCH_CHECK(false, "Unexpected block size, ", mb_size, "x", "nb_size");
      }
    }
  }
}

template <typename scalar_t, typename index_t>
void index_gemm_kernel_nn(
    const float* __restrict__ A, const scalar_t* __restrict__ B,
    float* __restrict__ C, const index_t* __restrict__ indices, float* __restrict__ scale,
    int M, int N, int K, int lda, int ldb, int ldc, int max_tokens) {

  constexpr int kVecSize = 16;
  if ((N & (kVecSize - 1)) != 0) {
    tinygemm_kernel_nn_scalar(A, B, C, indices, scale, M, N, K, lda, ldb, ldc, max_tokens);
    return;
  }

  // pattern: 1-8-8
  if (M == 1) {
    constexpr int BLOCK_N = 8 * kVecSize;
    const int NB = div_up(N, BLOCK_N);
    int mb_start = 0, lda = 1, ldc = 1;

    for (int nb = 0; nb < NB; ++nb) {
      int nb_start = nb * BLOCK_N;
      int nb_size = std::min(BLOCK_N, N - nb_start);

      switch(nb_size >> 4) {
        case 1: LAUNCH_TINYGEMM_KERNEL_NN(1, 16); break;
        case 2: LAUNCH_TINYGEMM_KERNEL_NN(1, 32); break;
        case 3: LAUNCH_TINYGEMM_KERNEL_NN(1, 48); break;
        case 4: LAUNCH_TINYGEMM_KERNEL_NN(1, 64); break;
        case 5: LAUNCH_TINYGEMM_KERNEL_NN(1, 80); break;
        case 6: LAUNCH_TINYGEMM_KERNEL_NN(1, 96); break;
        case 7: LAUNCH_TINYGEMM_KERNEL_NN(1, 112); break;
        case 8: LAUNCH_TINYGEMM_KERNEL_NN(1, 128); break;
        default: TORCH_CHECK(false, "Unexpected block size, 1x", "nb_size");
      }
    }
    return;
  }

  constexpr int BLOCK_M = 4;
  constexpr int BLOCK_N = 6 * kVecSize;
  const int MB = div_up(M, BLOCK_M);
  const int NB = div_up(N, BLOCK_N);

  for (int mb = 0; mb < MB; ++mb) {
    int mb_start = mb * BLOCK_M;
    int mb_size = std::min(BLOCK_M, M - mb_start);
    for (int nb = 0; nb < NB; ++nb) {
      int nb_start = nb * BLOCK_N;
      int nb_size = std::min(BLOCK_N, N - nb_start);

      switch(mb_size << 4 | nb_size >> 4) {
        // mb_size = 1
        // case 0x11: LAUNCH_TINYGEMM_KERNEL_NN(1, 16); break;
        // case 0x12: LAUNCH_TINYGEMM_KERNEL_NN(1, 32); break;
        // case 0x13: LAUNCH_TINYGEMM_KERNEL_NN(1, 48); break;
        // case 0x14: LAUNCH_TINYGEMM_KERNEL_NN(1, 64); break;
        // case 0x15: LAUNCH_TINYGEMM_KERNEL_NN(1, 80); break;
        // case 0x16: LAUNCH_TINYGEMM_KERNEL_NN(1, 96); break;
        // mb_size = 2
        case 0x21: LAUNCH_TINYGEMM_KERNEL_NN(2, 16); break;
        case 0x22: LAUNCH_TINYGEMM_KERNEL_NN(2, 32); break;
        case 0x23: LAUNCH_TINYGEMM_KERNEL_NN(2, 48); break;
        case 0x24: LAUNCH_TINYGEMM_KERNEL_NN(2, 64); break;
        case 0x25: LAUNCH_TINYGEMM_KERNEL_NN(2, 80); break;
        case 0x26: LAUNCH_TINYGEMM_KERNEL_NN(2, 96); break;
        // mb_size = 3
        case 0x31: LAUNCH_TINYGEMM_KERNEL_NN(3, 16); break;
        case 0x32: LAUNCH_TINYGEMM_KERNEL_NN(3, 32); break;
        case 0x33: LAUNCH_TINYGEMM_KERNEL_NN(3, 48); break;
        case 0x34: LAUNCH_TINYGEMM_KERNEL_NN(3, 64); break;
        case 0x35: LAUNCH_TINYGEMM_KERNEL_NN(3, 80); break;
        case 0x36: LAUNCH_TINYGEMM_KERNEL_NN(3, 96); break;
        // mb_size = 4
        case 0x41: LAUNCH_TINYGEMM_KERNEL_NN(4, 16); break;
        case 0x42: LAUNCH_TINYGEMM_KERNEL_NN(4, 32); break;
        case 0x43: LAUNCH_TINYGEMM_KERNEL_NN(4, 48); break;
        case 0x44: LAUNCH_TINYGEMM_KERNEL_NN(4, 64); break;
        case 0x45: LAUNCH_TINYGEMM_KERNEL_NN(4, 80); break;
        case 0x46: LAUNCH_TINYGEMM_KERNEL_NN(4, 96); break;
        default: TORCH_CHECK(false, "Unexpected block size, ", mb_size, "x", "nb_size");
      }
    }
  }
}

template <typename scalar_t, typename index_t>
void decode_attention_kernel_impl(
    scalar_t* __restrict__ output,
    float* __restrict__ attn_logits,
    const scalar_t* __restrict__ query,
    const scalar_t* __restrict__ k_cache,
    const scalar_t* __restrict__ v_cache,
    const index_t* __restrict__ req_to_token,
    const index_t* __restrict__ req_pool_indices,
    const index_t* __restrict__ seq_lens,
    int batches,
    int num_heads,
    int head_size,
    int head_size_v,
    int num_kv_splits,
    float scaling,
    float logit_cap,
    int max_num_reqs,
    int max_context_len,
    int max_total_num_tokens) {

  using Vec = at::vec::Vectorized<float>;

  // block length for k_cache and v_cache
  constexpr int BLOCK_N = 64;

  // strides
  const int stride_q0 = num_heads * head_size;
  const int stride_q1 = head_size;
  const int stride_k0 = num_heads * head_size;
  const int stride_k1 = head_size;
  const int stride_v0 = num_heads * head_size_v;
  const int stride_v1 = head_size_v;
  const int stride_l1 = num_kv_splits * (head_size_v + 1);
  const int stride_l2 = head_size_v + 1;

  const bool has_logit_cap = logit_cap > 0;
  float rlogit_cap = has_logit_cap ? 1 / logit_cap : 0.f;

  // parallel on [batches, num_heads, num_kv_splits]
  parallel_for(batches * num_heads * num_kv_splits, [&](int begin, int end) {
    int bs{0}, head_id{0}, kv_id{0};
    data_index_init(begin, bs, batches, head_id, num_heads, kv_id, num_kv_splits);

    // s_prime and s_delta
    static thread_local float s_i[BLOCK_N];
    float* __restrict__ s_delta = s_i;

    for (int i = begin; i < end; ++i) {
      // get query
      const scalar_t* __restrict__ q_ptr = query + bs * stride_q0 + head_id * stride_q1;

      // get key/value
      int seq_len_kv = seq_lens[bs];
      int req_pool_id = req_pool_indices[bs];
      TORCH_CHECK(seq_len_kv <= max_context_len, "seq_len_kv out of scope!");
      TORCH_CHECK(req_pool_id < max_num_reqs, "req_pool_id out of scope!");

      const int SPLIT_SIZE = div_up(seq_len_kv, num_kv_splits);
      const int kv_start = kv_id * SPLIT_SIZE;
      const int kv_end = std::min(kv_start + SPLIT_SIZE, seq_len_kv);

      float m_prime = -std::numeric_limits<float>::infinity();
      float s_prime = 0.f;

      // get v_prime, and init to zero
      float* __restrict__ v_prime = attn_logits + i * (head_size_v + 1);
      fill_stub(v_prime, 0.f, head_size_v);

      // loop over K and V sequence with BLOCK_N
      for (int n = kv_start; n < kv_end; n += BLOCK_N) {
        int n_size = std::min(BLOCK_N, kv_end - n);

        // calculate s_i <- scale * Q @ K
        index_gemm_kernel_nt<scalar_t, index_t>(
            /* A   */ q_ptr,
            /* B   */ k_cache + head_id * stride_k1,
            /* C   */ s_i,
            /* ind */ req_to_token + req_pool_id * max_context_len + n,
            /* scl */ scaling,
            /* M   */ 1,
            /* N   */ n_size,
            /* K   */ head_size,
            /* lda */ 1,
            /* ldb */ stride_k0,
            /* ldc */ 1,
            /* mtt */ max_total_num_tokens);

        // TODO: `tanh` from torch uses sleef u10, going to be slow
        if (has_logit_cap) {
          at::vec::map<float>(
              [logit_cap, rlogit_cap](Vec x) { return Vec(logit_cap) * (x * Vec(rlogit_cap)).tanh(); },
              s_i,
              s_i,
              n_size);
        }

        // m_i: max value per row
        float m_i = at::vec::reduce_all<float>(
            [](Vec& x, Vec& y) { return at::vec::maximum(x, y); },
            s_i,
            n_size);
        m_i = std::max(m_i, m_prime);

        // m_delta <- exp(m' - m_i)
        float m_delta = std::exp(m_prime - m_i);

        // s_delta <- exp(s_i - m_i)
        at::vec::map<float>(
            [m_i](Vec x) { return (x - Vec(m_i)).exp_u20(); },
            s_delta,
            s_i,
            n_size);

        // s' <- s' * m_delta + sum(s_delta)
        s_prime *= m_delta;
        s_prime += at::vec::reduce_all<float>(
            [](Vec& x, Vec& y) { return x + y; },
            s_delta,
            n_size);

        m_prime = m_i;

        // caculate V' <- s_delta @ V + V' * m_delta
        index_gemm_kernel_nn(
            /* A   */ s_delta,
            /* B   */ v_cache + head_id * stride_v1,
            /* C   */ v_prime,
            /* ind */ req_to_token + req_pool_id * max_context_len + n,
            /* scl */ &m_delta,
            /* M   */ 1,
            /* N   */ head_size_v,
            /* K   */ n_size,
            /* lda */ 1,
            /* ldb */ stride_v0,
            /* ldc */ 1,
            /* mtt */ max_total_num_tokens);
      } // loop with KV blocks

      float s = 1 / s_prime;
      at::vec::map<float>(
          [s](Vec out) { return out * Vec(s); },
          v_prime,
          v_prime,
          head_size_v);

      v_prime[head_size_v] = m_prime + std::log(s_prime);

      // move to the next index
      data_index_step(bs, batches, head_id, num_heads, kv_id, num_kv_splits);
    }
  });

  // parallel on [batches, num_heads]
  parallel_for(batches * num_heads, [&] (int begin, int end) {
    // NB: here we use logits[b][h][0] as acc, since
    // for the first kv split (kv_id == 0):
    //   m_delta = std::exp(-inf) = 0
    //   e_logic = std::exp(0) = 1
    //   acc = acc * m_delta + tv * e_logic = tv
    for (int i = begin; i < end; ++i) {
      float* __restrict__ acc = attn_logits + i * stride_l1;

      float s_prime = 0.f;
      float m_prime = -std::numeric_limits<scalar_t>::infinity();

      // update acc with from each kv_split
      for (int kv_id = 0; kv_id < num_kv_splits; ++kv_id) {
        float* __restrict__ tv = acc + kv_id * stride_l2;
        const float tlogic = (acc + kv_id * stride_l2)[head_size_v];

        float m_i = std::max(tlogic, m_prime);
        float m_delta = std::exp(m_prime - m_i);
        float e_logic = std::exp(tlogic - m_i);
        if (kv_id != 0) {
          at::vec::map2<float>(
              [m_delta, e_logic](Vec x, Vec y) { return x * Vec(m_delta) +  y * Vec(e_logic); },
              acc, acc, tv, head_size_v);
        }

        s_prime = s_prime * m_delta + e_logic;
        m_prime = m_i;
      }

      copy_stub<scalar_t>(output + i * head_size_v, acc, 1 / s_prime, head_size_v);
    }
  });
}

template <typename scalar_t, typename index_t>
void decode_attention_grouped_kernel_impl(
    scalar_t* __restrict__ output,
    float* __restrict__ attn_logits,
    const scalar_t* __restrict__ query,
    const scalar_t* __restrict__ k_cache,
    const scalar_t* __restrict__ v_cache,
    const index_t* __restrict__ req_to_token,
    const index_t* __restrict__ req_pool_indices,
    const index_t* __restrict__ seq_lens,
    int batches,
    int num_heads,
    int num_heads_kv,
    int head_size,
    int head_size_v,
    int num_kv_splits,
    float scaling,
    float logit_cap,
    int max_num_reqs,
    int max_context_len,
    int max_total_num_tokens) {

  using Vec = at::vec::Vectorized<float>;

  // block length for k_cache and v_cache
  constexpr int BLOCK_N = 64;
  // block length for heads
  constexpr int BLOCK_H = 16;

  // strides
  const int stride_q0 = num_heads * head_size;
  const int stride_q1 = head_size;
  const int stride_k0 = num_heads_kv * head_size;
  const int stride_k1 = head_size;
  const int stride_v0 = num_heads_kv * head_size_v;
  const int stride_v1 = head_size_v;
  const int stride_l0 = num_heads * num_kv_splits * (head_size_v + 1);
  const int stride_l1 = num_kv_splits * (head_size_v + 1);
  const int stride_l2 = head_size_v + 1;

  const bool has_logit_cap = logit_cap > 0;
  float rlogit_cap = has_logit_cap ? 1 / logit_cap : 0.f;

  // partition the heads into blocks for parallel
  const int num_groups = num_heads / num_heads_kv;
  const int num_blocks = div_up(num_heads, std::min(BLOCK_H, num_groups));
  const int num_groups_per_block = div_up(num_groups, BLOCK_H);
  const int num_heads_per_block = std::min(num_groups, BLOCK_H);

  // parallel on [batches, num_blocks, num_kv_splits]
  parallel_for(batches * num_blocks * num_kv_splits, [&](int begin, int end) {
    int bs{0}, head_id{0}, kv_id{0};
    data_index_init(begin, bs, batches, head_id, num_blocks, kv_id, num_kv_splits);

    static thread_local float s_i[BLOCK_H * BLOCK_N];
    float* __restrict__ s_delta = s_i;

    static thread_local float s_prime[BLOCK_H];
    static thread_local float m_prime[BLOCK_H];
    static thread_local float m_delta[BLOCK_H];

    for (int i = begin; i < end; ++i) {
      const int h_start = head_id * num_heads_per_block;
      const int h_end = std::min(h_start + num_heads_per_block, num_heads);
      const int h_size = h_end - h_start;

      // get query
      const scalar_t* __restrict__ q_ptr = query + bs * stride_q0 + h_start * stride_q1;

      // kv head id and valid block head size
      int head_kv_id = head_id / num_groups_per_block;
      int seq_len_kv = seq_lens[bs];
      int req_pool_id = req_pool_indices[bs];
      TORCH_CHECK(seq_len_kv <= max_context_len, "seq_len_kv out of scope!");
      TORCH_CHECK(req_pool_id < max_num_reqs, "req_pool_id out of scope!");

      const int SPLIT_SIZE = div_up(seq_len_kv, num_kv_splits);
      const int kv_start = kv_id * SPLIT_SIZE;
      const int kv_end = std::min(kv_start + SPLIT_SIZE, seq_len_kv);

      fill_stub(s_prime, 0.f, BLOCK_H);
      fill_stub(m_prime, -std::numeric_limits<float>::infinity(), BLOCK_H);

      // get v_prime, and init to zero
      float* __restrict__ v_prime = attn_logits + bs * stride_l0 + h_start * stride_l1 + kv_id * stride_l2;
      for (int h = 0; h < h_size; ++h) {
        fill_stub(v_prime + h * stride_l1, 0.f, head_size_v);
      }

      // loop over K and V sequence with BLOCK_N
      for (int n = kv_start; n < kv_end; n += BLOCK_N) {
        int n_size = std::min(BLOCK_N, kv_end - n);

        // calculate Q @ K
        index_gemm_kernel_nt<scalar_t, index_t>(
            /* A   */ q_ptr,
            /* B   */ k_cache + head_kv_id * stride_k1,
            /* C   */ s_i,
            /* ind */ req_to_token + req_pool_id * max_context_len + n,
            /* scl */ scaling,
            /* M   */ h_size,
            /* N   */ n_size,
            /* K   */ head_size,
            /* lda */ stride_q1,
            /* ldb */ stride_k0,
            /* ldc */ BLOCK_N,
            /* mtt */ max_total_num_tokens);

        if (has_logit_cap) {
          at::vec::map<float>(
              [logit_cap, rlogit_cap](Vec x) { return Vec(logit_cap) * (x * Vec(rlogit_cap)).tanh(); },
              s_i,
              s_i,
              n_size);
        }

        // update the scaling coefficients
        for (int h = 0; h < h_size; ++h) {
          // m_i: max value per row
          float m_i = at::vec::reduce_all<float>(
              [](Vec& x, Vec& y) { return at::vec::maximum(x, y); },
              s_i + h * BLOCK_N,
              n_size);
          m_i = std::max(m_i, m_prime[h]);

          // m_delta <- exp(m' - m_i)
          m_delta[h] = std::exp(m_prime[h] - m_i);

          // s_delta <- exp(s_i - m_i)
          at::vec::map<float>(
              [m_i](Vec x) { return (x - Vec(m_i)).exp_u20(); },
              s_delta + h * BLOCK_N,
              s_i + h * BLOCK_N,
              n_size);

          // s' <- s' * m_delta + sum(s_delta)
          s_prime[h] *= m_delta[h];
          s_prime[h] += at::vec::reduce_all<float>(
              [](Vec& x, Vec& y) { return x + y; },
              s_delta + h * BLOCK_N,
              n_size);

          m_prime[h] = m_i;
        }

        // caculate V' <- s_delta @ V + V' * m_delta
        index_gemm_kernel_nn(
            /* A   */ s_delta,
            /* B   */ v_cache + head_kv_id * stride_v1,
            /* C   */ v_prime,
            /* ind */ req_to_token + req_pool_id * max_context_len + n,
            /* scl */ m_delta,
            /* M   */ h_size,
            /* N   */ head_size_v,
            /* K   */ n_size,
            /* lda */ BLOCK_N,
            /* ldb */ stride_v0,
            /* ldc */ stride_l1,
            /* mtt */ max_total_num_tokens);
      } // loop with KV blocks

      for (int h = 0; h < h_size; ++h) {
        float s = 1 / s_prime[h];
        at::vec::map<float>(
            [s](Vec out) { return out * Vec(s); },
            v_prime + h * stride_l1,
            v_prime + h * stride_l1,
            head_size_v);
        (v_prime + h * stride_l1)[head_size_v] = m_prime[h] + std::log(s_prime[h]);
      }

      // move to the next index
      data_index_step(bs, batches, head_id, num_blocks, kv_id, num_kv_splits);
    }
  });

  // parallel on [batches, num_heads]
  parallel_for(batches * num_heads, [&] (int begin, int end) {
    // NB: same as above
    for (int i = begin; i < end; ++i) {
      float* __restrict__ acc = attn_logits + i * stride_l1;

      float s_prime = 0.f;
      float m_prime = -std::numeric_limits<scalar_t>::infinity();

      // update acc with from each kv_split
      for (int kv_id = 0; kv_id < num_kv_splits; ++kv_id) {
        float* __restrict__ tv = acc + kv_id * stride_l2;
        const float tlogic = (acc + kv_id * stride_l2)[head_size_v];

        float m_i = std::max(tlogic, m_prime);
        float m_delta = std::exp(m_prime - m_i);
        float e_logic = std::exp(tlogic - m_i);
        if (kv_id != 0) {
          at::vec::map2<float>(
              [m_delta, e_logic](Vec x, Vec y) { return x * Vec(m_delta) +  y * Vec(e_logic); },
              acc, acc, tv, head_size_v);
        }

        s_prime = s_prime * m_delta + e_logic;
        m_prime = m_i;
      }

      copy_stub<scalar_t>(output + i * head_size_v, acc, 1 / s_prime, head_size_v);
    }
  });
}

} // anonymous namespace

// query: [num_tokens, num_heads, head_size]
// output: [num_tokens, num_heads, head_size]
// k_cache: [max_total_num_tokens, num_heads, head_size]
// v_cache: [max_total_num_tokens, num_heads, head_size_v]
// attn_logits: [num_seqs, num_heads, num_kv_splits, head_size_v + 1]
// req_to_token: [max_num_reqs, max_context_len]
// req_pool_indices: [num_seqs]
// seq_lens: [num_seqs]
//
void decode_attention(
    at::Tensor& query,
    at::Tensor& output,
    at::Tensor& k_cache,
    at::Tensor& v_cache,
    at::Tensor& attn_logits,
    at::Tensor& req_to_token,
    at::Tensor& req_pool_indices,
    at::Tensor& seq_lens,
    double scaling,
    double logit_cap) {

  CHECK_INPUT(query);
  CHECK_INPUT(k_cache);
  CHECK_INPUT(v_cache);
  CHECK_DIM(3, query);
  CHECK_DIM(3, k_cache);
  CHECK_DIM(3, v_cache);

  int num_seqs = seq_lens.size(0);
  int max_num_reqs = req_to_token.size(0);
  int max_context_len = req_to_token.size(1);
  int max_total_num_tokens = k_cache.size(0);

  int num_heads = query.size(1);
  int num_heads_kv = k_cache.size(1);
  int head_size = query.size(2);
  int head_size_v = v_cache.size(2);

  int num_kv_splits = attn_logits.size(2);

  CHECK_EQ(attn_logits.size(0), num_seqs);
  CHECK_EQ(attn_logits.size(1), num_heads);
  CHECK_EQ(attn_logits.size(3), head_size_v + 1);
  CHECK_EQ(attn_logits.scalar_type(), at::kFloat);

  // make sure all the indices have the same data type
  const auto index_dtype = seq_lens.scalar_type();
  CHECK_EQ(req_to_token.scalar_type(), index_dtype);
  CHECK_EQ(req_pool_indices.scalar_type(), index_dtype);

  AT_DISPATCH_REDUCED_FLOATING_TYPES(query.scalar_type(), "decode_attention_kernel", [&] {
    AT_DISPATCH_INDEX_TYPES(index_dtype, "decode_attention_indices", [&] {
      if (num_heads == num_heads_kv) {
        // MHA
        decode_attention_kernel_impl<scalar_t, index_t>(
            output.data_ptr<scalar_t>(),
            attn_logits.data_ptr<float>(),
            query.data_ptr<scalar_t>(),
            k_cache.data_ptr<scalar_t>(),
            v_cache.data_ptr<scalar_t>(),
            req_to_token.data_ptr<index_t>(),
            req_pool_indices.data_ptr<index_t>(),
            seq_lens.data_ptr<index_t>(),
            num_seqs,
            num_heads,
            head_size,
            head_size_v,
            num_kv_splits,
            scaling,
            logit_cap,
            max_num_reqs,
            max_context_len,
            max_total_num_tokens);
      } else {
        // GQA/MQA/MLA
        decode_attention_grouped_kernel_impl<scalar_t, index_t>(
            output.data_ptr<scalar_t>(),
            attn_logits.data_ptr<float>(),
            query.data_ptr<scalar_t>(),
            k_cache.data_ptr<scalar_t>(),
            v_cache.data_ptr<scalar_t>(),
            req_to_token.data_ptr<index_t>(),
            req_pool_indices.data_ptr<index_t>(),
            seq_lens.data_ptr<index_t>(),
            num_seqs,
            num_heads,
            num_heads_kv,
            head_size,
            head_size_v,
            num_kv_splits,
            scaling,
            logit_cap,
            max_num_reqs,
            max_context_len,
            max_total_num_tokens);
      }
    });
  });
}