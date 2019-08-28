#include "operation_archive_schema.h"

namespace NYT::NApi {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TOrderedByIdTableDescriptor::TOrderedByIdTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TOrderedByIdTableDescriptor::TIndex::TIndex(const TNameTablePtr& nameTable)
    : IdHash(nameTable->RegisterName("id_hash"))
    , IdHi(nameTable->RegisterName("id_hi"))
    , IdLo(nameTable->RegisterName("id_lo"))
    , State(nameTable->RegisterName("state"))
    , AuthenticatedUser(nameTable->RegisterName("authenticated_user"))
    , OperationType(nameTable->RegisterName("operation_type"))
    , Progress(nameTable->RegisterName("progress"))
    , Spec(nameTable->RegisterName("spec"))
    , BriefProgress(nameTable->RegisterName("brief_progress"))
    , BriefSpec(nameTable->RegisterName("brief_spec"))
    , StartTime(nameTable->RegisterName("start_time"))
    , FinishTime(nameTable->RegisterName("finish_time"))
    , FilterFactors(nameTable->RegisterName("filter_factors"))
    , Result(nameTable->RegisterName("result"))
    , Events(nameTable->RegisterName("events"))
    , Alerts(nameTable->RegisterName("alerts"))
    , SlotIndex(nameTable->RegisterName("slot_index"))
    , UnrecognizedSpec(nameTable->RegisterName("unrecognized_spec"))
    , FullSpec(nameTable->RegisterName("full_spec"))
    , RuntimeParameters(nameTable->RegisterName("runtime_parameters"))
    , SlotIndexPerPoolTree(nameTable->RegisterName("slot_index_per_pool_tree"))
    , Annotations(nameTable->RegisterName("annotations"))
{ }

////////////////////////////////////////////////////////////////////////////////

TOrderedByStartTimeTableDescriptor::TOrderedByStartTimeTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TOrderedByStartTimeTableDescriptor::TIndex::TIndex(const TNameTablePtr& nameTable)
    : StartTime(nameTable->RegisterName("start_time"))
    , IdHi(nameTable->RegisterName("id_hi"))
    , IdLo(nameTable->RegisterName("id_lo"))
    , OperationType(nameTable->RegisterName("operation_type"))
    , State(nameTable->RegisterName("state"))
    , AuthenticatedUser(nameTable->RegisterName("authenticated_user"))
    , FilterFactors(nameTable->RegisterName("filter_factors"))
    , Pool(nameTable->RegisterName("pool"))
    , Pools(nameTable->RegisterName("pools"))
    , HasFailedJobs(nameTable->RegisterName("has_failed_jobs"))
    , Acl(nameTable->RegisterName("acl"))
{ }

////////////////////////////////////////////////////////////////////////////////

TJobTableDescriptor::TJobTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TJobTableDescriptor::TIndex::TIndex(const TNameTablePtr& n)
    : OperationIdHi(n->RegisterName("operation_id_hi"))
    , OperationIdLo(n->RegisterName("operation_id_lo"))
    , JobIdHi(n->RegisterName("job_id_hi"))
    , JobIdLo(n->RegisterName("job_id_lo"))
    , Type(n->RegisterName("type"))
    , State(n->RegisterName("state"))
    , TransientState(n->RegisterName("transient_state"))
    , StartTime(n->RegisterName("start_time"))
    , FinishTime(n->RegisterName("finish_time"))
    , UpdateTime(n->RegisterName("update_time"))
    , Address(n->RegisterName("address"))
    , Error(n->RegisterName("error"))
    , Statistics(n->RegisterName("statistics"))
    , Events(n->RegisterName("events"))
    , StderrSize(n->RegisterName("stderr_size"))
    , HasSpec(n->RegisterName("has_spec"))
    , HasFailContext(n->RegisterName("has_fail_context"))
    , FailContextSize(n->RegisterName("fail_context_size"))
    , CoreInfos(n->RegisterName("core_infos"))
{ }

////////////////////////////////////////////////////////////////////////////////

TJobSpecTableDescriptor::TJobSpecTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TJobSpecTableDescriptor::TIndex::TIndex(const NTableClient::TNameTablePtr& n)
    : JobIdHi(n->RegisterName("job_id_hi"))
    , JobIdLo(n->RegisterName("job_id_lo"))
    , Spec(n->RegisterName("spec"))
    , SpecVersion(n->RegisterName("spec_version"))
    , Type(n->RegisterName("type"))
{ }

////////////////////////////////////////////////////////////////////////////////

TJobStderrTableDescriptor::TJobStderrTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TJobStderrTableDescriptor::TIndex::TIndex(const NTableClient::TNameTablePtr& n)
    : OperationIdHi(n->RegisterName("operation_id_hi"))
    , OperationIdLo(n->RegisterName("operation_id_lo"))
    , JobIdHi(n->RegisterName("job_id_hi"))
    , JobIdLo(n->RegisterName("job_id_lo"))
    , Stderr(n->RegisterName("stderr"))
{ }

////////////////////////////////////////////////////////////////////////////////

TJobProfileTableDescriptor::TJobProfileTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TJobProfileTableDescriptor::TIndex::TIndex(const NTableClient::TNameTablePtr& n)
    : OperationIdHi(n->RegisterName("operation_id_hi"))
    , OperationIdLo(n->RegisterName("operation_id_lo"))
    , JobIdHi(n->RegisterName("job_id_hi"))
    , JobIdLo(n->RegisterName("job_id_lo"))
    , PartIndex(n->RegisterName("part_index"))
    , ProfileType(n->RegisterName("profile_type"))
    , ProfileBlob(n->RegisterName("profile_blob"))
{ }

////////////////////////////////////////////////////////////////////////////////

TJobFailContextTableDescriptor::TJobFailContextTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TJobFailContextTableDescriptor::TIndex::TIndex(const NTableClient::TNameTablePtr& n)
    : OperationIdHi(n->RegisterName("operation_id_hi"))
    , OperationIdLo(n->RegisterName("operation_id_lo"))
    , JobIdHi(n->RegisterName("job_id_hi"))
    , JobIdLo(n->RegisterName("job_id_lo"))
    , FailContext(n->RegisterName("fail_context"))
{ }

////////////////////////////////////////////////////////////////////////////////

TOperationAliasesTableDescriptor::TOperationAliasesTableDescriptor()
    : NameTable(New<TNameTable>())
    , Index(NameTable)
{ }

TOperationAliasesTableDescriptor::TIndex::TIndex(const NTableClient::TNameTablePtr& n)
    : AliasHash(n->RegisterName("alias_hash"))
    , Alias(n->RegisterName("alias"))
    , OperationIdHi(n->RegisterName("operation_id_hi"))
    , OperationIdLo(n->RegisterName("operation_id_lo"))
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi
