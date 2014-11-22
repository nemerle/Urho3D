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

#include "Precompiled.h"
#include "Context.h"
#include "CoreEvents.h"
#include "DebugRenderer.h"
#include "Graphics.h"
#include "Log.h"
#include "Profiler.h"
#include "Octree.h"
#include "Scene.h"
#include "SceneEvents.h"
#include "Timer.h"
#include "WorkQueue.h"

#include "DebugNew.h"
#include <algorithm>
#ifdef _MSC_VER
#pragma warning(disable:4355)
#endif

namespace Urho3D
{

static const float DEFAULT_OCTREE_SIZE = 1000.0f;
static const int DEFAULT_OCTREE_LEVELS = 8;
static const int RAYCASTS_PER_WORK_ITEM = 4;

extern const char* SUBSYSTEM_CATEGORY;

void RaycastDrawablesWork(const WorkItem* item, unsigned threadIndex)
{
    Octree* octree = reinterpret_cast<Octree*>(item->aux_);
    Drawable** start = reinterpret_cast<Drawable**>(item->start_);
    Drawable** end = reinterpret_cast<Drawable**>(item->end_);
    const RayOctreeQuery& query = *octree->rayQuery_;
    PODVector<RayQueryResult>& results = octree->rayQueryResults_[threadIndex];

    while (start != end)
    {
        Drawable* drawable = *start;
        drawable->ProcessRayQuery(query, results);
        ++start;
    }
}

void UpdateDrawablesWork(const WorkItem* item, unsigned threadIndex)
{
    const FrameInfo& frame = *(reinterpret_cast<FrameInfo*>(item->aux_));
    Drawable** start = reinterpret_cast<Drawable**>(item->start_);
    Drawable** end = reinterpret_cast<Drawable**>(item->end_);

    while (start != end)
    {
        Drawable* drawable = *start;
        if (drawable)
            drawable->Update(frame);
        ++start;
    }
}

inline bool CompareRayQueryResults(const RayQueryResult& lhs, const RayQueryResult& rhs)
{
    return lhs.distance_ < rhs.distance_;
}

Octant::Octant(const BoundingBox& box, unsigned level, Octant* parent, Octree* root, unsigned index) :
    level_(level),
    numDrawables_(0),
    parent_(parent),
    root_(root),
    index_(index)
{
    Initialize(box);

    for (auto & elem : children_)
        elem = nullptr;
}

Octant::~Octant()
{
    if (root_)
    {
        // Remove the drawables (if any) from this octant to the root octant
        for (Drawable* elem : drawables_)
        {
            elem->SetOctant(root_);
            root_->drawables_.push_back(elem);
            root_->QueueUpdate(elem);
        }
        drawables_.clear();
        numDrawables_ = 0;
    }

    for (unsigned i = 0; i < NUM_OCTANTS; ++i)
        DeleteChild(i);
}

Octant* Octant::GetOrCreateChild(unsigned index)
{
    if (children_[index])
        return children_[index];

    Vector3 newMin = worldBoundingBox_.min_;
    Vector3 newMax = worldBoundingBox_.max_;
    Vector3 oldCenter = worldBoundingBox_.Center();

    if (index & 1)
        newMin.x_ = oldCenter.x_;
    else
        newMax.x_ = oldCenter.x_;

    if (index & 2)
        newMin.y_ = oldCenter.y_;
    else
        newMax.y_ = oldCenter.y_;

    if (index & 4)
        newMin.z_ = oldCenter.z_;
    else
        newMax.z_ = oldCenter.z_;

    children_[index] = new Octant(BoundingBox(newMin, newMax), level_ + 1, this, root_, index);
    return children_[index];
}

void Octant::DeleteChild(unsigned index)
{
    assert(index < NUM_OCTANTS);
    delete children_[index];
    children_[index] = nullptr;
}

void Octant::InsertDrawable(Drawable* drawable)
{
    const BoundingBox& box = drawable->GetWorldBoundingBox();

    // If root octant, insert all non-occludees here, so that octant occlusion does not hide the drawable.
    // Also if drawable is outside the root octant bounds, insert to root
    bool insertHere;
    if (this == root_)
        insertHere = !drawable->IsOccludee() || cullingBox_.IsInside(box) != INSIDE || CheckDrawableFit(box);
    else
        insertHere = CheckDrawableFit(box);

    if (insertHere)
    {
        Octant* oldOctant = drawable->octant_;
        if (oldOctant != this)
        {
            // Add first, then remove, because drawable count going to zero deletes the octree branch in question
            AddDrawable(drawable);
            if (oldOctant)
                oldOctant->RemoveDrawable(drawable, false);
        }
    }
    else
    {
        Vector3 boxCenter = box.Center();
        unsigned x = boxCenter.x_ < center_.x_ ? 0 : 1;
        unsigned y = boxCenter.y_ < center_.y_ ? 0 : 2;
        unsigned z = boxCenter.z_ < center_.z_ ? 0 : 4;

        GetOrCreateChild(x + y + z)->InsertDrawable(drawable);
    }
}

bool Octant::CheckDrawableFit(const BoundingBox& box) const
{
    Vector3 boxSize = box.Size();

    // If max split level, size always OK, otherwise check that box is at least half size of octant
    if (level_ >= root_->GetNumLevels() || boxSize.x_ >= halfSize_.x_ || boxSize.y_ >= halfSize_.y_ ||
        boxSize.z_ >= halfSize_.z_)
        return true;
    // Also check if the box can not fit a child octant's culling box, in that case size OK (must insert here)
    else
    {
        if (box.min_.x_ <= worldBoundingBox_.min_.x_ - 0.5f * halfSize_.x_ ||
            box.max_.x_ >= worldBoundingBox_.max_.x_ + 0.5f * halfSize_.x_ ||
            box.min_.y_ <= worldBoundingBox_.min_.y_ - 0.5f * halfSize_.y_ ||
            box.max_.y_ >= worldBoundingBox_.max_.y_ + 0.5f * halfSize_.y_ ||
            box.min_.z_ <= worldBoundingBox_.min_.z_ - 0.5f * halfSize_.z_ ||
            box.max_.z_ >= worldBoundingBox_.max_.z_ + 0.5f * halfSize_.z_)
            return true;
    }

    // Bounding box too small, should create a child octant
    return false;
}

void Octant::ResetRoot()
{
    root_ = nullptr;

    // The whole octree is being destroyed, just detach the drawables
    for (Drawable* elem : drawables_)
        elem->SetOctant(nullptr);

    for (Octant* elem : children_)
    {
        if (elem)
            elem->ResetRoot();
    }
}

void Octant::DrawDebugGeometry(DebugRenderer* debug, bool depthTest)
{
    if (debug && debug->IsInside(worldBoundingBox_))
    {
        debug->AddBoundingBox(worldBoundingBox_, Color(0.25f, 0.25f, 0.25f), depthTest);

        for (Octant* elem : children_)
        {
            if (elem)
                elem->DrawDebugGeometry(debug, depthTest);
        }
    }
}

void Octant::Initialize(const BoundingBox& box)
{
    worldBoundingBox_ = box;
    center_ = box.Center();
    halfSize_ = 0.5f * box.Size();
    cullingBox_ = BoundingBox(worldBoundingBox_.min_ - halfSize_, worldBoundingBox_.max_ + halfSize_);
}

void Octant::GetDrawablesInternal(OctreeQuery& query, bool inside) const
{
    if (this != root_)
    {
        Intersection res = query.TestOctant(cullingBox_, inside);
        if (res == INSIDE)
            inside = true;
        else if (res == OUTSIDE)
        {
            // Fully outside, so cull this octant, its children & drawables
            return;
        }
    }

    if (drawables_.size())
    {
        Drawable** start = const_cast<Drawable**>(&drawables_[0]);
        Drawable** end = start + drawables_.size();
        query.TestDrawables(start, end, inside);
    }

    for (Octant* elem : children_)
    {
        if (elem)
            elem->GetDrawablesInternal(query, inside);
    }
}

void Octant::GetDrawablesInternal(RayOctreeQuery& query) const
{
    float octantDist = query.ray_.HitDistance(cullingBox_);
    if (octantDist >= query.maxDistance_)
        return;

    if (drawables_.size())
    {
        Drawable** start = const_cast<Drawable**>(&drawables_[0]);
        Drawable** end = start + drawables_.size();

        while (start != end)
        {
            Drawable* drawable = *start++;

            if ((drawable->GetDrawableFlags() & query.drawableFlags_) && (drawable->GetViewMask() & query.viewMask_))
                drawable->ProcessRayQuery(query, query.result_);
        }
    }

    for (Octant * elem : children_)
    {
        if (elem)
            elem->GetDrawablesInternal(query);
    }
}

void Octant::GetDrawablesOnlyInternal(RayOctreeQuery& query, PODVector<Drawable*>& drawables) const
{
    float octantDist = query.ray_.HitDistance(cullingBox_);
    if (octantDist >= query.maxDistance_)
        return;

    if (drawables_.size())
    {
        Drawable** start = const_cast<Drawable**>(&drawables_[0]);
        Drawable** end = start + drawables_.size();

        while (start != end)
        {
            Drawable* drawable = *start++;

            if ((drawable->GetDrawableFlags() & query.drawableFlags_) && (drawable->GetViewMask() & query.viewMask_))
                drawables.push_back(drawable);
        }
    }

    for (Octant * elem : children_)
    {
        if (elem)
            elem->GetDrawablesOnlyInternal(query, drawables);
    }
}

Octree::Octree(Context* context) :
    Component(context),
    Octant(BoundingBox(-DEFAULT_OCTREE_SIZE, DEFAULT_OCTREE_SIZE), 0, nullptr, this),
    numLevels_(DEFAULT_OCTREE_LEVELS)
{
    // Resize threaded ray query intermediate result vector according to number of worker threads
    WorkQueue* workQueue = GetSubsystem<WorkQueue>();
    rayQueryResults_.resize(workQueue ? workQueue->GetNumThreads() + 1 : 1);

    // If the engine is running headless, subscribe to RenderUpdate events for manually updating the octree
    // to allow raycasts and animation update
    if (!GetSubsystem<Graphics>())
        SubscribeToEvent(E_RENDERUPDATE, HANDLER(Octree, HandleRenderUpdate));
}

Octree::~Octree()
{
    // Reset root pointer from all child octants now so that they do not move their drawables to root
    drawableUpdates_.clear();
    drawableReinsertions_.clear();
    ResetRoot();
}

void Octree::RegisterObject(Context* context)
{
    context->RegisterFactory<Octree>(SUBSYSTEM_CATEGORY);

    Vector3 defaultBoundsMin = -Vector3::ONE * DEFAULT_OCTREE_SIZE;
    Vector3 defaultBoundsMax = Vector3::ONE * DEFAULT_OCTREE_SIZE;

    ATTRIBUTE("Bounding Box Min", Vector3, worldBoundingBox_.min_, defaultBoundsMin, AM_DEFAULT);
    ATTRIBUTE("Bounding Box Max", Vector3, worldBoundingBox_.max_, defaultBoundsMax, AM_DEFAULT);
    ATTRIBUTE("Number of Levels", int, numLevels_, DEFAULT_OCTREE_LEVELS, AM_DEFAULT);
}

void Octree::OnSetAttribute(const AttributeInfo& attr, const Variant& src)
{
    // If any of the (size) attributes change, resize the octree
    Serializable::OnSetAttribute(attr, src);
    SetSize(worldBoundingBox_, numLevels_);
}

void Octree::DrawDebugGeometry(DebugRenderer* debug, bool depthTest)
{
    if (debug)
    {
        PROFILE(OctreeDrawDebug);

        Octant::DrawDebugGeometry(debug, depthTest);
    }
}

void Octree::SetSize(const BoundingBox& box, unsigned numLevels)
{
    PROFILE(ResizeOctree);

    // If drawables exist, they are temporarily moved to the root
    for (unsigned i = 0; i < NUM_OCTANTS; ++i)
        DeleteChild(i);

    Initialize(box);
    numDrawables_ = drawables_.size();
    numLevels_ = Max((int)numLevels, 1);
}

void Octree::Update(const FrameInfo& frame)
{
    // Let drawables update themselves before reinsertion. This can be used for animation
    if (!drawableUpdates_.empty())
    {
        PROFILE(UpdateDrawables);

        // Perform updates in worker threads. Notify the scene that a threaded update is going on and components
        // (for example physics objects) should not perform non-threadsafe work when marked dirty
        Scene* scene = GetScene();
        WorkQueue* queue = GetSubsystem<WorkQueue>();
        scene->BeginThreadedUpdate();

        int numWorkItems = queue->GetNumThreads() + 1; // Worker threads + main thread
        int drawablesPerItem = Max((int)(drawableUpdates_.size() / numWorkItems), 1);

        Drawable ** start_ptr = &drawableUpdates_.front();
        Drawable ** fin_ptr = start_ptr + drawableUpdates_.size();

        // Create a work item for each thread
        for (int i = 0; i < numWorkItems; ++i)
        {
            SharedPtr<WorkItem> item(queue->GetFreeItem());
            item->priority_ = M_MAX_UNSIGNED;
            item->workFunction_ = UpdateDrawablesWork;
            item->aux_ = const_cast<FrameInfo*>(&frame);

            Drawable ** end_ptr = fin_ptr;
            if (i < numWorkItems - 1 && end_ptr - start_ptr > drawablesPerItem)
                end_ptr = start_ptr + drawablesPerItem;

            item->start_ = start_ptr;
            item->end_ = end_ptr;
            queue->AddWorkItem(item);

            start_ptr = end_ptr;
        }

        queue->Complete(M_MAX_UNSIGNED);
        scene->EndThreadedUpdate();
    }

    // Notify drawable update being finished. Custom animation (eg. IK) can be done at this point
    Scene* scene = GetScene();
    if (scene)
    {
        using namespace SceneDrawableUpdateFinished;

        VariantMap& eventData = GetEventDataMap();
        eventData[P_SCENE] = scene;
        eventData[P_TIMESTEP] = frame.timeStep_;
        scene->SendEvent(E_SCENEDRAWABLEUPDATEFINISHED, eventData);
    }

    // Reinsert drawables that have been moved or resized, or that have been newly added to the octree and do not sit inside
    // the proper octant yet
    if (!drawableUpdates_.empty())
    {
        PROFILE(ReinsertToOctree);

        for (auto drawable : drawableUpdates_)
        {

            drawable->updateQueued_ = false;
            Octant* octant = drawable->GetOctant();
            const BoundingBox& box = drawable->GetWorldBoundingBox();

            // Skip if no octant or does not belong to this octree anymore
            if (!octant || octant->GetRoot() != this)
                continue;
            // Skip if still fits the current octant
            if (drawable->IsOccludee() && octant->GetCullingBox().IsInside(box) == INSIDE && octant->CheckDrawableFit(box))
                continue;

            InsertDrawable(drawable);

            #ifdef _DEBUG
            // Verify that the drawable will be culled correctly
            octant = drawable->GetOctant();
            if (octant != this && octant->GetCullingBox().IsInside(box) != INSIDE)
            {
                LOGERROR("Drawable is not fully inside its octant's culling bounds: drawable box " + box.ToString() +
                    " octant box " + octant->GetCullingBox().ToString());
            }
            #endif
        }
    }

    drawableUpdates_.clear();
}

void Octree::AddManualDrawable(Drawable* drawable)
{
    if (!drawable || drawable->GetOctant())
        return;

    AddDrawable(drawable);
}

void Octree::RemoveManualDrawable(Drawable* drawable)
{
    if (!drawable)
        return;

    Octant* octant = drawable->GetOctant();
    if (octant && octant->GetRoot() == this)
        octant->RemoveDrawable(drawable);
}

void Octree::GetDrawables(OctreeQuery& query) const
{
    query.result_.clear();
    GetDrawablesInternal(query, false);
}

void Octree::Raycast(RayOctreeQuery& query) const
{
    PROFILE(Raycast);

    query.result_.clear();

    WorkQueue* queue = GetSubsystem<WorkQueue>();

    // If no worker threads or no triangle-level testing, do not create work items
    if (!queue->GetNumThreads() || query.level_ < RAY_TRIANGLE)
        GetDrawablesInternal(query);
    else
    {
        // Threaded ray query: first get the drawables
        rayQuery_ = &query;
        rayQueryDrawables_.clear();
        GetDrawablesOnlyInternal(query, rayQueryDrawables_);

        // Check that amount of drawables is large enough to justify threading
        if (rayQueryDrawables_.size() >= RAYCASTS_PER_WORK_ITEM * 2)
        {
            for (unsigned i = 0; i < rayQueryResults_.size(); ++i)
                rayQueryResults_[i].clear();

            PODVector<Drawable*>::iterator start = rayQueryDrawables_.begin();
            while (start != rayQueryDrawables_.end())
            {
                SharedPtr<WorkItem> item = queue->GetFreeItem();
                item->priority_ = M_MAX_UNSIGNED;
                item->workFunction_ = RaycastDrawablesWork;
                item->aux_ = const_cast<Octree*>(this);

                PODVector<Drawable*>::iterator end = rayQueryDrawables_.end();
                if (end - start > RAYCASTS_PER_WORK_ITEM)
                    end = start + RAYCASTS_PER_WORK_ITEM;

                item->start_ = &(*start);
                item->end_ = &(*end);
                queue->AddWorkItem(item);

                start = end;
            }

            // Merge per-thread results
            queue->Complete(M_MAX_UNSIGNED);
            for (unsigned i = 0; i < rayQueryResults_.size(); ++i)
                query.result_.insert(query.result_.end(), rayQueryResults_[i].begin(), rayQueryResults_[i].end());
        }
        else
        {
            for (Drawable* elem : rayQueryDrawables_)
                (elem)->ProcessRayQuery(query, query.result_);
        }
    }

    std::sort(query.result_.begin(), query.result_.end(), CompareRayQueryResults);
}

void Octree::RaycastSingle(RayOctreeQuery& query) const
{
    PROFILE(Raycast);

    query.result_.clear();
    rayQueryDrawables_.clear();
    GetDrawablesOnlyInternal(query, rayQueryDrawables_);

    // Sort by increasing hit distance to AABB
    for (auto drawable : rayQueryDrawables_)
    {

        drawable->SetSortValue(query.ray_.HitDistance(drawable->GetWorldBoundingBox()));
    }

    std::sort(rayQueryDrawables_.begin(), rayQueryDrawables_.end(), CompareDrawables);

    // Then do the actual test according to the query, and early-out as possible
    float closestHit = M_INFINITY;
    for (auto drawable : rayQueryDrawables_)
    {

        if (drawable->GetSortValue() < Min(closestHit, query.maxDistance_))
        {
            unsigned oldSize = query.result_.size();
            drawable->ProcessRayQuery(query, query.result_);
            if (query.result_.size() > oldSize)
                closestHit = Min(closestHit, query.result_.back().distance_);
        }
        else
            break;
    }

    if (query.result_.size() > 1)
    {
        std::sort(query.result_.begin(), query.result_.end(), CompareRayQueryResults);
        query.result_.resize(1);
    }
}

void Octree::QueueUpdate(Drawable* drawable)
{
    Scene* scene = GetScene();
    if (scene && scene->IsThreadedUpdate())
    {
        MutexLock lock(octreeMutex_);
        drawableUpdates_.push_back(drawable);
    }
    else
        drawableUpdates_.push_back(drawable);

    drawable->updateQueued_ = true;
}

void Octree::CancelUpdate(Drawable* drawable)
{
    drawableUpdates_.remove(drawable);
    drawable->updateQueued_ = false;
}

void Octree::DrawDebugGeometry(bool depthTest)
{
    DebugRenderer* debug = GetComponent<DebugRenderer>();
    DrawDebugGeometry(debug, depthTest);
}

void Octree::HandleRenderUpdate(StringHash eventType, VariantMap& eventData)
{
    // When running in headless mode, update the Octree manually during the RenderUpdate event
    Scene* scene = GetScene();
    if (!scene || !scene->IsUpdateEnabled())
        return;

    using namespace RenderUpdate;

    FrameInfo frame;
    frame.frameNumber_ = GetSubsystem<Time>()->GetFrameNumber();
    frame.timeStep_ = eventData[P_TIMESTEP].GetFloat();
    frame.camera_ = nullptr;

    Update(frame);
}

}
