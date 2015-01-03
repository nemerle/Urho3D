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

#include "../Container/Ptr.h"
#include "../Core/Variant.h"

namespace Urho3D
{

/// Attribute shown only in the editor, but not serialized.
static const unsigned AM_EDIT = 0x0;
/// Attribute used for file serialization.
static const unsigned AM_FILE = 0x1;
/// Attribute used for network replication.
static const unsigned AM_NET = 0x2;
/// Attribute used for both file serialization and network replication (default.)
static const unsigned AM_DEFAULT = 0x3;
/// Attribute should use latest data grouping instead of delta update in network replication.
static const unsigned AM_LATESTDATA = 0x4;
/// Attribute should not be shown in the editor.
static const unsigned AM_NOEDIT = 0x8;
/// Attribute is a node ID and may need rewriting.
static const unsigned AM_NODEID = 0x10;
/// Attribute is a component ID and may need rewriting.
static const unsigned AM_COMPONENTID = 0x20;
/// Attribute is a node ID vector where first element is the amount of nodes.
static const unsigned AM_NODEIDVECTOR = 0x40;

class Serializable;

/// Abstract base class for invoking attribute accessors.
class URHO3D_API AttributeAccessor : public RefCounted
{
public:
    /// Get the attribute.
    virtual void Get(const Serializable* ptr, Variant& dest) const = 0;
    /// Set the attribute.
    virtual void Set(Serializable* ptr, const Variant& src) = 0;
};

/// Description of an automatically serializable variable.
struct AttributeInfo
{
    /// Construct empty.
    AttributeInfo() :
        type_(VAR_NONE),
        offset_(0),
        enumNames_(0),
        mode_(AM_DEFAULT),
        ptr_(0)
    {
    }

    /// Construct offset attribute.
    AttributeInfo(VariantType type, const char* name, size_t offset, const Variant& defaultValue, unsigned mode) :
        type_(type),
        name_(name),
        offset_((unsigned)offset),
        enumNames_(0),
        defaultValue_(defaultValue),
        mode_(mode),
        ptr_(0)
    {
    }

    /// Construct offset enum attribute.
    AttributeInfo(const char* name, size_t offset, const char** enumNames, const Variant& defaultValue, unsigned mode) :
        type_(VAR_INT),
        name_(name),
        offset_((unsigned)offset),
        enumNames_(enumNames),
        defaultValue_(defaultValue),
        mode_(mode),
        ptr_(0)
    {
    }

    /// Construct accessor attribute.
    AttributeInfo(VariantType type, const char* name, AttributeAccessor* accessor, const Variant& defaultValue, unsigned mode) :
        type_(type),
        name_(name),
        offset_(0),
        enumNames_(0),
        accessor_(accessor),
        defaultValue_(defaultValue),
        mode_(mode),
        ptr_(0)
    {
    }

    /// Construct accessor enum attribute.
    AttributeInfo(const char* name, AttributeAccessor* accessor, const char** enumNames, const Variant& defaultValue, unsigned mode) :
        type_(VAR_INT),
        name_(name),
        offset_(0),
        enumNames_(enumNames),
        accessor_(accessor),
        defaultValue_(defaultValue),
        mode_(mode),
        ptr_(0)
    {
    }
    AttributeInfo(const AttributeInfo &other) :
        type_(other.type_),
        name_(other.name_),
        offset_(other.offset_),
        enumNames_(other.enumNames_),
        accessor_(other.accessor_),
        defaultValue_(other.defaultValue_),
        mode_(other.mode_),
        ptr_(other.ptr_)
    {}
    AttributeInfo(AttributeInfo &&other) :
        type_(other.type_),
        name_(other.name_),
        offset_(other.offset_),
        enumNames_(other.enumNames_),
        accessor_(other.accessor_),
        defaultValue_(other.defaultValue_),
        mode_(other.mode_),
        ptr_(other.ptr_)
    {
        other.type_ = VAR_NONE;
        other.offset_ = 0;
        other.enumNames_ = nullptr;
        other.mode_ = AM_DEFAULT;
        other.ptr_ = nullptr;
    }
    /// Unifying assignment operator
    AttributeInfo &operator=(AttributeInfo op) {
        swap(op);
        return *this;
    }
    void swap(AttributeInfo &rhs) {
        std::swap(type_,rhs.type_);
        std::swap(name_,rhs.name_);
        std::swap(offset_,rhs.offset_);
        std::swap(enumNames_,rhs.enumNames_);
        std::swap(accessor_,rhs.accessor_);
        std::swap(defaultValue_,rhs.defaultValue_);
        std::swap(mode_,rhs.mode_);
        std::swap(ptr_,rhs.ptr_);
    }
    /// Attribute type.
    VariantType type_;
    /// Name.
    String name_;
    /// Byte offset from start of object.
    unsigned offset_;
    /// Enum names.
    const char** enumNames_;
    /// Helper object for accessor mode.
    SharedPtr<AttributeAccessor> accessor_;
    /// Default value for network replication.
    Variant defaultValue_;
    /// Attribute mode: whether to use for serialization, network replication, or both.
    unsigned mode_;
    /// Attribute data pointer if elsewhere than in the Serializable.
    void* ptr_;
};

}
