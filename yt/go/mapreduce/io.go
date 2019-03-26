package mapreduce

import (
	"fmt"
	"os"

	"a.yandex-team.ru/yt/go/yson"

	"golang.org/x/xerrors"
)

type jobContext struct {
	vault map[string]string

	in  *reader
	out []*writer
}

func (c *jobContext) LookupVault(name string) (value string, ok bool) {
	value, ok = c.vault[name]
	return
}

func (c *jobContext) initEnv() error {
	c.vault = map[string]string{}

	_ = yson.Unmarshal([]byte(os.Getenv("YT_SECURE_VAULT")), &c.vault)

	return nil
}

func (c *jobContext) onError(err error) {
	fmt.Fprintf(os.Stderr, "error: %+v\n", err)
	os.Exit(1)
}

func (c *jobContext) finish() error {
	if c.in.err != nil {
		return xerrors.Errorf("input reader error: %w", c.in.err)
	}

	for _, out := range c.out {
		_ = out.Close()

		if out.err != nil {
			return xerrors.Errorf("output writer error: %w", out.err)
		}
	}

	return nil
}

func (c *jobContext) writers() (out []Writer) {
	for _, w := range c.out {
		out = append(out, w)
	}

	return
}
