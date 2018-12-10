#pragma once

#include <yt/core/misc/format.h>

namespace NYT {
namespace {

////////////////////////////////////////////////////////////////////////////////

// Below there is a serie of probe classes.
// They are defined in an anonymous namespace to alleviate linkage-related
// book-keeping minors. The main goal was to make all this stuff header-only.

// A state for probes that keeps various calls counts.
struct TProbeState
{
    int Constructors;
    int Destructors;
    int ShadowDestructors;
    int CopyConstructors;
    int CopyAssignments;
    int MoveConstructors;
    int MoveAssignments;
    int Tackles;

    TProbeState()
    {
        Reset();
    }

    void Reset()
    {
        memset(this, 0, sizeof(*this));
    }
};

// A scoper which clears state before and after entering a scope.
struct TProbeScoper
{
    TProbeState* State;

    TProbeScoper(TProbeState* state)
        : State(state)
    {
        State->Reset();
    }

    ~TProbeScoper()
    {
        State->Reset();
    }
};

// Used for probing the number of copies that occur if a type must be coerced.
class TCoercibleToProbe
{
public:
    TProbeState* State;
    TProbeState* ShadowState;

public:
    explicit TCoercibleToProbe(TProbeState* state)
        : State(state)
        , ShadowState(state)
    { }

private:
    TCoercibleToProbe(const TCoercibleToProbe&);
    TCoercibleToProbe(TCoercibleToProbe&&);
    TCoercibleToProbe& operator=(const TCoercibleToProbe&);
    TCoercibleToProbe& operator=(TCoercibleToProbe&&);
};

// Used for probing the number of copies in an argument.
class TProbe
{
public:
    TProbeState* State;
    TProbeState* ShadowState;

public:
    static TProbe ExplicitlyCreateInvalidProbe()
    {
        return TProbe();
    }

    explicit TProbe(TProbeState* state)
        : State(state)
        , ShadowState(state)
    {
        Y_ASSERT(State);
        ++State->Constructors;
    }

    ~TProbe()
    {
        if (State) {
            ++State->Destructors;
        }
        if (ShadowState) {
            ++ShadowState->ShadowDestructors;
        }
    }

    TProbe(const TProbe& other)
        : State(other.State)
        , ShadowState(other.ShadowState)
    {
        Y_ASSERT(State);
        ++State->CopyConstructors;
    }

    TProbe(TProbe&& other)
        : State(other.State)
        , ShadowState(other.ShadowState)
    {
        Y_ASSERT(State);
        other.State = nullptr;
        ++State->MoveConstructors;
    }

    TProbe(const TCoercibleToProbe& other)
        : State(other.State)
        , ShadowState(other.ShadowState)
    {
        Y_ASSERT(State);
        ++State->CopyConstructors;
    }

    TProbe(TCoercibleToProbe&& other)
        : State(other.State)
        , ShadowState(other.ShadowState)
    {
        Y_ASSERT(State);
        other.State = nullptr;
        ++State->MoveConstructors;
    }

    TProbe& operator=(const TProbe& other)
    {
        State = other.State;
        ShadowState = other.ShadowState;
        Y_ASSERT(State);
        ++State->CopyAssignments;
        return *this;
    }

    TProbe& operator=(TProbe&& other)
    {
        State = other.State;
        ShadowState = other.ShadowState;
        Y_ASSERT(State);
        other.State = nullptr;
        ++State->MoveAssignments;
        return *this;
    }

    void Tackle() const
    {
        Y_ASSERT(State);
        ++State->Tackles;
    }

    bool IsValid() const
    {
        return State != nullptr;
    }

private:
    TProbe()
        : State(nullptr)
    { }
};

void Tackle(const TProbe& probe)
{
    probe.Tackle();
}

// A helper functor which extracts from probe-like objectss their state.
struct TProbableTraits
{
    static const TProbeState& ExtractState(const TProbeState& arg)
    {
        return arg;
    }

    static const TProbeState& ExtractState(const TProbeState* arg)
    {
        return *arg;
    }

    static const TProbeState& ExtractState(const TProbe& arg)
    {
        return *arg.State;
    }

    static const TProbeState& ExtractState(const TCoercibleToProbe& arg)
    {
        return *arg.State;
    }
};

MATCHER(IsAlive, "is alive")
{
    Y_UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return
        state.Destructors <
        state.Constructors + state.CopyConstructors + state.CopyAssignments;
}

MATCHER(IsDead, "is dead")
{
    Y_UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return
        state.Destructors ==
        state.Constructors + state.CopyConstructors + state.CopyAssignments;
}

MATCHER_P2(HasCopyMoveCounts, copyCount, moveCount, "" + \
    ::testing::PrintToString(copyCount) + " copy constructors and " + \
    ::testing::PrintToString(moveCount) + " move constructors were called")
{
    Y_UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return
        state.CopyConstructors == copyCount &&
        state.MoveConstructors == moveCount;
}

MATCHER(NoCopies, "no copies were made")
{
    Y_UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return state.CopyConstructors == 0 && state.CopyAssignments == 0;
}

MATCHER(NoMoves, "no moves were made")
{
    Y_UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return state.MoveConstructors == 0 && state.MoveAssignments == 0;
}

MATCHER(NoAssignments, "no assignments were made")
{
    Y_UNUSED(result_listener);
    const TProbeState& state = TProbableTraits::ExtractState(arg);
    return state.CopyAssignments == 0 && state.MoveAssignments == 0;
}

void PrintTo(const TProbeState& state, ::std::ostream* os)
{
    int copies = state.CopyConstructors + state.CopyAssignments;
    int moves = state.MoveConstructors + state.MoveAssignments;

    *os << Format(
        "%v ctors, %v dtors; "
        "copies: %v = %v + %v; moves: %v = %v + %v",
        state.Constructors, state.Destructors,
        copies, state.CopyConstructors, state.CopyAssignments,
        moves, state.MoveConstructors, state.MoveAssignments);
}

void PrintTo(const TProbe& arg, ::std::ostream* os)
{
    PrintTo(TProbableTraits::ExtractState(arg), os);
}

void PrintTo(const TCoercibleToProbe& arg, ::std::ostream* os)
{
    PrintTo(TProbableTraits::ExtractState(arg), os);
}

////////////////////////////////////////////////////////////////////////////////

} // namepsace 
} // namespace NYT

