//========================================================================================
// (C) (or copyright) 2020. Triad National Security, LLC. All rights reserved.
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
#ifndef INTERFACE_CONTAINER_HPP_
#define INTERFACE_CONTAINER_HPP_

#include <map>
#include <memory>
#include <string>
#include <utility> // <pair>
#include <vector>
#include "globals.hpp"
#include "SparseVariable.hpp"
#include "Variable.hpp"

namespace parthenon {
///
/// Interface to underlying infrastructure for data declaration and access.
/// Date: August 22, 2019
///
///
/// The container class is a container for the variables that make up
/// the simulation.  At this point it is expected that this includes
/// both simulation parameters and state variables, but that could
/// change in the future.
///
/// The container class will provide the following methods:
///


class MeshBlock;

template <typename T>
class Container {
 public:
  //-----------------
  // Public Variables
  //-----------------
  MeshBlock* pmy_block = nullptr; // ptr to MeshBlock

  //-----------------
  //Public Methods
  //-----------------
  /// Constructor
  Container<T>() = default;

  /// We can initialize a container with slices from a different
  /// container.  For variables that have the sparse tag, this will
  /// return the sparse slice.  All other variables are added as
  /// is. This call returns a new container.
  ///
  /// @param sparse_id The sparse id
  /// @return New container with slices from all variables
  Container<T> sparseSlice(int sparse_id);

  ///
  /// Set the pointer to the mesh block for this container
  void setBlock(MeshBlock *pmb) { pmy_block = pmb; }

  ///
  /// Allocate and add a variable<T> to the container
  ///
  /// This function will eventually look at the metadata flags to
  /// identify the size of the first dimension based on the
  /// topological location.
  ///
  /// @param label the name of the variable
  /// @param metadata the metadata associated with the variable
  /// @param dims the size of each element
  ///
  void Add(const std::string label,
           const Metadata &metadata,
           const std::vector<int> dims);

  ///
  /// Allocate and add a variable<T> to the container
  ///
  /// This function will eventually look at the metadata flags to
  /// identify the size of the first dimension based on the
  /// topological location.
  ///
  /// @param labelVector the array of names of variables
  /// @param metadata the metadata associated with the variable
  /// @param dims the size of each element
  ///
  void Add(const std::vector<std::string> labelVector,
           const Metadata &metadata,
           const std::vector<int> dims);

  ///
  /// Allocate and add a variable<T> to the container
  ///
  /// This function will eventually look at the metadata flags to
  /// identify the size of the first dimension based on the
  /// topological location.  Dimensions will be taken from the metadata.
  ///
  /// @param label the name of the variable
  /// @param metadata the metadata associated with the variable
  ///
  void Add(const std::string label, const Metadata &metadata);

  ///
  /// Allocate and add a variable<T> to the container
  ///
  /// This function will eventually look at the metadata flags to
  /// identify the size of the first dimension based on the
  /// topological location.  Dimensions will be taken from the metadata.
  ///
  /// @param labelVector the array of names of variables
  /// @param metadata the metadata associated with the variable
  ///
  void Add(const std::vector<std::string> labelVector, const Metadata &metadata);

  void Add(std::shared_ptr<CellVariable<T>> var) {
    _varVector.push_back(var);
    _varMap[var->label()] = var;
  }
  void Add(std::shared_ptr<FaceVariable<T>> var) {
    _faceVector.push_back(var);
    _faceMap[var->label()] = var;
  }
  void Add(std::shared_ptr<SparseVariable<T>> var) {
    _sparseVector.push_back(var);
    _sparseMap[var->label()] = var;
  }

  //
  // Queries related to CellVariable objects
  //
  const CellVariableVector<T>& GetCellVariableVector() const {
    return _varVector;
  }
  CellVariable<T>& Get(std::string label) {
    auto it = _varMap.find(label);
    if (it == _varMap.end()) {
      throw std::invalid_argument(std::string("\n") +
                                 std::string(label) +
                                 std::string(" array not found in Get()\n") );
    }
    return *(it->second);
  }

  CellVariable<T>& Get(const int index) {
    return *(_varVector[index]);
  }

  int Index(const std::string& label) {
    for (int i = 0; i < _varVector.size(); i++) {
      if (! _varVector[i]->label().compare(label)) return i;
    }
    return -1;
  }

  //
  // Queries related to SparseVariable objects
  //
  const SparseVector<T> GetSparseVector() const {
    return _sparseVector;
  }
  SparseVariable<T>& GetSparseVariable(const std::string& label) {
    auto it = _sparseMap.find(label);
    if (it == _sparseMap.end()) {
      throw std::invalid_argument("_sparseMap does not have " + label);
    }
    return *(it->second);
  }

  SparseMap<T>& GetSparseMap(const std::string& label) {
    return GetSparseVariable(label).GetMap();
  }

  CellVariableVector<T>& GetSparseVector(const std::string& label) {
    return GetSparseVariable(label).GetVector();
  }

  CellVariable<T>& Get(const std::string& label, const int sparse_id) {
    return GetSparseVariable(label).Get(sparse_id);
  }

  std::vector<int>& GetSparseIndexMap(const std::string& label) {
    return GetSparseVariable(label).GetIndexMap();
  }

  //
  // Queries related to FaceVariable objects
  //
  FaceVariable<T>& GetFace(std::string label) {
    auto it = _faceMap.find(label);
    if (it == _faceMap.end()) {
      throw std::invalid_argument (std::string("\n") +
                                   std::string(label) +
                                   std::string(" array not found in Get() Face\n") );
    }
    return *(it->second);
  }

  ParArrayND<Real>& GetFace(std::string label, int dir) {
    return GetFace(label).Get(dir);
  }

  ///
  /// Get an edge variable from the container
  /// @param label the name of the variable
  /// @return the CellVariable<T> if found or throw exception
  ///
  EdgeVariable<T> *GetEdge(std::string label) {
    // for (auto v : _edgeVector) {
    //   if (! v->label().compare(label)) return v;
    // }
    throw std::invalid_argument (std::string("\n") +
                                 std::string(label) +
                                 std::string(" array not found in Get() Edge\n") );
  }

  /// Gets an array of real variables from container.
  /// @param names is the variables we want
  /// @param indexCount a map of names to std::pair<index,count> for each name
  /// @param sparse_ids if specified is list of sparse ids we are interested in.  Note
  ///        that non-sparse variables specified are aliased in as is.
  int GetCellVariables(const std::vector<std::string>& names,
                   std::vector<CellVariable<T>>& vRet,
                   std::map<std::string,std::pair<int,int>>& indexCount,
                   const std::vector<int>& sparse_ids = {});

  ///
  /// Remove a variable from the container or throw exception if not
  /// found.
  /// @param label the name of the variable to be deleted
  void Remove(const std::string label);


  /// Print list of labels in container
  void print();

  // return number of stored arrays
  int size() {return _varVector.size();}

  // // returne variable at index
  // std::weak_ptr<CellVariable<T>>& at(const int index) {
  //   return _varVector.at(index);
  // }

  const FaceVector<T>& GetFaceVector() const {
    return _faceVector;
  }


  // Communication routines
  void ResetBoundaryCellVariables();
  void SetupPersistentMPI();
  void SetBoundaries();
  void SendBoundaryBuffers();
  void ReceiveAndSetBoundariesWithWait();
  bool ReceiveBoundaryBuffers();
  void StartReceiving(BoundaryCommSubset phase);
  void ClearBoundary(BoundaryCommSubset phase);
  void SendFluxCorrection();
  bool ReceiveFluxCorrection();
  static TaskStatus StartReceivingTask(Container<T>& rc) {
    rc.StartReceiving(BoundaryCommSubset::all);
    return TaskStatus::complete;
  }
  static TaskStatus SendFluxCorrectionTask(Container<T>& rc) {
    rc.SendFluxCorrection();
    return TaskStatus::complete;
  }
  static TaskStatus ReceiveFluxCorrectionTask(Container<T>& rc) {
    if (!rc.ReceiveFluxCorrection()) return TaskStatus::incomplete;
    return TaskStatus::complete;
  }
  static TaskStatus SendBoundaryBuffersTask(Container<T>& rc) {
    rc.SendBoundaryBuffers();
    return TaskStatus::complete;
  }
  static TaskStatus ReceiveBoundaryBuffersTask(Container<T>& rc) {
    if ( !rc.ReceiveBoundaryBuffers() ) return TaskStatus::incomplete;
    return TaskStatus::complete;
  }
  static TaskStatus SetBoundariesTask(Container<T>& rc) {
    rc.SetBoundaries();
    return TaskStatus::complete;
  }
  static TaskStatus ClearBoundaryTask(Container<T>& rc) {
    rc.ClearBoundary(BoundaryCommSubset::all);
    return TaskStatus::complete;
  }

 private:
  int debug=0;

  CellVariableVector<T> _varVector = {}; ///< the saved variable array
  FaceVector<T> _faceVector = {};  ///< the saved face arrays
  SparseVector<T> _sparseVector = {};

  MapToCellVars<T> _varMap = {};
  MapToFace<T> _faceMap = {};
  MapToSparse<T> _sparseMap = {};

  void calcArrDims_(std::array<int, 6>& arrDims,
                    const std::vector<int>& dims);
};

} // namespace parthenon
#endif // INTERFACE_CONTAINER_HPP_
