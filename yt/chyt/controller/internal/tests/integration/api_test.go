package integration

import (
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"

	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/yt/chyt/controller/internal/api"
	"a.yandex-team.ru/yt/chyt/controller/internal/strawberry"
	"a.yandex-team.ru/yt/chyt/controller/internal/tests/helpers"
	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
)

func TestHTTPAPICreateAndRemove(t *testing.T) {
	t.Parallel()

	env, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	ok, err := env.YT.NodeExists(env.Ctx, helpers.StrawberryRoot.Child(alias), nil)
	require.NoError(t, err)
	require.True(t, ok)

	var speclet map[string]any
	err = env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "sleep",
			"stage":  "test_stage",
		},
		speclet)

	// Alias already exists.
	r = c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)

	// Wrong arguments.
	r = c.MakePostRequest("create", api.RequestParams{})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)
	r = c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{
			"alias": helpers.GenerateAlias(),
			"xxx":   "yyy",
		},
	})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)

	r = c.MakePostRequest("remove", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	ok, err = env.YT.NodeExists(env.Ctx, helpers.StrawberryRoot.Child(alias), nil)
	require.NoError(t, err)
	require.False(t, ok)

	// Alias does not exist anymore.
	r = c.MakePostRequest("remove", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)

	// Alias with leading *.
	r = c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": "*" + alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	ok, err = env.YT.NodeExists(env.Ctx, helpers.StrawberryRoot.Child(alias), nil)
	require.NoError(t, err)
	require.True(t, ok)
}

func TestHTTPAPIExists(t *testing.T) {
	t.Parallel()

	_, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("exists", api.RequestParams{Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var result any

	err := yson.Unmarshal(r.Body, &result)
	require.NoError(t, err)
	require.Equal(t, map[string]any{"result": false}, result)

	r = c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("exists", api.RequestParams{Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	err = yson.Unmarshal(r.Body, &result)
	require.NoError(t, err)
	require.Equal(t, map[string]any{"result": true}, result)
}

func TestHTTPAPISetAndRemoveOption(t *testing.T) {
	t.Parallel()

	env, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("set_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "test_option",
			"value": 1234,
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var speclet map[string]any
	err := env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family":      "sleep",
			"stage":       "test_stage",
			"test_option": int64(1234),
		},
		speclet)

	r = c.MakePostRequest("set_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "test_dict/option",
			"value": "1234",
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	err = env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family":      "sleep",
			"stage":       "test_stage",
			"test_option": int64(1234),
			"test_dict": map[string]any{
				"option": "1234",
			},
		},
		speclet)

	r = c.MakePostRequest("remove_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "test_option",
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("remove_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "test_dict",
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	err = env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "sleep",
			"stage":  "test_stage",
		},
		speclet)

	r = c.MakePostRequest("remove_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "test_option",
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)
}

func TestHTTPAPIParseParams(t *testing.T) {
	t.Parallel()

	env, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("set_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "test_dict/option",
			"value": "1234",
		},
		Unparsed: true,
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var speclet map[string]any
	err := env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "sleep",
			"stage":  "test_stage",
			"test_dict": map[string]any{
				"option": int64(1234),
			},
		},
		speclet)

	badAliases := []string{"9alias", "ali*s", "@alias"}
	for _, badAlias := range badAliases {
		r = c.MakePostRequest("create", api.RequestParams{
			Params: map[string]any{"alias": badAlias},
		})
		require.Equal(t, http.StatusBadRequest, r.StatusCode)
	}

	badOptions := []string{"@option", "op*tion", "very/@option"}
	for _, badOption := range badOptions {
		r = c.MakePostRequest("set_option", api.RequestParams{
			Params: map[string]any{
				"alias": alias,
				"key":   badOption,
				"value": "1234",
			},
			Unparsed: true,
		})
		require.Equal(t, http.StatusBadRequest, r.StatusCode)
	}
}

func TestHTTPAPISetPool(t *testing.T) {
	t.Parallel()

	env, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	pool := guid.New().String()
	subpool := guid.New().String()
	thisPoolDoesNotExist := guid.New().String()

	_, err := env.YT.CreateObject(env.Ctx, yt.NodeSchedulerPool, &yt.CreateObjectOptions{
		Attributes: map[string]any{
			"name":      pool,
			"pool_tree": "default",
		},
	})
	require.NoError(t, err)

	_, err = env.YT.CreateObject(env.Ctx, yt.NodeSchedulerPool, &yt.CreateObjectOptions{
		Attributes: map[string]any{
			"name":        subpool,
			"pool_tree":   "default",
			"parent_name": pool,
		},
	})
	require.NoError(t, err)

	r = c.MakePostRequest("set_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "pool",
			"value": thisPoolDoesNotExist,
		},
	})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)

	r = c.MakePostRequest("set_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "pool",
			"value": subpool,
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var speclet map[string]any
	err = env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "sleep",
			"stage":  "test_stage",
			"pool":   subpool,
		},
		speclet)
}

func TestHTTPAPIDescribeAndPing(t *testing.T) {
	t.Parallel()

	_, c := helpers.PrepareAPI(t)

	rsp, err := http.Get(c.Endpoint + "/ping")
	require.NoError(t, err)
	require.Equal(t, http.StatusOK, rsp.StatusCode)

	rsp, err = http.Get(c.Endpoint + "/describe")
	require.NoError(t, err)
	require.Equal(t, http.StatusOK, rsp.StatusCode)

	body, err := io.ReadAll(rsp.Body)
	require.NoError(t, err)

	var description map[string]any
	err = yson.Unmarshal(body, &description)
	require.NoError(t, err)

	require.Equal(t, []any{c.Proxy}, description["clusters"])

	// It's unlikely that the interface of the 'remove' command will be changed in the future,
	// so we rely on it in this test.
	deletePresent := false
	for _, anyCmd := range description["commands"].([]any) {
		cmd := anyCmd.(map[string]any)
		if reflect.DeepEqual(cmd["name"], "remove") {
			deletePresent = true
			params := cmd["parameters"].([]any)
			require.Equal(t, 1, len(params))
			param := params[0].(map[string]any)
			require.Equal(t, "alias", param["name"])
			require.Equal(t, true, param["required"])
		}
	}
	require.True(t, deletePresent)
}

func TestHTTPAPIList(t *testing.T) {
	t.Parallel()

	_, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("list", api.RequestParams{})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var result map[string][]string

	err := yson.Unmarshal(r.Body, &result)
	require.NoError(t, err)
	require.Contains(t, result["result"], alias)
}

func TestHTTPAPIStatus(t *testing.T) {
	t.Parallel()

	_, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("status", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var result map[string]strawberry.OpletStatus
	err := yson.Unmarshal(r.Body, &result)
	require.NoError(t, err)
	require.Equal(t, "Ok", result["result"].Status)

	r = c.MakePostRequest("set_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "active",
			"value": true,
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("status", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	err = yson.Unmarshal(r.Body, &result)
	require.NoError(t, err)
	require.True(t, strings.HasPrefix(result["result"].Status, "Waiting for restart"))
}

func TestHTTPAPIGetSpeclet(t *testing.T) {
	t.Parallel()

	_, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("get_speclet", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var result map[string]any

	err := yson.Unmarshal(r.Body, &result)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "sleep",
			"stage":  "test_stage",
		},
		result["result"])
}

func TestHTTPAPISetSpeclet(t *testing.T) {
	t.Parallel()

	env, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	pool := guid.New().String()
	_, err := env.YT.CreateObject(env.Ctx, yt.NodeSchedulerPool, &yt.CreateObjectOptions{
		Attributes: map[string]any{
			"name":      pool,
			"pool_tree": "default",
		},
	})
	require.NoError(t, err)

	r = c.MakePostRequest("set_speclet", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"speclet": map[string]any{
				"pool": pool,
			},
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var speclet map[string]any
	err = env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "sleep",
			"stage":  "test_stage",
			"pool":   pool,
		},
		speclet)

	nonExistentPool := guid.New().String()
	r = c.MakePostRequest("set_speclet", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"speclet": map[string]any{
				"pool": nonExistentPool,
			},
		},
	})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)
}

func TestHTTPAPIGetOption(t *testing.T) {
	t.Parallel()

	_, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("get_option", api.RequestParams{
		Params: map[string]any{
			"alias": alias,
			"key":   "stage",
		},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var result map[string]any
	err := yson.Unmarshal(r.Body, &result)
	require.NoError(t, err)
	require.Equal(t, "test_stage", result["result"])
}

func TestHTTPAPIStop(t *testing.T) {
	t.Parallel()

	env, c := helpers.PrepareAPI(t)
	alias := helpers.GenerateAlias()

	r := c.MakePostRequest("create", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakePostRequest("stop", api.RequestParams{
		Params: map[string]any{"alias": alias},
	})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var result bool
	err := env.YT.GetNode(env.Ctx, helpers.StrawberryRoot.JoinChild(alias, "speclet", "active"), &result, nil)
	require.NoError(t, err)
	require.Equal(t, false, result)
}
