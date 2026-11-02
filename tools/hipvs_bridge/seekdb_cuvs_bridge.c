/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* Reuse device scratch across queries; search is serialized per handle. */
  float *qd;
  unsigned *nd;
  float *dd;
  size_t q_capacity;
  size_t out_capacity;
  pthread_mutex_t mu;
  int mu_ready;
} sk_cuvs_index;

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  if (!base || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (pthread_mutex_init(&h->mu, NULL) != 0) { free(h); return NULL; }
  h->mu_ready = 1;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { pthread_mutex_destroy(&h->mu); free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  /* Keep the V5 graph-quality settings; optimize only the surrounding path. */
  h->ip->intermediate_graph_degree = 256;
  h->ip->nn_descent_niter = 100;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); if (h->mu_ready) pthread_mutex_destroy(&h->mu); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  if (!query || nq <= 0 || topk <= 0 || topk > h->n || h->dim <= 0) return 2;
  if ((uint64_t)nq > SIZE_MAX / (uint64_t)h->dim ||
      (uint64_t)nq * (uint64_t)h->dim > SIZE_MAX / sizeof(float) ||
      (uint64_t)nq > SIZE_MAX / (uint64_t)topk ||
      (uint64_t)nq * (uint64_t)topk > SIZE_MAX / sizeof(unsigned)) return 3;
  int rc = 0;
  cuvsCagraSearchParams_t sp = 0;
  const size_t qbytes = sizeof(float) * (size_t)nq * (size_t)h->dim;
  const size_t obytes = sizeof(unsigned) * (size_t)nq * (size_t)topk;
  if (pthread_mutex_lock(&h->mu) != 0) return 4;
  if (h->q_capacity < qbytes) {
    float *next = 0;
    if (cuvsRMMAlloc(h->res, (void**)&next, qbytes) != CUVS_SUCCESS) { rc=10; goto done; }
    if (h->qd) cuvsRMMFree(h->res, h->qd, h->q_capacity);
    h->qd = next; h->q_capacity = qbytes;
  }
  if (h->out_capacity < obytes) {
    unsigned *next_ids = 0; float *next_dist = 0;
    if (cuvsRMMAlloc(h->res, (void**)&next_ids, obytes) != CUVS_SUCCESS) { rc=11; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&next_dist, sizeof(float) * (size_t)nq * (size_t)topk) != CUVS_SUCCESS) {
      cuvsRMMFree(h->res, next_ids, obytes); rc=12; goto done;
    }
    if (h->nd) cuvsRMMFree(h->res, h->nd, h->out_capacity);
    if (h->dd) cuvsRMMFree(h->res, h->dd, h->out_capacity);
    h->nd = next_ids; h->dd = next_dist; h->out_capacity = obytes;
  }
  if (cudaMemcpy(h->qd, query, qbytes, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 512) required_itopk = 512;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 4;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, h->nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, h->dd,
                               sizeof(float) * (size_t)nq * (size_t)topk,
                               cudaMemcpyDefault) != cudaSuccess) { rc=17; goto done; }
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  pthread_mutex_unlock(&h->mu);
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  if (h->qd) cuvsRMMFree(h->res, h->qd, h->q_capacity);
  if (h->nd) cuvsRMMFree(h->res, h->nd, h->out_capacity);
  if (h->dd) cuvsRMMFree(h->res, h->dd, h->out_capacity);
  cuvsResourcesDestroy(h->res);
  if (h->mu_ready) pthread_mutex_destroy(&h->mu);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (pthread_mutex_init(&index->mu, NULL) != 0) { free(index); return NULL; }
  index->mu_ready = 1;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  if (index->mu_ready) pthread_mutex_destroy(&index->mu);
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
