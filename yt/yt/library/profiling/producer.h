#pragma once

#include "sensor.h"

#include <util/generic/string.h>

#include <yt/core/misc/ref_counted.h>
#include <yt/core/misc/intrusive_ptr.h>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

struct ISensorWriter
{
    virtual ~ISensorWriter() = default;

    virtual void PushTag(const TTag& tag) = 0;
    virtual void PopTag() = 0;

    virtual void AddGauge(const TString& name, double value) = 0;

    //! AddCounter emits single counter value.
    /*!
     *  #value MUST be monotonically increasing.
     */
    virtual void AddCounter(const TString& name, i64 value) = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TSensorBuffer final
    : public ISensorWriter
{
public:
    virtual void PushTag(const TTag& tag) override;
    virtual void PopTag() override;

    virtual void AddGauge(const TString& name, double value) override;
    virtual void AddCounter(const TString& name, i64 value) override;

    void WriteTo(ISensorWriter* writer);

    const std::vector<std::tuple<TString, TTagList, i64>>& GetCounters() const;
    const std::vector<std::tuple<TString, TTagList, double>>& GetGauges() const;

private:
    TTagList Tags_;

    std::vector<std::tuple<TString, TTagList, i64>> Counters_;
    std::vector<std::tuple<TString, TTagList, double>> Gauges_;
};

////////////////////////////////////////////////////////////////////////////////

struct ISensorProducer
    : virtual public TRefCounted
{
    //! Collect returns set of gauges or counters associated with this producer.
    /*!
     *  Registry keeps track of all (name, tags) pair that were ever returned from
     *  this producer.
     * 
     *  Do not use this interface, if set of tags might grow unbound. There is
     *  no way to cleanup removed tags.
     */
    virtual void Collect(ISensorWriter* writer) = 0;
};

DEFINE_REFCOUNTED_TYPE(ISensorProducer)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
