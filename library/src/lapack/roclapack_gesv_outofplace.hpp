/************************************************************************
 * Derived from the BSD3-licensed
 * LAPACK routine (version 3.7.0) --
 *     Univ. of Tennessee, Univ. of California Berkeley,
 *     Univ. of Colorado Denver and NAG Ltd..
 *     December 2016
 * Copyright (c) 2019-2021 Advanced Micro Devices, Inc.
 * ***********************************************************************/

#pragma once

#include "rocblas.hpp"
#include "roclapack_getrf.hpp"
#include "roclapack_getrs.hpp"
#include "rocsolver.h"

template <typename T>
rocblas_status rocsolver_gesv_outofplace_argCheck(rocblas_handle handle,
                                                  const rocblas_int n,
                                                  const rocblas_int nrhs,
                                                  const rocblas_int lda,
                                                  const rocblas_int ldb,
                                                  const rocblas_int ldx,
                                                  T A,
                                                  T B,
                                                  T X,
                                                  const rocblas_int* ipiv,
                                                  const rocblas_int* info,
                                                  const rocblas_int batch_count = 1)
{
    // order is important for unit tests:

    // 1. invalid/non-supported values
    // N/A

    // 2. invalid size
    if(n < 0 || nrhs < 0 || lda < n || ldb < n || ldx < n || batch_count < 0)
        return rocblas_status_invalid_size;

    // skip pointer check if querying memory size
    if(rocblas_is_device_memory_size_query(handle))
        return rocblas_status_continue;

    // 3. invalid pointers
    if((n && !A) || (n && !ipiv) || (nrhs * n && !B) || (nrhs * n && !X) || (batch_count && !info))
        return rocblas_status_invalid_pointer;

    return rocblas_status_continue;
}

template <bool BATCHED, bool STRIDED, typename T, typename S>
void rocsolver_gesv_outofplace_getMemorySize(const rocblas_int n,
                                             const rocblas_int nrhs,
                                             const rocblas_int batch_count,
                                             size_t* size_scalars,
                                             size_t* size_work,
                                             size_t* size_work1,
                                             size_t* size_work2,
                                             size_t* size_work3,
                                             size_t* size_work4,
                                             size_t* size_pivotval,
                                             size_t* size_pivotidx,
                                             size_t* size_iinfo)
{
    // if quick return, no workspace is needed
    if(n == 0 || nrhs == 0 || batch_count == 0)
    {
        *size_scalars = 0;
        *size_work = 0;
        *size_work1 = 0;
        *size_work2 = 0;
        *size_work3 = 0;
        *size_work4 = 0;
        *size_pivotval = 0;
        *size_pivotidx = 0;
        *size_iinfo = 0;
        return;
    }

    size_t w1, w2, w3, w4;

    // workspace required for calling GETRF
    rocsolver_getrf_getMemorySize<BATCHED, STRIDED, true, T, S>(
        n, n, batch_count, size_scalars, size_work, size_work1, size_work2, size_work3, size_work4,
        size_pivotval, size_pivotidx, size_iinfo);

    // workspace required for calling GETRS
    rocsolver_getrs_getMemorySize<BATCHED, T>(n, nrhs, batch_count, &w1, &w2, &w3, &w4);

    *size_work1 = std::max(*size_work1, w1);
    *size_work2 = std::max(*size_work2, w2);
    *size_work3 = std::max(*size_work3, w3);
    *size_work4 = std::max(*size_work4, w4);
}

template <bool BATCHED, bool STRIDED, typename T, typename S, typename U>
rocblas_status rocsolver_gesv_outofplace_template(rocblas_handle handle,
                                                  const rocblas_int n,
                                                  const rocblas_int nrhs,
                                                  U A,
                                                  const rocblas_int shiftA,
                                                  const rocblas_int lda,
                                                  const rocblas_stride strideA,
                                                  rocblas_int* ipiv,
                                                  const rocblas_stride strideP,
                                                  U B,
                                                  const rocblas_int shiftB,
                                                  const rocblas_int ldb,
                                                  const rocblas_stride strideB,
                                                  U X,
                                                  const rocblas_int shiftX,
                                                  const rocblas_int ldx,
                                                  const rocblas_stride strideX,
                                                  rocblas_int* info,
                                                  const rocblas_int batch_count,
                                                  T* scalars,
                                                  rocblas_index_value_t<S>* work,
                                                  void* work1,
                                                  void* work2,
                                                  void* work3,
                                                  void* work4,
                                                  T* pivotval,
                                                  rocblas_int* pivotidx,
                                                  rocblas_int* iinfo,
                                                  bool optim_mem)
{
    ROCSOLVER_ENTER("gesv_outofplace", "n:", n, "nrhs:", nrhs, "shiftA:", shiftA, "lda:", lda,
                    "shiftB:", shiftB, "ldb:", ldb, "bc:", batch_count);

    // quick return if zero instances in batch
    if(batch_count == 0)
        return rocblas_status_success;

    hipStream_t stream;
    rocblas_get_stream(handle, &stream);

    rocblas_int blocksReset = (batch_count - 1) / BLOCKSIZE + 1;
    dim3 gridReset(blocksReset, 1, 1);
    dim3 threads(BLOCKSIZE, 1, 1);

    // info=0 (starting with a nonsingular matrix)
    hipLaunchKernelGGL(reset_info, gridReset, threads, 0, stream, info, batch_count, 0);

    // quick return if A or B are empty
    if(n == 0 || nrhs == 0)
        return rocblas_status_success;

    // constants in host memory
    const rocblas_int copyblocksx = (n - 1) / 32 + 1;
    const rocblas_int copyblocksy = (nrhs - 1) / 32 + 1;

    // compute LU factorization of A
    rocsolver_getrf_template<BATCHED, STRIDED, true, T>(
        handle, n, n, A, shiftA, lda, strideA, ipiv, 0, strideP, info, batch_count, scalars, work,
        work1, work2, work3, work4, pivotval, pivotidx, iinfo, optim_mem);

    // copy B to X
    hipLaunchKernelGGL(copy_mat<T>, dim3(copyblocksx, copyblocksy, batch_count), dim3(32, 32), 0,
                       stream, n, nrhs, B, shiftB, ldb, strideB, X, shiftX, ldx, strideX);

    // solve AX = B
    rocsolver_getrs_template<BATCHED, T>(handle, rocblas_operation_none, n, nrhs, A, shiftA, lda,
                                         strideA, ipiv, strideP, X, shiftX, ldx, strideX,
                                         batch_count, work1, work2, work3, work4, optim_mem);

    return rocblas_status_success;
}