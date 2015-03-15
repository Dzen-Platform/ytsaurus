#pragma once

#include "public.h"
#include "automaton.h"

#include <core/misc/serialize.h>

#include <core/logging/log.h>

#include <core/profiling/profiler.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

struct TEntitySerializationKey
{
    TEntitySerializationKey()
        : Index(-1)
    { }

    explicit TEntitySerializationKey(int index)
        : Index(index)
    { }

#define DEFINE_OPERATOR(op) \
    bool operator op (TEntitySerializationKey other) const \
    { \
        return Index op other.Index; \
    }

    DEFINE_OPERATOR(==)
    DEFINE_OPERATOR(!=)
    DEFINE_OPERATOR(<)
    DEFINE_OPERATOR(<=)
    DEFINE_OPERATOR(>)
    DEFINE_OPERATOR(>=)
#undef DEFINE_OPERATOR

    void Save(TSaveContext& context) const;
    void Load(TLoadContext& context);

    int Index;
};

////////////////////////////////////////////////////////////////////////////////

class TSaveContext
    : public NYT::TStreamSaveContext
{
public:
    TEntitySerializationKey GenerateSerializationKey();

private:
    int SerializationKeyIndex_ = 0;

};

////////////////////////////////////////////////////////////////////////////////

class TLoadContext
    : public NYT::TStreamLoadContext
{
public:
    DEFINE_BYVAL_RW_PROPERTY(int, Version);

public:
    TLoadContext();

    void Reset();

    TEntitySerializationKey RegisterEntity(TEntityBase* entity);

    template <class T>
    T* GetEntity(TEntitySerializationKey key) const;

private:
    std::vector<TEntityBase*> Entities_;

};

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ESerializationPriority,
    (Keys)
    (Values)
);

class TCompositeAutomatonPart
    : public virtual TRefCounted
{
public:
    TCompositeAutomatonPart(
        IHydraManagerPtr hydraManager,
        TCompositeAutomatonPtr automaton);

protected:
    const IHydraManagerPtr HydraManager_;
    TCompositeAutomaton* const Automaton_;

    void RegisterSaver(
        ESerializationPriority priority,
        const Stroka& name,
        TClosure saver);

    void RegisterLoader(
        const Stroka& name,
        TClosure loader);

    void RegisterSaver(
        ESerializationPriority priority,
        const Stroka& name,
        TCallback<void(TSaveContext&)> saver);

    void RegisterLoader(
        const Stroka& name,
        TCallback<void(TLoadContext&)> loader);

    template <class TRequest, class TResponse>
    void RegisterMethod(TCallback<TResponse(const TRequest&)> handler);

    bool IsLeader() const;
    bool IsFollower() const;
    bool IsRecovery() const;

    virtual bool ValidateSnapshotVersion(int version);
    virtual int GetCurrentSnapshotVersion();

    virtual void Clear();

    virtual void OnBeforeSnapshotLoaded();
    virtual void OnAfterSnapshotLoaded();

    virtual void OnStartLeading();
    virtual void OnLeaderRecoveryComplete();
    virtual void OnLeaderActive();
    virtual void OnStopLeading();

    virtual void OnStartFollowing();
    virtual void OnFollowerRecoveryComplete();
    virtual void OnStopFollowing();

    virtual void OnRecoveryStarted();
    virtual void OnRecoveryComplete();

private:
    typedef TCompositeAutomatonPart TThis;
    friend class TCompositeAutomaton;

    template <class TRequest, class TResponse>
    struct TThunkTraits;

    void RegisterMethod(
        const Stroka& name,
        TCallback<void(TMutationContext*)> handler);

};

DEFINE_REFCOUNTED_TYPE(TCompositeAutomatonPart)

////////////////////////////////////////////////////////////////////////////////

class TCompositeAutomaton
    : public IAutomaton
{
protected:
    NLogging::TLogger Logger;
    NProfiling::TProfiler Profiler;


    TCompositeAutomaton();

    void RegisterPart(TCompositeAutomatonPart* part);

    virtual TSaveContext& SaveContext() = 0;
    virtual TLoadContext& LoadContext() = 0;

private:
    typedef TCompositeAutomaton TThis;
    friend class TCompositeAutomatonPart;

    struct TMethodInfo
    {
        TCallback<void(TMutationContext* context)> Callback;
        NProfiling::TTagId TagId;
    };

    struct TSaverInfo
    {
        ESerializationPriority Priority;
        Stroka Name;
        TClosure Saver;
        TCompositeAutomatonPart* Part;

        TSaverInfo(
            ESerializationPriority priority,
            const Stroka& name,
            TClosure saver,
            TCompositeAutomatonPart* part);
    };

    struct TLoaderInfo
    {
        Stroka Name;
        TClosure Loader;
        TCompositeAutomatonPart* Part;

        TLoaderInfo(
            const Stroka& name,
            TClosure loader,
            TCompositeAutomatonPart* part);
    };

    std::vector<TCompositeAutomatonPart*> Parts_;

    yhash_map<Stroka, TMethodInfo> Methods_;

    yhash_map<Stroka, TLoaderInfo> Loaders_;
    yhash_map<Stroka, TSaverInfo> Savers_;


    virtual void SaveSnapshot(TOutputStream* output) override;
    virtual void LoadSnapshot(TInputStream* input) override;

    virtual void ApplyMutation(TMutationContext* context) override;

    virtual void Clear() override;


    void OnRecoveryStarted();
    void OnRecoveryComplete();

};

DEFINE_REFCOUNTED_TYPE(TCompositeAutomaton)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

#define COMPOSITE_AUTOMATON_INL_H_
#include "composite_automaton-inl.h"
#undef COMPOSITE_AUTOMATON_INL_H_
