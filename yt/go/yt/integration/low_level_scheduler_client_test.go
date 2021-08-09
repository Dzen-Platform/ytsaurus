package integration

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yttest"
)

func TestLowLevelSchedulerClient(t *testing.T) {
	suite := NewSuite(t)

	RunClientTests(t, []ClientTest{
		{"StartOperation", suite.TestStartOperation},
		{"AbortOperation", suite.TestAbortOperation},
		{"ResumeOperation", suite.TestResumeOperation},
		{"FailedOperation", suite.TestFailedOperation},
		{"CompleteOperation", suite.TestCompleteOperation},
		{"UpdateOperationParameters", suite.TestUpdateOperationParameters},
		{"ListOperations", suite.TestListOperations},
		{"ListJobs", suite.TestListJobs},
	})
}

func (s *Suite) TestStartOperation(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "cat -",
		},
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateCompleted)
	require.NoError(t, err)

	rows := scanRows(t, s.Env, out)
	require.Len(t, rows, 1)
	require.Equal(t, Row{"a": int64(1)}, rows[0])
}

func (s *Suite) TestAbortOperation(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "sleep 100",
		},
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateRunning)
	require.NoError(t, err)

	tctx, cancel := context.WithTimeout(s.Ctx, time.Second*10)
	defer cancel()

	err = waitOpState(tctx, yc, opID, yt.StateCompleted)
	require.Error(t, err, "operation should not be completed after 10 seconds")

	err = yc.AbortOperation(s.Ctx, opID, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateAborted)
	require.NoError(t, err)
}

func (s *Suite) TestResumeOperation(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "sleep 10 && cat -",
		},
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateRunning)
	require.NoError(t, err)

	err = yc.SuspendOperation(s.Ctx, opID, &yt.SuspendOperationOptions{
		AbortRunningJobs: true,
	})
	require.NoError(t, err)

	time.Sleep(time.Second * 3)

	err = yc.ResumeOperation(s.Ctx, opID, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateCompleted)
	require.NoError(t, err)

	rows := scanRows(t, s.Env, out)
	require.Len(t, rows, 1)
	require.Equal(t, Row{"a": int64(1)}, rows[0])
}

func (s *Suite) TestFailedOperation(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "run_constantly_failing_command",
		},
		"max_failed_job_count": 1,
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateFailed)
	require.NoError(t, err)
}

func (s *Suite) TestCompleteOperation(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "run_constantly_failing_command",
		},
		"max_failed_job_count": 10000,
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateRunning)
	require.NoError(t, err)

	time.Sleep(time.Second * 5)

	status, err := yc.GetOperation(s.Ctx, opID, nil)
	require.NoError(t, err)
	require.Equal(t, yt.StateRunning, status.State)

	err = yc.CompleteOperation(s.Ctx, opID, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateCompleted)
	require.NoError(t, err)
}

func (s *Suite) TestUpdateOperationParameters(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "sleep 100",
		},
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateRunning)
	require.NoError(t, err)

	annotation := guid.New().String()
	err = yc.UpdateOperationParameters(s.Ctx, opID, map[string]interface{}{
		"annotations": map[string]interface{}{
			"test-annotations": annotation,
		},
	}, nil)
	require.NoError(t, err)

	status, err := yc.GetOperation(s.Ctx, opID, nil)
	require.NoError(t, err)
	require.Equal(t, yt.StateRunning, status.State)
	require.Contains(t, status.RuntimeParameters.Annotations, "test-annotations")
	require.Equal(t, annotation, status.RuntimeParameters.Annotations["test-annotations"])

	err = yc.AbortOperation(s.Ctx, opID, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateAborted)
	require.NoError(t, err)
}

func (s *Suite) TestListOperations(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "cat -",
		},
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateCompleted)
	require.NoError(t, err)

	result, err := yc.ListOperations(s.Ctx, nil)
	require.NoError(t, err)

	opIDs := make([]yt.OperationID, 0, len(result.Operations))
	for _, op := range result.Operations {
		opIDs = append(opIDs, op.ID)
	}

	require.Contains(t, opIDs, opID)
}

func (s *Suite) TestListJobs(t *testing.T, yc yt.Client) {
	t.Parallel()

	in := makeTable(t, s.Env, []Row{{"a": int64(1)}})
	out := makeTable(t, s.Env, nil)

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{in},
		"output_table_paths": []ypath.Path{out},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "echo hello >> /dev/stderr",
		},
	}

	opID, err := yc.StartOperation(s.Ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	err = waitOpState(s.Ctx, yc, opID, yt.StateCompleted)
	require.NoError(t, err)

	result, err := yc.ListJobs(s.Ctx, opID, nil)
	require.NoError(t, err)
	require.NotEmpty(t, result.Jobs)
}

type Row map[string]interface{}

func makeTable(t *testing.T, env *yttest.Env, rows []Row) ypath.Path {
	t.Helper()

	p := env.TmpPath()

	_, err := env.YT.CreateNode(env.Ctx, p, yt.NodeTable, nil)
	require.NoError(t, err)

	w, err := env.YT.WriteTable(env.Ctx, p, nil)
	require.NoError(t, err)
	for _, r := range rows {
		require.NoError(t, w.Write(r))
	}
	require.NoError(t, w.Commit())

	return p
}

func scanRows(t *testing.T, env *yttest.Env, p ypath.Path) []Row {
	t.Helper()

	r, err := env.YT.ReadTable(env.Ctx, p, nil)
	require.NoError(t, err)
	defer func() { _ = r.Close() }()

	var rows []Row
	for r.Next() {
		var row Row
		require.NoError(t, r.Scan(&row))
		rows = append(rows, row)
	}

	require.NoError(t, r.Err())

	return rows
}

func waitOpState(ctx context.Context, yc yt.Client, id yt.OperationID, target yt.OperationState) error {
	for {
		time.Sleep(time.Second)

		status, err := yc.GetOperation(ctx, id, nil)
		if err != nil {
			return err
		}

		if status.State == target {
			break
		}
	}

	return nil
}
