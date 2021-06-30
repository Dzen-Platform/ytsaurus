#pragma once

#include "public.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/core/misc/property.h>

#include <yt/yt/core/yson/string.h>

namespace NYT::NObjectServer {

////////////////////////////////////////////////////////////////////////////////

class TAttributeSet
{
public:
    using TAttributeMap = THashMap<TString, NYson::TYsonString>;
    DEFINE_BYREF_RO_PROPERTY(TAttributeMap, Attributes);
    DEFINE_BYVAL_RO_PROPERTY(i64, MasterMemoryUsage);

public:
    bool TryInsert(const TString& key, const NYson::TYsonString& value);
    void Set(const TString& key, const NYson::TYsonString& value);
    bool Remove(const TString& key);
    NYson::TYsonString Find(const TString& key) const;

    void Load(NCellMaster::TLoadContext& context);
    void Save(NCellMaster::TSaveContext& context) const;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
