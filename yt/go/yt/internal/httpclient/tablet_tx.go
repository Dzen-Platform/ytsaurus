package httpclient

import (
	"context"

	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/internal"
)

type tabletTx struct {
	internal.Encoder

	txID           yt.TxID
	coordinatorURL string
	c              *httpClient
	ctx            context.Context
}

func (c *httpClient) BeginTablet(ctx context.Context, options *yt.StartTabletTxOptions) (yt.TabletTx, error) {
	var tx tabletTx

	var err error
	tx.coordinatorURL, err = c.pickHeavyProxy(ctx)
	if err != nil {
		return nil, err
	}

	tx.Invoke = tx.do
	tx.InvokeReadRow = tx.doReadRow
	tx.InvokeWriteRow = tx.doWriteRow
	tx.c = c

	txType := "tablet"
	tx.txID, err = tx.StartTx(ctx, &yt.StartTxOptions{Type: &txType, Sticky: true})
	tx.ctx = ctx

	return &tx, err
}

func (tx *tabletTx) setTx(call *internal.Call) {
	txOpts := call.Params.(internal.TransactionParams).TransactionOptions()
	*txOpts = &yt.TransactionOptions{TransactionID: tx.txID}
}

func (tx *tabletTx) do(ctx context.Context, call *internal.Call) (res *internal.CallResult, err error) {
	call.ProxyURL = tx.coordinatorURL
	return tx.c.Invoke(ctx, call)
}

func (tx *tabletTx) doReadRow(ctx context.Context, call *internal.Call) (r yt.TableReader, err error) {
	call.ProxyURL = tx.coordinatorURL
	tx.setTx(call)
	return tx.c.InvokeReadRow(ctx, call)
}

func (tx *tabletTx) doWriteRow(ctx context.Context, call *internal.Call) (r yt.TableWriter, err error) {
	call.ProxyURL = tx.coordinatorURL
	tx.setTx(call)
	return tx.c.InvokeWriteRow(ctx, call)
}

func (tx *tabletTx) Commit() error {
	return tx.CommitTx(tx.ctx, tx.txID, &yt.CommitTxOptions{Sticky: true})
}

func (tx *tabletTx) Abort() error {
	return tx.AbortTx(tx.ctx, tx.txID, &yt.AbortTxOptions{Sticky: true})
}
