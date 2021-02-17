//========================================================================================
// (C) (or copyright) 2020-2021. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001 for Los
// Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
// for the U.S. Department of Energy/National Nuclear Security Administration. All rights
// in the program are reserved by Triad National Security, LLC, and the U.S. Department
// of Energy/National Nuclear Security Administration. The Government is granted for
// itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
// license in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do so.
//========================================================================================
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "mesh/mesh.hpp"
#include "swarm.hpp"

namespace parthenon {

SwarmDeviceContext Swarm::GetDeviceContext() const {
  SwarmDeviceContext context;
  context.marked_for_removal_ = marked_for_removal_.data;
  context.mask_ = mask_.data;
  context.blockIndex_ = blockIndex_;
  context.neighborIndices_ = neighborIndices_;

  auto pmb = GetBlockPointer();
  auto pmesh = pmb->pmy_mesh;
  auto mesh_size = pmesh->mesh_size;

  const IndexRange &ib = pmb->cellbounds.GetBoundsI(IndexDomain::interior);
  const IndexRange &jb = pmb->cellbounds.GetBoundsJ(IndexDomain::interior);
  const IndexRange &kb = pmb->cellbounds.GetBoundsK(IndexDomain::interior);
  context.x_min_ = pmb->coords.x1f(ib.s);
  context.y_min_ = pmb->coords.x2f(jb.s);
  context.z_min_ = pmb->coords.x3f(kb.s);
  context.x_max_ = pmb->coords.x1f(ib.e + 1);
  context.y_max_ = pmb->coords.x2f(jb.e + 1);
  context.z_max_ = pmb->coords.x3f(kb.e + 1);
  context.x_min_global_ = mesh_size.x1min;
  context.x_max_global_ = mesh_size.x1max;
  context.y_min_global_ = mesh_size.x2min;
  context.y_max_global_ = mesh_size.x2max;
  context.z_min_global_ = mesh_size.x3min;
  context.z_max_global_ = mesh_size.x3max;
  context.ndim_ = pmb->pmy_mesh->ndim;
  context.my_rank_ = Globals::my_rank;
  return context;
}

Swarm::Swarm(const std::string &label, const Metadata &metadata, const int nmax_pool_in)
    : label_(label), m_(metadata), nmax_pool_(nmax_pool_in),
      mask_("mask", nmax_pool_, Metadata({Metadata::Boolean})),
      marked_for_removal_("mfr", nmax_pool_, Metadata({Metadata::Boolean})),
      neighbor_send_index_("nsi", nmax_pool_, Metadata({Metadata::Integer})),
      blockIndex_("blockIndex_", nmax_pool_),
      neighborIndices_("neighborIndices_", 4, 4, 4), mpiStatus(true) {
  Add("x", Metadata({Metadata::Real}));
  Add("y", Metadata({Metadata::Real}));
  Add("z", Metadata({Metadata::Real}));
  num_active_ = 0;
  max_active_index_ = 0;

  auto mask_h = mask_.data.GetHostMirror();
  auto marked_for_removal_h = marked_for_removal_.data.GetHostMirror();

  for (int n = 0; n < nmax_pool_; n++) {
    mask_h(n) = false;
    marked_for_removal_h(n) = false;
    free_indices_.push_back(n);
  }

  mask_.data.DeepCopy(mask_h);
  marked_for_removal_.data.DeepCopy(marked_for_removal_h);
}

void Swarm::Add(const std::vector<std::string> &labelArray, const Metadata &metadata) {
  // generate the vector and call Add
  for (auto label : labelArray) {
    Add(label, metadata);
  }
}

std::shared_ptr<Swarm> Swarm::AllocateCopy(const bool allocComms, MeshBlock *pmb) {
  Metadata m = m_;

  auto swarm = std::make_shared<Swarm>(label(), m, nmax_pool_);

  return swarm;
}

///
/// The routine for allocating a particle variable in the current swarm.
///
/// @param label the name of the variable
/// @param metadata the metadata associated with the particle
void Swarm::Add(const std::string &label, const Metadata &metadata) {
  // labels must be unique, even between different types of data
  if (intMap_.count(label) > 0 || realMap_.count(label) > 0) {
    throw std::invalid_argument("swarm variable " + label +
                                " already enrolled during Add()!");
  }

  if (metadata.Type() == Metadata::Integer) {
    auto var = std::make_shared<ParticleVariable<int>>(label, nmax_pool_, metadata);
    intVector_.push_back(var);
    intMap_[label] = var;
  } else if (metadata.Type() == Metadata::Real) {
    auto var = std::make_shared<ParticleVariable<Real>>(label, nmax_pool_, metadata);
    realVector_.push_back(var);
    realMap_[label] = var;
  } else {
    throw std::invalid_argument("swarm variable " + label +
                                " does not have a valid type during Add()");
  }
}

///
/// The routine for removing a variable from a particle swarm.
///
/// @param label the name of the variable
void Swarm::Remove(const std::string &label) {
  bool found = false;

  // Find index of variable
  int idx = 0;
  for (auto v : intVector_) {
    if (label == v->label()) {
      found = true;
      break;
    }
    idx++;
  }
  if (found == true) {
    // first delete the variable
    intVector_[idx].reset();

    // Next move the last element into idx and pop last entry
    if (intVector_.size() > 1) intVector_[idx] = std::move(intVector_.back());
    intVector_.pop_back();

    // Also remove variable from map
    intMap_.erase(label);
  }

  if (found == false) {
    idx = 0;
    for (const auto &v : realVector_) {
      if (label == v->label()) {
        found = true;
        break;
      }
      idx++;
    }
  }
  if (found == true) {
    realVector_[idx].reset();
    if (realVector_.size() > 1) realVector_[idx] = std::move(realVector_.back());
    realVector_.pop_back();
    realMap_.erase(label);
  }

  if (found == false) {
    throw std::invalid_argument("swarm variable not found in Remove()");
  }
}

void Swarm::setPoolMax(const int nmax_pool) {
  PARTHENON_REQUIRE(nmax_pool > nmax_pool_, "Must request larger pool size!");
  int n_new_begin = nmax_pool_;
  int n_new = nmax_pool - nmax_pool_;

  auto pmb = GetBlockPointer();

  for (int n = 0; n < n_new; n++) {
    free_indices_.push_back(n + n_new_begin);
  }

  // Resize and copy data
  mask_.Get().Resize(nmax_pool);
  auto mask_data = mask_.Get();
  pmb->par_for(
      "setPoolMax_mask", nmax_pool_, nmax_pool - 1,
      KOKKOS_LAMBDA(const int n) { mask_data(n) = 0; });

  marked_for_removal_.Get().Resize(nmax_pool);
  auto marked_for_removal_data = marked_for_removal_.Get();
  pmb->par_for(
      "setPoolMax_marked_for_removal", nmax_pool_, nmax_pool - 1,
      KOKKOS_LAMBDA(const int n) { marked_for_removal_data(n) = false; });

  neighbor_send_index_.Get().Resize(nmax_pool);

  blockIndex_.Resize(nmax_pool);

  // TODO(BRR) Use ParticleVariable packs to reduce kernel launches
  for (int n = 0; n < intVector_.size(); n++) {
    auto oldvar = intVector_[n];
    auto newvar = std::make_shared<ParticleVariable<int>>(oldvar->label(), nmax_pool,
                                                          oldvar->metadata());
    auto oldvar_data = oldvar->data;
    auto newvar_data = newvar->data;
    pmb->par_for(
        "setPoolMax_int", 0, nmax_pool_ - 1,
        KOKKOS_LAMBDA(const int m) { newvar_data(m) = oldvar_data(m); });

    intVector_[n] = newvar;
    intMap_[oldvar->label()] = newvar;
  }

  for (int n = 0; n < realVector_.size(); n++) {
    auto oldvar = realVector_[n];
    auto newvar = std::make_shared<ParticleVariable<Real>>(oldvar->label(), nmax_pool,
                                                           oldvar->metadata());
    auto oldvar_data = oldvar->data;
    auto newvar_data = newvar->data;
    pmb->par_for(
        "setPoolMax_real", 0, nmax_pool_ - 1,
        KOKKOS_LAMBDA(const int m) { newvar_data(m) = oldvar_data(m); });
    realVector_[n] = newvar;
    realMap_[oldvar->label()] = newvar;
  }

  nmax_pool_ = nmax_pool;
}

ParArrayND<bool> Swarm::AddEmptyParticles(const int num_to_add,
                                          ParArrayND<int> &new_indices) {
  PARTHENON_REQUIRE(num_to_add > 0, "Attempting to add fewer than 1 new particles!");
  while (free_indices_.size() < num_to_add) {
    increasePoolMax();
  }

  ParArrayND<bool> new_mask("Newly created particles", nmax_pool_);
  auto new_mask_h = new_mask.GetHostMirror();
  for (int n = 0; n < nmax_pool_; n++) {
    new_mask_h(n) = false;
  }

  auto mask_h = mask_.data.GetHostMirror();
  mask_h.DeepCopy(mask_.data);
  auto blockIndex_h = blockIndex_.GetHostMirrorAndCopy();

  auto free_index = free_indices_.begin();

  new_indices = ParArrayND<int>("New indices", num_to_add);
  auto new_indices_h = new_indices.GetHostMirror();

  // Don't bother sanitizing the memory
  for (int n = 0; n < num_to_add; n++) {
    mask_h(*free_index) = true;
    new_mask_h(*free_index) = true;
    blockIndex_h(*free_index) = this_block_;
    max_active_index_ = std::max<int>(max_active_index_, *free_index);
    new_indices_h(n) = *free_index;

    free_index = free_indices_.erase(free_index);
  }

  new_indices.DeepCopy(new_indices_h);

  num_active_ += num_to_add;

  new_mask.DeepCopy(new_mask_h);
  mask_.data.DeepCopy(mask_h);
  blockIndex_.DeepCopy(blockIndex_h);

  return new_mask;
}

// No active particles: nmax_active_index = -1
// No particles removed: nmax_active_index unchanged
// Particles removed: nmax_active_index is new max active index
void Swarm::RemoveMarkedParticles() {
  auto mask_h = mask_.data.GetHostMirrorAndCopy();
  auto marked_for_removal_h = marked_for_removal_.data.GetHostMirror();
  marked_for_removal_h.DeepCopy(marked_for_removal_.data);

  // loop backwards to keep free_indices_ updated correctly
  for (int n = max_active_index_; n >= 0; n--) {
    if (mask_h(n)) {
      if (marked_for_removal_h(n)) {
        mask_h(n) = false;
        free_indices_.push_front(n);
        num_active_ -= 1;
        if (n == max_active_index_) {
          max_active_index_ -= 1;
        }
        marked_for_removal_h(n) = false;
      }
    }
  }

  mask_.data.DeepCopy(mask_h);
  marked_for_removal_.data.DeepCopy(marked_for_removal_h);
}

void Swarm::Defrag() {
  if (get_num_active() == 0) {
    return;
  }
  // TODO(BRR) Could this algorithm be more efficient? Does it matter?
  // Add 1 to convert max index to max number
  int num_free = (max_active_index_ + 1) - num_active_;
  auto pmb = GetBlockPointer();

  ParArrayND<int> from_to_indices("from_to_indices", max_active_index_ + 1);
  auto from_to_indices_h = from_to_indices.GetHostMirror();

  auto mask_h = mask_.data.GetHostMirrorAndCopy();

  for (int n = 0; n <= max_active_index_; n++) {
    from_to_indices_h(n) = unset_index_;
  }

  std::list<int> new_free_indices;

  int index = max_active_index_;
  int num_to_move = std::min<int>(num_free, num_active_);
  for (int n = 0; n < num_to_move; n++) {
    while (mask_h(index) == false) {
      index--;
    }
    int index_to_move_from = index;
    index--;

    // Below this number "moved" particles should actually stay in place
    if (index_to_move_from < num_active_) {
      break;
    }
    int index_to_move_to = free_indices_.front();
    free_indices_.pop_front();
    new_free_indices.push_back(index_to_move_from);
    from_to_indices_h(index_to_move_from) = index_to_move_to;
  }

  // TODO(BRR) Not all these sorts may be necessary
  free_indices_.sort();
  new_free_indices.sort();
  free_indices_.merge(new_free_indices);

  from_to_indices.DeepCopy(from_to_indices_h);

  auto mask = mask_.Get();
  pmb->par_for(
      "Swarm::DefragMask", 0, max_active_index_, KOKKOS_LAMBDA(const int n) {
        if (from_to_indices(n) >= 0) {
          mask(from_to_indices(n)) = mask(n);
          mask(n) = false;
        }
      });

  SwarmVariablePack<Real> vreal;
  SwarmVariablePack<int> vint;
  PackIndexMap rmap;
  PackIndexMap imap;
  vreal = PackAllVariablesReal(rmap);
  vint = PackAllVariablesInt(imap);
  int real_vars_size = realVector_.size();
  int int_vars_size = intVector_.size();

  pmb->par_for(
      "Swarm::DefragVariables", 0, max_active_index_, KOKKOS_LAMBDA(const int n) {
        if (from_to_indices(n) >= 0) {
          for (int i = 0; i < real_vars_size; i++) {
            vreal(i, from_to_indices(n)) = vreal(i, n);
          }
          for (int i = 0; i < int_vars_size; i++) {
            vint(i, from_to_indices(n)) = vint(i, n);
          }
        }
      });

  // Update max_active_index_
  max_active_index_ = num_active_ - 1;
}

void Swarm::SetupPersistentMPI() {
  vbswarm->SetupPersistentMPI();

  // Index into neighbor blocks
  auto pmb = GetBlockPointer();
  auto neighborIndices_h = neighborIndices_.GetHostMirror();

  // TODO(BRR) Checks against some current limitations
  const int ndim = pmb->pmy_mesh->ndim;
  auto mesh_bcs = pmb->pmy_mesh->mesh_bcs;
  for (int n = 0; n < 2 * ndim; n++) {
    PARTHENON_REQUIRE(mesh_bcs[n] == BoundaryFlag::periodic,
                      "Only periodic boundaries supported right now!");
  }

  // Indicate which neighbor regions correspond to this meshblock
  int kmin = 1;
  int kmax = 3;
  int jmin = 1;
  int jmax = 3;
  int imin = 1;
  int imax = 3;
  if (ndim < 3) {
    kmin = 0;
    kmax = 4;
    if (ndim < 2) {
      jmin = 0;
      jmax = 4;
    }
  }
  for (int k = kmin; k < kmax; k++) {
    for (int j = jmin; j < jmax; j++) {
      for (int i = imin; i < imax; i++) {
        neighborIndices_h(k, j, i) = this_block_;
      }
    }
  }

  for (int n = 0; n < pmb->pbval->nneighbor; n++) {
    NeighborBlock &nb = pmb->pbval->neighbor[n];

    const int i = nb.ni.ox1;
    const int j = nb.ni.ox2;
    const int k = nb.ni.ox3;

    if (ndim == 1) {
      if (i == -1) {
        neighborIndices_h(0, 0, 0) = n;
      } else if (i == 0) {
        neighborIndices_h(0, 0, 1) = n;
        neighborIndices_h(0, 0, 2) = n;
      } else {
        neighborIndices_h(0, 0, 3) = n;
      }
    } else if (ndim == 2) {
      if (i == -1) {
        if (j == -1) {
          neighborIndices_h(0, 0, 0) = n;
        } else if (j == 0) {
          neighborIndices_h(0, 1, 0) = n;
          neighborIndices_h(0, 2, 0) = n;
        } else if (j == 1) {
          neighborIndices_h(0, 3, 0) = n;
        }
      } else if (i == 0) {
        if (j == -1) {
          neighborIndices_h(0, 0, 1) = n;
          neighborIndices_h(0, 0, 2) = n;
        } else if (j == 1) {
          neighborIndices_h(0, 3, 1) = n;
          neighborIndices_h(0, 3, 2) = n;
        }
      } else if (i == 1) {
        if (j == -1) {
          neighborIndices_h(0, 0, 3) = n;
        } else if (j == 0) {
          neighborIndices_h(0, 1, 3) = n;
          neighborIndices_h(0, 2, 3) = n;
        } else if (j == 1) {
          neighborIndices_h(0, 3, 3) = n;
        }
      }
    } else if (ndim == 3) {
      if (i == -1) {
        if (j == -1) {
          if (k == -1) {
            neighborIndices_h(0, 0, 0) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 0, 0) = n;
            neighborIndices_h(2, 0, 0) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 0, 0) = n;
          }
        } else if (j == 0) {
          if (k == -1) {
            neighborIndices_h(0, 1, 0) = n;
            neighborIndices_h(0, 2, 0) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 1, 0) = n;
            neighborIndices_h(1, 2, 0) = n;
            neighborIndices_h(2, 1, 0) = n;
            neighborIndices_h(2, 2, 0) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 1, 0) = n;
            neighborIndices_h(3, 2, 0) = n;
          }
        } else if (j == 1) {
          if (k == -1) {
            neighborIndices_h(0, 3, 0) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 3, 0) = n;
            neighborIndices_h(2, 3, 0) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 3, 0) = n;
          }
        }
      } else if (i == 0) {
        if (j == -1) {
          if (k == -1) {
            neighborIndices_h(0, 0, 1) = n;
            neighborIndices_h(0, 0, 2) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 0, 1) = n;
            neighborIndices_h(1, 0, 2) = n;
            neighborIndices_h(2, 0, 1) = n;
            neighborIndices_h(2, 0, 2) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 0, 1) = n;
            neighborIndices_h(3, 0, 2) = n;
          }
        } else if (j == 0) {
          if (k == -1) {
            neighborIndices_h(0, 1, 1) = n;
            neighborIndices_h(0, 1, 2) = n;
            neighborIndices_h(0, 2, 1) = n;
            neighborIndices_h(0, 2, 2) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 1, 1) = n;
            neighborIndices_h(3, 1, 2) = n;
            neighborIndices_h(3, 2, 1) = n;
            neighborIndices_h(3, 2, 2) = n;
          }
        } else if (j == 1) {
          if (k == -1) {
            neighborIndices_h(0, 3, 1) = n;
            neighborIndices_h(0, 3, 2) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 3, 1) = n;
            neighborIndices_h(1, 3, 2) = n;
            neighborIndices_h(2, 3, 1) = n;
            neighborIndices_h(2, 3, 2) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 3, 1) = n;
            neighborIndices_h(3, 3, 2) = n;
          }
        }
      } else if (i == 1) {
        if (j == -1) {
          if (k == -1) {
            neighborIndices_h(0, 0, 3) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 0, 3) = n;
            neighborIndices_h(2, 0, 3) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 0, 3) = n;
          }
        } else if (j == 0) {
          if (k == -1) {
            neighborIndices_h(0, 1, 3) = n;
            neighborIndices_h(0, 2, 3) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 1, 3) = n;
            neighborIndices_h(1, 2, 3) = n;
            neighborIndices_h(2, 1, 3) = n;
            neighborIndices_h(2, 2, 3) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 1, 3) = n;
            neighborIndices_h(3, 2, 3) = n;
          }
        } else if (j == 1) {
          if (k == -1) {
            neighborIndices_h(0, 3, 3) = n;
          } else if (k == 0) {
            neighborIndices_h(1, 3, 3) = n;
            neighborIndices_h(2, 3, 3) = n;
          } else if (k == 1) {
            neighborIndices_h(3, 3, 3) = n;
          }
        }
      }
    }
  }

  neighborIndices_.DeepCopy(neighborIndices_h);
}

bool Swarm::Send(BoundaryCommSubset phase) {
  auto blockIndex_h = blockIndex_.GetHostMirrorAndCopy();
  auto mask_h = mask_.data.GetHostMirrorAndCopy();
  auto swarm_d = GetDeviceContext();

  auto pmb = GetBlockPointer();

  // Fence to make sure particles aren't currently being transported locally
  pmb->exec_space.fence();

  int nbmax = vbswarm->bd_var_.nbmax;
  ParArrayND<int> num_particles_to_send("npts", nbmax);
  auto num_particles_to_send_h = num_particles_to_send.GetHostMirror();
  for (int n = 0; n < nbmax; n++) {
    num_particles_to_send_h(n) = 0;
    auto &nb = pmb->pbval->neighbor[n];
  }
  const int particle_size = GetParticleDataSize();
  vbswarm->particle_size = particle_size;

  int max_indices_size = 0;
  for (int n = 0; n <= max_active_index_; n++) {
    if (mask_h(n)) {
      // This particle should be sent
      if (blockIndex_h(n) >= 0) {
        num_particles_to_send_h(blockIndex_h(n))++;
        if (max_indices_size < num_particles_to_send_h(blockIndex_h(n))) {
          max_indices_size = num_particles_to_send_h(blockIndex_h(n));
        }
      }
    }
  }
  // Size-0 arrays not permitted but we don't want to short-circuit subsequent logic that
  // indicates completed communications
  max_indices_size = std::max<int>(1, max_indices_size);
  // Not a ragged-right array, just for convenience
  ParArrayND<int> particle_indices_to_send("Particle indices to send", nbmax,
                                           max_indices_size);
  auto particle_indices_to_send_h = particle_indices_to_send.GetHostMirror();
  std::vector<int> counter(nbmax, 0);
  for (int n = 0; n <= max_active_index_; n++) {
    if (mask_h(n)) {
      if (blockIndex_h(n) >= 0) {
        particle_indices_to_send_h(blockIndex_h(n), counter[blockIndex_h(n)]) = n;
        counter[blockIndex_h(n)]++;
      }
    }
  }
  num_particles_to_send.DeepCopy(num_particles_to_send_h);
  particle_indices_to_send.DeepCopy(particle_indices_to_send_h);

  num_particles_sent_ = 0;
  for (int n = 0; n < nbmax; n++) {
    // Resize buffer if too small
    auto sendbuf = vbswarm->bd_var_.send[n];
    if (sendbuf.extent(0) < num_particles_to_send_h(n) * particle_size) {
      sendbuf = ParArray1D<Real>("Buffer", num_particles_to_send_h(n) * particle_size);
      vbswarm->bd_var_.send[n] = sendbuf;
    }
    vbswarm->send_size[n] = num_particles_to_send_h(n) * particle_size;
    num_particles_sent_ += num_particles_to_send_h(n);
  }

  SwarmVariablePack<Real> vreal;
  SwarmVariablePack<int> vint;
  PackIndexMap rmap;
  PackIndexMap imap;
  vreal = PackAllVariablesReal(rmap);
  vint = PackAllVariablesInt(imap);
  int real_vars_size = realVector_.size();
  int int_vars_size = intVector_.size();
  const int ix = rmap["x"].first;
  const int iy = rmap["y"].first;
  const int iz = rmap["z"].first;

  ParArrayND<int> nrank("Neighbor rank", nbmax);
  auto nrank_h = nrank.GetHostMirrorAndCopy();
  for (int n = 0; n < nbmax; n++) {
    NeighborBlock &nb = pmb->pbval->neighbor[n];
    nrank_h(n) = nb.snb.rank;
  }
  nrank.DeepCopy(nrank_h);

  auto &bdvar = vbswarm->bd_var_;
  pmb->par_for(
      "Pack Buffers", 0, max_indices_size,
      KOKKOS_LAMBDA(const int n) {        // Max index
        for (int m = 0; m < nbmax; m++) { // Number of neighbors
          if (n < num_particles_to_send(m)) {
            const int sidx = particle_indices_to_send(m, n);
            int buffer_index = n * particle_size;
            swarm_d.MarkParticleForRemoval(sidx);
            for (int i = 0; i < real_vars_size; i++) {
              bdvar.send[m](buffer_index) = vreal(i, sidx);
              buffer_index++;
            }
            for (int i = 0; i < int_vars_size; i++) {
              bdvar.send[m](buffer_index) = static_cast<Real>(vint(i, sidx));
              buffer_index++;
            }
            // If rank is shared, apply boundary conditions here
            // TODO(BRR) Don't hardcode periodic boundary conditions
            if (nrank(m) == swarm_d.GetMyRank()) {
              double &x = vreal(ix, sidx);
              double &y = vreal(iy, sidx);
              double &z = vreal(iz, sidx);
              if (x < swarm_d.x_min_global_) {
                x = swarm_d.x_max_global_ - (swarm_d.x_min_global_ - x);
              }
              if (x > swarm_d.x_max_global_) {
                x = swarm_d.x_min_global_ + (x - swarm_d.x_max_global_);
              }
              if (y < swarm_d.y_min_global_) {
                y = swarm_d.y_max_global_ - (swarm_d.y_min_global_ - y);
              }
              if (y > swarm_d.y_max_global_) {
                y = swarm_d.y_min_global_ + (y - swarm_d.y_max_global_);
              }
              if (z < swarm_d.z_min_global_) {
                z = swarm_d.z_max_global_ - (swarm_d.z_min_global_ - z);
              }
              if (z > swarm_d.z_max_global_) {
                z = swarm_d.z_min_global_ + (z - swarm_d.z_max_global_);
              }
            }
          }
        }
      });

  // Count all the particles that are Active and Not on this block, if nonzero,
  // copy into buffers (if no send already for that buffer) and send

  RemoveMarkedParticles();

  vbswarm->Send(phase);
  return true;
}

template <typename T>
vpack_types::SwarmVarList<T> Swarm::MakeVarListAll_(ParticleVariableVector<T> variables) {
  int size = 0;
  vpack_types::SwarmVarList<T> vars;

  for (auto it = variables.rbegin(); it != variables.rend(); ++it) {
    auto v = *it;
    vars.push_front(v);
    size++;
  }
  return vars;
}

SwarmVariablePack<Real> Swarm::PackAllVariablesReal(PackIndexMap &vmap) {
  std::vector<std::string> names;
  for (auto &v : realVector_) {
    names.push_back(v->label());
  }
  return PackVariablesReal(names, vmap);
}

SwarmVariablePack<int> Swarm::PackAllVariablesInt(PackIndexMap &vmap) {
  std::vector<std::string> names;
  for (auto &v : intVector_) {
    names.push_back(v->label());
  }
  return PackVariablesInt(names, vmap);
}

SwarmVariablePack<Real> Swarm::PackVariablesReal(const std::vector<std::string> &names,
                                                 PackIndexMap &vmap) {
  vpack_types::SwarmVarList<Real> vars = MakeVarListAll_<Real>(realVector_);

  auto pack = MakeSwarmPack<Real>(vars, &vmap);
  SwarmPackIndxPair<Real> value;
  value.pack = pack;
  value.map = vmap;
  return pack;
}
SwarmVariablePack<int> Swarm::PackVariablesInt(const std::vector<std::string> &names,
                                               PackIndexMap &vmap) {
  vpack_types::SwarmVarList<int> vars = MakeVarListAll_(intVector_);

  auto pack = MakeSwarmPack<int>(vars, &vmap);
  SwarmPackIndxPair<int> value;
  value.pack = pack;
  value.map = vmap;
  return pack;
}

bool Swarm::Receive(BoundaryCommSubset phase) {
  // Ensure all local deep copies marked BoundaryStatus::completed are actually received
  GetBlockPointer()->exec_space.fence();
  auto pmb = GetBlockPointer();

  // Populate buffers
  vbswarm->Receive(phase);

  // Copy buffers into swarm data on this proc
  const int maxneighbor = vbswarm->bd_var_.nbmax;
  int total_received_particles = 0;
  std::vector<int> neighbor_received_particles(maxneighbor);
  for (int n = 0; n < maxneighbor; n++) {
    if (vbswarm->bd_var_.flag[pmb->pbval->neighbor[n].bufid] == BoundaryStatus::arrived) {
      PARTHENON_DEBUG_REQUIRE(vbswarm->recv_size[n] % vbswarm->particle_size == 0,
                              "Receive buffer is not divisible by particle size!");
      neighbor_received_particles[n] = vbswarm->recv_size[n] / vbswarm->particle_size;
      total_received_particles += neighbor_received_particles[n];
    } else {
      neighbor_received_particles[n] = 0;
    }
  }

  auto &bdvar = vbswarm->bd_var_;

  if (total_received_particles > 0) {
    ParArrayND<int> new_indices;
    auto new_mask = AddEmptyParticles(total_received_particles, new_indices);
    SwarmVariablePack<Real> vreal;
    SwarmVariablePack<int> vint;
    PackIndexMap rmap;
    PackIndexMap imap;
    vreal = PackAllVariablesReal(rmap);
    vint = PackAllVariablesInt(imap);
    int real_vars_size = realVector_.size();
    int int_vars_size = intVector_.size();
    const int ix = rmap["x"].first;
    const int iy = rmap["y"].first;
    const int iz = rmap["z"].first;

    ParArrayND<int> neighbor_index("Neighbor index", total_received_particles);
    ParArrayND<int> buffer_index("Buffer index", total_received_particles);
    auto neighbor_index_h = neighbor_index.GetHostMirror();
    auto buffer_index_h = buffer_index.GetHostMirror();

    int id = 0;
    for (int n = 0; n < maxneighbor; n++) {
      for (int m = 0; m < neighbor_received_particles[n]; m++) {
        neighbor_index_h(id) = n;
        buffer_index_h(id) = m;
        id++;
      }
    }
    neighbor_index.DeepCopy(neighbor_index_h);
    buffer_index.DeepCopy(buffer_index_h);

    // construct map from buffer index to swarm index (or just return vector of indices!)
    const int particle_size = GetParticleDataSize();
    auto swarm_d = GetDeviceContext();

    pmb->par_for(
        "Unpack buffers", 0, total_received_particles - 1, KOKKOS_LAMBDA(const int n) {
          const int sid = new_indices(n);
          const int nid = neighbor_index(n);
          const int bid = buffer_index(n);
          for (int i = 0; i < real_vars_size; i++) {
            vreal(i, sid) = bdvar.recv[nid](bid * particle_size + i);
          }
          for (int i = 0; i < int_vars_size; i++) {
            vint(i, sid) = static_cast<int>(
                bdvar.recv[nid]((real_vars_size + bid) * particle_size + i));
          }

          double &x = vreal(ix, sid);
          double &y = vreal(iy, sid);
          double &z = vreal(iz, sid);
          // TODO(BRR) Don't hardcode periodic boundary conditions
          if (x < swarm_d.x_min_global_) {
            x = swarm_d.x_max_global_ - (swarm_d.x_min_global_ - x);
          }
          if (x > swarm_d.x_max_global_) {
            x = swarm_d.x_min_global_ + (x - swarm_d.x_max_global_);
          }
          if (y < swarm_d.y_min_global_) {
            y = swarm_d.y_max_global_ - (swarm_d.y_min_global_ - y);
          }
          if (y > swarm_d.y_max_global_) {
            y = swarm_d.y_min_global_ + (y - swarm_d.y_max_global_);
          }
          if (z < swarm_d.z_min_global_) {
            z = swarm_d.z_max_global_ - (swarm_d.z_min_global_ - z);
          }
          if (z > swarm_d.z_max_global_) {
            z = swarm_d.z_min_global_ + (z - swarm_d.z_max_global_);
          }
        });
  }

  bool all_boundaries_received = true;
  for (int n = 0; n < pmb->pbval->nneighbor; n++) {
    NeighborBlock &nb = pmb->pbval->neighbor[n];
    if (bdvar.flag[nb.bufid] == BoundaryStatus::arrived) {
      bdvar.flag[nb.bufid] = BoundaryStatus::completed;
    } else if (bdvar.flag[nb.bufid] == BoundaryStatus::waiting) {
      all_boundaries_received = false;
    }
  }

  return all_boundaries_received;
}

void Swarm::AllocateComms(std::weak_ptr<MeshBlock> wpmb) {
  if (wpmb.expired()) return;

  std::shared_ptr<MeshBlock> pmb = wpmb.lock();

  // Create the boundary object
  vbswarm = std::make_shared<BoundarySwarm>(pmb);

  // Enroll SwarmVariable object
  vbswarm->bswarm_index = pmb->pbswarm->bswarms.size();
  pmb->pbswarm->bswarms.push_back(vbswarm);
}

} // namespace parthenon
