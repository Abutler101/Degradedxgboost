/*!
 * Copyright 2014-2021 by Contributors
 * \file group_data.h
 * \brief this file defines utils to group data by integer keys
 *     Input: given input sequence (key,value), (k1,v1), (k2,v2)
 *     Ouptupt: an array of values data = [v1,v2,v3 .. vn]
 *              and a group pointer ptr,
 *              data[ptr[k]:ptr[k+1]] contains values that corresponds to key k
 *
 * This can be used to construct CSR/CSC matrix from un-ordered input
 * The major algorithm is a two pass linear scan algorithm that requires two pass scan over the data
 * \author Tianqi Chen
 */
#ifndef XGBOOST_COMMON_GROUP_DATA_H_
#define XGBOOST_COMMON_GROUP_DATA_H_

#include <cstddef>
#include <vector>
#include <algorithm>
#include <utility>

#include "xgboost/base.h"
#include "boost/container/small_vector.hpp"

namespace xgboost {
namespace common {
/*!
 * \brief multi-thread version of group builder
 * \tparam ValueType type of entries in the sparse matrix
 * \tparam SizeType type of the index range holder
 * \tparam is_row_major bool value helps to reduce memory for row major
 */
template<typename ValueType, typename SizeType = bst_ulong, bool is_row_major = false>
class ParallelGroupBuilder {
 public:
  /**
   * \brief parallel group builder of data.
   *
   * \param [in,out]  p_rptr          Row offsets for CSR matrix.
   * \param [in,out]  p_data          Data vector to populate
   * \param           base_row_offset (Optional) If the matrix we are building
   * is already partially populated, use this to indicate the row index we are
   * starting from. This saves considerable amounts of time/memory when
   * incrementaly building.
   */
  ParallelGroupBuilder(std::vector<SizeType> *p_rptr,
                       std::vector<ValueType> *p_data,
                       size_t base_row_offset = 0)
      : rptr_(*p_rptr),
        data_(*p_data),
        base_row_offset_(base_row_offset) {}

  /*!
   * \brief step 1: initialize the helper, with hint of number keys
   *                and thread used in the construction
   * \param max_key number of keys in the matrix, can be smaller than expected,
   *                for row major adapter max_key is equal to batch size
   * \param nthread number of thread that will be used in construction
   */
  void InitBudget(std::size_t max_key, int nthread) {
    thread_rptr_.resize(nthread);
    const size_t full_size = is_row_major ? max_key : max_key - std::min(base_row_offset_, max_key);
    thread_displacement_ = is_row_major ? full_size / nthread : 0;
    for (std::size_t i = 0; i < thread_rptr_.size() - 1; ++i) {
      const size_t thread_size = is_row_major ? thread_displacement_ : full_size;
      thread_rptr_[i].resize(thread_size, 0);
    }
    const size_t last_thread_size = is_row_major ? (full_size - (nthread - 1)*thread_displacement_)
                                                 : full_size;
    thread_rptr_[nthread - 1].resize(last_thread_size, 0);
  }

  /*!
   * \brief step 2: add budget to each key
   * \param key the key
   * \param threadid the id of thread that calls this function
   * \param nelem number of element budget add to this row
   */
  void AddBudget(std::size_t key, int threadid, SizeType nelem = 1) {
    std::vector<SizeType> &trptr = thread_rptr_[threadid];
    size_t offset_key = is_row_major ? (key - base_row_offset_ - threadid*thread_displacement_)
                                     : (key - base_row_offset_);
    if (trptr.size() < offset_key + 1) {
      trptr.resize(offset_key + 1, 0);
    }
    trptr[offset_key] += nelem;
  }

  /*! \brief step 3: initialize the necessary storage */
  inline void InitStorage() {
    if (is_row_major) {
      size_t expected_rows = 0;
      for (std::size_t tid = 0; tid < thread_rptr_.size(); ++tid) {
        expected_rows += thread_rptr_[tid].size();
      }
      // initialize rptr to be beginning of each segment
      SizeType rptr_fill_value = rptr_.empty() ? 0 : rptr_.back();
      rptr_.resize(expected_rows + base_row_offset_ + 1, rptr_fill_value);

      std::size_t count = 0;
      size_t offset_idx = base_row_offset_ + 1;
      for (std::size_t tid = 0; tid < thread_rptr_.size(); ++tid) {
        std::vector<SizeType> &trptr = thread_rptr_[tid];
        for (std::size_t i = 0; i < trptr.size(); ++i) {
          std::size_t thread_count = trptr[i];  // how many entries in this row
          trptr[i] = count + rptr_fill_value;
          count += thread_count;
          if (offset_idx < rptr_.size()) {
            rptr_[offset_idx++] += count;
          }
        }
      }
      data_.resize(rptr_.back());  // usage of empty allocator can help to improve performance
    } else {
      // set rptr to correct size
      SizeType rptr_fill_value = rptr_.empty() ? 0 : rptr_.back();
      for (std::size_t tid = 0; tid < thread_rptr_.size(); ++tid) {
        if (rptr_.size() <= thread_rptr_[tid].size() + base_row_offset_) {
          rptr_.resize(thread_rptr_[tid].size() + base_row_offset_ + 1,
                       rptr_fill_value);  // key + 1
        }
      }
      // initialize rptr to be beginning of each segment
      std::size_t count = 0;
      for (std::size_t i = base_row_offset_; i + 1 < rptr_.size(); ++i) {
        for (std::size_t tid = 0; tid < thread_rptr_.size(); ++tid) {
          std::vector<SizeType> &trptr = thread_rptr_[tid];
          if (i < trptr.size() +
                      base_row_offset_) {  // i^th row is assigned for this thread
            std::size_t thread_count =
                trptr[i - base_row_offset_];  // how many entries in this row
            trptr[i - base_row_offset_] = count + rptr_.back();
            count += thread_count;
          }
        }
        rptr_[i + 1] += count;  // pointer accumulated from all thread
      }
      data_.resize(rptr_.back());
    }
  }

  /*!
   * \brief step 4: add data to the allocated space,
   *   the calls to this function should be exactly match previous call to AddBudget
   *
   * \param key the key of group.
   * \param value The value to be pushed to the group.
   * \param threadid the id of thread that calls this function
   */
  void Push(std::size_t key, ValueType&& value, int threadid) {
    size_t offset_key = is_row_major ? (key - base_row_offset_ - threadid * thread_displacement_)
                                     : (key - base_row_offset_);
    SizeType &rp = thread_rptr_[threadid][offset_key];
    data_[rp++] = std::move(value);
  }

 private:
  /*! \brief pointer to the beginning and end of each continuous key */
  std::vector<SizeType> &rptr_;
  /*! \brief index of nonzero entries in each row */
  std::vector<ValueType> &data_;
  /*! \brief thread local data structure */
  boost::container::small_vector<std::vector<SizeType>,5 > thread_rptr_;
  /** \brief Used when rows being pushed into the builder are strictly above some number. */
  size_t base_row_offset_;
  /** \brief Used for row major adapters to handle reduced thread local memory allocation */
  size_t thread_displacement_;
};
}  // namespace common
}  // namespace xgboost
#endif  // XGBOOST_COMMON_GROUP_DATA_H_
