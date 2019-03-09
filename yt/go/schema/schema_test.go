package schema

import (
	"testing"

	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/yt/go/yson"
)

func TestSchemaMarshalYSON(t *testing.T) {
	s := MustInfer(&testBasicTypes{})

	ys, err := yson.Marshal(&s)
	require.NoError(t, err)

	var s1 Schema
	err = yson.Unmarshal(ys, &s1)
	require.NoError(t, err)

	require.Equal(t, s, s1)
}
