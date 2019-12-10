#include "helpers.h"

#include <yt/core/ypath/token.h>

#include <yt/core/ytree/permission.h>

namespace NYT::NSecurityClient {

using namespace NYPath;

////////////////////////////////////////////////////////////////////////////////

TYPath GetUserPath(const TString& name)
{
    return "//sys/users/" + ToYPathLiteral(name);
}

TYPath GetGroupPath(const TString& name)
{
    return "//sys/groups/" + ToYPathLiteral(name);
}

////////////////////////////////////////////////////////////////////////////////

ESecurityAction CheckPermissionsByAclAndSubjectClosure(
    const TSerializableAccessControlList& acl,
    const THashSet<TString>& subjectClosure,
    NYTree::EPermissionSet permissions)
{
    NYTree::EPermissionSet actualPermissions = {};
    for (const auto& ace : acl.Entries) {
        if (ace.Action != NSecurityClient::ESecurityAction::Allow) {
            THROW_ERROR_EXCEPTION("Action %Qv is not supported", FormatEnum(ace.Action));
        }
        for (const auto& aceSubject : ace.Subjects) {
            if (subjectClosure.contains(aceSubject)) {
                actualPermissions |= ace.Permissions;
                break;
            }
        }
    }
    return (actualPermissions & permissions) == permissions
        ? ESecurityAction::Allow
        : ESecurityAction::Deny;
}

void ValidateSecurityTag(const TSecurityTag& tag)
{
    if (tag.empty()) {
        THROW_ERROR_EXCEPTION("Security tag cannot be empty");
    }
    if (tag.length() > MaxSecurityTagLength) {
        THROW_ERROR_EXCEPTION("Security tag %Qv is too long: %v > %v",
            tag,
            tag.length(),
            MaxSecurityTagLength);
    }
}

void ValidateSecurityTags(const std::vector<TSecurityTag>& tags)
{
    for (const auto& tag : tags) {
        ValidateSecurityTag(tag);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityClient

