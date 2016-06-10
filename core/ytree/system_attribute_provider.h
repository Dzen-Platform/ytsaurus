#pragma once

#include "public.h"
#include "permission.h"

#include <yt/core/yson/consumer.h>
#include <yt/core/yson/string.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/nullable.h>

#include <yt/core/actions/future.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

struct ISystemAttributeProvider
{
    virtual ~ISystemAttributeProvider() = default;

    //! Describes a system attribute.
    struct TAttributeDescriptor
    {
        const char* Key = nullptr;
        bool Present = true;
        bool Opaque = false;
        bool Custom = false;
        bool Removable = false;
        bool Replicated = false;
        bool Mandatory = false;
        bool External = false;
        EPermissionSet WritePermission = EPermission::Write;

        TAttributeDescriptor& SetPresent(bool value)
        {
            Present = value;
            return *this;
        }

        TAttributeDescriptor& SetOpaque(bool value)
        {
            Opaque = value;
            return *this;
        }

        TAttributeDescriptor& SetCustom(bool value)
        {
            Custom = value;
            return *this;
        }

        TAttributeDescriptor& SetRemovable(bool value)
        {
            Removable = value;
            return *this;
        }

        TAttributeDescriptor& SetReplicated(bool value)
        {
            Replicated = value;
            return *this;
        }

        TAttributeDescriptor& SetMandatory(bool value)
        {
            Mandatory = value;
            return *this;
        }

        TAttributeDescriptor& SetExternal(bool value)
        {
            External = value;
            return *this;
        }

        TAttributeDescriptor& SetWritePermission(EPermission value)
        {
            WritePermission = value;
            return *this;
        }

        TAttributeDescriptor(const char* key)
            : Key(key)
        { }
    };

    //! Populates the list of all system attributes supported by this object.
    /*!
     *  \note
     *  Must not clear #attributes since additional items may be added in inheritors.
     */
    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) = 0;

    //! Returns a (typically cached) set consisting of all non-custom attributes keys.
    //! \see TAttributeDescriptor::Custom
    //! \see ListSystemAttributes
    virtual const yhash_set<const char*>& GetBuiltinAttributeKeys() = 0;

    //! Gets the value of a builtin attribute.
    /*!
     *  \returns |false| if there is no builtin attribute with the given key.
     */
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) = 0;

    //! Asynchronously gets the value of a builtin attribute.
    /*!
     *  \returns A future representing attribute value or null if there is no such async builtin attribute.
     */
    virtual TFuture<NYson::TYsonString> GetBuiltinAttributeAsync(const Stroka& key) = 0;

    //! Sets the value of a builtin attribute.
    /*!
     *  \returns |false| if there is no writable builtin attribute with the given key.
     */
    virtual bool SetBuiltinAttribute(const Stroka& key, const NYson::TYsonString& value) = 0;

    //! Asynchronously sets the value of a builtin attribute.
    /*!
     *  \returns A future representing the outcome of the operation or |Null| if no such asynchronous attribute is known.
     */
    virtual TFuture<void> SetBuiltinAttributeAsync(const Stroka& key, const NYson::TYsonString& value) = 0;

    //! Removes the builtin attribute.
    /*!
     *  \returns |false| if there is no removable builtin attribute with the given key.
     */
    virtual bool RemoveBuiltinAttribute(const Stroka& key) = 0;


    // Extension methods.

    //! Similar to its interface counterpart, but populates a map rather than a vector.
    void ListSystemAttributes(std::map<Stroka, TAttributeDescriptor>* descriptors);

    //! Populates the list of all builtin attributes supported by this object.
    void ListBuiltinAttributes(std::vector<TAttributeDescriptor>* descriptors);

    //! Returns an instance of TAttributeDescriptor matching a given #key or |Null| if no such
    //! builtin attribute is known.
    TNullable<TAttributeDescriptor> FindBuiltinAttributeDescriptor(const Stroka& key);

    //! Wraps #GetBuiltinAttribute and returns the YSON string instead
    //! of writing it into a consumer.
    TNullable<NYson::TYsonString> FindBuiltinAttribute(const Stroka& key);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
