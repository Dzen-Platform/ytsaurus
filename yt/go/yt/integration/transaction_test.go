package integration

import (
	"context"
	"testing"
	"time"

	"a.yandex-team.ru/yt/go/yttest"

	"a.yandex-team.ru/yt/go/yt"

	"github.com/stretchr/testify/require"
)

func TestTransactions(t *testing.T) {
	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*15)
	defer cancel()

	t.Run("CommitTransaction", func(t *testing.T) {
		tx, err := env.YT.Begin(ctx, nil)
		require.NoError(t, err)

		name := tmpPath()
		_, err = tx.CreateNode(ctx, name, yt.NodeMap, nil)
		require.NoError(t, err)

		ok, err := env.YT.NodeExists(ctx, name, nil)
		require.NoError(t, err)
		require.False(t, ok)

		require.NoError(t, tx.Commit())

		ok, err = env.YT.NodeExists(ctx, name, nil)
		require.NoError(t, err)
		require.True(t, ok)
	})

	t.Run("RollbackTransaction", func(t *testing.T) {
		tx, err := env.YT.Begin(ctx, nil)
		require.NoError(t, err)

		require.NoError(t, tx.Rollback())
	})
}
