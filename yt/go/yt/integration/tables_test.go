package integration

import (
	"context"
	"testing"
	"time"

	"a.yandex-team.ru/yt/go/schema"
	"a.yandex-team.ru/yt/go/ypath"

	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yttest"
	"github.com/stretchr/testify/require"
)

type exampleRow struct {
	A string `yt:"a"`
	B int    `yt:"b"`
}

func TestTables(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*15)
	defer cancel()

	validate := func(name ypath.Path) {
		w, err := env.YT.WriteTable(ctx, name, nil)
		require.NoError(t, err)

		require.NoError(t, w.Write(exampleRow{"foo", 1}))
		require.NoError(t, w.Write(exampleRow{"bar", 2}))
		require.NoError(t, w.Close())

		r, err := env.YT.ReadTable(ctx, name, nil)
		require.NoError(t, err)
		defer func() { _ = r.Close() }()

		var s exampleRow
		require.True(t, r.Next())
		require.NoError(t, r.Scan(&s))
		require.Equal(t, exampleRow{"foo", 1}, s)

		require.True(t, r.Next())
		require.NoError(t, r.Scan(&s))
		require.Equal(t, exampleRow{"bar", 2}, s)

		require.False(t, r.Next())
		require.NoError(t, r.Err())
	}

	t.Run("WriteReadTable", func(t *testing.T) {
		name := tmpPath()

		_, err := env.YT.CreateNode(ctx, name, yt.NodeTable, nil)
		require.NoError(t, err)

		validate(name)
	})

	t.Run("WriteWithSchema", func(t *testing.T) {
		name := tmpPath()

		_, err := env.YT.CreateNode(ctx, name, yt.NodeTable, &yt.CreateNodeOptions{
			Attributes: map[string]interface{}{
				"schema": schema.MustInfer(&exampleRow{}),
			},
		})
		require.NoError(t, err)

		validate(name)
	})

	t.Run("AppendToTable", func(t *testing.T) {
		name := tmpPath()

		_, err := env.YT.CreateNode(ctx, name, yt.NodeTable, nil)
		require.NoError(t, err)

		write := func() {
			appendAttr := true
			w, err := env.YT.WriteTable(ctx, ypath.Rich{Path: name, Append: &appendAttr}, nil)
			require.NoError(t, err)

			require.NoError(t, w.Write(exampleRow{"foo", 1}))
			require.NoError(t, w.Write(exampleRow{"bar", 2}))
			require.NoError(t, w.Close())
		}

		write()

		write()

		var rowCount int
		require.NoError(t, env.YT.GetNode(env.Ctx, name.Attr("row_count"), &rowCount, nil))
		require.Equal(t, 4, rowCount)
	})

	t.Run("ReadRanges", func(t *testing.T) {
		name := tmpPath()

		err := env.UploadSlice(name, []exampleRow{
			{"foo", 1},
			{"bar", 2},
		})
		require.NoError(t, err)

		richPath := ypath.NewRich(name).
			AddRange(ypath.Exact(ypath.RowIndex(1))).
			AddRange(ypath.Exact(ypath.RowIndex(0)))

		var result []exampleRow
		err = env.DownloadSlice(richPath, &result)
		require.NoError(t, err)

		require.Equal(t, []exampleRow{
			{"bar", 2},
			{"foo", 1},
		}, result)
	})
}
