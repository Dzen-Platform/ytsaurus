package integration

import (
	"bytes"
	"io"
	"net/http"
	"os"
	"testing"

	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/yt/chyt/controller/internal/api"
	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yttest"
)

type apiClient struct {
	Endpoint string
	User     string

	httpClient *http.Client
	t          *testing.T
	env        *yttest.Env
}

type apiResponse struct {
	StatusCode int
	Body       yson.RawValue
}

func (c *apiClient) MakeRequest(command string, params api.RequestParams) apiResponse {
	body, err := yson.Marshal(params)
	require.NoError(c.t, err)

	c.env.L.Debug("making http api request", log.String("command", command), log.Any("params", params))

	req, err := http.NewRequest(http.MethodPost, c.Endpoint+"/"+command, bytes.NewReader(body))
	require.NoError(c.t, err)

	req.Header.Set("Content-Type", "application/yson")
	req.Header.Set("X-YT-TestUser", c.User)

	rsp, err := c.httpClient.Do(req)
	require.NoError(c.t, err)

	body, err = io.ReadAll(rsp.Body)
	require.NoError(c.t, err)

	c.env.L.Debug("http api request finished",
		log.String("command", command),
		log.Any("params", params),
		log.Int("status_code", rsp.StatusCode),
		log.String("response_body", string(body)))

	return apiResponse{
		StatusCode: rsp.StatusCode,
		Body:       yson.RawValue(body),
	}
}

func prepareAPI(t *testing.T) (*yttest.Env, *apiClient) {
	env := prepareEnv(t)

	proxy := os.Getenv("YT_PROXY")

	c := api.HTTPServerConfig{
		HTTPAPIConfig: api.HTTPAPIConfig{
			APIConfig: api.APIConfig{
				Family: "test_family",
				Stage:  "test_stage",
				Root:   root,
			},
			Clusters:    []string{proxy},
			DisableAuth: true,
		},
		Endpoint: ":0",
	}

	server := api.NewHTTPServer(c, env.L.Logger())
	go server.Run()
	t.Cleanup(server.Stop)
	server.WaitReady()

	client := &apiClient{
		Endpoint:   "http://" + server.RealAddress() + "/" + proxy,
		User:       "root",
		httpClient: &http.Client{},
		t:          t,
		env:        env,
	}

	return env, client
}

func TestHTTPAPICreateAndDelete(t *testing.T) {
	t.Parallel()

	env, c := prepareAPI(t)
	alias := guid.New().String()

	r := c.MakeRequest("create", api.RequestParams{Args: []any{alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	ok, err := env.YT.NodeExists(env.Ctx, root.Child(alias), nil)
	require.NoError(t, err)
	require.True(t, ok)

	var speclet map[string]any
	err = env.YT.GetNode(env.Ctx, root.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "test_family",
			"stage":  "test_stage",
		},
		speclet)

	// Alias already exists.
	r = c.MakeRequest("create", api.RequestParams{Args: []any{alias}})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)

	// Wrong arguments.
	r = c.MakeRequest("create", api.RequestParams{})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)
	r = c.MakeRequest("create", api.RequestParams{Args: []any{"a", "b", "c"}})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)
	r = c.MakeRequest("create", api.RequestParams{
		Args:   []any{guid.New().String()},
		Params: map[string]any{"xxx": "yyy"},
	})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)

	r = c.MakeRequest("delete", api.RequestParams{Args: []any{alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	ok, err = env.YT.NodeExists(env.Ctx, root.Child(alias), nil)
	require.NoError(t, err)
	require.False(t, ok)

	// Alias does not exist anymore.
	r = c.MakeRequest("delete", api.RequestParams{Args: []any{alias}})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)
}

func TestHTTPAPISetAndRemove(t *testing.T) {
	t.Parallel()

	env, c := prepareAPI(t)
	alias := guid.New().String()

	r := c.MakeRequest("create", api.RequestParams{Args: []any{alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakeRequest("set", api.RequestParams{Args: []any{"test_option", 1234}, Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var speclet map[string]any
	err := env.YT.GetNode(env.Ctx, root.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family":      "test_family",
			"stage":       "test_stage",
			"test_option": int64(1234),
		},
		speclet)

	r = c.MakeRequest("set", api.RequestParams{Args: []any{"test_dict/option", "1234"}, Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	err = env.YT.GetNode(env.Ctx, root.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family":      "test_family",
			"stage":       "test_stage",
			"test_option": int64(1234),
			"test_dict": map[string]any{
				"option": "1234",
			},
		},
		speclet)

	r = c.MakeRequest("remove", api.RequestParams{Args: []any{"test_option"}, Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakeRequest("remove", api.RequestParams{Args: []any{"test_dict"}, Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	err = env.YT.GetNode(env.Ctx, root.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "test_family",
			"stage":  "test_stage",
		},
		speclet)

	r = c.MakeRequest("remove", api.RequestParams{Args: []any{"test_option"}, Params: map[string]any{"alias": alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)
}

func TestHTTPAPIParseParams(t *testing.T) {
	t.Parallel()

	env, c := prepareAPI(t)
	alias := guid.New().String()

	r := c.MakeRequest("create", api.RequestParams{Args: []any{alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	r = c.MakeRequest("set", api.RequestParams{Args: []any{"test_dict/option", "1234", alias}, Unparsed: true})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var speclet map[string]any
	err := env.YT.GetNode(env.Ctx, root.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "test_family",
			"stage":  "test_stage",
			"test_dict": map[string]any{
				"option": int64(1234),
			},
		},
		speclet)
}

func TestHTTPAPISetPool(t *testing.T) {
	t.Parallel()

	env, c := prepareAPI(t)
	alias := guid.New().String()

	r := c.MakeRequest("create", api.RequestParams{Args: []any{alias}})
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

	r = c.MakeRequest("set", api.RequestParams{Args: []any{"pool", thisPoolDoesNotExist, alias}})
	require.Equal(t, http.StatusBadRequest, r.StatusCode)

	r = c.MakeRequest("set", api.RequestParams{Args: []any{"pool", subpool, alias}})
	require.Equal(t, http.StatusOK, r.StatusCode)

	var speclet map[string]any
	err = env.YT.GetNode(env.Ctx, root.JoinChild(alias, "speclet"), &speclet, nil)
	require.NoError(t, err)
	require.Equal(t,
		map[string]any{
			"family": "test_family",
			"stage":  "test_stage",
			"pool":   subpool,
		},
		speclet)
}