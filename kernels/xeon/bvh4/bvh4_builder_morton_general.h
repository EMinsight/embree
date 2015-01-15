// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "bvh4.h"
#include "builders/heuristic_fallback.h"
#include "builders/workstack.h"
#include "bvh4_statistics.h"
#include "geometry/triangle4.h"

#include "tbb/tbb.h"
#include "algorithms/parallel_for_for.h"


namespace embree
{
  namespace isa
  {
    class BuildRecord 
      {
      public:
        unsigned int begin;
        unsigned int end;
        unsigned int depth;
        BVH4::NodeRef* parent;
        
        __forceinline unsigned int size() const {
          return end - begin;
        }
        
        __forceinline void init(const unsigned int _begin, const unsigned int _end)			 
        {
          begin = _begin;
          end = _end;
          depth = 1;
          parent = NULL;
	}
        
	struct Greater {
	  __forceinline bool operator()(const BuildRecord& a, const BuildRecord& b) {
	    return a.size() > b.size();
	  }
	};
      };

      struct __aligned(8) MortonID32Bit
      {
        union {
          struct {
	    unsigned int code;
	    unsigned int index;
	    //uint64 index;
          };
          //int64 all;
        };

        __forceinline operator unsigned() const { return code; }
        
        __forceinline unsigned int get(const unsigned int shift, const unsigned and_mask) const {
          return (code >> shift) & and_mask;
        }
        
        /*__forceinline void operator=(const MortonID32Bit& v) {   
          all = v.all; 
	  };*/  
        
        __forceinline friend std::ostream &operator<<(std::ostream &o, const MortonID32Bit& mc) {
          o << "index " << mc.index << " code = " << mc.code;
          return o;
        }
        
        __forceinline bool operator<(const MortonID32Bit &m) const { return code < m.code; } 
        __forceinline bool operator>(const MortonID32Bit &m) const { return code > m.code; } 
      };

    template<typename CreateLeafFunc>
    class BVH4BuilderMortonGeneral //: public Builder
    {
      ALIGNED_CLASS;
      
    protected:
      /*! Type shortcuts */
      typedef BVH4::Node    Node;
      typedef BVH4::NodeRef NodeRef;
      typedef FastAllocator::ThreadLocal2 Allocator;
            
      static const size_t LATTICE_BITS_PER_DIM = 10;
      static const size_t LATTICE_SIZE_PER_DIM = size_t(1) << LATTICE_BITS_PER_DIM;

    public:
  
      

      /*! Constructor. */
      BVH4BuilderMortonGeneral (CreateLeafFunc& createLeaf, BVH4* bvh, Scene* scene, TriangleMesh* mesh, size_t listMode, size_t logBlockSize, bool needVertices, size_t primBytes, const size_t minLeafSize, const size_t maxLeafSize)
        : createLeaf(createLeaf), bvh(bvh), scene(scene), minLeafSize(minLeafSize), maxLeafSize(maxLeafSize),
	  encodeShift(0), encodeMask(-1), morton(NULL),  numPrimitives(0)
      {
        //needAllThreads = true;
      }
      
      /*! Destruction */
      ~BVH4BuilderMortonGeneral ()
      {
        //if (morton) os_free(morton,bytesMorton);
        //bvh->alloc.shrink();
      }
      
     
      
    public:
      
      void splitFallback(BuildRecord& current, BuildRecord& leftChild, BuildRecord& rightChild) const
      {
        const unsigned int center = (current.begin + current.end)/2;
        leftChild.init(current.begin,center);
        rightChild.init(center,current.end);
      }
      
      BBox3fa createLargeLeaf(BuildRecord& current, Allocator* alloc)
      {
#if defined(DEBUG)
        if (current.depth > BVH4::maxBuildDepthLeaf) 
          THROW_RUNTIME_ERROR("ERROR: depth limit reached");
#endif
        
        /* create leaf for few primitives */
        if (current.size() <= minLeafSize) {
          BBox3fa bounds;
          createLeaf(current,alloc,bounds);
          return bounds;
        }
        
        /* first split level */
        BuildRecord record0, record1;
        splitFallback(current,record0,record1);
        
        /* second split level */
        BuildRecord children[4];
        splitFallback(record0,children[0],children[1]);
        splitFallback(record1,children[2],children[3]);
        
        /* allocate node */
        Node* node = (Node*) alloc->alloc0.malloc(sizeof(Node)); node->clear();
        *current.parent = bvh->encodeNode(node);
        
        /* recurse into each child */
        BBox3fa bounds0 = empty;
        for (size_t i=0; i<4; i++) {
          children[i].parent = &node->child(i);
          children[i].depth = current.depth+1;
          BBox3fa bounds = createLargeLeaf(children[i],alloc);
          bounds0.extend(bounds);
          node->set(i,bounds);
        }
        BVH4::compact(node); // move empty nodes to the end
        return bounds0;
      }  
      
      /*! recreates morton codes when reaching a region where all codes are identical */
      void recreateMortonCodes(BuildRecord& current) const
      {
        assert(current.size() > 4);
        CentGeomBBox3fa global_bounds;
        global_bounds.reset();
        
        for (size_t i=current.begin; i<current.end; i++)
        {
          const size_t index  = morton[i].index;
          const size_t primID = index & encodeMask; 
          const size_t geomID = index >> encodeShift; 
          const TriangleMesh* mesh = scene->getTriangleMesh(geomID);
          global_bounds.extend(mesh->bounds(primID));
        }
        
        /* compute mapping from world space into 3D grid */
        const ssef base  = (ssef)global_bounds.centBounds.lower;
        const ssef diag  = (ssef)global_bounds.centBounds.upper - (ssef)global_bounds.centBounds.lower;
        const ssef scale = select(diag > ssef(1E-19f), rcp(diag) * ssef(LATTICE_SIZE_PER_DIM * 0.99f),ssef(0.0f));
        
        for (size_t i=current.begin; i<current.end; i++)
        {
          const size_t index  = morton[i].index;
          const size_t primID = index & encodeMask; 
          const size_t geomID = index >> encodeShift; 
          const TriangleMesh* mesh = scene->getTriangleMesh(geomID);
          const BBox3fa b = mesh->bounds(primID);
          const ssef lower = (ssef)b.lower;
          const ssef upper = (ssef)b.upper;
          const ssef centroid = lower+upper;
          const ssei binID = ssei((centroid-base)*scale);
          const unsigned int bx = extract<0>(binID);
          const unsigned int by = extract<1>(binID);
          const unsigned int bz = extract<2>(binID);
          const unsigned int code = bitInterleave(bx,by,bz);
          morton[i].code  = code;
        }
        std::sort(morton+current.begin,morton+current.end);
      }

      __forceinline void split(BuildRecord& current,
                               BuildRecord& left,
                               BuildRecord& right) const
      {
        const unsigned int code_start = morton[current.begin].code;
        const unsigned int code_end   = morton[current.end-1].code;
        unsigned int bitpos = clz(code_start^code_end);
        
        /* if all items mapped to same morton code, then create new morton codes for the items */
        if (unlikely(bitpos == 32)) 
        {
          recreateMortonCodes(current);
          const unsigned int code_start = morton[current.begin].code;
          const unsigned int code_end   = morton[current.end-1].code;
          bitpos = clz(code_start^code_end);
          
          /* if the morton code is still the same, goto fall back split */
          if (unlikely(bitpos == 32)) 
          {
            size_t center = (current.begin + current.end)/2; 
            left.init(current.begin,center);
            right.init(center,current.end);
            return;
          }
        }
        
        /* split the items at the topmost different morton code bit */
        const unsigned int bitpos_diff = 31-bitpos;
        const unsigned int bitmask = 1 << bitpos_diff;
        
        /* find location where bit differs using binary search */
        size_t begin = current.begin;
        size_t end   = current.end;
        while (begin + 1 != end) {
          const size_t mid = (begin+end)/2;
          const unsigned bit = morton[mid].code & bitmask;
          if (bit == 0) begin = mid; else end = mid;
        }
        size_t center = end;
#if defined(DEBUG)      
        for (unsigned int i=begin;  i<center; i++) assert((morton[i].code & bitmask) == 0);
        for (unsigned int i=center; i<end;    i++) assert((morton[i].code & bitmask) == bitmask);
#endif
        
        left.init(current.begin,center);
        right.init(center,current.end);
      }
      
      BBox3fa recurse_tbb(BuildRecord& current, Allocator* alloc) 
      {
        if (alloc == NULL) 
          alloc = bvh->alloc2.threadLocal2();

        __aligned(64) BuildRecord children[BVH4::N];
        
        /* create leaf node */
        if (unlikely(current.depth >= BVH4::maxBuildDepth || current.size() <= minLeafSize)) {
          return createLargeLeaf(current,alloc);
        }
        
        /* fill all 4 children by always splitting the one with the largest surface area */
        size_t numChildren = 1;
        children[0] = current;
        
        do {
          
          /* find best child with largest bounding box area */
          int bestChild = -1;
          unsigned bestItems = 0;
          for (unsigned int i=0; i<numChildren; i++)
          {
            /* ignore leaves as they cannot get split */
            if (children[i].size() <= minLeafSize)
              continue;
            
            /* remember child with largest area */
            if (children[i].size() > bestItems) { 
              bestItems = children[i].size();
              bestChild = i;
            }
          }
          if (bestChild == -1) break;
          
          /*! split best child into left and right child */
          __aligned(64) BuildRecord left, right;
          split(children[bestChild],left,right);
          
          /* add new children left and right */
          left.depth = right.depth = current.depth+1;
          children[bestChild] = children[numChildren-1];
          children[numChildren-1] = left;
          children[numChildren+0] = right;
          numChildren++;
          
        } while (numChildren < BVH4::N);
        
        /* create leaf node if no split is possible */
        if (unlikely(numChildren == 1)) {
          BBox3fa bounds; createLeaf(current,alloc,bounds); return bounds;
        }
        
        /* allocate node */
        Node* node = (Node*) alloc->alloc0.malloc(sizeof(Node)); node->clear();
        *current.parent = bvh->encodeNode(node);
      
        /* process top parts of tree parallel */
        BBox3fa bounds0 = empty;
        if (current.size() > 4096)
        {
          BBox3fa bounds[4];
          tbb::task_group g;
          for (size_t i=0; i<numChildren; i++) {
            children[i].parent = &node->child(i);
            g.run([&,i]{ bounds[i] = recurse_tbb(children[i],NULL); });
          }
          g.wait();

          for (size_t i=0; i<numChildren; i++) {
            bounds0.extend(bounds[i]);
            node->set(i,bounds[i]);
          }
        } 

        /* finish tree sequential */
        else 
        {
          /* recurse into each child */
          for (size_t i=0; i<numChildren; i++) 
          {
            children[i].parent = &node->child(i);
            
            if (children[i].size() <= minLeafSize) {
              const BBox3fa bounds = createLargeLeaf(children[i],alloc);
              bounds0.extend(bounds);
              node->set(i,bounds);
            } else {
              const BBox3fa bounds = recurse_tbb(children[i],alloc);
              bounds0.extend(bounds);
              node->set(i,bounds);
            }
          }
        }
        return bounds0;
      }

       /* build function */
      void build(size_t threadIndex, size_t threadCount, MortonID32Bit*  morton_i, size_t numPrimitives_i, size_t encodeShift, size_t encodeMask) 
    {
      this->morton = morton_i;
      this->numPrimitives = numPrimitives_i;
      this->encodeShift = encodeShift;
      this->encodeMask = encodeMask;
     
      /* sort morton codes */
      radix_sort_copy_u32((MortonID32Bit*)bvh->alloc2.ptr(),morton,numPrimitives);

      BuildRecord br;
      br.init(0,numPrimitives);
      br.parent = &bvh->root;
      br.depth = 1;
      
      BBox3fa bounds = empty;
      LockStepTaskScheduler::execute_tbb([&] { bounds = recurse_tbb(br, NULL); });
      //bounds = recurse_tbb(br, NULL);
    }

    public:
      BVH4* bvh;               //!< Output BVH
      //LockStepTaskScheduler* scheduler;
      //std::unique_ptr<MortonBuilderState> state;

      Scene* scene;
      //TriangleMesh* mesh;
      //size_t logBlockSize;
      //size_t blocks(size_t N) { return (N+((1<<logBlockSize)-1)) >> logBlockSize; }
      //bool needVertices;
      //size_t primBytes; 
      size_t minLeafSize;
      size_t maxLeafSize;
      //size_t listMode;

      //size_t topLevelItemThreshold;
      size_t encodeShift;
      size_t encodeMask;
            
    public:
      MortonID32Bit* __restrict__ morton;
      //size_t bytesMorton;
      
    public:
      //size_t numGroups;
      size_t numPrimitives;
      //size_t numAllocatedPrimitives;
      //size_t numAllocatedNodes;
      //CentGeomBBox3fa global_bounds;
      //Barrier barrier;

      CreateLeafFunc& createLeaf;
    };
  }
}
