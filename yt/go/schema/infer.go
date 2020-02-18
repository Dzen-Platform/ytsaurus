package schema

import (
	"encoding"
	"reflect"
	"sort"

	"golang.org/x/xerrors"

	"a.yandex-team.ru/yt/go/yson"
)

var (
	typeOfBytes           = reflect.TypeOf([]byte(nil))
	typeOfBinaryMarshaler = reflect.TypeOf((*encoding.BinaryMarshaler)(nil)).Elem()
	typeOfTextMarshaler   = reflect.TypeOf((*encoding.TextMarshaler)(nil)).Elem()
)

func ytTypeFor(typ reflect.Type) (ytTyp Type, err error) {
	if typ == typeOfBytes {
		return TypeBytes, nil
	}

	if typ.Implements(typeOfTextMarshaler) || (typ.Kind() != reflect.Ptr && reflect.PtrTo(typ).Implements(typeOfTextMarshaler)) {
		return TypeString, nil
	}

	if typ.Implements(typeOfBinaryMarshaler) || (typ.Kind() != reflect.Ptr && reflect.PtrTo(typ).Implements(typeOfBinaryMarshaler)) {
		return TypeBytes, nil
	}

	if typ.Kind() == reflect.Ptr {
		typ = typ.Elem()
	}

	switch typ.Kind() {
	case reflect.Int, reflect.Int64:
		return TypeInt64, nil
	case reflect.Int32:
		return TypeInt32, nil
	case reflect.Int16:
		return TypeInt16, nil

	case reflect.Uint, reflect.Uint64:
		return TypeUint64, nil
	case reflect.Uint32:
		return TypeUint32, nil
	case reflect.Uint16:
		return TypeUint16, nil

	case reflect.String:
		return TypeString, nil
	case reflect.Bool:
		return TypeBoolean, nil
	case reflect.Float64:
		return TypeFloat64, nil

	case reflect.Struct, reflect.Slice, reflect.Map, reflect.Interface, reflect.Array:
		return TypeAny, nil
	}

	return "", xerrors.Errorf("type %v has no associated YT type", typ)
}

func parseTag(fieldName string, typ reflect.Type, tag reflect.StructTag) (c *Column, err error) {
	c = &Column{Required: true}

	decodedTag, skip := yson.ParseTag(fieldName, tag)
	if skip {
		return nil, nil
	}

	c.Name = decodedTag.Name
	if decodedTag.Key {
		c.SortOrder = SortAscending
	}
	if decodedTag.Omitempty {
		c.Required = false
	}

	c.Type, err = ytTypeFor(typ)
	if typ.Kind() == reflect.Ptr || c.Type == TypeAny {
		c.Required = false
	}

	return
}

// Infer infers Schema from go struct.
//
// By default Infer creates column for each struct field.
// Type of the column corresponds to go type of the field.
//
// Mapping between go and YT types is defined as follows:
//
// Go types int, int16, int32, int64, uint, uint16, uint32, uint64, bool and float64 are mapped to equivalent YT types.
// NOTE: int8 and uint8 types are not supported.
//
// Go type []byte is mapped to YT type string.
//
// Go types implementing encoding.BinaryMarshaler interface are mapper to YT type string.
//
// Go type string is mapped to YT type utf8.
//
// Go types implementing encoding.TextMarshaler interface are mapped to YT type utf8.
func Infer(value interface{}) (s Schema, err error) {
	v, err := reflectValueOfType(value, reflect.Struct)
	if err != nil {
		return
	}

	s = Schema{}

	var inferFields func(typ reflect.Type, forceOptional bool) error
	inferFields = func(typ reflect.Type, forceOptional bool) (err error) {
		if typ.Kind() == reflect.Ptr {
			forceOptional = true
			typ = typ.Elem()
		}

		if v.Kind() != reflect.Struct {
			err = xerrors.Errorf("can't infer schema from type %v", v.Type())
		}

		for i := 0; i < typ.NumField(); i++ {
			field := typ.Field(i)

			if field.Anonymous {
				if err = inferFields(field.Type, forceOptional); err != nil {
					return
				}

				continue
			} else if field.PkgPath != "" {
				continue
			}

			var column *Column
			column, err = parseTag(field.Name, field.Type, field.Tag)
			if err != nil {
				return err
			}

			if column != nil {
				if forceOptional {
					column.Required = false
				}

				s.Columns = append(s.Columns, *column)
			}
		}

		return
	}

	err = inferFields(v.Type(), false)
	return
}

// InferMap infers Schema from go map[string]interface{}.
//
// InferMap creates column for each key value pair.
// Column name inferred from key itself, and column type inferred from the type of value.
//
// To avoid ambiguity key type should always be string, while value type doesn't matter.
func InferMap(value interface{}) (s Schema, err error) {
	v, err := reflectValueOfType(value, reflect.Map)
	if err != nil {
		return
	}

	iter := v.MapRange()
	for iter.Next() {
		k := iter.Key()
		if k.Kind() != reflect.String {
			err = xerrors.Errorf("can't infer schema from map with key of type %v, only string is supported", k.Type())
			return
		}

		name := k.Interface().(string)
		v := iter.Value()

		var typ reflect.Type
		if v.IsNil() {
			// In general nil value is an empty interface
			typ = v.Type()
		} else {
			typ = v.Elem().Type()
		}

		var column *Column
		column, err = parseTag(name, typ, "")
		if err != nil {
			return
		}

		if column != nil {
			s.Columns = append(s.Columns, *column)
		}
	}

	sort.Slice(s.Columns, func(i, j int) bool {
		return s.Columns[i].Name < s.Columns[j].Name
	})

	return
}

// reflectValueOfType creates value reflection of requested type for schema inferring
func reflectValueOfType(value interface{}, k reflect.Kind) (v reflect.Value, err error) {
	// Check for nil, reflect of nil value causes panic
	if value == nil {
		err = xerrors.New("can't infer schema from nil value")
		return
	}

	v = reflect.ValueOf(value)
	if v.Kind() == reflect.Ptr {
		v = v.Elem()
	}

	if v.Kind() != k {
		err = xerrors.Errorf("can't infer schema from value of type %v", v.Type())
	}
	return
}

// MustInfer infers Schema from go struct.
//
// MustInfer panics on errors.
func MustInfer(value interface{}) Schema {
	s, err := Infer(value)
	if err != nil {
		panic(err)
	}
	return s
}
