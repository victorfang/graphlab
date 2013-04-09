/**  
 * Copyright (c) 2011 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://www.graphlab.ml.cmu.edu
 *
 */

/* *
 * Author: Haijie Gu (haijieg@cs.cmu.edu)
 * Date: 04/08/2013
 *
 * CSR+CSC implementation of a graph storage.
 * */

#ifndef GRAPHLAB_GRAPH_STORAGE_HPP
#define GRAPHLAB_GRAPH_STORAGE_HPP
#ifndef __NO_OPENMP__
#include <omp.h>
#endif

#include <cmath>
#include <string>
#include <list>
#include <vector>
#include <set>
#include <map>

#include <queue>
#include <algorithm>
#include <functional>

#include <boost/version.hpp>
#include <boost/bind.hpp>
#include <boost/unordered_set.hpp>
#include <boost/iterator.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/iterator/iterator_facade.hpp>

#include <graphlab/graph/local_edge_buffer.hpp>
#include <graphlab/graph/graph_basic_types.hpp>

#include <graphlab/logger/logger.hpp>
#include <graphlab/logger/assertions.hpp>

#include <graphlab/serialization/iarchive.hpp>
#include <graphlab/serialization/oarchive.hpp>

#include <graphlab/util/random.hpp>
#include <graphlab/util/generics/shuffle.hpp>
#include <graphlab/util/generics/counting_sort.hpp>
#include <graphlab/util/generics/vector_zip.hpp>
#include <graphlab/util/generics/csr_storage.hpp>

#include <graphlab/parallel/atomic.hpp>

#include <graphlab/macros_def.hpp>

namespace graphlab {
 
  template<typename VertexData, typename EdgeData>
  class graph_storage {
  public:
    typedef graphlab::lvid_type lvid_type;
    typedef graphlab::edge_id_type edge_id_type;

    /** The type of the edge data stored in the graph. */
    typedef EdgeData edge_data_type;

    /** The type of the vertex data stored in the graph. */
    typedef VertexData vertex_data_type;

    /** 
     * \internal
     * CSR/CSC storage types
     */
    typedef csr_storage<std::pair<lvid_type, EdgeData>, edge_id_type> csr_type;

    typedef csr_storage<std::pair<lvid_type, edge_id_type>, edge_id_type> csc_type; 

    /* ----------------------------------------------------------------------------- */
    /* helper data field and structures: edge_data_list, class edge, class edge_list */
    /* ----------------------------------------------------------------------------- */
  class edge_type {
   public:
     edge_type () : _source(-1), _target(-1), edata(NULL) {}
     edge_type(lvid_type _source, lvid_type _target, EdgeData* edata)
         : _source(_source), _target(_target), edata(edata) { }

     inline bool is_empty() { return edata == NULL; } 
     inline EdgeData& edge_data() { return *edata; } 
     inline const EdgeData& edge_data() const { return *edata; } 

     lvid_type source() const { return _source; }
     lvid_type target() const { return _target; }

   private:
     lvid_type _source;
     lvid_type _target; 
     EdgeData* edata;
  };

  struct make_edge_type_csr_functor {
    typedef typename csr_type::value_type& argument_type;
    typedef edge_type result_type;

    make_edge_type_csr_functor() : graph_ptr(NULL), sourceid(-1) { }

    make_edge_type_csr_functor(graph_storage* graph_ptr, lvid_type sourceid)
        : graph_ptr(graph_ptr), sourceid(sourceid) { }

    result_type operator() (argument_type arg) const {
      return edge_type(sourceid, arg.first, &(arg.second));
    }

    graph_storage* graph_ptr;
    lvid_type sourceid;
  };

  struct make_edge_type_csc_functor {
    typedef typename csc_type::value_type& argument_type;
    typedef edge_type result_type;
    make_edge_type_csc_functor() : graph_ptr(NULL), destid(-1) {}

    make_edge_type_csc_functor(graph_storage* graph_ptr, lvid_type destid)
        : graph_ptr(graph_ptr), destid(destid) { }

    result_type operator() (argument_type arg) const {
      return edge_type(arg.first, destid, &(graph_ptr->edge_data(arg.second)));
    }

    graph_storage* graph_ptr;
    lvid_type destid;
  };

  typedef boost::transform_iterator<make_edge_type_csr_functor,
          typename csr_type::iterator> csr_edge_iterator;
  typedef boost::transform_iterator<make_edge_type_csc_functor,
          typename csc_type::iterator> csc_edge_iterator;

  class edge_iterator : 
    public boost::iterator_facade <
        edge_iterator,
        edge_type,
        boost::random_access_traversal_tag,
        edge_type> {
    public:
     edge_iterator() : _type(UNDEFINED) {}
     edge_iterator(csc_edge_iterator iter) : _type(CSC), csc_iter(iter) {}
     edge_iterator(csr_edge_iterator iter) : _type(CSR), csr_iter(iter) {}

    private:
     friend class boost::iterator_core_access;

     void increment() {
       switch (_type) {
        case CSC: ++csc_iter; break;
        case CSR: ++csr_iter; break;
        default: return;
       }
     }
     bool equal(const edge_iterator& other) const
     {
       return (_type==other._type) 
           && (csc_iter == csc_iter) 
           && (csr_iter == csr_iter);
     }
     edge_type dereference() const { 
       switch (_type) {
        case CSC: return *csc_iter;
        case CSR: return *csr_iter;
        default: return edge_type();
       }
     }
     void decrement() {
       switch (_type) {
        case CSC: --csc_iter; break;
        case CSR: --csr_iter; break;
        default: return;
       }
     }
     void advance(int n) {
       switch (_type) {
        case CSC: csc_iter+=n; break;
        case CSR: csr_iter+=n; break;
        default: return;
       }
     } 
     int distance_to(const edge_iterator& other) const {
       switch (_type) {
        case CSC: return other.csc_iter - csc_iter;
        case CSR: return other.csr_iter - csr_iter;
        default: return 0;
       }
     }
    private:
     enum list_type {CSR, CSC, UNDEFINED}; 
     list_type _type;
     csc_edge_iterator csc_iter;
     csr_edge_iterator csr_iter;
  };

  class edge_list {
   public:
    edge_list(edge_iterator _begin, edge_iterator _end) :
        _begin(_begin), _end(_end) {}

    typedef edge_iterator iterator;
    typedef iterator const_iterator;

    inline size_t size() const { 
      return _end - _begin;
    }

    inline edge_type operator[](size_t i) const {
      return *(_begin+i);
    }

    bool is_empty() const { return size() == 0; }

    iterator begin() const { return _begin; }

    iterator end() const { return _end; }

   private:
    edge_iterator _begin;
    edge_iterator _end;
  };

  public:
    // CONSTRUCTORS ============================================================>
    graph_storage() {  }

    // METHODS =================================================================>
    /** \brief Returns the number of edges in the graph. */
    size_t num_edges() const { return _csr_storage.num_values(); }

    /** \brief Returns the number of vertices in the graph. */
    size_t num_vertices() const { return _csr_storage.num_keys(); }

    /** \brief Returns the number of in edges of the vertex. */
    size_t num_in_edges (const lvid_type v) const {
        return (_csc_storage.end(v) - _csc_storage.begin(v));
    }

    /** \brief Returns the number of out edges of the vertex. */
    size_t num_out_edges (const lvid_type v) const {
        return (_csr_storage.end(v) - _csr_storage.begin(v));
    }

    /** \brief Returns a list of in edges of a vertex. */
    edge_list in_edges(const lvid_type v) {
      make_edge_type_csc_functor functor(this, v);
      // make_edge_type_csc_functor functor;
      csc_edge_iterator begin =
          boost::make_transform_iterator (_csc_storage.begin(v), functor);
      csc_edge_iterator end =
          boost::make_transform_iterator (_csc_storage.end(v), functor);
      return edge_list ( edge_iterator(begin), edge_iterator(end));
    }

    /** \brief Returns a list of out edges of a vertex. */
    edge_list out_edges(const lvid_type v) {
      make_edge_type_csr_functor functor(this, v);
      // make_edge_type_csr_functor functor;
      csr_edge_iterator begin = boost::make_transform_iterator (
          _csr_storage.begin(v), functor);
      csr_edge_iterator end = boost::make_transform_iterator (
          _csr_storage.end(v), functor);
      return edge_list ( edge_iterator(begin), edge_iterator(end));
    }

    /** \brief Returns edge data of edge_type e*/
    EdgeData& edge_data(edge_id_type eid) {
      ASSERT_LT(eid, num_edges());
      return _csr_storage.get_values()[eid].second;
    }

    const EdgeData& edge_data(edge_id_type eid) const {
      ASSERT_LT(eid, num_edges());
      return _csr_storage.get_values()[eid].second;
    }

     /** \brief Finalize the graph storage. 
      * Construct the CSC, CSR, by sorting edges to maximize the
      * efficiency of graphlab.  
      * This function takes O(|V|log(degree)) time and will 
      * fail if there are any duplicate edges.
      */
    void finalize(local_edge_buffer<VertexData, EdgeData> &edges) {
#ifdef DEBUG_GRAPH
      logstream(LOG_DEBUG) << "Graph2 finalize starts." << std::endl;
#endif
      std::vector<edge_id_type> permute;
      std::vector<edge_id_type> src_counting_prefix_sum;
      std::vector<edge_id_type> dest_counting_prefix_sum;
           
#ifdef DEBUG_GRAPH
      logstream(LOG_DEBUG) << "Graph2 finalize: Sort by source vertex" << std::endl;
#endif
      // Sort edges by source;
      // Begin of counting sort.
      counting_sort(edges.source_arr, permute, &src_counting_prefix_sum);

      // Inplace permute of edge_data, edge_src, edge_target array.
      // Modified from src/graphlab/util/generics/shuffle.hpp.
#ifdef DEBUG_GRAPH
      logstream(LOG_DEBUG) << "Graph2 finalize: Inplace permute by source id" << std::endl;
#endif
      lvid_type swap_src; lvid_type swap_target; EdgeData swap_data;
      for (size_t i = 0; i < permute.size(); ++i) {
        if (i != permute[i]) {
          // Reserve the ith entry;
          size_t j = i;
          swap_data = edges.data[i];
          swap_src = edges.source_arr[i];
          swap_target = edges.target_arr[i];
          // Begin swap cycle:
          while (j != permute[j]) {
            size_t next = permute[j];
            if (next != i) {
              edges.data[j] = edges.data[next];
              edges.source_arr[j] = edges.source_arr[next];
              edges.target_arr[j] = edges.target_arr[next];
              permute[j] = j;
              j = next;
            } else {
              // end of cycle
              edges.data[j] = swap_data;
              edges.source_arr[j] = swap_src;
              edges.target_arr[j] = swap_target;
              permute[j] = j;
              break;
            }
          }
        }
      }

#ifdef DEBUG_GRAPH
      logstream(LOG_DEBUG) << "Graph2 finalize: Sort by dest id" << std::endl;
#endif
      counting_sort(edges.target_arr, permute, &dest_counting_prefix_sum); 
      // Shuffle source array
#ifdef DEBUG_GRAPH
      logstream(LOG_DEBUG) << "Graph2 finalize: Outofplace permute by dest id" << std::endl;
#endif
      outofplace_shuffle(edges.source_arr, permute);

      // warp into csr csc storage.
      std::vector<std::pair<lvid_type, EdgeData> > csr_value = vector_zip(edges.target_arr, edges.data);
      _csr_storage.wrap(src_counting_prefix_sum, csr_value);
      std::vector<std::pair<lvid_type, edge_id_type> > csc_value = vector_zip(edges.source_arr, permute);
      _csc_storage.wrap(dest_counting_prefix_sum, csc_value); 
#ifdef DEBGU_GRAPH
      logstream(LOG_DEBUG) << "End of finalize." << std::endl;
#endif
    } // end of finalize.


    /** \brief Reset the storage. */
    void clear() {
      _csr_storage.clear();
      _csc_storage.clear();
    }

    size_t estimate_sizeof() const {
      return 0;
      // // const size_t word_size = sizeof(size_t);
      // const size_t vid_size = sizeof(lvid_type);
      // const size_t eid_size = sizeof(edge_id_type);
      // // Actual content size;
      // const size_t CSR_size = eid_size * CSR_src.capacity() + 
      //   vid_size * CSR_dst.capacity();
      // const size_t CSC_size = eid_size *CSC_dst.capacity() + 
      //   vid_size * CSC_src.capacity() + eid_size * c2r_map.capacity();
      // const size_t edata_size = sizeof(EdgeData) * edge_data_list.capacity();

      // // Container size;
      // const size_t container_size = sizeof(CSR_src) + sizeof(CSR_dst) + 
      //   sizeof(CSC_src) + sizeof(CSC_dst) + sizeof(c2r_map) + 
      //   sizeof(edge_data_list);

      // logstream(LOG_DEBUG) << "CSR size: " 
      //           << (double)CSR_size/(1024*1024)
      //           << " CSC size: " 
      //           << (double)CSC_size/(1024*1024) 
      //           << " edata size: "
      //           << (double)edata_size/(1024*1024)
      //           << " container size: " 
      //           << (double)container_size/(1024*1024) 
      //           << " \n Total size: " 
      //           << double(CSR_size + CSC_size + container_size) << std::endl;

      // return CSR_size + CSC_size + edata_size + container_size;
    } // end of estimate_sizeof


    // ------------- Private data storage ----------------
  private:
    csr_type _csr_storage;

    csc_type _csc_storage;

  public:
    /** \brief Load the graph from an archive */
    void load(iarchive& arc) {
      clear();
      arc >> _csr_storage
          >> _csc_storage;
    }

    /** \brief Save the graph to an archive */
    void save(oarchive& arc) const {
      arc << _csr_storage
          << _csc_storage;
    }

    /** swap two graph storage*/
    void swap(graph_storage& other) {
      _csr_storage.swap(other._csr_storage);
      _csc_storage.swap(other._csc_storage);
    }
  };// End of graph store;
}// End of namespace;

namespace std {
  template<typename VertexData, typename EdgeData>
  inline void swap(graphlab::graph_storage<VertexData,EdgeData>& a, 
                   graphlab::graph_storage<VertexData,EdgeData>& b) {
    a.swap(b);
  } // end of swap
}; // end of std namespace
#include <graphlab/macros_undef.hpp>
#endif
