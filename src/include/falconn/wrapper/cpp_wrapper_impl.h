#ifndef __CPP_WRAPPER_IMPL_H__
#define __CPP_WRAPPER_IMPL_H__

#include "wrapper.h"

#include "composite_hash_table.h"
#include "cosine_distance.h"
#include "data_storage.h"
#include "hyperplane_hash.h"
#include "lsh_table.h"
#include "nn_query.h"
#include "polytope_hash.h"
#include "probing_hash_table.h"

namespace falconn {
namespace wrappers {

template<typename PointType>
struct PointTypeTraitsInternal {};

// TODO: get rid of these type trait classes once CosineDistance and the LSH
// classes are specialized on PointType (if we want to specialize on point 
// type).
template<typename CoordinateType>
class PointTypeTraitsInternal<DenseVector<CoordinateType>> {
 public:
  typedef core::CosineDistanceDense<CoordinateType> CosineDistance;
  template<typename HashType> using
      HPHash = core::HyperplaneHashDense<CoordinateType, HashType>;
  template<typename HashType> using
      CPHash = core::CrossPolytopeHashDense<CoordinateType, HashType>;
  
  template<typename HashType>
  static std::unique_ptr<CPHash<HashType>> construct_cp_hash(
      const LSHConstructionParameters& params) {
    std::unique_ptr<CPHash<HashType>> res(new CPHash<HashType>(params.dimension,
        params.k, params.l, params.num_rotations, params.last_cp_dimension,
        params.seed ^ 93384688));
    return std::move(res);
  }
};

template<typename CoordinateType, typename IndexType>
class PointTypeTraitsInternal<SparseVector<CoordinateType, IndexType>> {
 public:
  typedef core::CosineDistanceSparse<CoordinateType, IndexType> CosineDistance;
  template<typename HashType> using
      HPHash = core::HyperplaneHashSparse<CoordinateType, HashType, IndexType>;
  template<typename HashType> using
      CPHash = core::CrossPolytopeHashSparse<CoordinateType, HashType,
          IndexType>;

  template<typename HashType>
  static std::unique_ptr<CPHash<HashType>> construct_cp_hash(
      const LSHConstructionParameters& params) {
    std::unique_ptr<CPHash<HashType>> res(new CPHash<HashType>(params.dimension,
        params.k, params.l, params.num_rotations,
        params.feature_hashing_dimension, params.last_cp_dimension,
        params.seed ^ 93384688));
    return std::move(res);
  }
};

template<typename PointSet>
class DataStorageAdapter {
 public:
  DataStorageAdapter() {
    static_assert(FalseStruct<PointSet>::value,
        "Point set type not supported.");
  }
 
  template<typename PS>
  struct FalseStruct : std::false_type {};
};

template<typename PointType>
class DataStorageAdapter<std::vector<PointType>> {
 public:
  template<typename KeyType> using
      DataStorage = core::ArrayDataStorage<PointType, KeyType>;

  template<typename KeyType>
  static std::unique_ptr<DataStorage<KeyType>> construct_data_storage(
      const std::vector<PointType>& points) {
    std::unique_ptr<DataStorage<KeyType>> res(new core::ArrayDataStorage
        <PointType, KeyType>(points));
    return std::move(res);
  }
};



template<typename PointType>
struct ComputeNumberOfHashFunctions {
  static void compute(int_fast32_t, LSHConstructionParameters*) {
    static_assert(FalseStruct<PointType>::value, "Point type not supported.");
  }
  template<typename T> struct FalseStruct : std::false_type {};
};

template<typename CoordinateType>
struct ComputeNumberOfHashFunctions<DenseVector<CoordinateType>> {
  static void compute(int_fast32_t number_of_hash_bits,
      LSHConstructionParameters* params) {
    if (params->lsh_family == LSHFamily::Hyperplane) {
      params->k = number_of_hash_bits;
    } else if (params->lsh_family == LSHFamily::CrossPolytope) {
      if (params->dimension <= 0) {
        throw LSHNNTableSetupError("Vector dimension must be set to determine "
            "the number of dense cross polytope hash functions.");
      }
      core::cp_hash_helpers::compute_k_parameters_for_bits(params->dimension,
          number_of_hash_bits, &(params->k), &(params->last_cp_dimension));
    } else {
      throw LSHNNTableSetupError("Cannot set paramters for unknown hash "
          "family.");
    }
  }
};

template<typename CoordinateType, typename IndexType>
struct ComputeNumberOfHashFunctions<SparseVector<CoordinateType, IndexType>> {
  static void compute(int_fast32_t number_of_hash_bits,
      LSHConstructionParameters* params) {
    if (params->lsh_family == LSHFamily::Hyperplane) {
      params->k = number_of_hash_bits;
    } else if (params->lsh_family == LSHFamily::CrossPolytope) {
      if (params->feature_hashing_dimension <= 0) {
        throw LSHNNTableSetupError("Feature hashing dimension must be set to "
            "determine  the number of sparse cross polytope hash functions.");
      }
      core::cp_hash_helpers::compute_k_parameters_for_bits(
          params->feature_hashing_dimension, number_of_hash_bits, &(params->k),
          &(params->last_cp_dimension));
    } else {
      throw LSHNNTableSetupError("Cannot set paramters for unknown hash "
          "family.");
    }
  }
};

template<
typename PointType,
typename KeyType,
typename DistanceType,
typename DistanceFunction,
typename LSHTable,
typename LSHFunction,
typename HashTable,
typename CompositeHashTable,
typename NNQuery,
typename DataStorage>
class LSHNNTableWrapper : public LSHNearestNeighborTable<PointType, KeyType> {
 public:
  LSHNNTableWrapper(
      std::unique_ptr<LSHFunction> lsh,
      std::unique_ptr<LSHTable> lsh_table,
      std::unique_ptr<typename HashTable::Factory> hash_table_factory,
      std::unique_ptr<CompositeHashTable> composite_hash_table,
      std::unique_ptr<typename LSHTable::Query> query,
      std::unique_ptr<NNQuery> nn_query,
      std::unique_ptr<DataStorage> data_storage)
          : lsh_(std::move(lsh)),
            lsh_table_(std::move(lsh_table)),
            hash_table_factory_(std::move(hash_table_factory)),
            composite_hash_table_(std::move(composite_hash_table)),
            query_(std::move(query)),
            nn_query_(std::move(nn_query)),
            data_storage_(std::move(data_storage)) {
    num_probes_ = lsh_->get_l();
  }
  
  void set_num_probes(int_fast64_t num_probes) {
    if (num_probes <= 0) {
      throw LSHNearestNeighborTableError(
          "Number of probes must be at least 1.");
    }
    num_probes_ = num_probes;
  }

  int_fast64_t get_num_probes() {
    return num_probes_;
  }
  
  void set_max_num_candidates(int_fast64_t max_num_candidates) {
    max_num_candidates_ = max_num_candidates;
  }

  int_fast64_t get_max_num_candidates() {
    return max_num_candidates_;
  }

  KeyType find_closest(const PointType& q) {
    return nn_query_->find_closest(q, q, num_probes_, max_num_candidates_);
  }
  
  void find_k_nearest_neighbors(const PointType& q,
                                int_fast64_t k,
                                std::vector<KeyType>* result) {
    nn_query_->find_k_nearest_neighbors(q, q, k, num_probes_,
        max_num_candidates_, result);
  }

  void find_near_neighbors(const PointType& q,
                           DistanceType threshold,
                           std::vector<KeyType>* result) {
    nn_query_->find_near_neighbors(q, q, threshold, num_probes_,
                                   max_num_candidates_, result);
  }

  void get_candidates_with_duplicates(const PointType& q,
                                      std::vector<KeyType>* result) {
    query_->get_candidates_with_duplicates(q, num_probes_, max_num_candidates_,
                                           result);
  }

  void get_unique_candidates(const PointType& q,
                             std::vector<KeyType>* result) {
    query_->get_unique_candidates(q, num_probes_, max_num_candidates_, result);
  }

  void get_unique_sorted_candidates(const PointType& q,
                                    std::vector<KeyType>* result) {
    query_->get_unique_sorted_candidates(q, num_probes_, max_num_candidates_,
                                         result);
  }

  void reset_query_statistics() {
    nn_query_->reset_query_statistics();
  }

  QueryStatistics get_query_statistics() {
    return nn_query_->get_query_statistics();
  }

  ~LSHNNTableWrapper() {}

 protected:
  std::unique_ptr<LSHFunction> lsh_;
  std::unique_ptr<LSHTable> lsh_table_;
  std::unique_ptr<typename HashTable::Factory> hash_table_factory_;
  std::unique_ptr<CompositeHashTable> composite_hash_table_;
  std::unique_ptr<typename LSHTable::Query> query_;
  std::unique_ptr<NNQuery> nn_query_;
  std::unique_ptr<DataStorage> data_storage_;

  int_fast64_t num_probes_;
  int_fast64_t max_num_candidates_ = this->kNoMaxNumCandidates;
};


template<
typename PointType,
typename KeyType,
typename PointSet,
typename DistanceFunc,
typename LSH,
typename HashType>
std::unique_ptr<LSHNearestNeighborTable<PointType, KeyType>>
construction_helper(const PointSet& points,
                    const LSHConstructionParameters& params,
                    std::unique_ptr<LSH> lsh) {
  typedef typename PointTypeTraits<PointType>::ScalarType ScalarType;

  typedef typename DataStorageAdapter<PointSet>::template DataStorage<KeyType>
      DataStorage;
  std::unique_ptr<DataStorage> data_storage(std::move(
      DataStorageAdapter<PointSet>::template construct_data_storage<KeyType>(
          points)));
  
  typedef core::StaticLinearProbingHashTable<HashType, KeyType> HashTable;
  // TODO: should we go to the next prime here?
  std::unique_ptr<typename HashTable::Factory> factory(
      new typename HashTable::Factory(2 * points.size()));
  
  typedef core::StaticCompositeHashTable<HashType, KeyType, HashTable>
      CompositeTable;
  std::unique_ptr<CompositeTable> composite_table(new CompositeTable(
      params.l, factory.get()));
  
  typedef core::StaticLSHTable<PointType, KeyType, LSH, HashType,
      CompositeTable, DataStorage> LSHTable;
  std::unique_ptr<LSHTable> lsh_table(new LSHTable(lsh.get(),
      composite_table.get(), *data_storage));
  
  std::unique_ptr<typename LSHTable::Query> query(
      new typename LSHTable::Query(*lsh_table));

  typedef core::NearestNeighborQuery<typename LSHTable::Query, PointType,
      KeyType, PointType, ScalarType, DistanceFunc, DataStorage> NNQuery;
  std::unique_ptr<NNQuery> nn_query(new NNQuery(query.get(), *data_storage));

  std::unique_ptr<LSHNearestNeighborTable<PointType, KeyType>> result(
      new LSHNNTableWrapper<PointType, KeyType, ScalarType,
          DistanceFunc, LSHTable, LSH, HashTable, CompositeTable,
          NNQuery, DataStorage>(std::move(lsh), std::move(lsh_table),
              std::move(factory), std::move(composite_table), std::move(query),
              std::move(nn_query), std::move(data_storage)));

  return std::move(result);
}



}  // namespace wrappers
}  // namespace falconn



namespace falconn {

template<typename PointType>
void compute_number_of_hash_functions(int_fast32_t number_of_hash_bits,
    LSHConstructionParameters* params) {
  wrappers::ComputeNumberOfHashFunctions<PointType>::compute(
      number_of_hash_bits, params);
}


template<
typename PointType,
typename KeyType,
typename PointSet>
std::unique_ptr<LSHNearestNeighborTable<PointType, KeyType>> construct_table(
    const PointSet& points,
    const LSHConstructionParameters& params) {
  if (params.dimension < 1) {
    throw LSHNNTableSetupError("Point dimension must be at least 1. Maybe you "
        "forgot to set the point dimension in the parameter struct?");
  }
  if (params.k < 1) {
    throw LSHNNTableSetupError("The number of hash functions k must be at "
        "least 1. Maybe you forgot to set k in the parameter struct?");
  }
  if (params.l < 1) {
    throw LSHNNTableSetupError("The number of hash tables l must be at "
        "least 1. Maybe you forgot to set l in the parameter struct?");
  }

  // TODO: can we allow Unknown here, but then allow only to return all the
  // (unique) candidates?
  if (params.distance_function != DistanceFunction::NegativeInnerProduct) {
    throw LSHNNTableSetupError("Unknown distance function. Maybe you forgot to "
        "set the distance function in the parameter struct?");
  }

  // TODO: automatically adapt to 64 bit if necessary
  typedef uint32_t HashType;
  typedef typename wrappers::PointTypeTraitsInternal<PointType>::CosineDistance
      DistanceFunc;

  if (params.lsh_family == LSHFamily::Hyperplane) {
    typedef typename wrappers::PointTypeTraitsInternal<PointType>::template
        HPHash<HashType> LSH;
    std::unique_ptr<LSH> lsh(new LSH(params.dimension, params.k, params.l,
                                     params.seed ^ 93384688));
    return std::move(wrappers::construction_helper<PointType, KeyType,
        PointSet, DistanceFunc, LSH, HashType>(points, params,
        std::move(lsh)));
  } else if (params.lsh_family == LSHFamily::CrossPolytope) {
    if (params.num_rotations < 0) {
      throw LSHNNTableSetupError("The number of pseudo-random rotations for "
          "the cross polytope hash must be non-negative. Maybe you forgot to "
          "set num_rotations in the parameter struct?");
    }
    if (params.last_cp_dimension <= 0) {
      throw LSHNNTableSetupError("The last cross polytope dimension for "
          "the cross polytope hash must be at least 1. Maybe you forgot to "
          "set last_cp_dimension in the parameter struct?");
    }

    // TODO: for sparse vectors, also check feature_hashing_dimension here (it
    // is checked in the CP hash class, but the error message is less verbose).

    typedef typename wrappers::PointTypeTraitsInternal<PointType>::template
        CPHash<HashType> LSH;
    std::unique_ptr<LSH> lsh(std::move(
        wrappers::PointTypeTraitsInternal<PointType>::template
            construct_cp_hash<HashType>(params)));
    return std::move(wrappers::construction_helper<PointType, KeyType, PointSet,
        DistanceFunc, LSH, HashType>(points, params, std::move(lsh)));
  } else {
    throw LSHNNTableSetupError("Unknown hash family. Maybe you forgot to set "
        "the hash family in the parameter struct?");
  }

  throw LSHNNTableSetupError("Reached unexpected control flow point.");
}

}  // namespace falconn


#endif