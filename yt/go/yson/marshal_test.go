package yson

import (
	"bytes"
	"fmt"
	"math"
	"reflect"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func testRoundtrip(t *testing.T, value interface{}) {
	t.Helper()
	t.Logf("checking value: %v", value)

	data, err := Marshal(value)
	assert.Nilf(t, err, "%v", err)

	valueCopy := reflect.New(reflect.TypeOf(value))
	err = Unmarshal(data, valueCopy.Interface())
	assert.Nilf(t, err, "%v", err)

	t.Logf("after unmarshal: %v", valueCopy.Elem())
	switch vv := value.(type) {
	case float32:
		if math.IsNaN(float64(vv)) {
			assert.True(t, math.IsNaN(float64(valueCopy.Elem().Interface().(float32))))
			return
		}
	case float64:
		if math.IsNaN(vv) {
			assert.True(t, math.IsNaN(valueCopy.Elem().Interface().(float64)))
			return
		}
	}

	assert.True(t, reflect.DeepEqual(value, valueCopy.Elem().Interface()))
}

func TestRoundtripBasicTypes(t *testing.T) {
	testRoundtrip(t, 0)
	testRoundtrip(t, 1)

	testRoundtrip(t, int(-10))
	testRoundtrip(t, uint(10))

	testRoundtrip(t, int16(-10))
	testRoundtrip(t, int32(-10))
	testRoundtrip(t, int64(-10))

	testRoundtrip(t, uint16(10))
	testRoundtrip(t, uint32(10))
	testRoundtrip(t, uint64(10))

	testRoundtrip(t, "")
	testRoundtrip(t, []byte(nil))

	testRoundtrip(t, "foo0")
	testRoundtrip(t, []byte{0x01, 0x02, 0x03})

	testRoundtrip(t, true)
	testRoundtrip(t, false)

	testRoundtrip(t, 3.14)
	testRoundtrip(t, math.NaN())
	testRoundtrip(t, math.Inf(1))
	testRoundtrip(t, math.Inf(-1))
}

func TestMarhalStruct(t *testing.T) {
	var simple simpleStruct
	simple.String = "bar0"
	simple.Int = 10

	testRoundtrip(t, simple)
}

type structWithMaps struct {
	M1 map[string]interface{}
	M2 map[string]int
}

func TestMarshalMaps(t *testing.T) {
	s := structWithMaps{
		M1: map[string]interface{}{
			"a": "c",
		},
		M2: map[string]int{
			"b": 2,
		},
	}

	testRoundtrip(t, s)
}

type customMarshal struct{}

func (s *customMarshal) MarshalYSON() ([]byte, error) {
	var buf bytes.Buffer
	w := NewWriterFormat(&buf, FormatBinary)
	w.BeginMap()
	w.MapKeyString("a")
	w.String("b")
	w.MapKeyString("c")
	w.BeginList()
	w.Int64(1)
	w.Int64(2)
	w.Int64(3)
	w.EndList()
	w.EndMap()

	err := w.Finish()
	return buf.Bytes(), err
}

var _ Marshaler = (*customMarshal)(nil)

func TestYSONTranscoding(t *testing.T) {
	data, err := Marshal(&customMarshal{})
	require.Nil(t, err)
	assert.Equal(t, []byte("{a=b;c=[1;2;3;];}"), data)
}

func TestMarshalTime(t *testing.T) {
	t0 := time.Now()
	b, err := t0.MarshalText()
	require.NoError(t, err)

	data, err := Marshal(t0)
	require.NoError(t, err)

	assert.Equal(t, fmt.Sprintf("%q", b), string(data))
}

func TestDecodeTime(t *testing.T) {
	t0 := time.Now()
	b, _ := t0.MarshalText()
	_ = t0.UnmarshalText(b)

	testRoundtrip(t, t0)
}
