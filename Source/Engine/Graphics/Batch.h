//
// Copyright (c) 2008-2014 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#pragma once

#include "Drawable.h"
#include "MathDefs.h"
#include "Matrix3x4.h"
#include "Rect.h"
#include "HashMap.h"
#include "Light.h"
#include <functional>


namespace Urho3D
{

class Camera;
class Drawable;
class Geometry;
class Light;
class Material;
class Matrix3x4;
class Pass;
class ShaderVariation;
class Texture2D;
class VertexBuffer;
class View;
class Zone;
struct SourceBatch;
struct LightBatchQueue;

/// Queued 3D geometry draw call.
struct Batch
{
    /// Construct with defaults.
    Batch() :
        geometry_(0),
        lightQueue_(nullptr),
        isBase_(false)
    {
    }

    /// Construct from a drawable's source batch.
    Batch(const SourceBatch& rhs,bool is_base=false);
    Batch(const SourceBatch& rhs, Camera *cam,Zone *z, LightBatchQueue *l, Pass *p,
          unsigned char lmask=DEFAULT_LIGHTMASK,bool is_base=false) :
        Batch(rhs,is_base)
    {
        camera_=cam;
        zone_=z;
        lightQueue_=l;
        pass_=p;
        //lightMask_ = lmask;
    }

    /// Calculate state sorting key, which consists of base pass flag, light, pass and geometry.
    void CalculateSortKey();
    /// Prepare for rendering.
    void Prepare(View* view, bool setModelTransform = true) const;
    /// Prepare and draw.
    void Draw(View* view) const;

    /// State sorting key.
    unsigned long long sortKey_;
    /// Distance from camera.
    float distance_;
    /// Geometry.
    Geometry* geometry_;
    /// Material.
    Material* material_;
    /// World transform(s). For a skinned model, these are the bone transforms.
    const Matrix3x4* worldTransform_;
    /// Number of world transforms.
    unsigned numWorldTransforms_;
    /// Camera.
    Camera* camera_;
    /// Zone.
    Zone* zone_;
    /// Light properties.
    LightBatchQueue* lightQueue_;
    /// Material pass.
    Pass* pass_;
    /// Vertex shader.
    ShaderVariation* vertexShader_;
    /// Pixel shader.
    ShaderVariation* pixelShader_;
    /// %Geometry type.
    GeometryType geometryType_;
    /// Override view transform flag. When set, the camera's view transform is replaced with an identity matrix.
    bool overrideView_;
    /// Base batch flag. This tells to draw the object fully without light optimizations.
    bool isBase_;
    /// 8-bit light mask for stencil marking in deferred rendering.
    unsigned char lightMask_;
};

/// Data for one geometry instance.
struct InstanceData
{
    /// Construct undefined.
    InstanceData()
    {
    }

    /// Construct with transform and distance.
    constexpr InstanceData(const Matrix3x4* worldTransform, float distance) :
        worldTransform_(worldTransform),
        distance_(distance)
    {
    }

    /// World transform.
    const Matrix3x4* worldTransform_;
    /// Distance from camera.
    float distance_;
};

/// Instanced 3D geometry draw call.
struct BatchGroup : public Batch
{
    /// Construct with defaults.
    BatchGroup() :
        startIndex_(M_MAX_UNSIGNED)
    {
    }

    /// Construct from a batch.
    BatchGroup(const Batch& batch) :
        Batch(batch),
        startIndex_(M_MAX_UNSIGNED)
    {
    }

    /// Destruct.
    ~BatchGroup()
    {
    }

    /// Add world transform(s) from a batch.
    void AddTransforms(const Batch& batch)
    {
        for (unsigned i = 0,fin=batch.numWorldTransforms_; i < fin; ++i)
        {
            instances_.emplace_back( &batch.worldTransform_[i],batch.distance_ );
        }
    }

    /// Pre-set the instance transforms. Buffer must be big enough to hold all transforms.
    void SetTransforms(void* lockedData, unsigned& freeIndex);
    /// Prepare and draw.
    void Draw(View* view) const;

    /// Instance data.
    PODVectorN<InstanceData,4> instances_;
    /// Instance stream start index, or M_MAX_UNSIGNED if transforms not pre-set.
    unsigned startIndex_;
};
/// Instanced draw call grouping key.
struct BatchGroupKey
{
private:
    /// Zone.
    Zone* zone_;
    /// Light properties.
    LightBatchQueue* lightQueue_;
    /// Material pass.
    Pass* pass_;
    /// Material.
    Material* material_;
    /// Geometry.
    Geometry* geometry_;
    uintptr_t hashCode_;
public:
    /// Construct undefined.
    BatchGroupKey() = default;

    /// Construct from a batch.
    BatchGroupKey(const Batch& batch) :
        zone_(batch.zone_),
        lightQueue_(batch.lightQueue_),
        pass_(batch.pass_),
        material_(batch.material_),
        geometry_(batch.geometry_),
        hashCode_ ( (uintptr_t(zone_) >> 1) ^
                    (uintptr_t(lightQueue_) >>3) ^
                    (uintptr_t(pass_) >>5) ^
                    (uintptr_t(material_) >>7) ^
                    (uintptr_t(geometry_)))
    {
    }


    /// Test for equality with another batch group key.
    constexpr bool operator == (const BatchGroupKey& rhs) const { return hashCode_ == rhs.hashCode_ && zone_ == rhs.zone_ && lightQueue_ == rhs.lightQueue_ && pass_ == rhs.pass_ && material_ == rhs.material_ && geometry_ == rhs.geometry_; }
    /// Test for inequality with another batch group key.
    constexpr bool operator != (const BatchGroupKey& rhs) const {
        return  hashCode_   != rhs.hashCode_ ||
                zone_       != rhs.zone_ ||
                lightQueue_ != rhs.lightQueue_ ||
                pass_       != rhs.pass_ ||
                material_   != rhs.material_ ||
                geometry_   != rhs.geometry_;
    }

    /// Return hash value.
    constexpr unsigned ToHash() const { return  hashCode_; }
};
}
namespace std {
template<> struct hash<Urho3D::BatchGroupKey> {
    inline size_t operator()(const Urho3D::BatchGroupKey & key) const
    {
        return key.ToHash();
    }
};
}

namespace Urho3D {

/// Queue that contains both instanced and non-instanced draw calls.
struct BatchQueue
{
public:
    /// Clear for new frame by clearing all groups and batches.
    void Clear(int maxSortedInstances);
    /// Sort non-instanced draw calls back to front.
    void SortBackToFront();
    /// Sort instanced and non-instanced draw calls front to back.
    void SortFrontToBack();
    /// Sort batches front to back while also maintaining state sorting.
    void SortFrontToBack2Pass(PODVector<Batch*>& batches);
    /// Pre-set instance transforms of all groups. The vertex buffer must be big enough to hold all transforms.
    void SetTransforms(void* lockedData, unsigned& freeIndex);
    /// Draw.
    void Draw(View* view, bool markToStencil = false, bool usingLightOptimization = false) const;
    /// Return the combined amount of instances.
    unsigned GetNumInstances() const;
    /// Return whether the batch group is empty.
    bool IsEmpty() const { return batches_.empty() && batchGroups_.isEmpty(); }

    /// Instanced draw calls.
    HashMap<BatchGroupKey, BatchGroup> batchGroups_;
    /// Shader remapping table for 2-pass state and distance sort.
    HashMap<unsigned, unsigned> shaderRemapping_;
    /// Material remapping table for 2-pass state and distance sort.
    HashMap<unsigned short, unsigned short> materialRemapping_;
    /// Geometry remapping table for 2-pass state and distance sort.
    HashMap<unsigned short, unsigned short> geometryRemapping_;

    /// Unsorted non-instanced draw calls.
    PODVector<Batch> batches_;
    /// Sorted non-instanced draw calls.
    PODVector<Batch*> sortedBatches_;
    /// Sorted instanced draw calls.
    PODVector<BatchGroup*> sortedBatchGroups_;
    /// Maximum sorted instances.
    unsigned maxSortedInstances_;
};

/// Queue for shadow map draw calls
struct ShadowBatchQueue
{
    /// Shadow map camera.
    Camera* shadowCamera_;
    /// Shadow map viewport.
    IntRect shadowViewport_;
    /// Shadow caster draw calls.
    BatchQueue shadowBatches_;
    /// Directional light cascade near split distance.
    float nearSplit_;
    /// Directional light cascade far split distance.
    float farSplit_;
};

/// Queue for light related draw calls.
struct LightBatchQueue
{
    /// Per-pixel light.
    Light* light_;
    /// Shadow map depth texture.
    Texture2D* shadowMap_;
    /// Lit geometry draw calls, base (replace blend mode)
    BatchQueue litBaseBatches_;
    /// Lit geometry draw calls, non-base (additive)
    BatchQueue litBatches_;
    /// Shadow map split queues.
    PODVectorN<ShadowBatchQueue,MAX_LIGHT_SPLITS> shadowSplits_;
    /// Per-vertex lights.
    PODVector4<Light*> vertexLights_;
    /// Light volume draw calls.
    PODVector<Batch> volumeBatches_;
};

typedef unsigned int uint;
inline uint qHash(const Urho3D::BatchGroupKey & key)
{
    return key.ToHash();
}
}
