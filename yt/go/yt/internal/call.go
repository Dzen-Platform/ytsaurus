package internal

import (
	"context"
	"io"

	"a.yandex-team.ru/yt/go/guid"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"

	"a.yandex-team.ru/library/go/core/log"
)

type Params interface {
	HTTPVerb() Verb
	Log() []log.Field
	MarshalHTTP(w *yson.Writer)
}

type Call struct {
	Params Params
	CallID guid.GUID

	YSONValue []byte
	ProxyURL  string
}

type CallResult struct {
	YSONValue []byte
}

func (res *CallResult) decodeSingle(key string, value interface{}) (err error) {
	err = yson.Unmarshal(res.YSONValue, &unmapper{key: key, value: value})
	return
}

func (res *CallResult) decodeValue(value interface{}) (err error) {
	return res.decodeSingle("value", value)
}

func (res *CallResult) decode(value interface{}) (err error) {
	err = yson.Unmarshal(res.YSONValue, value)
	return
}

type CallInvoker func(ctx context.Context, call *Call) (res *CallResult, err error)

func (c CallInvoker) Wrap(interceptor CallInterceptor) CallInvoker {
	return func(ctx context.Context, call *Call) (res *CallResult, err error) {
		return interceptor(ctx, call, c)
	}
}

type CallInterceptor func(ctx context.Context, call *Call, invoke CallInvoker) (res *CallResult, err error)

type ReadInvoker func(ctx context.Context, call *Call) (r io.ReadCloser, err error)

func (c ReadInvoker) Wrap(interceptor ReadInterceptor) ReadInvoker {
	return func(ctx context.Context, call *Call) (r io.ReadCloser, err error) {
		return interceptor(ctx, call, c)
	}
}

type ReadInterceptor func(ctx context.Context, call *Call, invoke ReadInvoker) (r io.ReadCloser, err error)

type WriteInvoker func(ctx context.Context, call *Call) (r io.WriteCloser, err error)

func (c WriteInvoker) Wrap(interceptor WriteInterceptor) WriteInvoker {
	return func(ctx context.Context, call *Call) (r io.WriteCloser, err error) {
		return interceptor(ctx, call, c)
	}
}

type WriteInterceptor func(ctx context.Context, call *Call, invoke WriteInvoker) (r io.WriteCloser, err error)

type ReadRowInvoker func(ctx context.Context, call *Call) (r yt.TableReader, err error)

func (c ReadRowInvoker) Wrap(interceptor ReadRowInterceptor) ReadRowInvoker {
	return func(ctx context.Context, call *Call) (r yt.TableReader, err error) {
		return interceptor(ctx, call, c)
	}
}

type ReadRowInterceptor func(ctx context.Context, call *Call, invoke ReadRowInvoker) (r yt.TableReader, err error)

type WriteRowInvoker func(ctx context.Context, call *Call) (r yt.TableWriter, err error)

func (c WriteRowInvoker) Wrap(interceptor WriteRowInterceptor) WriteRowInvoker {
	return func(ctx context.Context, call *Call) (r yt.TableWriter, err error) {
		return interceptor(ctx, call, c)
	}
}

type WriteRowInterceptor func(ctx context.Context, call *Call, invoke WriteRowInvoker) (r yt.TableWriter, err error)
