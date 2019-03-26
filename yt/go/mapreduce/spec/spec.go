// package spec defines specification of YT operation.
//
// See https://wiki.yandex-team.ru/yt/userdoc/operations/
package spec

import (
	"github.com/mitchellh/copystructure"

	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
)

type File struct {
	FileName   string      `yson:"file_name,attr,omitempty"`
	Format     interface{} `yson:"format,attr,omitempty"`
	Executable bool        `yson:"executable,attr,omitempty"`

	CypressPath ypath.Path `yson:",value"`
}

type UserScript struct {
	Command     string            `yson:"command"`
	Format      interface{}       `yson:"format,omitempty"`
	Environment map[string]string `yson:"environment,omitempty"`
	FilePaths   []File            `yson:"file_paths,omitempty"`

	CpuLimit    int   `yson:"cpu_limit,omitempty"`
	MemoryLimit int64 `yson:"memory_limit,omitempty"`

	// JobCount is used only in vanilla operations
	JobCount int `yson:"job_count,omitempty"`
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
	MapperOutputTableCount int           `yson:"mapper_output_table_count"`

	Ordered   bool            `yson:"ordered,omitempty"`
	ReduceBy  []string        `yson:"reduce_by,omitempty"`
	SortBy    []string        `yson:"sort_by,omitempty"`
	PivotKeys [][]interface{} `yson:"pivot_keys,omitempty"`

	MergeMode      string   `yson:"merge_mode,omitempty"`
	MergeBy        []string `yson:"merge_by,omitempty"`
	CombineChunks  bool     `yson:"combine_chunks,omitempty"`
	ForceTransform bool     `yson:"force_transform,omitempty"`

	JobCount       int   `yson:"job_count,omitempty"`
	DataSizePerJob int64 `yson:"data_size_per_job,omitempty"`

	Mapper         *UserScript            `yson:"mapper,omitempty"`
	Reducer        *UserScript            `yson:"reducer,omitempty"`
	ReduceCombiner *UserScript            `yson:"reduce_combiner,omitempty"`
	Tasks          map[string]*UserScript `yson:"tasks,omitempty"`
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

func (s *Spec) AddSecureVaultVar(name, value string) *Spec {
	if s.SecureVault == nil {
		s.SecureVault = map[string]string{}
	}

	s.SecureVault[name] = value
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
