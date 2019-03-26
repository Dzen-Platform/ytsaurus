// package yttest contains testing helpers.
package yttest

import (
	"context"
	"reflect"
	"testing"

	"a.yandex-team.ru/library/go/core/xerrors"

	"go.uber.org/zap/zaptest"

	"a.yandex-team.ru/library/go/core/log/zap"

	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/mapreduce"
	"a.yandex-team.ru/yt/go/schema"
	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/ythttp"
)

type Env struct {
	Ctx context.Context
	YT  yt.Client
	MR  mapreduce.Client
}

func NewEnv(t testing.TB) (env *Env, cancel func()) {
	config, err := yt.NewConfigFromEnv()
	if err != nil {
		t.Fatalf("failed to get YT config from env: %+v", err)
	}
	config.Logger = &zap.Logger{L: zaptest.NewLogger(t)}

	var cancelCtx func()
	env = &Env{}
	env.Ctx, cancelCtx = context.WithCancel(context.Background())
	env.YT, err = ythttp.NewClient(config)
	if err != nil {
		t.Fatalf("failed to create YT client: %+v", err)
	}

	env.MR = mapreduce.New(env.YT)

	cancel = func() {
		cancelCtx()
		env.YT.Stop()
	}
	return
}

func (e *Env) TmpPath() ypath.Path {
	uid := guid.New()
	return ypath.Path("//tmp").Child(uid.String())
}

func UploadSlice(ctx context.Context, c yt.Client, path ypath.YPath, slice interface{}) error {
	sliceType := reflect.TypeOf(slice)
	if sliceType.Kind() != reflect.Slice {
		return xerrors.Errorf("type %T is not a slice", slice)
	}

	tableSchema, err := schema.Infer(reflect.New(sliceType.Elem()).Interface())
	if err != nil {
		return err
	}

	_, err = c.CreateNode(ctx, path, yt.NodeTable, &yt.CreateNodeOptions{
		Attributes: map[string]interface{}{"schema": tableSchema},
	})
	if err != nil {
		return err
	}

	w, err := c.WriteTable(ctx, path, nil)
	if err != nil {
		return err
	}

	sliceValue := reflect.ValueOf(slice)
	for i := 0; i < sliceValue.Len(); i++ {
		if err = w.Write(sliceValue.Index(i).Interface()); err != nil {
			return err
		}
	}

	return w.Close()
}

func (e *Env) UploadSlice(path ypath.YPath, slice interface{}) error {
	return UploadSlice(e.Ctx, e.YT, path, slice)
}

func DownloadSlice(ctx context.Context, c yt.Client, path ypath.YPath, value interface{}) error {
	sliceValue := reflect.ValueOf(value).Elem()

	r, err := c.ReadTable(ctx, path, nil)
	if err != nil {
		return err
	}
	defer func() { _ = r.Close() }()

	for r.Next() {
		row := reflect.New(sliceValue.Type().Elem())

		if err = r.Scan(row.Interface()); err != nil {
			return err
		}

		sliceValue = reflect.Append(sliceValue, row.Elem())
	}

	reflect.ValueOf(value).Elem().Set(sliceValue)
	return nil
}

func (e *Env) DownloadSlice(path ypath.YPath, value interface{}) error {
	return DownloadSlice(e.Ctx, e.YT, path, value)
}
