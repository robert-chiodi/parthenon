//========================================================================================
// Parthenon performance portable AMR framework
// Copyright(C) 2020-2022 The Parthenon collaboration
// Licensed under the 3-clause BSD License, see LICENSE file for details
//========================================================================================
// (C) (or copyright) 2021. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001
// for Los Alamos National Laboratory (LANL), which is operated by Triad
// National Security, LLC for the U.S. Department of Energy/National Nuclear
// Security Administration. All rights in the program are reserved by Triad
// National Security, LLC, and the U.S. Department of Energy/National Nuclear
// Security Administration. The Government is granted for itself and others
// acting on its behalf a nonexclusive, paid-up, irrevocable worldwide license
// in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do
// so.
//========================================================================================

#ifndef MESH_REFINEMENT_CC_IN_ONE_HPP_
#define MESH_REFINEMENT_CC_IN_ONE_HPP_

#include <algorithm>
#include <vector>

#include "bvals/cc/bvals_cc_in_one.hpp" // for buffercache_t
#include "coordinates/coordinates.hpp"  // for coordinates
#include "interface/mesh_data.hpp"
#include "mesh/domain.hpp" // for IndexShape

namespace parthenon {
namespace cell_centered_refinement {
void Restrict(const cell_centered_bvars::BufferCache_t &info,
              const IndexShape &cellbounds, const IndexShape &c_cellbounds);
TaskStatus RestrictPhysicalBounds(MeshData<Real> *md);

std::vector<bool> ComputePhysicalRestrictBoundsAllocStatus(MeshData<Real> *md);
void ComputePhysicalRestrictBounds(MeshData<Real> *md);

void Prolongate(const cell_centered_bvars::BufferCache_t &info,
                const IndexShape &cellbounds, const IndexShape &c_cellbounds);

// TODO(JMM): We may wish to expose some of these impl functions eventually.
namespace impl {

// If the info object has more buffers than this, do
// hierarchical parallelism. If it does not, loop over buffers on the
// host and launch kernels manually.
//
// TODO(JMM): Experiment here? We could expose this as a run-time or
// compile-time parameter, if it ends up being hardware dependent. My
// suspicion is that, given kernel launch latencies, MIN_NUM_BUFS
// should be either 1 or 6.
//
// MIN_NUM_BUFS = 1 implies that the old per-buffer machinery doesn't
// use hierarchical parallelism. This also means that for
// prolongation/restriction over a whole meshblock, hierarchical
// parallelism is not used, which is probably important for
// re-meshing.
//
// MIN_NUM_BUFS = 6 implies that in a unigrid sim a meshblock pack of
// size 1 would be looped over manually while a pack of size 2 would
// use hierarchical parallelism.
constexpr int MIN_NUM_BUFS = 1;

template <typename Info_t>
KOKKOS_FORCEINLINE_FUNCTION bool DoRefinementOp(const Info_t &info,
                                                const RefinementOp_t op) {
  return (info.allocated && info.refinement_op == op);
}

template <int DIM, typename Info_t>
KOKKOS_FORCEINLINE_FUNCTION void
GetLoopBoundsFromBndInfo(const Info_t &info, const int ckbs, const int cjbs, int &sk,
                         int &ek, int &sj, int &ej, int &si, int &ei) {
  sk = info.sk;
  ek = info.ek;
  sj = info.sj;
  ej = info.ej;
  si = info.si;
  ei = info.ei;
  if (DIM < 3) sk = ek = ckbs; // TODO(C++17) make constexpr
  if (DIM < 2) sj = ej = cjbs;
}

// JMM: A single prolongation/restriction loop template without
// specializations is possible, if we're willing to always do the 6D
// loop with different specialized loop bounds. The danger of that
// approach is that if, e.g., a TVVR loop pattern is utilized at lower
// dimensionality but not higher-dimensionality, the pattern may not
// work out optimally. I have implemented it here, but we may wish to
// revert to separate loops per dimension, if the performance hit is
// too large.
template <int DIM, template <int> typename Stencil>
void ProlongationRestrictionLoop(const cell_centered_bvars::BufferCache_t &info,
                                 const IndexShape &cellbounds,
                                 const IndexShape &c_cellbounds,
                                 const RefinementOp_t op) {

  const IndexDomain interior = IndexDomain::interior;
  const IndexDomain entire = IndexDomain::entire;
  auto ckb = c_cellbounds.GetBoundsK(interior);
  auto cjb = c_cellbounds.GetBoundsJ(interior);
  auto cib = c_cellbounds.GetBoundsI(interior);
  auto kb = cellbounds.GetBoundsK(interior);
  auto jb = cellbounds.GetBoundsJ(interior);
  auto ib = cellbounds.GetBoundsI(interior);

  const int nbuffers = info.extent_int(0);

  if (nbuffers > MIN_NUM_BUFS) {
    const int scratch_level = 1; // 0 is actual scratch (tiny); 1 is HBM
    size_t scratch_size_in_bytes = 1;
    par_for_outer(
        DEFAULT_OUTER_LOOP_PATTERN, "ProlongateOrRestrictCellCenteredValues",
        DevExecSpace(), scratch_size_in_bytes, scratch_level, 0, nbuffers - 1,
        KOKKOS_LAMBDA(team_mbr_t team_member, const int buf) {
          if (DoRefinementOp(info(buf), op)) {
            int sk, ek, sj, ej, si, ei;
            GetLoopBoundsFromBndInfo<DIM>(info(buf), ckb.s, cjb.s, sk, ek, sj, ej, si,
                                          ei);
            par_for_inner(inner_loop_pattern_ttr_tag, team_member, 0, info(buf).Nt - 1, 0,
                          info(buf).Nu - 1, 0, info(buf).Nv - 1, sk, ek, sj, ej, si, ei,
                          [&](const int l, const int m, const int n, const int k,
                              const int j, const int i) {
                            Stencil<DIM>::Do(l, m, n, k, j, i, ckb, cjb, cib, kb, jb, ib,
                                             info(buf).coords, info(buf).coarse_coords,
                                             info(buf).coarse, info(buf).fine);
                          });
          }
        });
  } else {
    // TODO(JMM): This implies both an extra DtoH and an extra HtoD
    // copy. If this turns out to be a serious problem, we can resolve
    // by always passing around both host and device copies of the
    // `BufferCache_t` object, or by making it host-pinned memory.
    auto info_h = Kokkos::create_mirror_view(info);
    Kokkos::deep_copy(info_h, info);
    for (int buf = 0; buf < nbuffers; ++buf) {
      if (DoRefinementOp(info(buf), op)) {
        int sk, ek, sj, ej, si, ei;
        GetLoopBoundsFromBndInfo<DIM>(info_h(buf), ckb.s, cjb.s, sk, ek, sj, ej, si, ei);
        par_for(
            DEFAULT_LOOP_PATTERN, "ProlongateOrRestrictCellCenteredValues",
            DevExecSpace(), 0, info_h(buf).Nt - 1, 0, info_h(buf).Nu - 1, 0,
            info_h(buf).Nv - 1, sk, ek, sj, ej, si, ei,
            KOKKOS_LAMBDA(const int l, const int m, const int n, const int k, const int j,
                          const int i) {
              Stencil<DIM>::Do(l, m, n, k, j, i, ckb, cjb, cib, kb, jb, ib,
                               info_h(buf).coords, info_h(buf).coarse_coords,
                               info_h(buf).coarse, info_h(buf).fine);
            });
      }
    }
  }
}

} // namespace impl

} // namespace cell_centered_refinement
} // namespace parthenon

#endif // MESH_REFINEMENT_CC_IN_ONE_HPP_
