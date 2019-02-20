package internal

import (
	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
)

func writeTransactionOptions(w *yson.Writer, o *yt.TransactionOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("transaction_id")
	w.Any(o.TransactionID)
	w.MapKeyString("ping")
	w.Any(o.Ping)
	w.MapKeyString("ping_ancestor_transactions")
	w.Any(o.PingAncestors)
}

func writeAccessTrackingOptions(w *yson.Writer, o *yt.AccessTrackingOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("suppress_access_tracking")
	w.Any(o.SuppressAccessTracking)
	w.MapKeyString("suppress_modification_tracking")
	w.Any(o.SuppressModificationTracking)
}

func writeMutatingOptions(w *yson.Writer, o *yt.MutatingOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("mutation_id")
	w.Any(o.MutationID)
	w.MapKeyString("retry")
	w.Any(o.Retry)
}

func writeReadRetryOptions(w *yson.Writer, o *yt.ReadRetryOptions) {
	if o == nil {
		return
	}
}

func writeMasterReadOptions(w *yson.Writer, o *yt.MasterReadOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("read_from")
	w.Any(o.ReadFrom)
}

func writePrerequisiteOptions(w *yson.Writer, o *yt.PrerequisiteOptions) {
	if o == nil {
		return
	}
	if o.TransactionIDs != nil {
		w.MapKeyString("prerequisite_transaction_ids")
		w.Any(o.TransactionIDs)
	}
	if o.Revisions != nil {
		w.MapKeyString("prerequisite_revisions")
		w.Any(o.Revisions)
	}
}

func writeCreateNodeOptions(w *yson.Writer, o *yt.CreateNodeOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("recursive")
	w.Any(o.Recursive)
	w.MapKeyString("ignore_existing")
	w.Any(o.IgnoreExisting)
	w.MapKeyString("force")
	w.Any(o.Force)
	if o.Attributes != nil {
		w.MapKeyString("attributes")
		w.Any(o.Attributes)
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
	writeMutatingOptions(w, o.MutatingOptions)
}

func writeCreateObjectOptions(w *yson.Writer, o *yt.CreateObjectOptions) {
	if o == nil {
		return
	}
	if o.Attributes != nil {
		w.MapKeyString("attributes")
		w.Any(o.Attributes)
	}
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
	writeMutatingOptions(w, o.MutatingOptions)
}

func writeNodeExistsOptions(w *yson.Writer, o *yt.NodeExistsOptions) {
	if o == nil {
		return
	}
	writeMasterReadOptions(w, o.MasterReadOptions)
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
	writeReadRetryOptions(w, o.ReadRetryOptions)
}

func writeRemoveNodeOptions(w *yson.Writer, o *yt.RemoveNodeOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("recursive")
	w.Any(o.Recursive)
	w.MapKeyString("force")
	w.Any(o.Force)
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
	writeMoveNodeOptions(w, o.MoveNodeOptions)
	writeMutatingOptions(w, o.MutatingOptions)
}

func writeGetNodeOptions(w *yson.Writer, o *yt.GetNodeOptions) {
	if o == nil {
		return
	}
	if o.Attributes != nil {
		w.MapKeyString("attributes")
		w.Any(o.Attributes)
	}
	if o.MaxSize != nil {
		w.MapKeyString("max_size")
		w.Any(o.MaxSize)
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
	writeMasterReadOptions(w, o.MasterReadOptions)
}

func writeSetNodeOptions(w *yson.Writer, o *yt.SetNodeOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("recursive")
	w.Any(o.Recursive)
	w.MapKeyString("force")
	w.Any(o.Force)
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
	writeMutatingOptions(w, o.MutatingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeListNodeOptions(w *yson.Writer, o *yt.ListNodeOptions) {
	if o == nil {
		return
	}
	if o.Attributes != nil {
		w.MapKeyString("attributes")
		w.Any(o.Attributes)
	}
	if o.MaxSize != nil {
		w.MapKeyString("max_size")
		w.Any(o.MaxSize)
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMasterReadOptions(w, o.MasterReadOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeCopyNodeOptions(w *yson.Writer, o *yt.CopyNodeOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("recursive")
	w.Any(o.Recursive)
	w.MapKeyString("ignore_existing")
	w.Any(o.IgnoreExisting)
	w.MapKeyString("force")
	w.Any(o.Force)
	if o.PreserveAccount != nil {
		w.MapKeyString("preserve_account")
		w.Any(o.PreserveAccount)
	}
	if o.PreserveExpirationTime != nil {
		w.MapKeyString("preserve_expiration_time")
		w.Any(o.PreserveExpirationTime)
	}
	if o.PreserveCreationTime != nil {
		w.MapKeyString("preserve_creation_time")
		w.Any(o.PreserveCreationTime)
	}
	if o.PessimisticQuotaCheck != nil {
		w.MapKeyString("pessimistic_quota_check")
		w.Any(o.PessimisticQuotaCheck)
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMutatingOptions(w, o.MutatingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeMoveNodeOptions(w *yson.Writer, o *yt.MoveNodeOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("recursive")
	w.Any(o.Recursive)
	w.MapKeyString("force")
	w.Any(o.Force)
	if o.PreserveAccount != nil {
		w.MapKeyString("preserve_account")
		w.Any(o.PreserveAccount)
	}
	if o.PreserveExpirationTime != nil {
		w.MapKeyString("preserve_expiration_time")
		w.Any(o.PreserveExpirationTime)
	}
	if o.PessimisticQuotaCheck != nil {
		w.MapKeyString("pessimistic_quota_check")
		w.Any(o.PessimisticQuotaCheck)
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMutatingOptions(w, o.MutatingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeLinkNodeOptions(w *yson.Writer, o *yt.LinkNodeOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("recursive")
	w.Any(o.Recursive)
	w.MapKeyString("ignore_existing")
	w.Any(o.IgnoreExisting)
	w.MapKeyString("force")
	w.Any(o.Force)
	if o.Attributes != nil {
		w.MapKeyString("attributes")
		w.Any(o.Attributes)
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMutatingOptions(w, o.MutatingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeStartTxOptions(w *yson.Writer, o *yt.StartTxOptions) {
	if o == nil {
		return
	}
	if o.Timeout != nil {
		w.MapKeyString("timeout")
		w.Any(o.Timeout)
	}
	if o.Deadline != nil {
		w.MapKeyString("deadline")
		w.Any(o.Deadline)
	}
	if o.ParentID != nil {
		w.MapKeyString("parent_id")
		w.Any(o.ParentID)
	}
	w.MapKeyString("ping")
	w.Any(o.Ping)
	w.MapKeyString("ping_ancestor_transactions")
	w.Any(o.PingAncestors)
	w.MapKeyString("sticky")
	w.Any(o.Sticky)
	if o.PrerequisiteTransactionIDs != nil {
		w.MapKeyString("prerequisite_transaction_ids")
		w.Any(o.PrerequisiteTransactionIDs)
	}
	if o.Attributes != nil {
		w.MapKeyString("attributes")
		w.Any(o.Attributes)
	}
	writeMutatingOptions(w, o.MutatingOptions)
}

func writePingTxOptions(w *yson.Writer, o *yt.PingTxOptions) {
	if o == nil {
		return
	}
	writeTransactionOptions(w, o.TransactionOptions)
}

func writeAbortTxOptions(w *yson.Writer, o *yt.AbortTxOptions) {
	if o == nil {
		return
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMutatingOptions(w, o.MutatingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeCommitTxOptions(w *yson.Writer, o *yt.CommitTxOptions) {
	if o == nil {
		return
	}
	writeMutatingOptions(w, o.MutatingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
	writeTransactionOptions(w, o.TransactionOptions)
}

func writeWriteFileOptions(w *yson.Writer, o *yt.WriteFileOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("compute_md5")
	w.Any(o.ComputeMD5)
	w.MapKeyString("file_writer")
	w.Any(o.FileWriter)
	writeTransactionOptions(w, o.TransactionOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeReadFileOptions(w *yson.Writer, o *yt.ReadFileOptions) {
	if o == nil {
		return
	}
	if o.Offset != nil {
		w.MapKeyString("offset")
		w.Any(o.Offset)
	}
	if o.Length != nil {
		w.MapKeyString("length")
		w.Any(o.Length)
	}
	w.MapKeyString("file_reader")
	w.Any(o.FileReader)
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
}

func writePutFileToCacheOptions(w *yson.Writer, o *yt.PutFileToCacheOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("cache_path")
	w.Any(o.CachePath)
	writeMasterReadOptions(w, o.MasterReadOptions)
	writeMutatingOptions(w, o.MutatingOptions)
	writePrerequisiteOptions(w, o.PrerequisiteOptions)
}

func writeGetFileFromCacheOptions(w *yson.Writer, o *yt.GetFileFromCacheOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("cache_path")
	w.Any(o.CachePath)
	writeMasterReadOptions(w, o.MasterReadOptions)
}

func writeWriteTableOptions(w *yson.Writer, o *yt.WriteTableOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("table_writer")
	w.Any(o.TableWriter)
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
}

func writeReadTableOptions(w *yson.Writer, o *yt.ReadTableOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("unordered")
	w.Any(o.Unordered)
	w.MapKeyString("table_reader")
	w.Any(o.TableReader)
	if o.ControlAttributes != nil {
		w.MapKeyString("control_attributes")
		w.Any(o.ControlAttributes)
	}
	if o.StartRowIndexOnly != nil {
		w.MapKeyString("start_row_index_only")
		w.Any(o.StartRowIndexOnly)
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeAccessTrackingOptions(w, o.AccessTrackingOptions)
}

func writeStartOperationOptions(w *yson.Writer, o *yt.StartOperationOptions) {
	if o == nil {
		return
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMutatingOptions(w, o.MutatingOptions)
}

func writeAbortOperationOptions(w *yson.Writer, o *yt.AbortOperationOptions) {
	if o == nil {
		return
	}
	if o.AbortMessage != nil {
		w.MapKeyString("abort_message")
		w.Any(o.AbortMessage)
	}
}

func writeSuspendOperationOptions(w *yson.Writer, o *yt.SuspendOperationOptions) {
	if o == nil {
		return
	}
	w.MapKeyString("abort_running_jobs")
	w.Any(o.AbortRunningJobs)
}

func writeResumeOperationOptions(w *yson.Writer, o *yt.ResumeOperationOptions) {
	if o == nil {
		return
	}
}

func writeCompleteOperationOptions(w *yson.Writer, o *yt.CompleteOperationOptions) {
	if o == nil {
		return
	}
}

func writeUpdateOperationParametersOptions(w *yson.Writer, o *yt.UpdateOperationParametersOptions) {
	if o == nil {
		return
	}
}

func writeListOperationsOptions(w *yson.Writer, o *yt.ListOperationsOptions) {
	if o == nil {
		return
	}
	writeMasterReadOptions(w, o.MasterReadOptions)
}

func writeGetOperationOptions(w *yson.Writer, o *yt.GetOperationOptions) {
	if o == nil {
		return
	}
	if o.Attributes != nil {
		w.MapKeyString("attributes")
		w.Any(o.Attributes)
	}
	if o.IncludeRuntime != nil {
		w.MapKeyString("include_runtime")
		w.Any(o.IncludeRuntime)
	}
	writeMasterReadOptions(w, o.MasterReadOptions)
}

func writeAddMemberOptions(w *yson.Writer, o *yt.AddMemberOptions) {
	if o == nil {
		return
	}
}

func writeRemoveMemberOptions(w *yson.Writer, o *yt.RemoveMemberOptions) {
	if o == nil {
		return
	}
}

func writeLockNodeOptions(w *yson.Writer, o *yt.LockNodeOptions) {
	if o == nil {
		return
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMutatingOptions(w, o.MutatingOptions)
}

func writeUnlockNodeOptions(w *yson.Writer, o *yt.UnlockNodeOptions) {
	if o == nil {
		return
	}
	writeTransactionOptions(w, o.TransactionOptions)
	writeMutatingOptions(w, o.MutatingOptions)
}

type CreateNodeParams struct {
	verb    Verb
	path    ypath.Path
	typ     yt.NodeType
	options *yt.CreateNodeOptions
}

func NewCreateNodeParams(
	path ypath.Path,
	typ yt.NodeType,
	options *yt.CreateNodeOptions,
) *CreateNodeParams {
	if options == nil {
		options = &yt.CreateNodeOptions{}
	}
	return &CreateNodeParams{
		Verb("create"),
		path,
		typ,
		options,
	}
}

func (p *CreateNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *CreateNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
		log.Any("typ", p.typ),
	}
}

func (p *CreateNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	w.MapKeyString("type")
	w.Any(p.typ)
	writeCreateNodeOptions(w, p.options)
}

func (p *CreateNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *CreateNodeParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

func (p *CreateNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

type CreateObjectParams struct {
	verb    Verb
	typ     yt.NodeType
	options *yt.CreateObjectOptions
}

func NewCreateObjectParams(
	typ yt.NodeType,
	options *yt.CreateObjectOptions,
) *CreateObjectParams {
	if options == nil {
		options = &yt.CreateObjectOptions{}
	}
	return &CreateObjectParams{
		Verb("create"),
		typ,
		options,
	}
}

func (p *CreateObjectParams) HTTPVerb() Verb {
	return p.verb
}
func (p *CreateObjectParams) Log() []log.Field {
	return []log.Field{
		log.Any("typ", p.typ),
	}
}

func (p *CreateObjectParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("type")
	w.Any(p.typ)
	writeCreateObjectOptions(w, p.options)
}

func (p *CreateObjectParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

func (p *CreateObjectParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

type NodeExistsParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.NodeExistsOptions
}

func NewNodeExistsParams(
	path ypath.Path,
	options *yt.NodeExistsOptions,
) *NodeExistsParams {
	if options == nil {
		options = &yt.NodeExistsOptions{}
	}
	return &NodeExistsParams{
		Verb("exists"),
		path,
		options,
	}
}

func (p *NodeExistsParams) HTTPVerb() Verb {
	return p.verb
}
func (p *NodeExistsParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *NodeExistsParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeNodeExistsOptions(w, p.options)
}

func (p *NodeExistsParams) MasterReadOptions() **yt.MasterReadOptions {
	return &p.options.MasterReadOptions
}

func (p *NodeExistsParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *NodeExistsParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

func (p *NodeExistsParams) ReadRetryOptions() **yt.ReadRetryOptions {
	return &p.options.ReadRetryOptions
}

type RemoveNodeParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.RemoveNodeOptions
}

func NewRemoveNodeParams(
	path ypath.Path,
	options *yt.RemoveNodeOptions,
) *RemoveNodeParams {
	if options == nil {
		options = &yt.RemoveNodeOptions{}
	}
	return &RemoveNodeParams{
		Verb("remove"),
		path,
		options,
	}
}

func (p *RemoveNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *RemoveNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *RemoveNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeRemoveNodeOptions(w, p.options)
}

func (p *RemoveNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *RemoveNodeParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

func (p *RemoveNodeParams) MoveNodeOptions() **yt.MoveNodeOptions {
	return &p.options.MoveNodeOptions
}

func (p *RemoveNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

type GetNodeParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.GetNodeOptions
}

func NewGetNodeParams(
	path ypath.Path,
	options *yt.GetNodeOptions,
) *GetNodeParams {
	if options == nil {
		options = &yt.GetNodeOptions{}
	}
	return &GetNodeParams{
		Verb("get"),
		path,
		options,
	}
}

func (p *GetNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *GetNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *GetNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeGetNodeOptions(w, p.options)
}

func (p *GetNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *GetNodeParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

func (p *GetNodeParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

func (p *GetNodeParams) MasterReadOptions() **yt.MasterReadOptions {
	return &p.options.MasterReadOptions
}

type SetNodeParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.SetNodeOptions
}

func NewSetNodeParams(
	path ypath.Path,
	options *yt.SetNodeOptions,
) *SetNodeParams {
	if options == nil {
		options = &yt.SetNodeOptions{}
	}
	return &SetNodeParams{
		Verb("set"),
		path,
		options,
	}
}

func (p *SetNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *SetNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *SetNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeSetNodeOptions(w, p.options)
}

func (p *SetNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *SetNodeParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

func (p *SetNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

func (p *SetNodeParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type ListNodeParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.ListNodeOptions
}

func NewListNodeParams(
	path ypath.Path,
	options *yt.ListNodeOptions,
) *ListNodeParams {
	if options == nil {
		options = &yt.ListNodeOptions{}
	}
	return &ListNodeParams{
		Verb("list"),
		path,
		options,
	}
}

func (p *ListNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *ListNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *ListNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeListNodeOptions(w, p.options)
}

func (p *ListNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *ListNodeParams) MasterReadOptions() **yt.MasterReadOptions {
	return &p.options.MasterReadOptions
}

func (p *ListNodeParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

func (p *ListNodeParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type CopyNodeParams struct {
	verb    Verb
	src     ypath.Path
	dst     ypath.Path
	options *yt.CopyNodeOptions
}

func NewCopyNodeParams(
	src ypath.Path,
	dst ypath.Path,
	options *yt.CopyNodeOptions,
) *CopyNodeParams {
	if options == nil {
		options = &yt.CopyNodeOptions{}
	}
	return &CopyNodeParams{
		Verb("copy"),
		src,
		dst,
		options,
	}
}

func (p *CopyNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *CopyNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("src", p.src),
		log.Any("dst", p.dst),
	}
}

func (p *CopyNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("source_path")
	w.Any(p.src)
	w.MapKeyString("destination_path")
	w.Any(p.dst)
	writeCopyNodeOptions(w, p.options)
}

func (p *CopyNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *CopyNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

func (p *CopyNodeParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type MoveNodeParams struct {
	verb    Verb
	src     ypath.Path
	dst     ypath.Path
	options *yt.MoveNodeOptions
}

func NewMoveNodeParams(
	src ypath.Path,
	dst ypath.Path,
	options *yt.MoveNodeOptions,
) *MoveNodeParams {
	if options == nil {
		options = &yt.MoveNodeOptions{}
	}
	return &MoveNodeParams{
		Verb("move"),
		src,
		dst,
		options,
	}
}

func (p *MoveNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *MoveNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("src", p.src),
		log.Any("dst", p.dst),
	}
}

func (p *MoveNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("source_path")
	w.Any(p.src)
	w.MapKeyString("destination_path")
	w.Any(p.dst)
	writeMoveNodeOptions(w, p.options)
}

func (p *MoveNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *MoveNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

func (p *MoveNodeParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type LinkNodeParams struct {
	verb    Verb
	target  ypath.Path
	link    ypath.Path
	options *yt.LinkNodeOptions
}

func NewLinkNodeParams(
	target ypath.Path,
	link ypath.Path,
	options *yt.LinkNodeOptions,
) *LinkNodeParams {
	if options == nil {
		options = &yt.LinkNodeOptions{}
	}
	return &LinkNodeParams{
		Verb("link"),
		target,
		link,
		options,
	}
}

func (p *LinkNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *LinkNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("target", p.target),
		log.Any("link", p.link),
	}
}

func (p *LinkNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("target_path")
	w.Any(p.target)
	w.MapKeyString("link_path")
	w.Any(p.link)
	writeLinkNodeOptions(w, p.options)
}

func (p *LinkNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *LinkNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

func (p *LinkNodeParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type StartTxParams struct {
	verb    Verb
	options *yt.StartTxOptions
}

func NewStartTxParams(
	options *yt.StartTxOptions,
) *StartTxParams {
	if options == nil {
		options = &yt.StartTxOptions{}
	}
	return &StartTxParams{
		Verb("start_transaction"),
		options,
	}
}

func (p *StartTxParams) HTTPVerb() Verb {
	return p.verb
}
func (p *StartTxParams) Log() []log.Field {
	return []log.Field{}
}

func (p *StartTxParams) MarshalHTTP(w *yson.Writer) {
	writeStartTxOptions(w, p.options)
}

func (p *StartTxParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

type PingTxParams struct {
	verb    Verb
	id      yt.TxID
	options *yt.PingTxOptions
}

func NewPingTxParams(
	id yt.TxID,
	options *yt.PingTxOptions,
) *PingTxParams {
	if options == nil {
		options = &yt.PingTxOptions{}
	}
	return &PingTxParams{
		Verb("ping_transaction"),
		id,
		options,
	}
}

func (p *PingTxParams) HTTPVerb() Verb {
	return p.verb
}
func (p *PingTxParams) Log() []log.Field {
	return []log.Field{
		log.Any("id", p.id),
	}
}

func (p *PingTxParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("transaction_id")
	w.Any(p.id)
	writePingTxOptions(w, p.options)
}

func (p *PingTxParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

type AbortTxParams struct {
	verb    Verb
	id      yt.TxID
	options *yt.AbortTxOptions
}

func NewAbortTxParams(
	id yt.TxID,
	options *yt.AbortTxOptions,
) *AbortTxParams {
	if options == nil {
		options = &yt.AbortTxOptions{}
	}
	return &AbortTxParams{
		Verb("abort_transaction"),
		id,
		options,
	}
}

func (p *AbortTxParams) HTTPVerb() Verb {
	return p.verb
}
func (p *AbortTxParams) Log() []log.Field {
	return []log.Field{
		log.Any("id", p.id),
	}
}

func (p *AbortTxParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("transaction_id")
	w.Any(p.id)
	writeAbortTxOptions(w, p.options)
}

func (p *AbortTxParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *AbortTxParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

func (p *AbortTxParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type CommitTxParams struct {
	verb    Verb
	id      yt.TxID
	options *yt.CommitTxOptions
}

func NewCommitTxParams(
	id yt.TxID,
	options *yt.CommitTxOptions,
) *CommitTxParams {
	if options == nil {
		options = &yt.CommitTxOptions{}
	}
	return &CommitTxParams{
		Verb("commit_transaction"),
		id,
		options,
	}
}

func (p *CommitTxParams) HTTPVerb() Verb {
	return p.verb
}
func (p *CommitTxParams) Log() []log.Field {
	return []log.Field{
		log.Any("id", p.id),
	}
}

func (p *CommitTxParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("transaction_id")
	w.Any(p.id)
	writeCommitTxOptions(w, p.options)
}

func (p *CommitTxParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

func (p *CommitTxParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

func (p *CommitTxParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

type WriteFileParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.WriteFileOptions
}

func NewWriteFileParams(
	path ypath.Path,
	options *yt.WriteFileOptions,
) *WriteFileParams {
	if options == nil {
		options = &yt.WriteFileOptions{}
	}
	return &WriteFileParams{
		Verb("write_file"),
		path,
		options,
	}
}

func (p *WriteFileParams) HTTPVerb() Verb {
	return p.verb
}
func (p *WriteFileParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *WriteFileParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeWriteFileOptions(w, p.options)
}

func (p *WriteFileParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *WriteFileParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type ReadFileParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.ReadFileOptions
}

func NewReadFileParams(
	path ypath.Path,
	options *yt.ReadFileOptions,
) *ReadFileParams {
	if options == nil {
		options = &yt.ReadFileOptions{}
	}
	return &ReadFileParams{
		Verb("read_file"),
		path,
		options,
	}
}

func (p *ReadFileParams) HTTPVerb() Verb {
	return p.verb
}
func (p *ReadFileParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *ReadFileParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeReadFileOptions(w, p.options)
}

func (p *ReadFileParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *ReadFileParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

type PutFileToCacheParams struct {
	verb    Verb
	path    ypath.Path
	md5     string
	options *yt.PutFileToCacheOptions
}

func NewPutFileToCacheParams(
	path ypath.Path,
	md5 string,
	options *yt.PutFileToCacheOptions,
) *PutFileToCacheParams {
	if options == nil {
		options = &yt.PutFileToCacheOptions{}
	}
	return &PutFileToCacheParams{
		Verb("put_file_to_cache"),
		path,
		md5,
		options,
	}
}

func (p *PutFileToCacheParams) HTTPVerb() Verb {
	return p.verb
}
func (p *PutFileToCacheParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
		log.Any("md5", p.md5),
	}
}

func (p *PutFileToCacheParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	w.MapKeyString("md5")
	w.Any(p.md5)
	writePutFileToCacheOptions(w, p.options)
}

func (p *PutFileToCacheParams) MasterReadOptions() **yt.MasterReadOptions {
	return &p.options.MasterReadOptions
}

func (p *PutFileToCacheParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

func (p *PutFileToCacheParams) PrerequisiteOptions() **yt.PrerequisiteOptions {
	return &p.options.PrerequisiteOptions
}

type GetFileFromCacheParams struct {
	verb    Verb
	md5     string
	options *yt.GetFileFromCacheOptions
}

func NewGetFileFromCacheParams(
	md5 string,
	options *yt.GetFileFromCacheOptions,
) *GetFileFromCacheParams {
	if options == nil {
		options = &yt.GetFileFromCacheOptions{}
	}
	return &GetFileFromCacheParams{
		Verb("get_file_from_cache"),
		md5,
		options,
	}
}

func (p *GetFileFromCacheParams) HTTPVerb() Verb {
	return p.verb
}
func (p *GetFileFromCacheParams) Log() []log.Field {
	return []log.Field{
		log.Any("md5", p.md5),
	}
}

func (p *GetFileFromCacheParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("md5")
	w.Any(p.md5)
	writeGetFileFromCacheOptions(w, p.options)
}

func (p *GetFileFromCacheParams) MasterReadOptions() **yt.MasterReadOptions {
	return &p.options.MasterReadOptions
}

type WriteTableParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.WriteTableOptions
}

func NewWriteTableParams(
	path ypath.Path,
	options *yt.WriteTableOptions,
) *WriteTableParams {
	if options == nil {
		options = &yt.WriteTableOptions{}
	}
	return &WriteTableParams{
		Verb("write_table"),
		path,
		options,
	}
}

func (p *WriteTableParams) HTTPVerb() Verb {
	return p.verb
}
func (p *WriteTableParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *WriteTableParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeWriteTableOptions(w, p.options)
}

func (p *WriteTableParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *WriteTableParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

type ReadTableParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.ReadTableOptions
}

func NewReadTableParams(
	path ypath.Path,
	options *yt.ReadTableOptions,
) *ReadTableParams {
	if options == nil {
		options = &yt.ReadTableOptions{}
	}
	return &ReadTableParams{
		Verb("read_table"),
		path,
		options,
	}
}

func (p *ReadTableParams) HTTPVerb() Verb {
	return p.verb
}
func (p *ReadTableParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *ReadTableParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeReadTableOptions(w, p.options)
}

func (p *ReadTableParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *ReadTableParams) AccessTrackingOptions() **yt.AccessTrackingOptions {
	return &p.options.AccessTrackingOptions
}

type StartOperationParams struct {
	verb    Verb
	opType  yt.OperationType
	spec    interface{}
	options *yt.StartOperationOptions
}

func NewStartOperationParams(
	opType yt.OperationType,
	spec interface{},
	options *yt.StartOperationOptions,
) *StartOperationParams {
	if options == nil {
		options = &yt.StartOperationOptions{}
	}
	return &StartOperationParams{
		Verb("start_operation"),
		opType,
		spec,
		options,
	}
}

func (p *StartOperationParams) HTTPVerb() Verb {
	return p.verb
}
func (p *StartOperationParams) Log() []log.Field {
	return []log.Field{
		log.Any("opType", p.opType),
	}
}

func (p *StartOperationParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("operation_type")
	w.Any(p.opType)
	w.MapKeyString("spec")
	w.Any(p.spec)
	writeStartOperationOptions(w, p.options)
}

func (p *StartOperationParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *StartOperationParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

type AbortOperationParams struct {
	verb    Verb
	opID    yt.OperationID
	options *yt.AbortOperationOptions
}

func NewAbortOperationParams(
	opID yt.OperationID,
	options *yt.AbortOperationOptions,
) *AbortOperationParams {
	if options == nil {
		options = &yt.AbortOperationOptions{}
	}
	return &AbortOperationParams{
		Verb("abort_operation"),
		opID,
		options,
	}
}

func (p *AbortOperationParams) HTTPVerb() Verb {
	return p.verb
}
func (p *AbortOperationParams) Log() []log.Field {
	return []log.Field{
		log.Any("opID", p.opID),
	}
}

func (p *AbortOperationParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("operation_id")
	w.Any(p.opID)
	writeAbortOperationOptions(w, p.options)
}

type SuspendOperationParams struct {
	verb    Verb
	opID    yt.OperationID
	options *yt.SuspendOperationOptions
}

func NewSuspendOperationParams(
	opID yt.OperationID,
	options *yt.SuspendOperationOptions,
) *SuspendOperationParams {
	if options == nil {
		options = &yt.SuspendOperationOptions{}
	}
	return &SuspendOperationParams{
		Verb("suspend_operation"),
		opID,
		options,
	}
}

func (p *SuspendOperationParams) HTTPVerb() Verb {
	return p.verb
}
func (p *SuspendOperationParams) Log() []log.Field {
	return []log.Field{
		log.Any("opID", p.opID),
	}
}

func (p *SuspendOperationParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("operation_id")
	w.Any(p.opID)
	writeSuspendOperationOptions(w, p.options)
}

type ResumeOperationParams struct {
	verb    Verb
	opID    yt.OperationID
	options *yt.ResumeOperationOptions
}

func NewResumeOperationParams(
	opID yt.OperationID,
	options *yt.ResumeOperationOptions,
) *ResumeOperationParams {
	if options == nil {
		options = &yt.ResumeOperationOptions{}
	}
	return &ResumeOperationParams{
		Verb("resume_operation"),
		opID,
		options,
	}
}

func (p *ResumeOperationParams) HTTPVerb() Verb {
	return p.verb
}
func (p *ResumeOperationParams) Log() []log.Field {
	return []log.Field{
		log.Any("opID", p.opID),
	}
}

func (p *ResumeOperationParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("operation_id")
	w.Any(p.opID)
	writeResumeOperationOptions(w, p.options)
}

type CompleteOperationParams struct {
	verb    Verb
	opID    yt.OperationID
	options *yt.CompleteOperationOptions
}

func NewCompleteOperationParams(
	opID yt.OperationID,
	options *yt.CompleteOperationOptions,
) *CompleteOperationParams {
	if options == nil {
		options = &yt.CompleteOperationOptions{}
	}
	return &CompleteOperationParams{
		Verb("complete_operation"),
		opID,
		options,
	}
}

func (p *CompleteOperationParams) HTTPVerb() Verb {
	return p.verb
}
func (p *CompleteOperationParams) Log() []log.Field {
	return []log.Field{
		log.Any("opID", p.opID),
	}
}

func (p *CompleteOperationParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("operation_id")
	w.Any(p.opID)
	writeCompleteOperationOptions(w, p.options)
}

type UpdateOperationParametersParams struct {
	verb    Verb
	opID    yt.OperationID
	params  interface{}
	options *yt.UpdateOperationParametersOptions
}

func NewUpdateOperationParametersParams(
	opID yt.OperationID,
	params interface{},
	options *yt.UpdateOperationParametersOptions,
) *UpdateOperationParametersParams {
	if options == nil {
		options = &yt.UpdateOperationParametersOptions{}
	}
	return &UpdateOperationParametersParams{
		Verb("update_operation_parameters"),
		opID,
		params,
		options,
	}
}

func (p *UpdateOperationParametersParams) HTTPVerb() Verb {
	return p.verb
}
func (p *UpdateOperationParametersParams) Log() []log.Field {
	return []log.Field{
		log.Any("opID", p.opID),
		log.Any("params", p.params),
	}
}

func (p *UpdateOperationParametersParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("operation_id")
	w.Any(p.opID)
	w.MapKeyString("parameters")
	w.Any(p.params)
	writeUpdateOperationParametersOptions(w, p.options)
}

type GetOperationParams struct {
	verb    Verb
	opID    yt.OperationID
	options *yt.GetOperationOptions
}

func NewGetOperationParams(
	opID yt.OperationID,
	options *yt.GetOperationOptions,
) *GetOperationParams {
	if options == nil {
		options = &yt.GetOperationOptions{}
	}
	return &GetOperationParams{
		Verb("get_operation"),
		opID,
		options,
	}
}

func (p *GetOperationParams) HTTPVerb() Verb {
	return p.verb
}
func (p *GetOperationParams) Log() []log.Field {
	return []log.Field{
		log.Any("opID", p.opID),
	}
}

func (p *GetOperationParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("operation_id")
	w.Any(p.opID)
	writeGetOperationOptions(w, p.options)
}

func (p *GetOperationParams) MasterReadOptions() **yt.MasterReadOptions {
	return &p.options.MasterReadOptions
}

type AddMemberParams struct {
	verb    Verb
	group   string
	member  string
	options *yt.AddMemberOptions
}

func NewAddMemberParams(
	group string,
	member string,
	options *yt.AddMemberOptions,
) *AddMemberParams {
	if options == nil {
		options = &yt.AddMemberOptions{}
	}
	return &AddMemberParams{
		Verb("add_member"),
		group,
		member,
		options,
	}
}

func (p *AddMemberParams) HTTPVerb() Verb {
	return p.verb
}
func (p *AddMemberParams) Log() []log.Field {
	return []log.Field{
		log.Any("group", p.group),
		log.Any("member", p.member),
	}
}

func (p *AddMemberParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("group")
	w.Any(p.group)
	w.MapKeyString("member")
	w.Any(p.member)
	writeAddMemberOptions(w, p.options)
}

type RemoveMemberParams struct {
	verb    Verb
	group   string
	member  string
	options *yt.RemoveMemberOptions
}

func NewRemoveMemberParams(
	group string,
	member string,
	options *yt.RemoveMemberOptions,
) *RemoveMemberParams {
	if options == nil {
		options = &yt.RemoveMemberOptions{}
	}
	return &RemoveMemberParams{
		Verb("remove_member"),
		group,
		member,
		options,
	}
}

func (p *RemoveMemberParams) HTTPVerb() Verb {
	return p.verb
}
func (p *RemoveMemberParams) Log() []log.Field {
	return []log.Field{
		log.Any("group", p.group),
		log.Any("member", p.member),
	}
}

func (p *RemoveMemberParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("group")
	w.Any(p.group)
	w.MapKeyString("member")
	w.Any(p.member)
	writeRemoveMemberOptions(w, p.options)
}

type LockNodeParams struct {
	verb    Verb
	path    ypath.Path
	mode    yt.LockMode
	options *yt.LockNodeOptions
}

func NewLockNodeParams(
	path ypath.Path,
	mode yt.LockMode,
	options *yt.LockNodeOptions,
) *LockNodeParams {
	if options == nil {
		options = &yt.LockNodeOptions{}
	}
	return &LockNodeParams{
		Verb("lock"),
		path,
		mode,
		options,
	}
}

func (p *LockNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *LockNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
		log.Any("mode", p.mode),
	}
}

func (p *LockNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	w.MapKeyString("mode")
	w.Any(p.mode)
	writeLockNodeOptions(w, p.options)
}

func (p *LockNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *LockNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}

type UnlockNodeParams struct {
	verb    Verb
	path    ypath.Path
	options *yt.UnlockNodeOptions
}

func NewUnlockNodeParams(
	path ypath.Path,
	options *yt.UnlockNodeOptions,
) *UnlockNodeParams {
	if options == nil {
		options = &yt.UnlockNodeOptions{}
	}
	return &UnlockNodeParams{
		Verb("unlock"),
		path,
		options,
	}
}

func (p *UnlockNodeParams) HTTPVerb() Verb {
	return p.verb
}
func (p *UnlockNodeParams) Log() []log.Field {
	return []log.Field{
		log.Any("path", p.path),
	}
}

func (p *UnlockNodeParams) MarshalHTTP(w *yson.Writer) {
	w.MapKeyString("path")
	w.Any(p.path)
	writeUnlockNodeOptions(w, p.options)
}

func (p *UnlockNodeParams) TransactionOptions() **yt.TransactionOptions {
	return &p.options.TransactionOptions
}

func (p *UnlockNodeParams) MutatingOptions() **yt.MutatingOptions {
	return &p.options.MutatingOptions
}
