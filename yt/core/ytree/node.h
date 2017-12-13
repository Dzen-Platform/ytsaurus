#pragma once

#include "public.h"
#include "attribute_owner.h"
#include "ypath_service.h"

#include <yt/core/misc/mpl.h>
#include <yt/core/misc/serialize.h>

#include <yt/core/yson/public.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <class T>
struct TScalarTypeTraits
{ };

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

//! A base DOM-like interface representing a node.
struct INode
    : public virtual IYPathService
    , public virtual IAttributeOwner
{
    //! Returns the static type of the node.
    virtual ENodeType GetType() const = 0;

    //! Returns a new instance of transactional factory for creating new nodes.
    /*!
     *  Every YTree implementation provides its own set of
     *  node implementations. E.g., for an ephemeral implementation
     *  this factory creates ephemeral nodes while for
     *  a persistent implementation (see Cypress) this factory
     *  creates persistent nodes.
     */
    virtual std::unique_ptr<ITransactionalNodeFactory> CreateFactory() const = 0;

    //! Returns a YPath for this node.
    virtual TYPath GetPath() const = 0;

    // A bunch of "AsSomething" methods that return a pointer
    // to the same node but typed as "Something".
    // These methods throw an exception on type mismatch.
#define DECLARE_AS_METHODS(name) \
    virtual TIntrusivePtr<I##name##Node> As##name() = 0; \
    virtual TIntrusivePtr<const I##name##Node> As##name() const = 0;

    DECLARE_AS_METHODS(Entity)
    DECLARE_AS_METHODS(Composite)
    DECLARE_AS_METHODS(String)
    DECLARE_AS_METHODS(Int64)
    DECLARE_AS_METHODS(Uint64)
    DECLARE_AS_METHODS(Double)
    DECLARE_AS_METHODS(Boolean)
    DECLARE_AS_METHODS(List)
    DECLARE_AS_METHODS(Map)
#undef DECLARE_AS_METHODS

    //! Returns the parent of the node.
    //! |nullptr| indicates that the current node is the root.
    virtual ICompositeNodePtr GetParent() const = 0;
    
    //! Sets the parent of the node.
    /*!
     *  This method is called automatically when one subtree (possibly)
     *  consisting of a single node is attached to another.
     *
     *  This method must not be called explicitly.
     */
    virtual void SetParent(const ICompositeNodePtr& parent) = 0;

    //! A helper method for retrieving a scalar value from a node.
    //! Invokes the appropriate |AsSomething| followed by |GetValue|.
    template <class T>
    typename NMpl::TCallTraits<T>::TType GetValue() const
    {
        return NDetail::TScalarTypeTraits<T>::GetValue(this);
    }

    //! A helper method for assigning a scalar value to a node.
    //! Invokes the appropriate |AsSomething| followed by |SetValue|.
    template <class T>
    void SetValue(typename NMpl::TCallTraits<T>::TType value)
    {
        NDetail::TScalarTypeTraits<T>::SetValue(this, value);
    }
};

DEFINE_REFCOUNTED_TYPE(INode)

////////////////////////////////////////////////////////////////////////////////

//! A base interface for all scalar nodes, i.e. nodes containing a single atomic value.
template <class T>
struct IScalarNode
    : public virtual INode
{
    typedef T TValue;

    //! Gets the value.
    virtual typename NMpl::TCallTraits<TValue>::TType GetValue() const = 0;

    //! Sets the value.
    virtual void SetValue(typename NMpl::TCallTraits<TValue>::TType value) = 0;
};

// Define the actual scalar node types: IStringNode, IInt64Node, IUint64Node, IDoubleNode, IBooleanNode.
#define DECLARE_SCALAR_TYPE(name, type) \
    struct I##name##Node \
        : IScalarNode<type> \
    { }; \
    \
    DEFINE_REFCOUNTED_TYPE(I##name##Node) \
    \
    namespace NDetail { \
    \
    template <> \
    struct TScalarTypeTraits<type> \
    { \
        typedef I##name##Node TNode; \
        typedef type TType; \
        typedef NMpl::TConditional< \
            NMpl::TIsSame<type, TString>::Value, \
            /* if-true  */ const TStringBuf&, \
            /* if-false */ type \
        >::TType TConsumerType; \
        \
        static const ENodeType NodeType; \
        \
        static NMpl::TCallTraits<type>::TType GetValue(const IConstNodePtr& node) \
        { \
            return node->As##name()->GetValue(); \
        } \
        \
        static void SetValue(const INodePtr& node, NMpl::TCallTraits<type>::TType value) \
        { \
            node->As##name()->SetValue(value); \
        } \
    }; \
    \
    }

// Don't forget to define a #TScalarTypeTraits<>::NodeType constant in "node.cpp".
DECLARE_SCALAR_TYPE(String, TString)
DECLARE_SCALAR_TYPE(Int64, i64)
DECLARE_SCALAR_TYPE(Uint64, ui64)
DECLARE_SCALAR_TYPE(Double, double)
DECLARE_SCALAR_TYPE(Boolean, bool)

#undef DECLARE_SCALAR_TYPE

////////////////////////////////////////////////////////////////////////////////

//! A base interface for all composite nodes, i.e. nodes containing other nodes.
struct ICompositeNode
    : public virtual INode
{
    //! Removes all child nodes.
    virtual void Clear() = 0;

    //! Returns the number of child nodes.
    virtual int GetChildCount() const = 0;

    //! Replaces one child by the other.
    //! #newChild must be a root.
    virtual void ReplaceChild(const INodePtr& oldChild, const INodePtr& newChild) = 0;

    //! Removes a child.
    //! The removed child becomes a root.
    virtual void RemoveChild(const INodePtr& child) = 0;
};

DEFINE_REFCOUNTED_TYPE(ICompositeNode)

////////////////////////////////////////////////////////////////////////////////

//! A map node, which keeps a dictionary mapping strings (TString) to child nodes.
struct IMapNode
    : public virtual ICompositeNode
{
    using ICompositeNode::RemoveChild;

    //! Returns the current snapshot of the map.
    /*!
     *  Map items are returned in unspecified order.
     */
    virtual std::vector<std::pair<TString, INodePtr>> GetChildren() const = 0;

    //! Returns map keys.
    /*!
     *  Keys are returned in unspecified order.
     */
    virtual std::vector<TString> GetKeys() const = 0;

    //! Gets a child by its key.
    /*!
     *  \param key A key.
     *  \return A child with the given key or NULL if the index is not valid.
     */
    virtual INodePtr FindChild(const TString& key) const = 0;

    //! Adds a new child with a given key.
    /*!
     *  \param child A child.
     *  \param key A key.
     *  \return True iff the key was not in the map already and thus the child is inserted.
     *
     *  \note
     *  #child must be a root.
     */
    virtual bool AddChild(const INodePtr& child, const TString& key) = 0;

    //! Removes a child by its key.
    /*!
     *  \param key A key.
     *  \return True iff there was a child with the given key.
     */
    virtual bool RemoveChild(const TString& key) = 0;

    //! Similar to #FindChild but throws if no child is found.
    INodePtr GetChild(const TString& key) const;

    //! Returns the key for a given child.
    /*!
     *  \param child A node that must be a child.
     *  \return Child's key.
     */
    virtual TString GetChildKey(const IConstNodePtr& child) = 0;
};

DEFINE_REFCOUNTED_TYPE(IMapNode)

////////////////////////////////////////////////////////////////////////////////

//! A list node, which keeps a list (vector) of children.
struct IListNode
    : public virtual ICompositeNode
{
    using ICompositeNode::RemoveChild;

    //! Returns the current snapshot of the list.
    virtual std::vector<INodePtr> GetChildren() const = 0;

    //! Gets a child by its index.
    /*!
     *  \param index An index.
     *  \return A child with the given index or NULL if the index is not valid.
     */
    virtual INodePtr FindChild(int index) const = 0;

    //! Adds a new child at a given position.
    /*!
     *  \param child A child.
     *  \param beforeIndex A position before which the insertion must happen.
     *  -1 indicates the end of the list.
     *
     *  \note
     *  #child must be a root.
     */

    virtual void AddChild(const INodePtr& child, int beforeIndex = -1) = 0;

    //! Removes a child by its index.
    /*!
     *  \param index An index.
     *  \return True iff the index is valid and thus the child is removed.
     */
    virtual bool RemoveChild(int index) = 0;

    //! Similar to #FindChild but fails if the index is not valid.
    INodePtr GetChild(int index) const;

    //! Returns the index for a given child.
    /*!
     *  \param child A node that must be a child.
     *  \return Child's index.
     */
    virtual int GetChildIndex(const IConstNodePtr& child) = 0;

    //! Normalizes negative indexes (by adding child count).
    //! Throws if the index is invalid.
    /*!
     *  \param index Original (possibly negative) index.
     *  \returns Adjusted (valid non-negative) index.
     */
    int AdjustChildIndex(int index) const;

};

DEFINE_REFCOUNTED_TYPE(IListNode)

////////////////////////////////////////////////////////////////////////////////

//! An structureless entity node.
struct IEntityNode
    : public virtual INode
{ };

DEFINE_REFCOUNTED_TYPE(IEntityNode)

////////////////////////////////////////////////////////////////////////////////

//! A factory for creating nodes.
/*!
 *  All freshly created nodes are roots, i.e. have no parent.
 *  
 *  The factory also acts as a "transaction context" that holds all created nodes.
 *
 *  One must call #Commit at the end if the operation was a success.
 *
 *  Releasing the instance without calling #Commit or calling #Rollback abandons all changes
 *  and invokes all handlers installed via #RegisterRollbackHandler.
 */
struct INodeFactory
{
    virtual ~INodeFactory() = default;

    //! Creates a string node.
    virtual IStringNodePtr CreateString() = 0;

    //! Creates an int64 node.
    virtual IInt64NodePtr CreateInt64() = 0;

    //! Creates an uint64 node.
    virtual IUint64NodePtr CreateUint64() = 0;

    //! Creates an FP number node.
    virtual IDoubleNodePtr CreateDouble() = 0;

    //! Creates an boolean node.
    virtual IBooleanNodePtr CreateBoolean() = 0;

    //! Creates a map node.
    virtual IMapNodePtr CreateMap() = 0;

    //! Creates a list node.
    virtual IListNodePtr CreateList() = 0;

    //! Creates an entity node.
    virtual IEntityNodePtr CreateEntity() = 0;
};

////////////////////////////////////////////////////////////////////////////////

//! A node factory with extented transactional capabilities.
/*!
 *  The factory also acts as a "transaction context" that holds all created nodes.
 *
 *  One must call #Commit at the end if the operation was a success.
 *  Releasing the instance without calling #Commit or calling #Rollback abandons all changes.
 */
struct ITransactionalNodeFactory
    : public INodeFactory
{
    //! Must be called before releasing the factory to indicate that all created nodes
    //! must persist.
    virtual void Commit() = 0;

    //! Invokes all rollback handlers.
    virtual void Rollback() = 0;
};

////////////////////////////////////////////////////////////////////////////////

void Serialize(INode& value, NYson::IYsonConsumer* consumer);
void Deserialize(INodePtr& value, const INodePtr& node);
void Deserialize(IStringNodePtr& value, const INodePtr& node);
void Deserialize(IInt64NodePtr& value, const INodePtr& node);
void Deserialize(IUint64NodePtr& value, const INodePtr& node);
void Deserialize(IDoubleNodePtr& value, const INodePtr& node);
void Deserialize(IBooleanNodePtr& value, const INodePtr& node);
void Deserialize(IMapNodePtr& value, const INodePtr& node);
void Deserialize(IListNodePtr& value, const INodePtr& node);
void Deserialize(IEntityNodePtr& value, const INodePtr& node);

NYson::TYsonString ConvertToYsonStringStable(const INodePtr& node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

