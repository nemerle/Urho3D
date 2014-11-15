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
#include "Component.h"
#include "SceneResolver.h"
#include "Log.h"
#include "Node.h"

#include "DebugNew.h"
#include <QtCore/QSet>

namespace Urho3D
{

SceneResolver::SceneResolver()
{
}

SceneResolver::~SceneResolver()
{
}

void SceneResolver::Reset()
{
    nodes_.clear();
    components_.clear();
}

void SceneResolver::AddNode(unsigned oldID, Node* node)
{
    if (node)
        nodes_[oldID] = node;
}

void SceneResolver::AddComponent(unsigned oldID, Component* component)
{
    if (component)
        components_[oldID] = component;
}

void SceneResolver::Resolve()
{
    // Nodes do not have component or node ID attributes, so only have to go through components
    QSet<StringHash> noIDAttributes;
    for (QHash<unsigned, WeakPtr<Component> >::ConstIterator i = components_.begin(); i != components_.end(); ++i)
    {
        Component* component = *i;
        if (!component || noIDAttributes.contains(component->GetType()))
            continue;

        bool hasIDAttributes = false;
        const Vector<AttributeInfo>* attributes = component->GetAttributes();
        if (!attributes)
        {
            noIDAttributes.insert(component->GetType());
            continue;
        }

        for (unsigned j = 0; j < attributes->size(); ++j)
        {
            const AttributeInfo& info = attributes->At(j);
            if (info.mode_ & AM_NODEID)
            {
                hasIDAttributes = true;
                unsigned oldNodeID = component->GetAttribute(j).GetUInt();

                if (oldNodeID)
                {
                    QHash<unsigned, WeakPtr<Node> >::ConstIterator k = nodes_.find(oldNodeID);

                    if (k != nodes_.end() && *k)
                    {
                        unsigned newNodeID = (*k)->GetID();
                        component->SetAttribute(j, Variant(newNodeID));
                    }
                    else
                        LOGWARNING("Could not resolve node ID " + String(oldNodeID));
                }
            }
            else if (info.mode_ & AM_COMPONENTID)
            {
                hasIDAttributes = true;
                unsigned oldComponentID = component->GetAttribute(j).GetUInt();

                if (oldComponentID)
                {
                    QHash<unsigned, WeakPtr<Component> >::ConstIterator k = components_.find(oldComponentID);

                    if (k != components_.end() && *k)
                    {
                        unsigned newComponentID = (*k)->GetID();
                        component->SetAttribute(j, Variant(newComponentID));
                    }
                    else
                        LOGWARNING("Could not resolve component ID " + String(oldComponentID));
                }
            }
            else if (info.mode_ & AM_NODEIDVECTOR)
            {
                hasIDAttributes = true;
                const VariantVector& oldNodeIDs = component->GetAttribute(j).GetVariantVector();

                if (oldNodeIDs.size())
                {
                    // The first index stores the number of IDs redundantly. This is for editing
                    unsigned numIDs = oldNodeIDs[0].GetUInt();
                    VariantVector newIDs;
                    newIDs.Push(numIDs);

                    for (unsigned k = 1; k < oldNodeIDs.size(); ++k)
                    {
                        unsigned oldNodeID = oldNodeIDs[k].GetUInt();
                        QHash<unsigned, WeakPtr<Node> >::ConstIterator l = nodes_.find(oldNodeID);

                        if (l != nodes_.end() && (*l))
                            newIDs.Push((*l)->GetID());
                        else
                        {
                            // If node was not found, retain number of elements, just store ID 0
                            newIDs.Push(0);
                            LOGWARNING("Could not resolve node ID " + String(oldNodeID));
                        }
                    }

                    component->SetAttribute(j, newIDs);
                }
            }
        }

        // If component type had no ID attributes, cache this fact for optimization
        if (!hasIDAttributes)
            noIDAttributes.insert(component->GetType());
    }

    // Attributes have been resolved, so no need to remember the nodes after this
    Reset();
}

}
