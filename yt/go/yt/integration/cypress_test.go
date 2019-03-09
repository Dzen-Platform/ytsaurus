package integration

import (
	"context"
	"testing"
	"time"

	"github.com/gofrs/uuid/v3"
	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yttest"
)

func tmpPath() ypath.Path {
	id, err := uuid.NewV4()
	if err != nil {
		panic(err)
	}
	return ypath.Path("//tmp").Child(id.String())
}

func TestCypress(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*15)
	defer cancel()

	t.Run("P", func(t *testing.T) {
		t.Run("Get", func(t *testing.T) {
			t.Parallel()

			var attrs struct {
				Account  string `yson:"account"`
				Revision int64  `yson:"revision"`
			}

			err := env.YT.GetNode(ctx, ypath.Path("//@"), &attrs, nil)
			require.NoError(t, err)

			require.NotEqual(t, "", attrs.Account)
			require.NotZero(t, attrs.Revision)
		})

		t.Run("List", func(t *testing.T) {
			t.Parallel()

			var list []struct {
				Owner string `yson:"owner,attr"`
				Name  string `yson:",value"`
			}

			err := env.YT.ListNode(ctx, ypath.Path("/"), &list, &yt.ListNodeOptions{Attributes: []string{"owner"}})
			require.NoError(t, err)

			require.NotEmpty(t, list)
			for _, node := range list {
				require.NotEqual(t, "", node.Name)
				require.NotEqual(t, "", node.Owner)
			}
		})

		t.Run("Exists", func(t *testing.T) {
			t.Parallel()

			ok, err := env.YT.NodeExists(ctx, ypath.Path("/"), nil)
			require.NoError(t, err)
			require.True(t, ok)

			ok, err = env.YT.NodeExists(ctx, tmpPath(), nil)
			require.NoError(t, err)
			require.False(t, ok)
		})

		t.Run("CreateNode", func(t *testing.T) {
			t.Parallel()

			name := tmpPath()

			id, err := env.YT.CreateNode(ctx, name, yt.NodeMap, nil)
			require.NoError(t, err)

			ok, err := env.YT.NodeExists(ctx, name, nil)
			require.NoError(t, err)
			require.True(t, ok)

			ok, err = env.YT.NodeExists(ctx, id.YPath(), nil)
			require.NoError(t, err)
			require.True(t, ok)
		})

		t.Run("LinkNode", func(t *testing.T) {
			t.Parallel()

			targetName := tmpPath()
			linkName := tmpPath()

			_, err := env.YT.CreateNode(ctx, targetName, yt.NodeMap, nil)
			require.NoError(t, err)

			_, err = env.YT.LinkNode(ctx, targetName, linkName, nil)
			require.NoError(t, err)

			var typ yt.NodeType
			err = env.YT.GetNode(ctx, linkName.SuppressSymlink().Attr("type"), &typ, nil)
			require.NoError(t, err)
			require.Equal(t, yt.NodeLink, typ)
		})

		t.Run("CopyNode", func(t *testing.T) {
			t.Parallel()

			name := tmpPath()
			copyName := tmpPath()

			_, err := env.YT.CreateNode(ctx, name, yt.NodeMap, &yt.CreateNodeOptions{
				Attributes: map[string]interface{}{"foo": "bar"},
			})
			require.NoError(t, err)

			_, err = env.YT.CopyNode(ctx, name, copyName, nil)
			require.NoError(t, err)

			ok, err := env.YT.NodeExists(ctx, copyName.Attr("foo"), nil)
			require.NoError(t, err)
			require.True(t, ok)
		})

		t.Run("MoveNode", func(t *testing.T) {
			t.Parallel()

			name := tmpPath()
			movedName := tmpPath()

			_, err := env.YT.CreateNode(ctx, name, yt.NodeMap, &yt.CreateNodeOptions{
				Attributes: map[string]interface{}{"foo": "bar"},
			})
			require.NoError(t, err)

			_, err = env.YT.MoveNode(ctx, name, movedName, nil)
			require.NoError(t, err)

			ok, err := env.YT.NodeExists(ctx, name, nil)
			require.NoError(t, err)
			require.False(t, ok)

			ok, err = env.YT.NodeExists(ctx, movedName, nil)
			require.NoError(t, err)
			require.True(t, ok)
		})

		t.Run("RemoveNode", func(t *testing.T) {
			t.Parallel()

			name := tmpPath()

			err := env.YT.RemoveNode(ctx, name, nil)
			require.Error(t, err)
			require.True(t, yt.ContainsErrorCode(err, yt.ErrorCode(500)))

			_, err = env.YT.CreateNode(ctx, name, yt.NodeMap, nil)
			require.NoError(t, err)

			err = env.YT.RemoveNode(ctx, name, nil)
			require.NoError(t, err)

			ok, err := env.YT.NodeExists(ctx, name, nil)
			require.NoError(t, err)
			require.False(t, ok)
		})
	})
}
