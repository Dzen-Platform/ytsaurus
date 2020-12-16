package httpclient

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"net"
	"net/http"
	"time"

	"github.com/cenkalti/backoff/v4"
	"golang.org/x/xerrors"

	"a.yandex-team.ru/library/go/blockcodecs"
	_ "a.yandex-team.ru/library/go/blockcodecs/all"
	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/library/go/core/log/ctxlog"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/internal"
	"a.yandex-team.ru/yt/go/yterrors"
)

func decodeYTErrorFromHeaders(h http.Header) (ytErr *yterrors.Error, err error) {
	header := h.Get("X-YT-Error")
	if header == "" {
		return nil, nil
	}

	ytErr = &yterrors.Error{}
	if decodeErr := json.Unmarshal([]byte(header), ytErr); decodeErr != nil {
		err = xerrors.Errorf("yt: malformed 'X-YT-Error' header: %w", decodeErr)
	}

	return
}

type httpClient struct {
	internal.Encoder

	requestLogger   *internal.LoggingInterceptor
	mutationRetrier *internal.MutationRetrier
	readRetrier     *internal.Retrier

	clusterURL yt.ClusterURL
	netDialer  *net.Dialer
	httpClient *http.Client
	log        log.Structured
	config     *yt.Config
	stop       *internal.StopGroup

	credentials yt.Credentials

	proxySet *internal.ProxySet
}

func (c *httpClient) listHeavyProxies() ([]string, error) {
	if !c.stop.TryAdd() {
		return nil, fmt.Errorf("client is stopped")
	}
	defer c.stop.Done()

	req, err := http.NewRequest("GET", "http://"+c.clusterURL.Address+"/hosts", nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(c.stop.Context()))
	if err != nil {
		return nil, err
	}
	defer func() { _ = rsp.Body.Close() }()

	if rsp.StatusCode != 200 {
		return nil, unexpectedStatusCode(rsp)
	}

	var proxies []string
	if err = json.NewDecoder(rsp.Body).Decode(&proxies); err != nil {
		return nil, err
	}

	if len(proxies) == 0 {
		return nil, xerrors.New("proxy list is empty")
	}

	return proxies, nil
}

func (c *httpClient) pickHeavyProxy(ctx context.Context) (string, error) {
	if c.clusterURL.DisableDiscovery {
		return c.clusterURL.Address, nil
	}

	proxy, err := c.proxySet.PickRandom(ctx)
	if err != nil {
		return "", err
	}

	return proxy, nil
}

func (c *httpClient) writeParams(req *http.Request, call *internal.Call) error {
	var params bytes.Buffer

	w := yson.NewWriterFormat(&params, yson.FormatText)
	w.BeginMap()
	call.Params.MarshalHTTP(w)
	w.EndMap()
	if err := w.Finish(); err != nil {
		return err
	}

	h := req.Header
	h.Add("X-YT-Header-Format", "yson")
	if req.Method == http.MethodPost && req.Body == http.NoBody {
		req.Body = ioutil.NopCloser(&params)
		req.ContentLength = int64(params.Len())
		req.GetBody = func() (body io.ReadCloser, e error) {
			return ioutil.NopCloser(&params), nil
		}
	} else {
		h.Add("X-YT-Parameters", params.String())
	}
	h.Add("X-YT-Correlation-ID", call.CallID.String())
	h.Set("User-Agent", "go-yt-client")

	return nil
}

func (c *httpClient) newHTTPRequest(ctx context.Context, call *internal.Call, body io.Reader) (req *http.Request, err error) {
	var address string
	if call.RequestedProxy != "" {
		address = call.RequestedProxy
		call.SelectedProxy = address
	} else if call.Params.HTTPVerb().IsHeavy() {
		address, err = c.pickHeavyProxy(ctx)
		if err != nil {
			return nil, err
		}
		call.SelectedProxy = address
	} else {
		address = c.clusterURL.Address
	}

	if body == nil {
		body = bytes.NewBuffer(call.YSONValue)
	}

	verb := call.Params.HTTPVerb()
	req, err = http.NewRequest(verb.HTTPMethod(), "http://"+address+"/api/v4/"+verb.String(), body)
	if err != nil {
		return
	}

	if err = c.writeParams(req, call); err != nil {
		return
	}

	if body != nil {
		req.Header.Add("X-YT-Input-Format", "yson")
	}
	req.Header.Add("X-YT-Header-Format", "<format=text>yson")
	req.Header.Add("X-YT-Output-Format", "yson")

	if requestCredentials := yt.ContextCredentials(ctx); requestCredentials != nil {
		requestCredentials.Set(req)
	} else if c.credentials != nil {
		c.credentials.Set(req)
	}

	c.logRequest(ctx, req)
	return
}

func (c *httpClient) logRequest(ctx context.Context, req *http.Request) {
	ctxlog.Debug(ctx, c.log.Logger(), "sending HTTP request",
		log.String("proxy", req.URL.Hostname()))
}

func (c *httpClient) logResponse(ctx context.Context, rsp *http.Response) {
	ctxlog.Debug(ctx, c.log.Logger(), "received HTTP response",
		log.String("yt_proxy", rsp.Header.Get("X-YT-Proxy")),
		log.String("yt_request_id", rsp.Header.Get("X-YT-Request-Id")))
}

// unexpectedStatusCode is last effort attempt to get useful error message from a failed request.
func unexpectedStatusCode(rsp *http.Response) error {
	d := json.NewDecoder(rsp.Body)
	d.UseNumber()

	var ytErr yterrors.Error
	if err := d.Decode(&ytErr); err == nil {
		return &ytErr
	}

	return xerrors.Errorf("unexpected status code %d", rsp.StatusCode)
}

func (c *httpClient) readResult(rsp *http.Response) (res *internal.CallResult, err error) {
	defer func() { _ = rsp.Body.Close() }()

	res = &internal.CallResult{}

	var ytErr *yterrors.Error
	ytErr, err = decodeYTErrorFromHeaders(rsp.Header)
	if err != nil {
		return
	}
	if ytErr != nil {
		return nil, ytErr
	}

	if rsp.StatusCode/100 != 2 {
		return nil, unexpectedStatusCode(rsp)
	}

	res.YSONValue, err = ioutil.ReadAll(rsp.Body)
	return
}

func (c *httpClient) startCall() *internal.Call {
	bf := backoff.NewExponentialBackOff()
	bf.MaxElapsedTime = c.config.GetLightRequestTimeout()
	return &internal.Call{
		Backoff: bf,
	}
}

func (c *httpClient) do(ctx context.Context, call *internal.Call) (res *internal.CallResult, err error) {
	var req *http.Request
	req, err = c.newHTTPRequest(ctx, call, nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		select {
		case <-ctx.Done():
			err = ctx.Err()
		default:
		}
	}

	if err == nil {
		c.logResponse(ctx, rsp)
		res, err = c.readResult(rsp)
	}

	return
}

func (c *httpClient) doWrite(ctx context.Context, call *internal.Call) (w io.WriteCloser, err error) {
	pr, pw := io.Pipe()
	errChan := make(chan error, 1)

	req, err := c.newHTTPRequest(ctx, call, ioutil.NopCloser(pr))
	if err != nil {
		return nil, err
	}

	switch c.config.GetClientCompressionCodec() {
	case yt.ClientCodecGZIP, yt.ClientCodecNone:
		// do nothing
	default:
		encoding, err := toHTTPEncoding(c.config.GetClientCompressionCodec())
		if err != nil {
			return nil, err
		}

		req.Header.Add("Content-Encoding", encoding)
	}

	go func() {
		defer close(errChan)

		rsp, err := c.httpClient.Do(req.WithContext(ctx))
		closeErr := func(err error) {
			errChan <- err
			_ = pr.CloseWithError(err)
		}

		if err != nil {
			closeErr(err)
			return
		}

		c.logResponse(ctx, rsp)
		defer func() { _ = rsp.Body.Close() }()

		if rsp.StatusCode/100 == 2 {
			return
		}

		callErr, err := decodeYTErrorFromHeaders(rsp.Header)
		if err != nil {
			closeErr(err)
			return
		}

		if callErr != nil {
			closeErr(callErr)
			return
		}

		closeErr(unexpectedStatusCode(rsp))
	}()

	hw := &httpWriter{w: pw, c: pw, errChan: errChan}
	w = hw

	switch c.config.GetClientCompressionCodec() {
	case yt.ClientCodecGZIP, yt.ClientCodecNone:
		// do nothing
	default:
		block, ok := c.config.GetClientCompressionCodec().BlockCodec()
		if !ok {
			err = fmt.Errorf("unsupported compression codec %d", c.config.GetClientCompressionCodec())
			return
		}

		codec := blockcodecs.FindCodecByName(block)
		if codec == nil {
			err = fmt.Errorf("unsupported compression codec %q", block)
			return
		}

		encoder := blockcodecs.NewEncoder(hw.w, codec)
		hw.w = encoder
		hw.c = &compressorCloser{enc: encoder, pw: pw}
	}

	return
}

type compressorCloser struct {
	enc io.WriteCloser
	pw  io.Closer
}

func (c *compressorCloser) Close() error {
	if err := c.enc.Close(); err != nil {
		return err
	}

	return c.pw.Close()
}

func (c *httpClient) doWriteRow(ctx context.Context, call *internal.Call) (w yt.TableWriter, err error) {
	var ww io.WriteCloser

	ctx, cancelFunc := context.WithCancel(ctx)
	ww, err = c.InvokeWrite(ctx, call)
	if err != nil {
		cancelFunc()
		return
	}

	w = newTableWriter(ww, cancelFunc)
	return
}

func (c *httpClient) doReadRow(ctx context.Context, call *internal.Call) (r yt.TableReader, err error) {
	if call.OnRspParams != nil {
		panic("call.OnRspParams is already set")
	}

	var rspParams *tableReaderRspParams
	call.OnRspParams = func(ys []byte) (err error) {
		rspParams, err = decodeRspParams(ys)
		return
	}

	var rr io.ReadCloser
	rr, err = c.InvokeRead(ctx, call)
	if err != nil {
		return
	}

	tr := newTableReader(rr)

	if rspParams != nil {
		if err := tr.setRspParams(rspParams); err != nil {
			return nil, xerrors.Errorf("invalid rsp params: %w", err)
		}
	}

	r = tr
	return
}

func toHTTPEncoding(c yt.ClientCompressionCodec) (string, error) {
	if block, ok := c.BlockCodec(); ok {
		return "z-" + block, nil
	}

	return "", fmt.Errorf("codec %d is not supported", c)
}

func (c *httpClient) doRead(ctx context.Context, call *internal.Call) (r io.ReadCloser, err error) {
	var req *http.Request
	req, err = c.newHTTPRequest(ctx, call, nil)
	if err != nil {
		return nil, err
	}

	switch c.config.GetClientCompressionCodec() {
	case yt.ClientCodecGZIP, yt.ClientCodecNone:
		// do nothing
	default:
		encoding, err := toHTTPEncoding(c.config.GetClientCompressionCodec())
		if err != nil {
			return nil, err
		}

		req.Header.Add("Accept-Encoding", encoding)
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		return nil, err
	}

	c.logResponse(ctx, rsp)
	if rsp.StatusCode != 200 {
		_ = rsp.Body.Close()

		var callErr *yterrors.Error
		callErr, err = decodeYTErrorFromHeaders(rsp.Header)
		if err == nil && callErr != nil {
			err = callErr
		} else {
			err = unexpectedStatusCode(rsp)
		}
	}

	if err == nil && call.OnRspParams != nil {
		if p := rsp.Header.Get("X-YT-Response-Parameters"); p != "" {
			err = call.OnRspParams([]byte(p))
			if err != nil {
				_ = rsp.Body.Close()
			}
		}
	}

	if err != nil {
		return
	}

	br := &httpReader{
		body:       rsp.Body,
		bodyCloser: rsp.Body,
		rsp:        rsp,
	}
	r = br

	switch c.config.GetClientCompressionCodec() {
	case yt.ClientCodecGZIP, yt.ClientCodecNone:
		// do nothing
	default:
		if rspEncoding := rsp.Header.Get("Content-Encoding"); rspEncoding != req.Header.Get("Accept-Encoding") {
			_ = rsp.Body.Close()
			err = fmt.Errorf("unexpected response encoding %q != %q", rspEncoding, req.Header.Get("Accept-Encoding"))
			return
		}

		br.body = blockcodecs.NewDecoder(br.body)
	}

	return
}

func (c *httpClient) BeginTx(ctx context.Context, options *yt.StartTxOptions) (yt.Tx, error) {
	return internal.NewTx(ctx, c.Encoder, c.stop, c.config, options)
}

func (c *httpClient) Stop() {
	c.stop.Stop()
}

func (c *httpClient) dialContext(ctx context.Context, network, addr string) (net.Conn, error) {
	for i := 0; ; i++ {
		conn, err := c.netDialer.DialContext(ctx, network, addr)
		if err != nil {
			var temporary interface{ Temporary() bool }
			if errors.As(err, &temporary) && temporary.Temporary() && i < 3 {
				continue
			}

			var timeout interface{ Timeout() bool }
			if errors.As(err, &timeout) && timeout.Timeout() && i < 3 {
				continue
			}

			return nil, err
		}

		return conn, nil
	}
}

func NewHTTPClient(c *yt.Config) (yt.Client, error) {
	var client httpClient

	client.log = c.GetLogger()

	proxy, err := c.GetProxy()
	if err != nil {
		return nil, err
	}

	client.config = c
	client.clusterURL = yt.NormalizeProxyURL(proxy)
	client.netDialer = &net.Dialer{
		Timeout:   30 * time.Second,
		KeepAlive: 30 * time.Second,
	}
	client.httpClient = &http.Client{
		Transport: &http.Transport{
			DialContext:         client.dialContext,
			ForceAttemptHTTP2:   true,
			MaxIdleConns:        100,
			IdleConnTimeout:     90 * time.Second,
			TLSHandshakeTimeout: 10 * time.Second,

			DisableCompression: c.GetClientCompressionCodec() != yt.ClientCodecGZIP,
		},
	}
	client.stop = internal.NewStopGroup()
	client.proxySet = &internal.ProxySet{UpdateFn: client.listHeavyProxies}

	client.Encoder.StartCall = client.startCall
	client.Encoder.Invoke = client.do
	client.Encoder.InvokeRead = client.doRead
	client.Encoder.InvokeReadRow = client.doReadRow
	client.Encoder.InvokeWrite = client.doWrite
	client.Encoder.InvokeWriteRow = client.doWriteRow

	client.mutationRetrier = &internal.MutationRetrier{Log: client.log}
	client.readRetrier = &internal.Retrier{Config: client.config, Log: client.log}
	client.requestLogger = &internal.LoggingInterceptor{Structured: client.log}
	proxyBouncer := &internal.ProxyBouncer{ProxySet: client.proxySet}
	errorWrapper := &internal.ErrorWrapper{}

	client.Encoder.Invoke = client.Encoder.Invoke.
		Wrap(proxyBouncer.Intercept).
		Wrap(client.requestLogger.Intercept).
		Wrap(client.mutationRetrier.Intercept).
		Wrap(client.readRetrier.Intercept).
		Wrap(errorWrapper.Intercept)

	client.Encoder.InvokeRead = client.Encoder.InvokeRead.
		Wrap(proxyBouncer.Read).
		Wrap(client.requestLogger.Read).
		Wrap(errorWrapper.Read)

	client.Encoder.InvokeWrite = client.Encoder.InvokeWrite.
		Wrap(proxyBouncer.Write).
		Wrap(client.requestLogger.Write).
		Wrap(errorWrapper.Write)

	if token := c.GetToken(); token != "" {
		client.credentials = &yt.TokenCredentials{Token: token}
	}

	return &client, nil
}
