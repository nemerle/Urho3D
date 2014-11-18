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
#include "Log.h"
#include "MemoryBuffer.h"
#ifdef URHO3D_PHYSICS
#include "PhysicsEvents.h"
#include "PhysicsWorld.h"
#endif
#include "Profiler.h"
#include "ResourceCache.h"
#include "ResourceEvents.h"
#include "Scene.h"
#include "SceneEvents.h"
#include "Script.h"
#include "ScriptFile.h"
#include "ScriptInstance.h"

#include <angelscript.h>

#include "DebugNew.h"

namespace Urho3D
{

static const char* methodDeclarations[] = {
    "void Start()",
    "void Stop()",
    "void DelayedStart()",
    "void Update(float)",
    "void PostUpdate(float)",
    "void FixedUpdate(float)",
    "void FixedPostUpdate(float)",
    "void Load(Deserializer&)",
    "void Save(Serializer&)",
    "void ReadNetworkUpdate(Deserializer&)",
    "void WriteNetworkUpdate(Serializer&)",
    "void ApplyAttributes()",
    "void TransformChanged()"
};

ScriptInstance::ScriptInstance(Context* context) :
    Component(context),
    script_(GetSubsystem<Script>()),
    scriptObject_(nullptr),
    subscribed_(false),
    subscribedPostFixed_(false)
{
    ClearScriptMethods();
    ClearScriptAttributes();
}

ScriptInstance::~ScriptInstance()
{
    ReleaseObject();
}

void ScriptInstance::RegisterObject(Context* context)
{
    context->RegisterFactory<ScriptInstance>(LOGIC_CATEGORY);

    ACCESSOR_ATTRIBUTE(ScriptInstance, VAR_BOOL, "Is Enabled", IsEnabled, SetEnabled, bool, true, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(ScriptInstance, VAR_BUFFER, "Delayed Method Calls", GetDelayedCallsAttr, SetDelayedCallsAttr, PODVector<unsigned char>, Variant::emptyBuffer, AM_FILE | AM_NOEDIT);
    ACCESSOR_ATTRIBUTE(ScriptInstance, VAR_RESOURCEREF, "Script File", GetScriptFileAttr, SetScriptFileAttr, ResourceRef, ResourceRef(ScriptFile::GetTypeStatic()), AM_DEFAULT);
    REF_ACCESSOR_ATTRIBUTE(ScriptInstance, VAR_STRING, "Class Name", GetClassName, SetClassName, String, String::EMPTY, AM_DEFAULT);
    ACCESSOR_ATTRIBUTE(ScriptInstance, VAR_BUFFER, "Script Data", GetScriptDataAttr, SetScriptDataAttr, PODVector<unsigned char>, Variant::emptyBuffer, AM_FILE | AM_NOEDIT);
    ACCESSOR_ATTRIBUTE(ScriptInstance, VAR_BUFFER, "Script Network Data", GetScriptNetworkDataAttr, SetScriptNetworkDataAttr, PODVector<unsigned char>, Variant::emptyBuffer, AM_NET | AM_NOEDIT);
}

void ScriptInstance::OnSetAttribute(const AttributeInfo& attr, const Variant& src)
{
    if (attr.mode_ & (AM_NODEID | AM_COMPONENTID))
    {
        // The component / node to which the ID refers to may not be in the scene yet, and furthermore the ID must go through the
        // SceneResolver first. Delay searching for the object to ApplyAttributes
        AttributeInfo* attrPtr = const_cast<AttributeInfo*>(&attr);
        idAttributes_[attrPtr] = src.GetUInt();
    }
    else if (attr.type_ == VAR_RESOURCEREF && attr.ptr_)
    {
        Resource*& resourcePtr = *(reinterpret_cast<Resource**>(attr.ptr_));
        // Decrease reference count of the old object if any, then increment the new
        if (resourcePtr)
            resourcePtr->ReleaseRef();
        const ResourceRef& ref = src.GetResourceRef();
        resourcePtr = GetSubsystem<ResourceCache>()->GetResource(ref.type_, ref.name_);
        if (resourcePtr)
            resourcePtr->AddRef();
    }
    else
        Serializable::OnSetAttribute(attr, src);
}

void ScriptInstance::OnGetAttribute(const AttributeInfo& attr, Variant& dest) const
{
    AttributeInfo* attrPtr = const_cast<AttributeInfo*>(&attr);

    // Get ID's for node / component handle attributes
    if (attr.mode_ & (AM_NODEID | AM_COMPONENTID))
    {
        // If a cached ID value has been stored, return it instead of querying from the actual object
        // (the object handle is likely null at that point)
        QHash<AttributeInfo*, unsigned>::const_iterator i = idAttributes_.find(attrPtr);
        if (i != idAttributes_.end())
            dest = *i;
        else if (attr.mode_ & AM_NODEID)
        {
            Node* node = *(reinterpret_cast<Node**>(attr.ptr_));
            unsigned nodeID = node ? node->GetID() : 0;
            dest = nodeID;
        }
        else
        {
            Component* component = *(reinterpret_cast<Component**>(attr.ptr_));
            unsigned componentID = component ? component->GetID() : 0;
            dest = componentID;
        }
    }
    else if (attr.type_ == VAR_RESOURCEREF && attr.ptr_)
    {
        Resource* resource = *(reinterpret_cast<Resource**>(attr.ptr_));
        // If resource is non-null get its type and name hash. Otherwise get type from the default value
        dest = GetResourceRef(resource, attr.defaultValue_.GetResourceRef().type_);
    }
    else
        Serializable::OnGetAttribute(attr, dest);
}

void ScriptInstance::ApplyAttributes()
{
    // Apply node / component ID attributes now (find objects from the scene and assign to the object handles)
    for (auto elem=idAttributes_.begin(),fin=idAttributes_.end(); elem!=fin; ++elem)
    {
        AttributeInfo& attr = *elem.key();
        if (attr.mode_ & AM_NODEID)
        {
            Node*& nodePtr = *(reinterpret_cast<Node**>(attr.ptr_));
            // Decrease reference count of the old object if any, then increment the new
            if (nodePtr)
                nodePtr->ReleaseRef();
            nodePtr = GetScene()->GetNode(*elem);
            if (nodePtr)
                nodePtr->AddRef();
        }
        else if (attr.mode_ & AM_COMPONENTID)
        {
            Component*& componentPtr = *(reinterpret_cast<Component**>(attr.ptr_));
            if (componentPtr)
                componentPtr->ReleaseRef();
            componentPtr = GetScene()->GetComponent(*elem);
            if (componentPtr)
                componentPtr->AddRef();
        }
    }

    idAttributes_.clear();

    if (scriptObject_ && methods_[METHOD_APPLYATTRIBUTES])
        scriptFile_->Execute(scriptObject_, methods_[METHOD_APPLYATTRIBUTES]);
}

void ScriptInstance::OnSetEnabled()
{
    UpdateEventSubscription();
}

bool ScriptInstance::CreateObject(ScriptFile* scriptFile, const String& className)
{
    className_ = String::EMPTY; // Do not create object during SetScriptFile()
    SetScriptFile(scriptFile);
    SetClassName(className);
    return scriptObject_ != nullptr;
}

void ScriptInstance::SetScriptFile(ScriptFile* scriptFile)
{
    if (scriptFile == scriptFile_ && scriptObject_)
        return;

    ReleaseObject();

    // Unsubscribe from the reload event of previous script file (if any), then subscribe to the new
    if (scriptFile_)
    {
        UnsubscribeFromEvent(scriptFile_, E_RELOADSTARTED);
        UnsubscribeFromEvent(scriptFile_, E_RELOADFINISHED);
    }
    if (scriptFile)
    {
        SubscribeToEvent(scriptFile, E_RELOADSTARTED, HANDLER(ScriptInstance, HandleScriptFileReload));
        SubscribeToEvent(scriptFile, E_RELOADFINISHED, HANDLER(ScriptInstance, HandleScriptFileReloadFinished));
    }

    scriptFile_ = scriptFile;

    CreateObject();
    MarkNetworkUpdate();
}

void ScriptInstance::SetClassName(const String& className)
{
    if (className == className_ && scriptObject_)
        return;

    ReleaseObject();

    className_ = className;
    CreateObject();
    MarkNetworkUpdate();
}

bool ScriptInstance::Execute(const String& declaration, const VariantVector& parameters)
{
    if (!scriptObject_)
        return false;

    asIScriptFunction* method = scriptFile_->GetMethod(scriptObject_, declaration);
    return scriptFile_->Execute(scriptObject_, method, parameters);
}

bool ScriptInstance::Execute(asIScriptFunction* method, const VariantVector& parameters)
{
    if (!method || !scriptObject_)
        return false;

    return scriptFile_->Execute(scriptObject_, method, parameters);
}

void ScriptInstance::DelayedExecute(float delay, bool repeat, const String& declaration, const VariantVector& parameters)
{
    if (!scriptObject_)
        return;

    DelayedCall call;
    call.period_ = call.delay_ = Max(delay, 0.0f);
    call.repeat_ = repeat;
    call.declaration_ = declaration;
    call.parameters_ = parameters;
    delayedCalls_.push_back(call);

    // Make sure we are registered to the scene update event, because delayed calls are executed there
    if (!subscribed_)
        UpdateEventSubscription();
}

void ScriptInstance::ClearDelayedExecute(const String& declaration)
{
    if (declaration.Empty())
        delayedCalls_.clear();
    else
    {
        for (Vector<DelayedCall>::iterator i = delayedCalls_.begin(); i != delayedCalls_.end();)
        {
            if (declaration == i->declaration_)
                i = delayedCalls_.erase(i);
            else
                ++i;
        }
    }
}

void ScriptInstance::AddEventHandler(StringHash eventType, const String& handlerName)
{
    if (!scriptObject_)
        return;

    String declaration = "void " + handlerName + "(StringHash, VariantMap&)";
    asIScriptFunction* method = scriptFile_->GetMethod(scriptObject_, declaration);
    if (!method)
    {
        declaration = "void " + handlerName + "()";
        method = scriptFile_->GetMethod(scriptObject_, declaration);
        if (!method)
        {
            LOGERROR("Event handler method " + handlerName + " not found in " + scriptFile_->GetName());
            return;
        }
    }

    SubscribeToEvent(eventType, HANDLER_USERDATA(ScriptInstance, HandleScriptEvent, (void*)method));
}

void ScriptInstance::AddEventHandler(Object* sender, StringHash eventType, const String& handlerName)
{
    if (!scriptObject_)
        return;

    if (!sender)
    {
        LOGERROR("Null event sender for event " + String(eventType) + ", handler " + handlerName);
        return;
    }

    String declaration = "void " + handlerName + "(StringHash, VariantMap&)";
    asIScriptFunction* method = scriptFile_->GetMethod(scriptObject_, declaration);
    if (!method)
    {
        declaration = "void " + handlerName + "()";
        method = scriptFile_->GetMethod(scriptObject_, declaration);
        if (!method)
        {
            LOGERROR("Event handler method " + handlerName + " not found in " + scriptFile_->GetName());
            return;
        }
    }

    SubscribeToEvent(sender, eventType, HANDLER_USERDATA(ScriptInstance, HandleScriptEvent, (void*)method));
}

void ScriptInstance::RemoveEventHandler(StringHash eventType)
{
    UnsubscribeFromEvent(eventType);
}

void ScriptInstance::RemoveEventHandler(Object* sender, StringHash eventType)
{
    UnsubscribeFromEvent(sender, eventType);
}

void ScriptInstance::RemoveEventHandlers(Object* sender)
{
    UnsubscribeFromEvents(sender);
}

void ScriptInstance::RemoveEventHandlers()
{
    UnsubscribeFromAllEventsExcept(PODVector<StringHash>(), true);
}

void ScriptInstance::RemoveEventHandlersExcept(const PODVector<StringHash>& exceptions)
{
    UnsubscribeFromAllEventsExcept(exceptions, true);
}

void ScriptInstance::SetScriptFileAttr(ResourceRef value)
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    SetScriptFile(cache->GetResource<ScriptFile>(value.name_));
}

void ScriptInstance::SetDelayedCallsAttr(PODVector<unsigned char> value)
{
    MemoryBuffer buf(value);
    delayedCalls_.resize(buf.ReadVLE());
    for (auto & elem : delayedCalls_)
    {
        elem.period_ = buf.ReadFloat();
        elem.delay_ = buf.ReadFloat();
        elem.repeat_ = buf.ReadBool();
        elem.declaration_ = buf.ReadString();
        elem.parameters_ = buf.ReadVariantVector();
    }

    if (scriptObject_ && delayedCalls_.size() && !subscribed_)
        UpdateEventSubscription();
}

void ScriptInstance::SetScriptDataAttr(PODVector<unsigned char> data)
{
    if (scriptObject_ && methods_[METHOD_LOAD])
    {
        MemoryBuffer buf(data);
        VariantVector parameters;
        parameters.push_back(Variant((void*)static_cast<Deserializer*>(&buf)));
        scriptFile_->Execute(scriptObject_, methods_[METHOD_LOAD], parameters);
    }
}

void ScriptInstance::SetScriptNetworkDataAttr(PODVector<unsigned char> data)
{
    if (scriptObject_ && methods_[METHOD_READNETWORKUPDATE])
    {
        MemoryBuffer buf(data);
        VariantVector parameters;
        parameters.push_back(Variant((void*)static_cast<Deserializer*>(&buf)));
        scriptFile_->Execute(scriptObject_, methods_[METHOD_READNETWORKUPDATE], parameters);
    }
}

ResourceRef ScriptInstance::GetScriptFileAttr() const
{
    return GetResourceRef(scriptFile_, ScriptFile::GetTypeStatic());
}

PODVector<unsigned char> ScriptInstance::GetDelayedCallsAttr() const
{
    VectorBuffer buf;
    buf.WriteVLE(delayedCalls_.size());
    for (const auto & elem : delayedCalls_)
    {
        buf.WriteFloat(elem.period_);
        buf.WriteFloat(elem.delay_);
        buf.WriteBool(elem.repeat_);
        buf.WriteString(elem.declaration_);
        buf.WriteVariantVector(elem.parameters_);
    }
    return buf.GetBuffer();
}

PODVector<unsigned char> ScriptInstance::GetScriptDataAttr() const
{
    if (!scriptObject_ || !methods_[METHOD_SAVE])
        return PODVector<unsigned char>();
    else
    {
        VectorBuffer buf;
        VariantVector parameters;
        parameters.push_back(Variant((void*)static_cast<Serializer*>(&buf)));
        scriptFile_->Execute(scriptObject_, methods_[METHOD_SAVE], parameters);
        return buf.GetBuffer();
    }
}

PODVector<unsigned char> ScriptInstance::GetScriptNetworkDataAttr() const
{
    if (!scriptObject_ || !methods_[METHOD_WRITENETWORKUPDATE])
        return PODVector<unsigned char>();
    else
    {
        VectorBuffer buf;
        VariantVector parameters;
        parameters.push_back(Variant((void*)static_cast<Serializer*>(&buf)));
        scriptFile_->Execute(scriptObject_, methods_[METHOD_WRITENETWORKUPDATE], parameters);
        return buf.GetBuffer();
    }
}

void ScriptInstance::OnMarkedDirty(Node* node)
{
    // Script functions are not safe from worker threads
    Scene* scene = GetScene();
    if (scene && scene->IsThreadedUpdate())
    {
        scene->DelayedMarkedDirty(this);
        return;
    }

    if (scriptObject_ && methods_[METHOD_TRANSFORMCHANGED])
        scriptFile_->Execute(scriptObject_, methods_[METHOD_TRANSFORMCHANGED]);
}

void ScriptInstance::CreateObject()
{
    if (!scriptFile_ || className_.Empty())
        return;

    PROFILE(CreateScriptObject);

    scriptObject_ = scriptFile_->CreateObject(className_);
    if (scriptObject_)
    {
        // Map script object to script instance with userdata
        scriptObject_->SetUserData(this);

        GetScriptMethods();
        GetScriptAttributes();
        UpdateEventSubscription();

        if (methods_[METHOD_START])
            scriptFile_->Execute(scriptObject_, methods_[METHOD_START]);
    }
    else
        LOGERROR("Failed to create object of class " + className_ + " from " + scriptFile_->GetName());
}

void ScriptInstance::ReleaseObject()
{
    if (scriptObject_)
    {
        if (methods_[METHOD_STOP])
            scriptFile_->Execute(scriptObject_, methods_[METHOD_STOP]);

        PODVector<StringHash> exceptions;
        exceptions.push_back(E_RELOADSTARTED);
        exceptions.push_back(E_RELOADFINISHED);
        UnsubscribeFromAllEventsExcept(exceptions, false);
        if (node_)
            node_->RemoveListener(this);
        subscribed_ = false;
        subscribedPostFixed_ = false;

        ClearScriptMethods();
        ClearScriptAttributes();

        scriptObject_->SetUserData(nullptr);
        scriptObject_->Release();
        scriptObject_ = nullptr;
    }
}

void ScriptInstance::ClearScriptMethods()
{
    for (auto & elem : methods_)
        elem = nullptr;

    delayedCalls_.clear();
}

void ScriptInstance::ClearScriptAttributes()
{
    attributeInfos_ = *context_->GetAttributes(GetTypeStatic());
    idAttributes_.clear();
}

void ScriptInstance::GetScriptMethods()
{
    for (unsigned i = 0; i < MAX_SCRIPT_METHODS; ++i)
        methods_[i] = scriptFile_->GetMethod(scriptObject_, methodDeclarations[i]);
}

void ScriptInstance::GetScriptAttributes()
{
    asIScriptEngine* engine = GetSubsystem<Script>()->GetScriptEngine();
    attributeInfos_ = *context_->GetAttributes(GetTypeStatic());

    unsigned numProperties = scriptObject_->GetPropertyCount();
    for (unsigned i = 0; i < numProperties; ++i)
    {
        const char* name;
        int typeId;
        bool isPrivate, isHandle;

        scriptObject_->GetObjectType()->GetProperty(i, &name, &typeId, &isPrivate);

        // Hide private variables or ones that begin with an underscore
        if (isPrivate || name[0] == '_')
            continue;

        String typeName = engine->GetTypeDeclaration(typeId);
        isHandle = typeName.EndsWith("@");
        if (isHandle)
            typeName = typeName.Substring(0, typeName.Length() - 1);

        AttributeInfo info;
        info.mode_ = AM_FILE;
        info.name_ = name;
        info.ptr_ = scriptObject_->GetAddressOfProperty(i);

        if (!isHandle)
        {
            switch (typeId)
            {
            case asTYPEID_BOOL:
                info.type_ = VAR_BOOL;
                break;

            case asTYPEID_INT32:
            case asTYPEID_UINT32:
                info.type_ = VAR_INT;
                break;

            case asTYPEID_FLOAT:
                info.type_ = VAR_FLOAT;
                break;

            default:
                info.type_ = Variant::GetTypeFromName(typeName);
                break;
            }
        }
        else
        {
            // For a handle type, check if it's an Object subclass with a registered factory
            StringHash typeHash(typeName);
            const QHash<StringHash, SharedPtr<ObjectFactory> >& factories = context_->GetObjectFactories();
            QHash<StringHash, SharedPtr<ObjectFactory> >::const_iterator j = factories.find(typeHash);
            if (j != factories.end())
            {
                // Check base class type. Node & Component are supported as ID attributes, Resource as a resource reference
                StringHash baseType = (*j)->GetBaseType();
                if (baseType == Node::GetTypeStatic())
                {
                    info.mode_ |= AM_NODEID;
                    info.type_ = VAR_INT;
                }
                else if (baseType == Component::GetTypeStatic())
                {
                    info.mode_ |= AM_COMPONENTID;
                    info.type_ = VAR_INT;
                }
                else if (baseType == Resource::GetTypeStatic())
                {
                    info.type_ = VAR_RESOURCEREF;
                    info.defaultValue_ = ResourceRef(typeHash);
                }
            }
        }

        if (info.type_ != VAR_NONE)
            attributeInfos_.push_back(info);
    }
}

void ScriptInstance::UpdateEventSubscription()
{
    Scene* scene = GetScene();
    if (!scene)
    {
        LOGWARNING("Node is detached from scene, can not subscribe script object to update events");
        return;
    }

    bool enabled = scriptObject_ && IsEnabledEffective();

    if (enabled)
    {
        if (!subscribed_ && (methods_[METHOD_UPDATE] || methods_[METHOD_DELAYEDSTART] || delayedCalls_.size()))
        {
            SubscribeToEvent(scene, E_SCENEUPDATE, HANDLER(ScriptInstance, HandleSceneUpdate));
            subscribed_ = true;
        }

        if (!subscribedPostFixed_)
        {
            if (methods_[METHOD_POSTUPDATE])
                SubscribeToEvent(scene, E_SCENEPOSTUPDATE, HANDLER(ScriptInstance, HandleScenePostUpdate));

#ifdef URHO3D_PHYSICS
            if (methods_[METHOD_FIXEDUPDATE] || methods_[METHOD_FIXEDPOSTUPDATE])
            {
                PhysicsWorld* world = scene->GetOrCreateComponent<PhysicsWorld>();
                if (world)
                {
                    if (methods_[METHOD_FIXEDUPDATE])
                        SubscribeToEvent(world, E_PHYSICSPRESTEP, HANDLER(ScriptInstance, HandlePhysicsPreStep));
                    if (methods_[METHOD_FIXEDPOSTUPDATE])
                        SubscribeToEvent(world, E_PHYSICSPOSTSTEP, HANDLER(ScriptInstance, HandlePhysicsPostStep));
                }
                else
                    LOGERROR("No physics world, can not subscribe script object to fixed update events");
            }
#endif
            subscribedPostFixed_ = true;
        }

        if (methods_[METHOD_TRANSFORMCHANGED])
            node_->AddListener(this);
    }
    else
    {
        if (subscribed_)
        {
            UnsubscribeFromEvent(scene, E_SCENEUPDATE);
            subscribed_ = false;
        }

        if (subscribedPostFixed_)
        {
            UnsubscribeFromEvent(scene, E_SCENEPOSTUPDATE);
#ifdef URHO3D_PHYSICS
            PhysicsWorld* world = scene->GetComponent<PhysicsWorld>();
            if (world)
            {
                UnsubscribeFromEvent(world, E_PHYSICSPRESTEP);
                UnsubscribeFromEvent(world, E_PHYSICSPOSTSTEP);
            }
#endif

            subscribedPostFixed_ = false;
        }

        if (methods_[METHOD_TRANSFORMCHANGED])
            node_->RemoveListener(this);
    }
}

void ScriptInstance::HandleSceneUpdate(StringHash eventType, VariantMap& eventData)
{
    if (!scriptObject_)
        return;

    using namespace SceneUpdate;

    float timeStep = eventData[P_TIMESTEP].GetFloat();

    // Execute delayed calls
    for (unsigned i = 0; i < delayedCalls_.size();)
    {
        DelayedCall& call = delayedCalls_[i];
        bool remove = false;

        call.delay_ -= timeStep;
        if (call.delay_ <= 0.0f)
        {
            if (!call.repeat_)
                remove = true;
            else
                call.delay_ += call.period_;

            Execute(call.declaration_, call.parameters_);
        }

        if (remove)
            delayedCalls_.erase(delayedCalls_.begin()+i);
        else
            ++i;
    }

    // Execute delayed start before first update
    if (methods_[METHOD_DELAYEDSTART])
    {
        scriptFile_->Execute(scriptObject_, methods_[METHOD_DELAYEDSTART]);
        methods_[METHOD_DELAYEDSTART] = nullptr;  // Only execute once
    }

    if (methods_[METHOD_UPDATE])
    {
        VariantVector parameters;
        parameters.push_back(timeStep);
        scriptFile_->Execute(scriptObject_, methods_[METHOD_UPDATE], parameters);
    }
}

void ScriptInstance::HandleScenePostUpdate(StringHash eventType, VariantMap& eventData)
{
    if (!scriptObject_)
        return;

    using namespace ScenePostUpdate;

    VariantVector parameters;
    parameters.push_back(eventData[P_TIMESTEP]);
    scriptFile_->Execute(scriptObject_, methods_[METHOD_POSTUPDATE], parameters);
}

#ifdef URHO3D_PHYSICS
void ScriptInstance::HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData)
{
    if (!scriptObject_)
        return;

    using namespace PhysicsPreStep;

    VariantVector parameters;
    parameters.push_back(eventData[P_TIMESTEP]);
    scriptFile_->Execute(scriptObject_, methods_[METHOD_FIXEDUPDATE], parameters);
}

void ScriptInstance::HandlePhysicsPostStep(StringHash eventType, VariantMap& eventData)
{
    if (!scriptObject_)
        return;

    using namespace PhysicsPostStep;

    VariantVector parameters;
    parameters.push_back(eventData[P_TIMESTEP]);
    scriptFile_->Execute(scriptObject_, methods_[METHOD_FIXEDPOSTUPDATE], parameters);
}
#endif
void ScriptInstance::HandleScriptEvent(StringHash eventType, VariantMap& eventData)
{
    if (!IsEnabledEffective() || !scriptFile_ || !scriptObject_)
        return;

    asIScriptFunction* method = static_cast<asIScriptFunction*>(GetEventHandler()->GetUserData());

    VariantVector parameters;
    if (method->GetParamCount() > 0)
    {
        parameters.push_back(Variant((void*)&eventType));
        parameters.push_back(Variant((void*)&eventData));
    }

    scriptFile_->Execute(scriptObject_, method, parameters);
}

void ScriptInstance::HandleScriptFileReload(StringHash eventType, VariantMap& eventData)
{
    ReleaseObject();
}

void ScriptInstance::HandleScriptFileReloadFinished(StringHash eventType, VariantMap& eventData)
{
    if (!className_.Empty())
        CreateObject();
}

Context* GetScriptContext()
{
    return static_cast<Script*>(asGetActiveContext()->GetEngine()->GetUserData())->GetContext();
}

ScriptInstance* GetScriptContextInstance()
{
    asIScriptContext* context = asGetActiveContext();
    asIScriptObject* object = context ? static_cast<asIScriptObject*>(context->GetThisPointer()) : nullptr;
    if (object)
        return static_cast<ScriptInstance*>(object->GetUserData());
    else
        return nullptr;
}

Node* GetScriptContextNode()
{
    ScriptInstance* instance = GetScriptContextInstance();
    return instance ? instance->GetNode() : nullptr;
}

Scene* GetScriptContextScene()
{
    Scene* scene = nullptr;
    Node* node = GetScriptContextNode();
    if (node)
        scene = node->GetScene();
    // If null, try to get the default scene
    if (!scene)
        scene = GetScriptContext()->GetSubsystem<Script>()->GetDefaultScene();

    return scene;
}

ScriptEventListener* GetScriptContextEventListener()
{
    // If the context has an object and that object has user data set, try and get the ScriptInstance, otherwise try and get a ScriptFile.
    asIScriptContext* context = asGetActiveContext();
    if (context)
    {
        asIScriptObject* object = static_cast<asIScriptObject*>(context->GetThisPointer());
        if (object && object->GetUserData())
            return GetScriptContextInstance();
        else
            return GetScriptContextFile();
    }
    else
        return nullptr;
}

Object* GetScriptContextEventListenerObject()
{
    return dynamic_cast<Object*>(GetScriptContextEventListener());
}


}
