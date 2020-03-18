// package spec defines specification of YT operation.
//
// See https://wiki.yandex-team.ru/yt/userdoc/operations/
package spec

import (
	"github.com/mitchellh/copystructure"

	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
)

const EnvAllowRequests = "YT_ALLOW_REQUESTS_FROM_JOBS"

type File struct {
	FileName            string      `yson:"file_name,attr,omitempty"`
	Format              interface{} `yson:"format,attr,omitempty"`
	Executable          bool        `yson:"executable,attr,omitempty"`
	BypassArtifactCache bool        `yson:"bypass_artifact_cache"`

	CypressPath ypath.Path `yson:",value"`
}

type UserScript struct {
	Command         string            `yson:"command"`
	Format          interface{}       `yson:"format,omitempty"`
	InputFormat     interface{}       `yson:"input_format,omitempty"`
	OutputFormat    interface{}       `yson:"output_format,omitempty"`
	Environment     map[string]string `yson:"environment,omitempty"`
	FilePaths       []File            `yson:"file_paths,omitempty"`
	LayerPaths      []ypath.YPath     `yson:"layer_paths,omitempty"`
	StderrTablePath ypath.YPath       `yson:"stderr_table_path,omitempty"`
	CoreTablePath   ypath.YPath       `yson:"core_table_path,omitempty"`

	TmpfsPath string `yson:"tmpfs_path,omitempty"`
	CopyFiles bool   `yson:"copy_files,omitempty"`

	// CPULimit corresponds to cpu_limit job setting.
	//
	// This setting results in GOMAXPROCS set to max(1, ceil(CPULimit)).
	CPULimit         float32 `yson:"cpu_limit,omitempty"`
	EnableCPUReclaim *bool   `yson:"enable_cpu_reclaim,omitempty"`
	MemoryLimit      int64   `yson:"memory_limit,omitempty"`

	// JobCount and OutputTablePaths are used only in vanilla operations.
	JobCount         int           `yson:"job_count,omitempty"`
	OutputTablePaths []ypath.YPath `yson:"output_table_paths,omitempty"`
}

type ControlAttributes struct {
	EnableTableIndex bool `yson:"enable_table_index"`
	EnableRowIndex   bool `yson:"enable_row_index"`
	EnableRangeIndex bool `yson:"enable_range_index"`
	EnableKeySwitch  bool `yson:"enable_key_switch"`
}

type JobIO struct {
	TableReader       interface{}        `yson:"table_reader,omitempty"`
	TableWriter       interface{}        `yson:"table_writer,omitempty"`
	ControlAttributes *ControlAttributes `yson:"control_attributes,omitempty"`
}

const (
	AutoMergeRelaxed  = "relaxed"
	AutoMergeEconomic = "economic"
	AutoMergeDisabled = "disabled"
	AutoMergeManual   = "manual"
)

type AutoMerge struct {
	Mode string `yson:"mode"`

	// Setting for manual mode.
	MaxIntermediateChunkCount int `yson:"max_intermediate_chunk_count,omitempty"`
	ChunkCountPerMergeJob     int `yson:"chunk_count_per_merge_job,omitempty"`
	ChunkSizeThreshold        int `yson:"chunk_size_threshold,omitempty"`
}

type Spec struct {
	Type yt.OperationType `yson:"-"`

	Title       string                 `yson:"title,omitempty"`
	StartedBy   map[string]interface{} `yson:"started_by,omitempty"`
	Annotations map[string]interface{} `yson:"annotations"`
	Description map[string]interface{} `yson:"description,omitempty"`

	Pool      string   `yson:"pool,omitempty"`
	Weight    float64  `yson:"weight,omitempty"`
	PoolTrees []string `yson:"pool_trees,omitempty"`

	TentativePoolTrees []string `yson:"tentative_pool_trees,omitempty"`

	SecureVault map[string]string `yson:"secure_vault,omitempty"`

	InputTablePaths        []ypath.YPath `yson:"input_table_paths,omitempty"`
	OutputTablePaths       []ypath.YPath `yson:"output_table_paths,omitempty"`
	OutputTablePath        ypath.YPath   `yson:"output_table_path,omitempty"`
	MapperOutputTableCount int           `yson:"mapper_output_table_count,omitempty"`

	Ordered   bool            `yson:"ordered,omitempty"`
	ReduceBy  []string        `yson:"reduce_by,omitempty"`
	SortBy    []string        `yson:"sort_by,omitempty"`
	PivotKeys [][]interface{} `yson:"pivot_keys,omitempty"`

	MergeMode      string   `yson:"mode,omitempty"`
	MergeBy        []string `yson:"merge_by,omitempty"`
	CombineChunks  bool     `yson:"combine_chunks,omitempty"`
	ForceTransform bool     `yson:"force_transform,omitempty"`

	JobCount       int   `yson:"job_count,omitempty"`
	DataSizePerJob int64 `yson:"data_size_per_job,omitempty"`

	MaxFailedJobCount int `yson:"max_failed_job_count,omitempty"`

	Mapper         *UserScript            `yson:"mapper,omitempty"`
	Reducer        *UserScript            `yson:"reducer,omitempty"`
	ReduceCombiner *UserScript            `yson:"reduce_combiner,omitempty"`
	Tasks          map[string]*UserScript `yson:"tasks,omitempty"`

	JobIO          *JobIO `yson:"job_io,omitempty"`
	MapJobIO       *JobIO `yson:"map_job_io,omitempty"`
	ReduceJobIO    *JobIO `yson:"reduce_job_io,omitempty"`
	PartitionJobIO *JobIO `yson:"partition_job_io,omitempty"`
	MergeJobIO     *JobIO `yson:"merge_job_io,omitempty"`
	SortJobIO      *JobIO `yson:"sort_job_io,omitempty"`

	AutoMerge *AutoMerge `yson:"auto_merge,omitempty"`

	ACL []yt.ACE `yson:"acl,omitempty"`
}

func (s *Spec) ReduceByColumns(columns ...string) *Spec {
	s.ReduceBy = columns
	return s
}

func (s *Spec) SortByColumns(columns ...string) *Spec {
	s.SortBy = columns
	return s
}

func (s *Spec) AddInput(path ypath.YPath) *Spec {
	s.InputTablePaths = append(s.InputTablePaths, path)
	return s
}

func (s *Spec) AddOutput(path ypath.YPath) *Spec {
	s.OutputTablePaths = append(s.OutputTablePaths, path)
	return s
}

func (s *Spec) SetOutput(path ypath.YPath) *Spec {
	s.OutputTablePath = path
	return s
}

func (s *Spec) AddSecureVaultVar(name, value string) *Spec {
	if s.SecureVault == nil {
		s.SecureVault = map[string]string{}
	}

	s.SecureVault[name] = value
	return s
}

func (s *Spec) AllowRequestsFromJob() *Spec {
	s.VisitUserScripts(func(script *UserScript) {
		if script.Environment == nil {
			script.Environment = map[string]string{}
		}

		script.Environment[EnvAllowRequests] = "1"
	})

	return s
}

func (s *Spec) Clone() *Spec {
	return copystructure.Must(copystructure.Copy(s)).(*Spec)
}

func (s *Spec) VisitUserScripts(cb func(*UserScript)) {
	if s.Mapper != nil {
		cb(s.Mapper)
	}

	if s.Reducer != nil {
		cb(s.Reducer)
	}

	if s.ReduceCombiner != nil {
		cb(s.ReduceCombiner)
	}

	for _, t := range s.Tasks {
		cb(t)
	}
}

func (s *Spec) PatchUserBinary(path ypath.Path) {
	s.VisitUserScripts(func(u *UserScript) {
		u.FilePaths = append(u.FilePaths, File{
			FileName:    "go-binary",
			CypressPath: path,
			Executable:  true,
		})
	})
}
